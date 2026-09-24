/*
 * Copyright (c) 2025 kmx.io.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * JBD2 journal replay for ext4fs.
 *
 * Implements the standard three-pass replay algorithm:
 *   1. SCAN   - walk the journal to find valid transactions
 *   2. REVOKE - collect revoked blocks
 *   3. REPLAY - write surviving data blocks to the filesystem
 *
 * All JBD2 on-disk fields are big-endian.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/dkio.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/vnode.h>

#include <lib/libkern/crc32c.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_journal.h>

/*
 * Read the internal journal inode directly from the inode table.
 * Returns a pointer into the buffer; caller must brelse(*bpp).
 */
int
jbd2_read_journal_inode (struct jbd2_replay_ctx *ctx, struct buf **bpp,
	struct ext4fs_dinode **dpp)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	struct ext4fs_block_group_descriptor *gd;
	u_int32_t ino;
	u_int32_t group, index;
	u_int64_t inode_offset, itb;
	u_int64_t blk;
	u_int32_t off;
	int error;

	ino = fs->m_journal_inode_number;
	if (ino == 0 || ino > fs->m_inodes_count ||
	    fs->m_inodes_per_group == 0) {
		printf("ext4fs: invalid journal inode %u\n", ino);
		return (EINVAL);
	}

	group = (ino - 1) / fs->m_inodes_per_group;
	index = (ino - 1) % fs->m_inodes_per_group;
	if (group >= fs->m_block_group_count ||
	    fs->m_inode_size < sizeof(struct ext4fs_dinode) ||
	    ((fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM) &&
	    fs->m_inode_size < sizeof(struct ext4fs_dinode_256)))
		return (EINVAL);
	gd = &fs->m_gd[group];

	itb = letoh32(gd->bgd_inode_table_block_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		itb |= (u_int64_t)letoh32(gd->bgd_inode_table_block_hi) << 32;

	inode_offset = (u_int64_t)index * fs->m_inode_size;
	if (itb >= fs->m_blocks_count ||
	    inode_offset / fs->m_block_size >= fs->m_blocks_count - itb)
		return (EINVAL);
	blk = itb + inode_offset / fs->m_block_size;
	off = inode_offset % fs->m_block_size;
	if (off + fs->m_inode_size > fs->m_block_size)
		return (EINVAL);

	error = bread(ctx->rc_devvp, (daddr_t)EXT4FS_FSBTODB(fs, blk),
	    fs->m_block_size, bpp);
	if (error) {
		brelse(*bpp);
		*bpp = NULL;
		printf("ext4fs: can't read journal inode\n");
		return (error);
	}

	*dpp = (struct ext4fs_dinode *)((char *)(*bpp)->b_data + off);
	if (ext4fs_inode_csum_verify(fs,
	    (struct ext4fs_dinode_256 *)*dpp, ino) != 0) {
		printf("ext4fs: journal inode checksum is invalid\n");
		brelse(*bpp);
		*bpp = NULL;
		*dpp = NULL;
		return (EINVAL);
	}
	if ((letoh16((*dpp)->i_mode) & S_IFMT) != S_IFREG) {
		printf("ext4fs: journal inode is not a regular file\n");
		brelse(*bpp);
		*bpp = NULL;
		*dpp = NULL;
		return (EINVAL);
	}
	return (0);
}

/* Validate an extent header before following any of its entries. */
int
jbd2_extent_header_check (struct ext4fs_extent_header *eh, size_t bytes,
    int expected_depth)
{
	u_int16_t depth, entries, max;
	size_t capacity;

	if (bytes < sizeof(*eh))
		return (EINVAL);
	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC)
		return (EINVAL);

	depth = letoh16(eh->eh_depth);
	entries = letoh16(eh->eh_entries);
	max = letoh16(eh->eh_max);
	capacity = (bytes - sizeof(*eh)) / sizeof(struct ext4fs_extent);

	if (depth > EXT4FS_EXTENT_DEPTH_MAX ||
	    (expected_depth >= 0 && depth != expected_depth) ||
	    max == 0 || max > capacity || entries > max)
		return (EINVAL);

	return (0);
}

int
jbd2_extent_block_csum_verify (struct jbd2_replay_ctx *ctx, void *buf)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	struct ext4fs_extent_header *eh = buf;
	u_int32_t crc, ino_le, provided, *tail;
	size_t tail_offset;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return (1);
	tail_offset = sizeof(*eh) +
	    (size_t)letoh16(eh->eh_max) * sizeof(struct ext4fs_extent);
	if (tail_offset > fs->m_block_size - sizeof(*tail))
		return (0);
	tail = (u_int32_t *)((char *)buf + tail_offset);
	provided = *tail;
	*tail = 0;
	ino_le = htole32(ctx->rc_journal_ino);
	crc = crc32c(ext4fs_csum_seed(fs), (const uint8_t *)&ino_le,
	    sizeof(ino_le));
	crc = crc32c(crc, (const uint8_t *)&ctx->rc_journal_gen,
	    sizeof(ctx->rc_journal_gen));
	crc = crc32c(crc, buf, tail_offset);
	*tail = provided;
	return (letoh32(provided) == ~crc);
}

/* Map one logical journal block through an extent tree of arbitrary depth. */
int
jbd2_extent_lookup (struct jbd2_replay_ctx *ctx,
    struct ext4fs_extent_header *eh, size_t bytes, int expected_depth,
    u_int32_t logical, u_int64_t *physical)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	struct ext4fs_extent *ext;
	struct ext4fs_extent_idx *idx;
	struct buf *bp = NULL;
	u_int16_t depth, entries, i;
	int chosen, error;

	error = jbd2_extent_header_check(eh, bytes, expected_depth);
	if (error)
		return (error);

	depth = letoh16(eh->eh_depth);
	entries = letoh16(eh->eh_entries);
	if (entries == 0)
		return (ENOENT);

	if (depth == 0) {
		ext = (struct ext4fs_extent *)(eh + 1);
		for (i = 0; i < entries; i++) {
			u_int16_t rawlen = letoh16(ext[i].e_len);
			u_int32_t lblk = letoh32(ext[i].e_block);
			u_int32_t len;
			u_int64_t pblock;

			/* Values above 0x8000 describe unwritten extents. */
			if (rawlen == 0 || rawlen > 0x8000)
				return (EINVAL);
			len = rawlen;
			pblock = (u_int64_t)letoh16(ext[i].e_start_hi) << 32 |
			    letoh32(ext[i].e_start_lo);
			if (pblock == 0 || pblock >= fs->m_blocks_count ||
			    len > fs->m_blocks_count - pblock)
				return (EINVAL);
			if (logical >= lblk && logical - lblk < len) {
				*physical = pblock + logical - lblk;
				return (0);
			}
		}
		return (ENOENT);
	}

	idx = (struct ext4fs_extent_idx *)(eh + 1);
	chosen = -1;
	for (i = 0; i < entries; i++) {
		u_int32_t lblk = letoh32(idx[i].ei_block);

		if (i > 0 && lblk <= letoh32(idx[i - 1].ei_block))
			return (EINVAL);
		if (lblk > logical)
			break;
		chosen = i;
	}
	if (chosen < 0)
		return (ENOENT);

	{
		u_int64_t child;

		child = (u_int64_t)letoh16(idx[chosen].ei_leaf_hi) << 32 |
		    letoh32(idx[chosen].ei_leaf_lo);
		if (child == 0 || child >= fs->m_blocks_count)
			return (EINVAL);
		error = bread(ctx->rc_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, child), fs->m_block_size, &bp);
		if (error) {
			if (bp != NULL)
				brelse(bp);
			return (error);
		}
		if (!jbd2_extent_block_csum_verify(ctx, bp->b_data)) {
			brelse(bp);
			return (EINVAL);
		}
		error = jbd2_extent_lookup(ctx,
		    (struct ext4fs_extent_header *)bp->b_data,
		    fs->m_block_size, depth - 1, logical, physical);
		brelse(bp);
	}

	return (error);
}

/* Fill the logical-to-physical journal block map recursively. */
int
jbd2_fill_blockmap_from_eh (struct jbd2_replay_ctx *ctx,
    struct ext4fs_extent_header *eh, size_t bytes, int expected_depth)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	struct ext4fs_extent *ext;
	struct ext4fs_extent_idx *idx;
	u_int16_t depth, entries, i;
	int error;

	error = jbd2_extent_header_check(eh, bytes, expected_depth);
	if (error) {
		printf("ext4fs: invalid journal extent tree\n");
		return (EINVAL);
	}

	depth = letoh16(eh->eh_depth);
	entries = letoh16(eh->eh_entries);

	if (depth == 0) {
		ext = (struct ext4fs_extent *)(eh + 1);
		for (i = 0; i < entries; i++) {
			u_int16_t rawlen = letoh16(ext[i].e_len);
			u_int32_t lblk = letoh32(ext[i].e_block);
			u_int32_t jblock, len;
			u_int64_t pblock;

			if (i > 0 && lblk <= letoh32(ext[i - 1].e_block))
				return (EINVAL);
			if (rawlen == 0 || rawlen > 0x8000)
				return (EINVAL);
			len = rawlen;
			pblock = (u_int64_t)letoh16(ext[i].e_start_hi) << 32 |
			    letoh32(ext[i].e_start_lo);
			if (pblock == 0 || pblock >= fs->m_blocks_count ||
			    len > fs->m_blocks_count - pblock)
				return (EINVAL);

			for (jblock = 0; jblock < len; jblock++) {
				u_int32_t j = lblk + jblock;
				if (j < lblk)
					return (EINVAL);
				if (j < ctx->rc_blockmap_count) {
					if (ctx->rc_blockmap[j].jb_fsblock != 0)
						return (EINVAL);
					ctx->rc_blockmap[j].jb_fsblock =
					    pblock + jblock;
				}
			}
		}
	} else {
		idx = (struct ext4fs_extent_idx *)(eh + 1);
		for (i = 0; i < entries; i++) {
			struct buf *bp = NULL;
			u_int64_t child;

			if (i > 0 && letoh32(idx[i].ei_block) <=
			    letoh32(idx[i - 1].ei_block))
				return (EINVAL);
			child =
			    (u_int64_t)letoh16(idx[i].ei_leaf_hi) << 32 |
			    letoh32(idx[i].ei_leaf_lo);
			if (child == 0 || child >= fs->m_blocks_count)
				return (EINVAL);

			error = bread(ctx->rc_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, child),
			    fs->m_block_size, &bp);
			if (error) {
				if (bp != NULL)
					brelse(bp);
				printf("ext4fs: journal blockmap: "
				    "can't read index block\n");
				return (error);
			}
			if (!jbd2_extent_block_csum_verify(ctx, bp->b_data)) {
				brelse(bp);
				return (EINVAL);
			}
			error = jbd2_fill_blockmap_from_eh(ctx,
			    (struct ext4fs_extent_header *)bp->b_data,
			    fs->m_block_size, depth - 1);
			brelse(bp);
			if (error)
				return (error);
		}
	}

	return (0);
}

/*
 * Build the journal block map by reading inode 8's extent tree.
 */
int
jbd2_build_blockmap (struct jbd2_replay_ctx *ctx)
{
	u_int32_t hash, i, mask, maxblocks, slots;
	u_int64_t fsblock;
	int error;

	maxblocks = ctx->rc_maxlen;
	ctx->rc_blockmap = mallocarray(maxblocks,
	    sizeof(struct jbd2_blockmap_entry), M_TEMP, M_WAITOK | M_ZERO);
	ctx->rc_blockmap_count = maxblocks;

	error = jbd2_fill_blockmap_from_eh(ctx, ctx->rc_journal_eh,
	    sizeof(((struct ext4fs_dinode *)0)->i_block), -1);
	if (error)
		return (error);

	for (i = 0; i < maxblocks; i++) {
		if (ctx->rc_blockmap[i].jb_fsblock == 0) {
			printf("ext4fs: journal logical block %u is not mapped\n", i);
			return (EINVAL);
		}
	}

	/* Build an O(1) membership set and reject physical extent aliases. */
	slots = 1;
	while (slots < maxblocks * 2)
		slots <<= 1;
	ctx->rc_blockset = mallocarray(slots, sizeof(*ctx->rc_blockset),
	    M_TEMP, M_WAITOK | M_ZERO);
	ctx->rc_blockset_mask = mask = slots - 1;
	for (i = 0; i < maxblocks; i++) {
		fsblock = ctx->rc_blockmap[i].jb_fsblock;
		hash = ((u_int32_t)fsblock ^ (u_int32_t)(fsblock >> 32)) *
		    2654435761U;
		hash &= mask;
		while (ctx->rc_blockset[hash] != 0) {
			if (ctx->rc_blockset[hash] == fsblock) {
				printf("ext4fs: journal extents alias block %llu\n",
				    (unsigned long long)fsblock);
				return (EINVAL);
			}
			hash = (hash + 1) & mask;
		}
		ctx->rc_blockset[hash] = fsblock;
	}

	return (0);
}

/*
 * Read a journal block by journal-relative block number.
 */
int
jbd2_read_block (struct jbd2_replay_ctx *ctx, u_int32_t jblock,
    struct buf **bpp)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	u_int64_t fsblock;
	int error;

	*bpp = NULL;

	if (jblock >= ctx->rc_blockmap_count) {
		printf("ext4fs: journal block %u out of range (%u)\n",
		    jblock, ctx->rc_blockmap_count);
		return (EIO);
	}

	fsblock = ctx->rc_blockmap[jblock].jb_fsblock;
	if (fsblock == 0) {
		printf("ext4fs: journal block %u not mapped\n", jblock);
		return (EIO);
	}

	error = bread(ctx->rc_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, fsblock), fs->m_block_size, bpp);
	if (error && *bpp != NULL) {
		brelse(*bpp);
		*bpp = NULL;
	}
	return (error);
}

/*
 * Wrap journal block number circularly.
 */
u_int32_t
jbd2_next_block (struct jbd2_replay_ctx *ctx, u_int32_t block)
{
	block++;
	if (block >= ctx->rc_maxlen)
		block = ctx->rc_first;
	return block;
}

int
jbd2_is_journal_block (struct jbd2_replay_ctx *ctx, u_int64_t fsblock)
{
	u_int32_t hash;

	if (ctx->rc_blockset == NULL)
		return (0);
	hash = ((u_int32_t)fsblock ^ (u_int32_t)(fsblock >> 32)) *
	    2654435761U;
	hash &= ctx->rc_blockset_mask;
	while (ctx->rc_blockset[hash] != 0) {
		if (ctx->rc_blockset[hash] == fsblock)
			return (1);
		hash = (hash + 1) & ctx->rc_blockset_mask;
	}
	return (0);
}

int
jbd2_has_csum_v2or3 (struct jbd2_replay_ctx *ctx)
{
	return ((ctx->rc_features_incompat &
	    (JBD2_FEATURE_INCOMPAT_CSUM_V2 |
	    JBD2_FEATURE_INCOMPAT_CSUM_V3)) != 0);
}

/*
 * crc32c() exposes the complemented CRC representation, whereas JBD2
 * stores the raw crc32c(~0, ...) result.  Complement the final value when
 * comparing it with a JBD2 field.
 */
u_int32_t
jbd2_block_checksum (struct jbd2_replay_ctx *ctx, const void *data, size_t len)
{
	return (~crc32c(ctx->rc_checksum_seed, data, len));
}

int
jbd2_metadata_block_csum_verify (struct jbd2_replay_ctx *ctx, void *data)
{
	struct jbd2_block_tail *tail;
	u_int32_t provided, calculated;

	if (!jbd2_has_csum_v2or3(ctx))
		return (1);

	tail = (struct jbd2_block_tail *)((char *)data +
	    ctx->rc_blocksize - sizeof(*tail));
	provided = tail->t_checksum;
	tail->t_checksum = 0;
	calculated = jbd2_block_checksum(ctx, data, ctx->rc_blocksize);
	tail->t_checksum = provided;

	return (betoh32(provided) == calculated);
}

int
jbd2_commit_block_csum_verify (struct jbd2_replay_ctx *ctx, void *data)
{
	struct jbd2_commit_header *commit;
	u_int32_t provided, calculated;

	if (!jbd2_has_csum_v2or3(ctx))
		return (1);

	commit = data;
	/*
	 * Current checksum-v2/v3 writers select CRC32C in the journal
	 * superblock and may leave the legacy per-commit type/size pair zero.
	 */
	if (!((commit->h_checksum_type == 0 &&
	    commit->h_checksum_size == 0) ||
	    (commit->h_checksum_type == JBD2_CHECKSUM_CRC32C &&
	    commit->h_checksum_size == JBD2_CHECKSUM_SIZE)))
		return (0);
	provided = commit->h_checksum[0];
	commit->h_checksum[0] = 0;
	calculated = jbd2_block_checksum(ctx, data, ctx->rc_blocksize);
	commit->h_checksum[0] = provided;

	return (betoh32(provided) == calculated);
}

int
jbd2_data_block_csum_verify (struct jbd2_replay_ctx *ctx, void *data,
    u_int32_t sequence, u_int32_t provided)
{
	u_int32_t crc;
	u_int32_t sequence_be;

	if (!jbd2_has_csum_v2or3(ctx))
		return (1);

	sequence_be = htobe32(sequence);
	crc = crc32c(ctx->rc_checksum_seed, (const uint8_t *)&sequence_be,
	    sizeof(sequence_be));
	crc = ~crc32c(crc, data, ctx->rc_blocksize);

	if (ctx->rc_features_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3)
		return (crc == provided);
	return ((u_int16_t)crc == (u_int16_t)provided);
}

u_int32_t
jbd2_descriptor_limit (struct jbd2_replay_ctx *ctx)
{
	u_int32_t limit = ctx->rc_blocksize;

	if (jbd2_has_csum_v2or3(ctx))
		limit -= sizeof(struct jbd2_block_tail);
	return (limit);
}

/*
 * Parse one descriptor tag from a descriptor block.
 *
 * Returns 0 on success, sets *target to the filesystem block,
 * *flags to the tag flags, and advances *offset past the tag.
 */
int
jbd2_parse_tag (struct jbd2_replay_ctx *ctx, char *buf, u_int32_t bufsize,
    u_int32_t *offset, u_int64_t *target, u_int32_t *flags,
    u_int32_t *checksum, int *uuid_seen)
{
	const u_int8_t zero_uuid[16] = { 0 };
	int has_csum_v2, has_csum_v3, has_64bit;
	u_int32_t known_flags, tag_size;

	has_csum_v2 = ctx->rc_features_incompat &
	    JBD2_FEATURE_INCOMPAT_CSUM_V2;
	has_csum_v3 = ctx->rc_features_incompat &
	    JBD2_FEATURE_INCOMPAT_CSUM_V3;
	has_64bit = ctx->rc_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT;

	if (has_csum_v3) {
		struct jbd2_block_tag3 *tag3;

		tag_size = sizeof(struct jbd2_block_tag3);
		if (*offset + tag_size > bufsize)
			return (EINVAL);

		tag3 = (struct jbd2_block_tag3 *)(buf + *offset);
		*target = betoh32(tag3->t_blocknr);
		*flags = betoh32(tag3->t_flags);
		*checksum = betoh32(tag3->t_checksum);
		if (has_64bit)
			*target |= (u_int64_t)betoh32(tag3->t_blocknr_high)
			    << 32;
		else if (tag3->t_blocknr_high != 0)
			return (EINVAL);

		*offset += tag_size;
	} else {
		struct jbd2_block_tag *tag;

		tag_size = 8;	/* minimum: blocknr + checksum + flags */
		if (*offset + tag_size > bufsize)
			return (EINVAL);

		tag = (struct jbd2_block_tag *)(buf + *offset);
		*target = betoh32(tag->t_blocknr);
		*flags = betoh16(tag->t_flags);
		*checksum = betoh16(tag->t_checksum);

		*offset += tag_size;
		if (has_64bit) {
			if (*offset + 4 > bufsize)
				return (EINVAL);
			*target |= (u_int64_t)betoh32(tag->t_blocknr_high)
			    << 32;
			*offset += 4;
		}
	}
	if (has_csum_v2) {
		if (*offset + JBD2_CSUM_V2_TAG_EXTRA > bufsize)
			return (EINVAL);
		*offset += JBD2_CSUM_V2_TAG_EXTRA;
	}
	/* Only the low 16 bits carry defined tag flags on disk. */
	if (has_csum_v3)
		*flags &= 0xffff;

	known_flags = JBD2_FLAG_ESCAPE | JBD2_FLAG_SAME_UUID |
	    JBD2_FLAG_DELETED | JBD2_FLAG_LAST_TAG;
	if (*flags & ~known_flags)
		return (EINVAL);
	if (*target >= ctx->rc_fs->m_blocks_count)
		return (EINVAL);
	if (jbd2_is_journal_block(ctx, *target))
		return (EINVAL);

	if (*flags & JBD2_FLAG_SAME_UUID) {
		if (!*uuid_seen)
			return (EINVAL);
	} else {
		/*
		 * The open-coded UUID is present only when SAME_UUID is clear.
		 * e2fsprogs writes a zero UUID here; accept it because the journal
		 * superblock and the data checksum still bind the tag to this journal.
		 */
		if (*offset + sizeof(ctx->rc_uuid) > bufsize ||
		    (memcmp(buf + *offset, ctx->rc_uuid,
		    sizeof(ctx->rc_uuid)) != 0 &&
		    memcmp(buf + *offset, zero_uuid, sizeof(zero_uuid)) != 0))
			return (EINVAL);
		*offset += sizeof(ctx->rc_uuid);
		*uuid_seen = 1;
	}

	return (0);
}

/*
 * Add a block to the revocation table.
 */
int
jbd2_revoke_add (struct jbd2_replay_ctx *ctx, u_int64_t block,
    u_int32_t sequence)
{
	u_int32_t hash, mask;
	u_int64_t key;

	if (ctx->rc_revoke == NULL) {
		ctx->rc_revoke_alloc = JBD2_MAX_REVOKE_ENTRIES * 2;
		ctx->rc_revoke = mallocarray(ctx->rc_revoke_alloc,
		    sizeof(*ctx->rc_revoke), M_TEMP, M_WAITOK | M_ZERO);
	}
	key = block + 1;
	mask = ctx->rc_revoke_alloc - 1;
	hash = ((u_int32_t)block ^ (u_int32_t)(block >> 32)) *
	    2654435761U;
	hash &= mask;
	while (ctx->rc_revoke[hash].re_block != 0) {
		if (ctx->rc_revoke[hash].re_block == key) {
			if ((int32_t)(sequence -
			    ctx->rc_revoke[hash].re_sequence) >= 0)
				ctx->rc_revoke[hash].re_sequence = sequence;
			return (0);
		}
		hash = (hash + 1) & mask;
	}
	if (ctx->rc_revoke_count >= JBD2_MAX_REVOKE_ENTRIES)
		return (EFBIG);
	ctx->rc_revoke[hash].re_block = key;
	ctx->rc_revoke[hash].re_sequence = sequence;
	ctx->rc_revoke_count++;
	return (0);
}

/*
 * Check if a block is revoked at or after the given sequence.
 */
int
jbd2_revoke_check (struct jbd2_replay_ctx *ctx, u_int64_t block,
    u_int32_t sequence)
{
	u_int32_t hash, mask;
	u_int64_t key;

	if (ctx->rc_revoke == NULL)
		return (0);
	key = block + 1;
	mask = ctx->rc_revoke_alloc - 1;
	hash = ((u_int32_t)block ^ (u_int32_t)(block >> 32)) *
	    2654435761U;
	hash &= mask;
	while (ctx->rc_revoke[hash].re_block != 0) {
		if (ctx->rc_revoke[hash].re_block == key)
			return ((int32_t)(ctx->rc_revoke[hash].re_sequence -
			    sequence) >= 0);
		hash = (hash + 1) & mask;
	}
	return (0);
}

int
jbd2_revoke_block_check (struct jbd2_replay_ctx *ctx, struct buf *bp,
    u_int32_t *count)
{
	struct jbd2_revoke_header *rh;
	u_int32_t bytes, limit, record_size;

	if (!(ctx->rc_features_incompat & JBD2_FEATURE_INCOMPAT_REVOKE))
		return (EINVAL);
	if (!jbd2_metadata_block_csum_verify(ctx, bp->b_data))
		return (EINVAL);

	rh = (struct jbd2_revoke_header *)bp->b_data;
	bytes = betoh32(rh->r_count);
	limit = jbd2_descriptor_limit(ctx);
	record_size = (ctx->rc_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT) ? 8 : 4;

	if (bytes < sizeof(*rh) || bytes > limit ||
	    (bytes - sizeof(*rh)) % record_size != 0)
		return (EINVAL);
	*count = (bytes - sizeof(*rh)) / record_size;
	return (0);
}

/*
 * Count data blocks described by a descriptor block's tags.
 */
int
jbd2_count_tags (struct jbd2_replay_ctx *ctx, struct buf *bp,
    u_int32_t *count)
{
	char *buf;
	u_int32_t checksum, offset, bufsize, flags;
	u_int64_t target;
	int error, seen, uuid_seen;

	buf = (char *)bp->b_data;
	bufsize = jbd2_descriptor_limit(ctx);
	offset = sizeof(struct jbd2_header);
	*count = 0;
	seen = 0;
	uuid_seen = 0;

	while (offset < bufsize) {
		error = jbd2_parse_tag(ctx, buf, bufsize, &offset,
		    &target, &flags, &checksum, &uuid_seen);
		if (error)
			return (error);
		seen = 1;
		if (!(flags & JBD2_FLAG_DELETED))
			(*count)++;
		if (flags & JBD2_FLAG_LAST_TAG)
			return (0);
	}
	/* A descriptor that exactly fills the tag area needs no LAST_TAG. */
	return (seen && offset == bufsize ? 0 : EINVAL);
}

/*
 * Pass 1: SCAN
 *
 * Walk the journal from s_start/s_sequence, verify each transaction
 * has a matching DESCRIPTOR and COMMIT block, and find the end of
 * the valid journal.
 */
int
jbd2_pass_scan (struct jbd2_replay_ctx *ctx)
{
	u_int32_t block, consumed, loglen, revoke_count, seq, tag_count;
	struct buf *bp;
	struct jbd2_header *hdr;
	int error, in_transaction;

	block = ctx->rc_start;
	seq = ctx->rc_sequence;
	ctx->rc_end_sequence = seq;
	consumed = 0;
	in_transaction = 0;
	loglen = ctx->rc_maxlen - ctx->rc_first;

	printf("ext4fs: journal scan: start block %u sequence %u\n",
	    block, seq);

	while (consumed < loglen) {
		error = jbd2_read_block(ctx, block, &bp);
		if (error) {
			printf("ext4fs: journal scan: read error at "
			    "block %u\n", block);
			return (error);
		}
		consumed++;

		hdr = (struct jbd2_header *)bp->b_data;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC) {
			brelse(bp);
			if (in_transaction)
				return (EINVAL);
			break;
		}
		if (betoh32(hdr->h_sequence) != seq) {
			brelse(bp);
			if (in_transaction)
				return (EINVAL);
			break;
		}

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			in_transaction = 1;
			if (!jbd2_metadata_block_csum_verify(ctx,
			    bp->b_data)) {
				brelse(bp);
				printf("ext4fs: bad journal descriptor checksum\n");
				return (EINVAL);
			}
			error = jbd2_count_tags(ctx, bp, &tag_count);
			brelse(bp);
			if (error)
				return (error);
			if (tag_count > loglen - consumed)
				return (EINVAL);

			{
				u_int32_t i;
				for (i = 0; i < tag_count; i++)
					block = jbd2_next_block(ctx, block);
			}
			consumed += tag_count;
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_REVOKE_BLOCK:
			in_transaction = 1;
			error = jbd2_revoke_block_check(ctx, bp,
			    &revoke_count);
			brelse(bp);
			if (error)
				return (error);
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_COMMIT_BLOCK:
			if (!in_transaction) {
				brelse(bp);
				return (EINVAL);
			}
			if (!jbd2_commit_block_csum_verify(ctx, bp->b_data)) {
				brelse(bp);
				printf("ext4fs: bad journal commit checksum\n");
				return (EINVAL);
			}
			brelse(bp);
			seq++;
			in_transaction = 0;
			ctx->rc_end_sequence = seq;
			block = jbd2_next_block(ctx, block);
			break;

		default:
			brelse(bp);
			return (EINVAL);
		}
	}
	if (in_transaction)
		return (EINVAL);

	printf("ext4fs: journal scan: end sequence %u (%u transactions)\n",
	    ctx->rc_end_sequence,
	    ctx->rc_end_sequence - ctx->rc_sequence);
	return (0);
}

/*
 * Pass 2: REVOKE
 *
 * Walk the journal again, collecting revoked blocks.
 */
int
jbd2_pass_revoke (struct jbd2_replay_ctx *ctx)
{
	u_int32_t block, consumed, loglen, revoke_count, seq, tag_count;
	struct buf *bp;
	struct jbd2_header *hdr;
	int has_64bit;
	int error;

	has_64bit = ctx->rc_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT;

	block = ctx->rc_start;
	seq = ctx->rc_sequence;
	consumed = 0;
	loglen = ctx->rc_maxlen - ctx->rc_first;

	while (seq != ctx->rc_end_sequence && consumed < loglen) {
		error = jbd2_read_block(ctx, block, &bp);
		if (error)
			return (error);
		consumed++;

		hdr = (struct jbd2_header *)bp->b_data;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq) {
			brelse(bp);
			return (EINVAL);
		}

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			if (!jbd2_metadata_block_csum_verify(ctx,
			    bp->b_data)) {
				brelse(bp);
				return (EINVAL);
			}
			error = jbd2_count_tags(ctx, bp, &tag_count);
			brelse(bp);
			if (error)
				return (error);
			if (tag_count > loglen - consumed)
				return (EINVAL);
			{
				u_int32_t i;
				for (i = 0; i < tag_count; i++)
					block = jbd2_next_block(ctx, block);
			}
			consumed += tag_count;
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_REVOKE_BLOCK:
			error = jbd2_revoke_block_check(ctx, bp,
			    &revoke_count);
			if (error) {
				brelse(bp);
				return (error);
			}
			{
				u_int32_t i, off;

				off = sizeof(struct jbd2_revoke_header);
				for (i = 0; i < revoke_count; i++) {
					u_int64_t revblk;
					if (has_64bit) {
						u_int32_t high, low;

						memcpy(&high,
						    (char *)bp->b_data + off, 4);
						memcpy(&low,
						    (char *)bp->b_data + off + 4, 4);
						revblk = (u_int64_t)betoh32(high)
						    << 32 | betoh32(low);
						off += 8;
					} else {
						u_int32_t value;

						memcpy(&value,
						    (char *)bp->b_data + off, 4);
						revblk = betoh32(value);
						off += 4;
					}
					if (revblk >= ctx->rc_fs->m_blocks_count) {
						brelse(bp);
						return (EINVAL);
					}
					if (jbd2_is_journal_block(ctx, revblk)) {
						brelse(bp);
						return (EINVAL);
					}
					error = jbd2_revoke_add(ctx, revblk, seq);
					if (error) {
						brelse(bp);
						return (error);
					}
				}
			}
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_COMMIT_BLOCK:
			if (!jbd2_commit_block_csum_verify(ctx, bp->b_data)) {
				brelse(bp);
				return (EINVAL);
			}
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			seq++;
			break;

		default:
			brelse(bp);
			return (EINVAL);
		}
	}
	if (seq != ctx->rc_end_sequence)
		return (EINVAL);

	if (ctx->rc_revoke_count > 0)
		printf("ext4fs: journal revoke: %u blocks revoked\n",
		    ctx->rc_revoke_count);

	return (0);
}

/*
 * Pass 3: REPLAY
 *
 * Walk the journal a third time, writing data blocks to the filesystem
 * that have not been revoked.
 */
int
jbd2_pass_replay (struct jbd2_replay_ctx *ctx, int apply)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	u_int32_t block, consumed, loglen, revoke_count, seq, tag_count;
	u_int32_t replayed = 0;
	struct buf *bp, *dbp, *wbp;
	struct jbd2_header *hdr;
	char *buf;
	u_int32_t checksum, offset, bufsize, flags;
	u_int64_t target;
	int error, uuid_seen;

	block = ctx->rc_start;
	seq = ctx->rc_sequence;
	consumed = 0;
	loglen = ctx->rc_maxlen - ctx->rc_first;

	while (seq != ctx->rc_end_sequence && consumed < loglen) {
		error = jbd2_read_block(ctx, block, &bp);
		if (error)
			return (error);
		consumed++;

		hdr = (struct jbd2_header *)bp->b_data;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq) {
			brelse(bp);
			return (EINVAL);
		}

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			if (!jbd2_metadata_block_csum_verify(ctx,
			    bp->b_data)) {
				brelse(bp);
				return (EINVAL);
			}
			error = jbd2_count_tags(ctx, bp, &tag_count);
			if (error) {
				brelse(bp);
				return (error);
			}
			if (tag_count > loglen - consumed) {
				brelse(bp);
				return (EINVAL);
			}
			buf = (char *)bp->b_data;
			bufsize = jbd2_descriptor_limit(ctx);
			offset = sizeof(struct jbd2_header);
			uuid_seen = 0;

			/* Iterate over tags, each followed by a data block */
			while (offset < bufsize) {
				error = jbd2_parse_tag(ctx, buf, bufsize,
				    &offset, &target, &flags, &checksum,
				    &uuid_seen);
				if (error) {
					brelse(bp);
					return (error);
				}

				/* Deleted tags have no corresponding data block. */
				if (flags & JBD2_FLAG_DELETED) {
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
				}

				/* Advance to data block */
				block = jbd2_next_block(ctx, block);
				consumed++;

				/* Skip if revoked */
				if (jbd2_revoke_check(ctx, target, seq)) {
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
				}

				/* Read the journal data block */
				error = jbd2_read_block(ctx, block, &dbp);
				if (error) {
					printf("ext4fs: journal replay: "
					    "read error block %u\n", block);
					brelse(bp);
					return (error);
				}
				if (!jbd2_data_block_csum_verify(ctx,
				    dbp->b_data, seq, checksum)) {
					printf("ext4fs: journal replay: bad data "
					    "checksum at block %u\n", block);
					brelse(dbp);
					brelse(bp);
					return (EINVAL);
				}
				if (!apply) {
					brelse(dbp);
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
				}

				/* Read the target filesystem block */
				wbp = NULL;
				error = bread(ctx->rc_devvp,
				    (daddr_t)EXT4FS_FSBTODB(fs, target),
				    fs->m_block_size, &wbp);
				if (error) {
					if (wbp != NULL)
						brelse(wbp);
					brelse(dbp);
					printf("ext4fs: journal replay: "
					    "can't read target %llu\n",
					    (unsigned long long)target);
					brelse(bp);
					return (error);
				}

				/* Copy data */
				memcpy(wbp->b_data, dbp->b_data,
				    fs->m_block_size);
				brelse(dbp);

				/* Un-escape: restore JBD2 magic if needed */
				if (flags & JBD2_FLAG_ESCAPE) {
					u_int32_t magic = htobe32(JBD2_MAGIC);
					memcpy(wbp->b_data, &magic, 4);
				}

				/* Write to filesystem */
				error = bwrite(wbp);
				if (error) {
					printf("ext4fs: journal replay: "
					    "write error target %llu\n",
					    (unsigned long long)target);
					brelse(bp);
					return (error);
				} else {
					replayed++;
				}

				if (flags & JBD2_FLAG_LAST_TAG)
					break;
			}

			brelse(bp);
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_REVOKE_BLOCK:
			error = jbd2_revoke_block_check(ctx, bp,
			    &revoke_count);
			brelse(bp);
			if (error)
				return (error);
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_COMMIT_BLOCK:
			if (!jbd2_commit_block_csum_verify(ctx, bp->b_data)) {
				brelse(bp);
				return (EINVAL);
			}
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			seq++;
			break;

		default:
			brelse(bp);
			return (EINVAL);
		}
	}
	if (seq != ctx->rc_end_sequence)
		return (EINVAL);

	if (apply) {
		ctx->rc_replay_count = replayed;
		printf("ext4fs: journal replay: %u blocks replayed\n",
		    replayed);
	}

	return (0);
}

int
jbd2_superblock_csum_verify (struct jbd2_replay_ctx *ctx,
    struct jbd2_superblock *jsb)
{
	u_int32_t provided, calculated;

	if (!jbd2_has_csum_v2or3(ctx))
		return (1);

	provided = jsb->s_checksum;
	jsb->s_checksum = 0;
	calculated = ~crc32c(0, (const uint8_t *)jsb, sizeof(*jsb));
	jsb->s_checksum = provided;
	return (betoh32(provided) == calculated);
}

void
jbd2_superblock_csum_set (struct jbd2_replay_ctx *ctx,
    struct jbd2_superblock *jsb)
{
	u_int32_t checksum;

	if (!jbd2_has_csum_v2or3(ctx))
		return;
	jsb->s_checksum = 0;
	checksum = ~crc32c(0, (const uint8_t *)jsb, sizeof(*jsb));
	jsb->s_checksum = htobe32(checksum);
}

int
jbd2_recovered_super_check (struct jbd2_replay_ctx *ctx)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	struct ext4fs *sble;
	struct buf *bp;
	int error;

	bp = NULL;
	error = bread(ctx->rc_devvp,
	    (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE),
	    EXT4FS_SUPER_BLOCK_SIZE, &bp);
	if (error) {
		if (bp != NULL)
			brelse(bp);
		return (error);
	}
	sble = (struct ext4fs *)bp->b_data;
	error = ext4fs_sbcheck(sble, fs->m_read_only);
	if (error == 0 && memcmp(sble->sb_uuid, fs->m_sble.sb_uuid,
	    sizeof(sble->sb_uuid)) != 0)
		error = EINVAL;
	brelse(bp);
	return (error);
}

int
jbd2_flush_device (struct vnode *devvp, struct proc *p)
{
	int error, force;

	if (p == NULL)
		p = curproc;
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_FSYNC(devvp, FSCRED, MNT_WAIT, p);
	VOP_UNLOCK(devvp);
	if (error)
		return (error);

	/*
	 * VOP_FSYNC on a block device drains the buffer cache, but it does
	 * not order those writes past a drive's volatile write cache.  Request a
	 * cache synchronization when the device supports it; virtual devices may
	 * expose VOP_FSYNC as their only durability primitive.
	 */
	force = 1;
	error = VOP_IOCTL(devvp, DIOCCACHESYNC, &force, FWRITE, FSCRED, p);
	/* File-backed and virtual block devices may provide only VOP_FSYNC. */
	if (error == ENOTTY || error == EOPNOTSUPP)
		return (0);
	return (error);
}

/*
 * Open and validate the internal journal, then build its complete logical to
 * physical block map.  Recovery and the runtime journal core both use this
 * path so they cannot disagree about journal geometry or feature support.
 */
int
jbd2_journal_open (struct vnode *devvp, struct m_ext4fs *fs,
    struct jbd2_replay_ctx *ctx, u_int64_t *jblock0p)
{
	struct jbd2_superblock *jsb;
	struct ext4fs_dinode *jdi;
	struct buf *bp, *ibp;
	u_int64_t jblock0, journal_bytes, journal_blocks;
	u_int32_t btype, unsupported;
	int error;

	memset(ctx, 0, sizeof(*ctx));
	ctx->rc_devvp = devvp;
	ctx->rc_fs = fs;
	bp = NULL;
	ibp = NULL;

	error = jbd2_read_journal_inode(ctx, &ibp, &jdi);
	if (error)
		goto out;

	ctx->rc_journal_eh = &jdi->i_extent_header;
	ctx->rc_journal_ino = fs->m_journal_inode_number;
	ctx->rc_journal_gen = jdi->i_nfs_generation;
	if (!(letoh32(jdi->i_flags) & EXTFS_INODE_FLAG_EXTENTS)) {
		printf("ext4fs: journal inode does not use extents\n");
		error = EINVAL;
		goto out;
	}
	journal_bytes = letoh32(jdi->i_size_lo) |
	    (u_int64_t)letoh32(jdi->i_size_hi) << 32;
	if (journal_bytes < fs->m_block_size) {
		printf("ext4fs: journal inode is too small\n");
		error = EINVAL;
		goto out;
	}

	error = jbd2_extent_lookup(ctx, ctx->rc_journal_eh,
	    sizeof(jdi->i_block), -1, 0, &jblock0);
	if (error || jblock0 == 0) {
		printf("ext4fs: can't locate journal block 0\n");
		error = error ? error : EINVAL;
		goto out;
	}

	error = bread(devvp, (daddr_t)EXT4FS_FSBTODB(fs, jblock0),
	    fs->m_block_size, &bp);
	if (error) {
		printf("ext4fs: can't read journal superblock\n");
		goto out;
	}
	jsb = (struct jbd2_superblock *)bp->b_data;

	if (betoh32(jsb->s_header.h_magic) != JBD2_MAGIC) {
		printf("ext4fs: bad journal magic 0x%x\n",
		    betoh32(jsb->s_header.h_magic));
		error = EINVAL;
		goto out;
	}
	btype = betoh32(jsb->s_header.h_blocktype);
	if (btype != JBD2_SUPERBLOCK_V2) {
		printf("ext4fs: unsupported journal superblock version %u\n",
		    btype);
		error = EINVAL;
		goto out;
	}
	if (betoh32(jsb->s_errno) != 0) {
		printf("ext4fs: journal records error %u\n",
		    betoh32(jsb->s_errno));
		error = EIO;
		goto out;
	}

	ctx->rc_blocksize = betoh32(jsb->s_blocksize);
	ctx->rc_maxlen = betoh32(jsb->s_maxlen);
	ctx->rc_first = betoh32(jsb->s_first);
	ctx->rc_sequence = betoh32(jsb->s_sequence);
	ctx->rc_start = betoh32(jsb->s_start);
	ctx->rc_max_transaction = betoh32(jsb->s_max_transaction);
	ctx->rc_features_compat = betoh32(jsb->s_feature_compat);
	ctx->rc_features_incompat = betoh32(jsb->s_feature_incompat);
	ctx->rc_features_ro_compat = betoh32(jsb->s_feature_ro_compat);
	if (betoh32(jsb->s_nr_users) != 1) {
		printf("ext4fs: shared journal is not supported\n");
		error = EINVAL;
		goto out;
	}

	if (ctx->rc_blocksize != fs->m_block_size) {
		printf("ext4fs: journal blocksize %u != fs blocksize %llu\n",
		    ctx->rc_blocksize, (unsigned long long)fs->m_block_size);
		error = EINVAL;
		goto out;
	}
	if (journal_bytes % ctx->rc_blocksize != 0) {
		printf("ext4fs: journal inode size is not block aligned\n");
		error = EINVAL;
		goto out;
	}
	journal_blocks = journal_bytes / ctx->rc_blocksize;
	if (ctx->rc_first == 0 || ctx->rc_first >= ctx->rc_maxlen ||
	    ctx->rc_maxlen > journal_blocks ||
	    ctx->rc_maxlen > JBD2_MAX_BLOCKMAP_ENTRIES ||
	    (ctx->rc_start != 0 && (ctx->rc_start < ctx->rc_first ||
	    ctx->rc_start >= ctx->rc_maxlen))) {
		printf("ext4fs: invalid journal geometry: first=%u start=%u "
		    "maxlen=%u inode-blocks=%llu\n", ctx->rc_first,
		    ctx->rc_start, ctx->rc_maxlen,
		    (unsigned long long)journal_blocks);
		error = EINVAL;
		goto out;
	}

	unsupported = ctx->rc_features_compat &
	    ~JBD2_FEATURE_COMPAT_SUPPORTED;
	if (unsupported != 0) {
		printf("ext4fs: unsupported journal compat features 0x%x\n",
		    unsupported);
		error = EINVAL;
		goto out;
	}
	unsupported = ctx->rc_features_incompat &
	    ~JBD2_FEATURE_INCOMPAT_SUPPORTED;
	if (unsupported != 0) {
		printf("ext4fs: unsupported journal incompat features 0x%x\n",
		    unsupported);
		error = EINVAL;
		goto out;
	}
	unsupported = ctx->rc_features_ro_compat &
	    ~JBD2_FEATURE_RO_COMPAT_SUPPORTED;
	if (unsupported != 0) {
		printf("ext4fs: unsupported journal ro-compat features 0x%x\n",
		    unsupported);
		error = EINVAL;
		goto out;
	}
	if ((ctx->rc_features_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) &&
	    (ctx->rc_features_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3)) {
		printf("ext4fs: journal enables both checksum v2 and v3\n");
		error = EINVAL;
		goto out;
	}
	{
		const u_int8_t zero_uuid[16] = { 0 };
		const u_int8_t *expected_uuid;

		expected_uuid = fs->m_sble.sb_journal_uuid;
		if (memcmp(expected_uuid, zero_uuid, sizeof(zero_uuid)) == 0)
			expected_uuid = fs->m_sble.sb_uuid;
		if (memcmp(jsb->s_uuid, expected_uuid,
		    sizeof(jsb->s_uuid)) != 0) {
			printf("ext4fs: journal UUID does not match filesystem\n");
			error = EINVAL;
			goto out;
		}
	}
	memcpy(ctx->rc_uuid, jsb->s_uuid, sizeof(ctx->rc_uuid));
	ctx->rc_checksum_seed = crc32c(0, jsb->s_uuid,
	    sizeof(jsb->s_uuid));
	if (jbd2_has_csum_v2or3(ctx) &&
	    jsb->s_checksum_type != JBD2_CHECKSUM_CRC32C) {
		printf("ext4fs: unsupported journal checksum type %u\n",
		    jsb->s_checksum_type);
		error = EINVAL;
		goto out;
	}
	if (!jbd2_superblock_csum_verify(ctx, jsb)) {
		printf("ext4fs: invalid journal superblock checksum\n");
		error = EINVAL;
		goto out;
	}

	brelse(bp);
	bp = NULL;
	error = jbd2_build_blockmap(ctx);
	if (error)
		goto out;
	*jblock0p = jblock0;

out:
	if (bp != NULL)
		brelse(bp);
	if (ibp != NULL)
		brelse(ibp);
	ctx->rc_journal_eh = NULL;
	if (error)
		jbd2_journal_close(ctx);
	return (error);
}

void
jbd2_journal_close (struct jbd2_replay_ctx *ctx)
{
	if (ctx->rc_blockset != NULL)
		free(ctx->rc_blockset, M_TEMP,
		    (ctx->rc_blockset_mask + 1) * sizeof(*ctx->rc_blockset));
	if (ctx->rc_blockmap != NULL)
		free(ctx->rc_blockmap, M_TEMP,
		    ctx->rc_blockmap_count * sizeof(*ctx->rc_blockmap));
	if (ctx->rc_revoke != NULL)
		free(ctx->rc_revoke, M_TEMP,
		    ctx->rc_revoke_alloc * sizeof(*ctx->rc_revoke));
	memset(ctx, 0, sizeof(*ctx));
}

/*
 * Main entry point: replay the ext4 journal.
 *
 * Called during mount when the RECOVER incompat flag is set.
 * Reads the internal journal inode directly from the inode table,
 * runs the three-pass replay, then clears the journal.
 */
int
ext4fs_journal_replay (struct vnode *devvp, struct m_ext4fs *fs,
    struct proc *p)
{
	struct jbd2_replay_ctx ctx;
	struct jbd2_superblock *jsb;
	struct buf *bp;
	u_int64_t jblock0;
	int error;

	bp = NULL;
	error = jbd2_journal_open(devvp, fs, &ctx, &jblock0);
	if (error)
		goto out;
	if (ctx.rc_start == 0) {
		/*
		 * Replay may already have made the home blocks and the clean
		 * journal marker durable, then lost power before clearing RECOVER.
		 * Complete that final step without changing the journal sequence.
		 */
		printf("ext4fs: journal is already clean; completing recovery\n");
		ctx.rc_end_sequence = ctx.rc_sequence;
		goto clear_recover;
	}

	printf("ext4fs: replaying journal (sequence %u, start block %u, "
	    "%u journal blocks)\n",
	    ctx.rc_sequence, ctx.rc_start, ctx.rc_maxlen);

	/* Pass 1: SCAN */
	error = jbd2_pass_scan(&ctx);
	if (error)
		goto out;

	if (ctx.rc_end_sequence == ctx.rc_sequence) {
		printf("ext4fs: journal has no valid transactions\n");
		goto clear;
	}

	/* Pass 2: REVOKE */
	error = jbd2_pass_revoke(&ctx);
	if (error)
		goto out;

	/* Validate every payload before allowing the first home-block write. */
	error = jbd2_pass_replay(&ctx, 0);
	if (error)
		goto out;

	/* Pass 3: REPLAY */
	error = jbd2_pass_replay(&ctx, 1);
	if (error)
		goto out;

	/* Home blocks must be durable before the journal is invalidated. */
	error = jbd2_flush_device(devvp, p);
	if (error) {
		printf("ext4fs: can't flush replayed journal blocks\n");
		goto out;
	}
	error = jbd2_recovered_super_check(&ctx);
	if (error) {
		printf("ext4fs: replay produced an invalid superblock\n");
		goto out;
	}

clear:
	/*
	 * Mark journal clean: set s_start=0 in journal superblock.
	 */
	error = bread(devvp, (daddr_t)EXT4FS_FSBTODB(fs, jblock0),
	    fs->m_block_size, &bp);
	if (error) {
		printf("ext4fs: can't reread journal superblock\n");
		goto out;
	}

	jsb = (struct jbd2_superblock *)bp->b_data;
	jsb->s_start = htobe32(0);
	/* Advance sequence past what we replayed */
	jsb->s_sequence = htobe32(ctx.rc_end_sequence);
	jbd2_superblock_csum_set(&ctx, jsb);
	error = bwrite(bp);
	bp = NULL;
	if (error) {
		printf("ext4fs: can't write journal superblock\n");
		goto out;
	}
	error = jbd2_flush_device(devvp, p);
	if (error) {
		printf("ext4fs: can't flush clean journal superblock\n");
		goto out;
	}

clear_recover:
	/*
	 * Re-read the post-replay superblock before changing RECOVER.  The
	 * transaction may itself have contained a newer superblock image;
	 * copying fs->m_sble here would undo that recovered metadata.
	 */
	{
		struct ext4fs *sble;
		u_int32_t incompat;
		u_int16_t state;

		error = bread(devvp,
		    (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE),
		    EXT4FS_SUPER_BLOCK_SIZE, &bp);
		if (error) {
			printf("ext4fs: can't read recovered superblock\n");
			goto out;
		}
		sble = (struct ext4fs *)bp->b_data;
		if (ext4fs_sbcheck(sble, fs->m_read_only) != 0 ||
		    memcmp(sble->sb_uuid, fs->m_sble.sb_uuid,
		    sizeof(sble->sb_uuid)) != 0) {
			printf("ext4fs: recovered superblock is invalid\n");
			error = EINVAL;
			goto out;
		}

		incompat = letoh32(sble->sb_feature_incompat);
		incompat &= ~EXT4FS_FEATURE_INCOMPAT_RECOVER;
		sble->sb_feature_incompat = htole32(incompat);
		/* Mountfs marks a writable mount dirty only after setup succeeds. */
		state = letoh16(sble->sb_state);
		state |= EXT4FS_STATE_VALID;
		sble->sb_state = htole16(state);

		/* Recompute superblock checksum */
		sble->sb_checksum = htole32(ext4fs_sb_csum(sble));
		error = bwrite(bp);
		bp = NULL;
		if (error) {
			printf("ext4fs: can't write superblock\n");
			goto out;
		}
	}
	error = jbd2_flush_device(devvp, p);
	if (error) {
		printf("ext4fs: can't flush recovered filesystem state\n");
		goto out;
	}

	printf("ext4fs: journal replay complete\n");

out:
	if (bp != NULL)
		brelse(bp);
	jbd2_journal_close(&ctx);
	return (error);
}
