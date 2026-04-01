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
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/vnode.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_journal.h>

/*
 * Read journal inode (inode 8) directly from the inode table.
 * Returns a pointer into the buffer; caller must brelse(*bpp).
 */
static int
jbd2_read_journal_inode(struct jbd2_replay_ctx *ctx, struct buf **bpp,
    struct ext4fs_dinode **dpp)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	struct ext4fs_block_group_descriptor *gd;
	u_int32_t ino = EXT4FS_INODE_JOURNAL;
	u_int32_t group, index;
	u_int64_t itb;
	u_int64_t blk;
	u_int32_t off;
	int error;

	group = (ino - 1) / fs->m_inodes_per_group;
	index = (ino - 1) % fs->m_inodes_per_group;
	gd = &fs->m_gd[group];

	itb = letoh32(gd->bgd_inode_table_block_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		itb |= (u_int64_t)letoh32(gd->bgd_inode_table_block_hi) << 32;

	blk = itb + (index * fs->m_inode_size) / fs->m_block_size;
	off = (index * fs->m_inode_size) % fs->m_block_size;

	error = bread(ctx->rc_devvp, (daddr_t)EXT4FS_FSBTODB(fs, blk),
	    fs->m_block_size, bpp);
	if (error) {
		brelse(*bpp);
		*bpp = NULL;
		printf("ext4fs: can't read journal inode\n");
		return (error);
	}

	*dpp = (struct ext4fs_dinode *)((char *)(*bpp)->b_data + off);
	return (0);
}

/*
 * Fill blockmap from an extent tree rooted at an extent header.
 * Handles depth 0 (inline extents) and depth 1 (one level of index).
 */
static int
jbd2_fill_blockmap_from_eh(struct jbd2_replay_ctx *ctx,
    struct ext4fs_extent_header *eh)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	struct ext4fs_extent *ext;
	struct ext4fs_extent_idx *idx;
	u_int16_t depth, entries, i;
	u_int32_t jblock, maxblocks, len;
	u_int64_t pblock;

	maxblocks = ctx->rc_blockmap_count;

	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC) {
		printf("ext4fs: journal inode has bad extent magic 0x%x\n",
		    letoh16(eh->eh_magic));
		return (EINVAL);
	}

	depth = letoh16(eh->eh_depth);
	entries = letoh16(eh->eh_entries);

	if (depth == 0) {
		ext = (struct ext4fs_extent *)(eh + 1);
		for (i = 0; i < entries; i++) {
			u_int32_t lblk = letoh32(ext[i].e_block);
			len = letoh16(ext[i].e_len);
			pblock = (u_int64_t)letoh16(ext[i].e_start_hi) << 32 |
			    letoh32(ext[i].e_start_lo);

			for (jblock = 0; jblock < len; jblock++) {
				u_int32_t j = lblk + jblock;
				if (j < maxblocks)
					ctx->rc_blockmap[j].jb_fsblock =
					    pblock + jblock;
			}
		}
	} else {
		idx = (struct ext4fs_extent_idx *)(eh + 1);
		for (i = 0; i < entries; i++) {
			struct buf *bp;
			struct ext4fs_extent_header *leh;
			struct ext4fs_extent *lext;
			u_int16_t lentries, j;
			u_int64_t leaf_block;
			int error;

			leaf_block =
			    (u_int64_t)letoh16(idx[i].ei_leaf_hi) << 32 |
			    letoh32(idx[i].ei_leaf_lo);

			error = bread(ctx->rc_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, leaf_block),
			    fs->m_block_size, &bp);
			if (error) {
				brelse(bp);
				printf("ext4fs: journal blockmap: "
				    "can't read index block\n");
				return (error);
			}

			leh = (struct ext4fs_extent_header *)bp->b_data;
			if (letoh16(leh->eh_magic) !=
			    EXT4FS_EXTENT_HEADER_MAGIC) {
				brelse(bp);
				printf("ext4fs: journal blockmap: "
				    "bad leaf magic\n");
				return (EINVAL);
			}
			if (letoh16(leh->eh_depth) != 0) {
				brelse(bp);
				printf("ext4fs: journal blockmap: "
				    "depth > 1 not supported\n");
				return (EINVAL);
			}

			lentries = letoh16(leh->eh_entries);
			lext = (struct ext4fs_extent *)(leh + 1);

			for (j = 0; j < lentries; j++) {
				u_int32_t lblk = letoh32(lext[j].e_block);
				len = letoh16(lext[j].e_len);
				pblock =
				    (u_int64_t)letoh16(lext[j].e_start_hi)
				    << 32 | letoh32(lext[j].e_start_lo);

				for (jblock = 0; jblock < len; jblock++) {
					u_int32_t k = lblk + jblock;
					if (k < maxblocks)
						ctx->rc_blockmap[k].
						    jb_fsblock =
						    pblock + jblock;
				}
			}
			brelse(bp);
		}
	}

	return (0);
}

/*
 * Build the journal block map by reading inode 8's extent tree.
 */
static int
jbd2_build_blockmap(struct jbd2_replay_ctx *ctx)
{
	u_int32_t maxblocks;
	int error;

	maxblocks = ctx->rc_maxlen;
	ctx->rc_blockmap = mallocarray(maxblocks,
	    sizeof(struct jbd2_blockmap_entry), M_TEMP, M_WAITOK | M_ZERO);
	ctx->rc_blockmap_count = maxblocks;

	error = jbd2_fill_blockmap_from_eh(ctx, ctx->rc_journal_eh);

	return (error);
}

/*
 * Read a journal block by journal-relative block number.
 */
static int
jbd2_read_block(struct jbd2_replay_ctx *ctx, u_int32_t jblock,
    struct buf **bpp)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	u_int64_t fsblock;

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

	return bread(ctx->rc_devvp, (daddr_t)EXT4FS_FSBTODB(fs, fsblock),
	    fs->m_block_size, bpp);
}

/*
 * Wrap journal block number circularly.
 */
static u_int32_t
jbd2_next_block(struct jbd2_replay_ctx *ctx, u_int32_t block)
{
	block++;
	if (block >= ctx->rc_maxlen)
		block = ctx->rc_first;
	return block;
}

/*
 * Parse one descriptor tag from a descriptor block.
 *
 * Returns 0 on success, sets *target to the filesystem block,
 * *flags to the tag flags, and advances *offset past the tag.
 */
static int
jbd2_parse_tag(struct jbd2_replay_ctx *ctx, char *buf, u_int32_t bufsize,
    u_int32_t *offset, u_int64_t *target, u_int32_t *flags)
{
	int has_csum_v3, has_64bit;
	u_int32_t tag_size;

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
		if (has_64bit)
			*target |= (u_int64_t)betoh32(tag3->t_blocknr_high)
			    << 32;

		*offset += tag_size;
		if (!(*flags & JBD2_FLAG_SAME_UUID))
			*offset += 16;	/* skip UUID */
	} else {
		struct jbd2_block_tag *tag;

		tag_size = 8;	/* minimum: blocknr + checksum + flags */
		if (*offset + tag_size > bufsize)
			return (EINVAL);

		tag = (struct jbd2_block_tag *)(buf + *offset);
		*target = betoh32(tag->t_blocknr);
		*flags = betoh16(tag->t_flags);

		*offset += tag_size;
		if (has_64bit) {
			if (*offset + 4 > bufsize)
				return (EINVAL);
			*target |= (u_int64_t)betoh32(tag->t_blocknr_high)
			    << 32;
			*offset += 4;
		}
		if (!(*flags & JBD2_FLAG_SAME_UUID))
			*offset += 16;
	}

	return (0);
}

/*
 * Add a block to the revocation table.
 */
static void
jbd2_revoke_add(struct jbd2_replay_ctx *ctx, u_int64_t block,
    u_int32_t sequence)
{
	u_int32_t i;

	/* Update existing entry if present */
	for (i = 0; i < ctx->rc_revoke_count; i++) {
		if (ctx->rc_revoke[i].re_block == block) {
			if (sequence > ctx->rc_revoke[i].re_sequence ||
			    (sequence < 0x10000 &&
			     ctx->rc_revoke[i].re_sequence > 0xFFFF0000))
				ctx->rc_revoke[i].re_sequence = sequence;
			return;
		}
	}

	/* Grow table if needed */
	if (ctx->rc_revoke_count >= ctx->rc_revoke_alloc) {
		struct jbd2_revoke_entry *newrev;
		u_int32_t newalloc;

		newalloc = ctx->rc_revoke_alloc ? ctx->rc_revoke_alloc * 2
		    : 64;
		newrev = mallocarray(newalloc,
		    sizeof(struct jbd2_revoke_entry), M_TEMP,
		    M_WAITOK | M_ZERO);
		if (ctx->rc_revoke_count > 0)
			memcpy(newrev, ctx->rc_revoke,
			    ctx->rc_revoke_count *
			    sizeof(struct jbd2_revoke_entry));
		if (ctx->rc_revoke != NULL)
			free(ctx->rc_revoke, M_TEMP,
			    ctx->rc_revoke_alloc *
			    sizeof(struct jbd2_revoke_entry));
		ctx->rc_revoke = newrev;
		ctx->rc_revoke_alloc = newalloc;
	}

	ctx->rc_revoke[ctx->rc_revoke_count].re_block = block;
	ctx->rc_revoke[ctx->rc_revoke_count].re_sequence = sequence;
	ctx->rc_revoke_count++;
}

/*
 * Check if a block is revoked at or after the given sequence.
 */
static int
jbd2_revoke_check(struct jbd2_replay_ctx *ctx, u_int64_t block,
    u_int32_t sequence)
{
	u_int32_t i;

	for (i = 0; i < ctx->rc_revoke_count; i++) {
		if (ctx->rc_revoke[i].re_block == block &&
		    (ctx->rc_revoke[i].re_sequence >= sequence ||
		     (ctx->rc_revoke[i].re_sequence < 0x10000 &&
		      sequence > 0xFFFF0000)))
			return (1);
	}
	return (0);
}

/*
 * Check if a block looks like a valid journal header.
 */
static int
jbd2_check_header(struct buf *bp, u_int32_t expected_seq, u_int32_t type)
{
	struct jbd2_header *hdr;

	hdr = (struct jbd2_header *)bp->b_data;
	if (betoh32(hdr->h_magic) != JBD2_MAGIC)
		return (0);
	if (betoh32(hdr->h_sequence) != expected_seq)
		return (0);
	if (type != 0 && betoh32(hdr->h_blocktype) != type)
		return (0);
	return (1);
}

/*
 * Count data blocks described by a descriptor block's tags.
 */
static int
jbd2_count_tags(struct jbd2_replay_ctx *ctx, struct buf *bp,
    u_int32_t *count)
{
	char *buf;
	u_int32_t offset, bufsize, flags;
	u_int64_t target;
	int error;

	buf = (char *)bp->b_data;
	bufsize = ctx->rc_blocksize;
	offset = sizeof(struct jbd2_header);
	*count = 0;

	while (offset < bufsize) {
		error = jbd2_parse_tag(ctx, buf, bufsize, &offset,
		    &target, &flags);
		if (error)
			break;
		(*count)++;
		if (flags & JBD2_FLAG_LAST_TAG)
			break;
	}
	return (0);
}

/*
 * Pass 1: SCAN
 *
 * Walk the journal from s_start/s_sequence, verify each transaction
 * has a matching DESCRIPTOR and COMMIT block, and find the end of
 * the valid journal.
 */
static int
jbd2_pass_scan(struct jbd2_replay_ctx *ctx)
{
	u_int32_t block, seq, next_seq;
	u_int32_t tag_count;
	struct buf *bp;
	struct jbd2_header *hdr;
	int error;

	block = ctx->rc_start;
	seq = ctx->rc_sequence;
	ctx->rc_end_sequence = seq;

	printf("ext4fs: journal scan: start block %u sequence %u\n",
	    block, seq);

	while (1) {
		error = jbd2_read_block(ctx, block, &bp);
		if (error) {
			printf("ext4fs: journal scan: read error at "
			    "block %u\n", block);
			break;
		}

		hdr = (struct jbd2_header *)bp->b_data;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC) {
			brelse(bp);
			break;
		}
		if (betoh32(hdr->h_sequence) != seq) {
			brelse(bp);
			break;
		}

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			/* Count tags to know how many data blocks follow */
			error = jbd2_count_tags(ctx, bp, &tag_count);
			brelse(bp);
			if (error)
				goto done;

			/* Skip over data blocks */
			{
				u_int32_t i;
				for (i = 0; i < tag_count; i++)
					block = jbd2_next_block(ctx, block);
			}

			/* Next block should be commit */
			block = jbd2_next_block(ctx, block);
			error = jbd2_read_block(ctx, block, &bp);
			if (error) {
				printf("ext4fs: journal scan: missing commit "
				    "for seq %u\n", seq);
				goto done;
			}

			if (!jbd2_check_header(bp, seq,
			    JBD2_COMMIT_BLOCK)) {
				brelse(bp);
				printf("ext4fs: journal scan: bad commit "
				    "for seq %u\n", seq);
				goto done;
			}
			brelse(bp);

			/* Valid transaction */
			next_seq = seq + 1;
			ctx->rc_end_sequence = next_seq;
			seq = next_seq;
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_REVOKE_BLOCK:
			/*
			 * A revoke block by itself is part of a transaction.
			 * There may be multiple revoke blocks before the
			 * commit.
			 */
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_COMMIT_BLOCK:
			/* Unexpected standalone commit — end of journal */
			brelse(bp);
			goto done;

		default:
			brelse(bp);
			goto done;
		}
	}

done:
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
static int
jbd2_pass_revoke(struct jbd2_replay_ctx *ctx)
{
	u_int32_t block, seq;
	u_int32_t tag_count;
	struct buf *bp;
	struct jbd2_header *hdr;
	struct jbd2_revoke_header *rh;
	int has_64bit;
	int error;

	has_64bit = ctx->rc_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT;

	block = ctx->rc_start;
	seq = ctx->rc_sequence;

	while (seq < ctx->rc_end_sequence) {
		error = jbd2_read_block(ctx, block, &bp);
		if (error)
			return (error);

		hdr = (struct jbd2_header *)bp->b_data;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq) {
			brelse(bp);
			break;
		}

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			error = jbd2_count_tags(ctx, bp, &tag_count);
			brelse(bp);
			if (error)
				return (error);
			{
				u_int32_t i;
				for (i = 0; i < tag_count; i++)
					block = jbd2_next_block(ctx, block);
			}
			/* Skip commit block */
			block = jbd2_next_block(ctx, block);
			block = jbd2_next_block(ctx, block);
			seq++;
			break;

		case JBD2_REVOKE_BLOCK:
			rh = (struct jbd2_revoke_header *)bp->b_data;
			{
				u_int32_t rcount, off;
				rcount = betoh32(rh->r_count);
				off = sizeof(struct jbd2_revoke_header);
				while (off < rcount) {
					u_int64_t revblk;
					if (has_64bit) {
						if (off + 8 > rcount)
							break;
						revblk =
						    (u_int64_t)betoh32(
						    *(u_int32_t *)
						    ((char *)bp->b_data +
						    off)) << 32 |
						    betoh32(
						    *(u_int32_t *)
						    ((char *)bp->b_data +
						    off + 4));
						off += 8;
					} else {
						if (off + 4 > rcount)
							break;
						revblk = betoh32(
						    *(u_int32_t *)
						    ((char *)bp->b_data +
						    off));
						off += 4;
					}
					jbd2_revoke_add(ctx, revblk, seq);
				}
			}
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_COMMIT_BLOCK:
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			seq++;
			break;

		default:
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			break;
		}
	}

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
static int
jbd2_pass_replay(struct jbd2_replay_ctx *ctx)
{
	struct m_ext4fs *fs = ctx->rc_fs;
	u_int32_t block, seq;
	u_int32_t replayed = 0;
	struct buf *bp, *dbp, *wbp;
	struct jbd2_header *hdr;
	char *buf;
	u_int32_t offset, bufsize, flags;
	u_int64_t target;
	int error;

	block = ctx->rc_start;
	seq = ctx->rc_sequence;

	while (seq < ctx->rc_end_sequence) {
		error = jbd2_read_block(ctx, block, &bp);
		if (error)
			return (error);

		hdr = (struct jbd2_header *)bp->b_data;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq) {
			brelse(bp);
			break;
		}

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			buf = (char *)bp->b_data;
			bufsize = ctx->rc_blocksize;
			offset = sizeof(struct jbd2_header);

			/* Iterate over tags, each followed by a data block */
			while (offset < bufsize) {
				error = jbd2_parse_tag(ctx, buf, bufsize,
				    &offset, &target, &flags);
				if (error)
					break;

				/* Advance to data block */
				block = jbd2_next_block(ctx, block);

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
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
				}

				/* Read the target filesystem block */
				error = bread(ctx->rc_devvp,
				    (daddr_t)EXT4FS_FSBTODB(fs, target),
				    fs->m_block_size, &wbp);
				if (error) {
					brelse(dbp);
					printf("ext4fs: journal replay: "
					    "can't read target %llu\n",
					    (unsigned long long)target);
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
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
				} else {
					replayed++;
				}

				if (flags & JBD2_FLAG_LAST_TAG)
					break;
			}

			brelse(bp);

			/* Skip to commit block and past it */
			block = jbd2_next_block(ctx, block);
			/* Skip the commit block */
			block = jbd2_next_block(ctx, block);
			seq++;
			break;

		case JBD2_REVOKE_BLOCK:
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			break;

		case JBD2_COMMIT_BLOCK:
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			seq++;
			break;

		default:
			brelse(bp);
			block = jbd2_next_block(ctx, block);
			break;
		}
	}

	ctx->rc_replay_count = replayed;
	printf("ext4fs: journal replay: %u blocks replayed\n", replayed);

	return (0);
}

/*
 * Main entry point: replay the ext4 journal.
 *
 * Called during mount when the RECOVER incompat flag is set.
 * Reads inode 8 directly from the inode table to locate the journal,
 * runs the three-pass replay, then clears the journal.
 */
int
ext4fs_journal_replay(struct vnode *devvp, struct m_ext4fs *fs)
{
	struct jbd2_replay_ctx ctx;
	struct jbd2_superblock *jsb;
	struct ext4fs_dinode *jdi;
	struct buf *bp, *ibp;
	u_int64_t jblock0;
	int error;

	memset(&ctx, 0, sizeof(ctx));
	ctx.rc_devvp = devvp;
	ctx.rc_fs = fs;

	/*
	 * Read journal inode (inode 8) from the inode table.
	 */
	error = jbd2_read_journal_inode(&ctx, &ibp, &jdi);
	if (error)
		return (error);

	ctx.rc_journal_eh = &jdi->i_extent_header;

	/* Find block 0 from the first extent */
	if (letoh16(jdi->i_extent_header.eh_magic) !=
	    EXT4FS_EXTENT_HEADER_MAGIC ||
	    letoh16(jdi->i_extent_header.eh_entries) == 0) {
		printf("ext4fs: journal inode has no extents\n");
		brelse(ibp);
		return (EINVAL);
	}

	if (letoh16(jdi->i_extent_header.eh_depth) == 0) {
		struct ext4fs_extent *ext = jdi->i_extent;
		jblock0 = (u_int64_t)letoh16(ext->e_start_hi) << 32 |
		    letoh32(ext->e_start_lo);
	} else {
		/*
		 * Depth > 0: need to read the first index block to
		 * find the first leaf extent.
		 */
		struct ext4fs_extent_idx *idx = jdi->i_extent_idx;
		struct ext4fs_extent_header *leh;
		struct ext4fs_extent *lext;
		struct buf *lbp;
		u_int64_t leaf_block;

		leaf_block =
		    (u_int64_t)letoh16(idx->ei_leaf_hi) << 32 |
		    letoh32(idx->ei_leaf_lo);
		error = bread(devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, leaf_block),
		    fs->m_block_size, &lbp);
		if (error) {
			brelse(lbp);
			brelse(ibp);
			return (error);
		}
		leh = (struct ext4fs_extent_header *)lbp->b_data;
		lext = (struct ext4fs_extent *)(leh + 1);
		jblock0 = (u_int64_t)letoh16(lext->e_start_hi) << 32 |
		    letoh32(lext->e_start_lo);
		brelse(lbp);
	}

	if (jblock0 == 0) {
		printf("ext4fs: can't locate journal block 0\n");
		brelse(ibp);
		return (EINVAL);
	}

	/* Read journal superblock (journal block 0) */
	error = bread(devvp, (daddr_t)EXT4FS_FSBTODB(fs, jblock0),
	    fs->m_block_size, &bp);
	if (error) {
		printf("ext4fs: can't read journal superblock\n");
		brelse(ibp);
		return (error);
	}

	jsb = (struct jbd2_superblock *)bp->b_data;

	/* Validate journal superblock */
	if (betoh32(jsb->s_header.h_magic) != JBD2_MAGIC) {
		printf("ext4fs: bad journal magic 0x%x\n",
		    betoh32(jsb->s_header.h_magic));
		brelse(bp);
		brelse(ibp);
		return (EINVAL);
	}
	{
		u_int32_t btype = betoh32(jsb->s_header.h_blocktype);
		if (btype != JBD2_SUPERBLOCK_V1 &&
		    btype != JBD2_SUPERBLOCK_V2) {
			printf("ext4fs: bad journal superblock version %u\n",
			    btype);
			brelse(bp);
			brelse(ibp);
			return (EINVAL);
		}
	}

	ctx.rc_blocksize = betoh32(jsb->s_blocksize);
	ctx.rc_maxlen = betoh32(jsb->s_maxlen);
	ctx.rc_first = betoh32(jsb->s_first);
	ctx.rc_sequence = betoh32(jsb->s_sequence);
	ctx.rc_start = betoh32(jsb->s_start);

	if (betoh32(jsb->s_header.h_blocktype) == JBD2_SUPERBLOCK_V2)
		ctx.rc_features_incompat = betoh32(jsb->s_feature_incompat);
	else
		ctx.rc_features_incompat = 0;

	brelse(bp);

	if (ctx.rc_blocksize != fs->m_block_size) {
		printf("ext4fs: journal blocksize %u != fs blocksize %llu\n",
		    ctx.rc_blocksize, (unsigned long long)fs->m_block_size);
		brelse(ibp);
		return (EINVAL);
	}

	if (ctx.rc_start == 0) {
		printf("ext4fs: journal is clean, no replay needed\n");
		brelse(ibp);
		error = 0;
		goto out;
	}

	/* Build full blockmap from inode 8's extent tree */
	error = jbd2_build_blockmap(&ctx);
	brelse(ibp);
	ctx.rc_journal_eh = NULL;
	if (error)
		goto out;

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

	/* Pass 3: REPLAY */
	error = jbd2_pass_replay(&ctx);
	if (error)
		goto out;

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
	error = bwrite(bp);
	if (error) {
		printf("ext4fs: can't write journal superblock\n");
		goto out;
	}

	/*
	 * Clear RECOVER flag and set STATE_VALID in ext4 superblock.
	 */
	{
		u_int32_t incompat;

		incompat = letoh32(fs->m_sble.sb_feature_incompat);
		incompat &= ~EXT4FS_FEATURE_INCOMPAT_RECOVER;
		fs->m_sble.sb_feature_incompat = htole32(incompat);
		fs->m_feature_incompat = incompat;

		fs->m_sble.sb_state = htole16(EXT4FS_STATE_VALID);
		fs->m_state = EXT4FS_STATE_VALID;

		/* Recompute superblock checksum */
		fs->m_sble.sb_checksum =
		    htole32(ext4fs_sb_csum(&fs->m_sble));

		/* Write superblock to disk */
		error = bread(devvp,
		    (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE),
		    EXT4FS_SUPER_BLOCK_SIZE, &bp);
		if (error) {
			printf("ext4fs: can't read superblock for update\n");
			goto out;
		}
		memcpy(bp->b_data, &fs->m_sble, sizeof(struct ext4fs));
		error = bwrite(bp);
		if (error) {
			printf("ext4fs: can't write superblock\n");
			goto out;
		}
	}

	printf("ext4fs: journal replay complete\n");

out:
	if (ctx.rc_blockmap != NULL)
		free(ctx.rc_blockmap, M_TEMP,
		    ctx.rc_blockmap_count *
		    sizeof(struct jbd2_blockmap_entry));
	if (ctx.rc_revoke != NULL)
		free(ctx.rc_revoke, M_TEMP,
		    ctx.rc_revoke_alloc *
		    sizeof(struct jbd2_revoke_entry));

	return (error);
}

#define EXT4FS_ORPHAN_BLOCK_TAIL_MAGIC	0x0b10ca04

struct ext4fs_orphan_block_tail {
	u_int32_t	ob_magic;
	u_int32_t	ob_checksum;
} __attribute__((packed));

static int
ext4fs_process_orphan(struct mount *mp, u_int32_t ino, int *count)
{
	struct vnode *vp;
	struct inode *ip;
	struct ext4fs_dinode *din;
	u_int16_t nlink;
	off_t size;
	int error;

	error = VFS_VGET(mp, ino, &vp);
	if (error) {
		printf("ext4fs: orphan cleanup: can't get inode %u\n", ino);
		return (error);
	}

	ip = VTOI(vp);
	din = &ip->i_e4din->dinode;
	nlink = letoh16(din->i_links_count);
	size = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	din->i_dtime = htole32(0);
	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	if (nlink > 0) {
		printf("ext4fs: orphan inode %u: truncating (nlink=%u)\n",
		    ino, nlink);
		error = ext4fs_truncate(ip, size, 0, NOCRED);
		if (error)
			printf("ext4fs: orphan inode %u: "
			    "truncate failed: %d\n", ino, error);
		ext4fs_update(ip, 1);
	} else {
		printf("ext4fs: orphan inode %u: deleting\n", ino);
	}

	vput(vp);
	(*count)++;
	return (0);
}

static int
ext4fs_orphan_file_scan(struct mount *mp, int *count)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	struct vnode *ovp;
	struct inode *oip;
	struct ext4fs_dinode *odin;
	u_int32_t orphan_ino;
	u_int32_t nblocks, entries_per_block;
	off_t osize;
	u_int32_t b, e;
	int error;

	orphan_ino = fs->m_orphan_file_inode;
	if (orphan_ino == 0)
		return (0);

	error = VFS_VGET(mp, orphan_ino, &ovp);
	if (error) {
		printf("ext4fs: can't read orphan file inode %u\n",
		    orphan_ino);
		return (error);
	}

	oip = VTOI(ovp);
	odin = &oip->i_e4din->dinode;
	osize = (off_t)letoh32(odin->i_size_lo) |
	    ((off_t)letoh32(odin->i_size_hi) << 32);
	nblocks = osize / fs->m_block_size;

	/* Each block has __le32 entries minus the 8-byte tail */
	entries_per_block = (fs->m_block_size -
	    sizeof(struct ext4fs_orphan_block_tail)) / sizeof(u_int32_t);

	for (b = 0; b < nblocks; b++) {
		struct buf *bp;
		u_int32_t *entries;
		struct ext4fs_orphan_block_tail *tail;
		struct ext4fs_extent *ext;
		struct ext4fs_extent_header *eh;
		u_int64_t pblock = 0;
		u_int16_t i, nent;

		/* Find physical block for logical block b */
		eh = &odin->i_extent_header;
		if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC)
			break;
		if (letoh16(eh->eh_depth) != 0) {
			/* depth > 0 orphan file not supported yet */
			printf("ext4fs: orphan file has depth > 0\n");
			break;
		}
		nent = letoh16(eh->eh_entries);
		ext = odin->i_extent;
		for (i = 0; i < nent; i++) {
			u_int32_t lblk_start = letoh32(ext[i].e_block);
			u_int16_t len = letoh16(ext[i].e_len);
			if (b >= lblk_start && b < lblk_start + len) {
				pblock =
				    ((u_int64_t)letoh16(ext[i].e_start_hi)
				    << 32 | letoh32(ext[i].e_start_lo)) +
				    (b - lblk_start);
				break;
			}
		}
		if (pblock == 0)
			continue;

		error = bread(ump->um_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblock),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			continue;
		}

		entries = (u_int32_t *)bp->b_data;
		tail = (struct ext4fs_orphan_block_tail *)
		    ((char *)bp->b_data + fs->m_block_size -
		    sizeof(struct ext4fs_orphan_block_tail));

		if (letoh32(tail->ob_magic) != EXT4FS_ORPHAN_BLOCK_TAIL_MAGIC) {
			brelse(bp);
			continue;
		}

		for (e = 0; e < entries_per_block; e++) {
			u_int32_t ino = letoh32(entries[e]);
			if (ino == 0)
				continue;
			/* Clear entry in orphan file block */
			entries[e] = 0;
			ext4fs_process_orphan(mp, ino, count);
		}

		/* Write back cleared block */
		error = bwrite(bp);
		if (error)
			printf("ext4fs: orphan file: write error block %u\n",
			    b);
	}

	vput(ovp);
	return (0);
}

int
ext4fs_orphan_cleanup(struct mount *mp)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	u_int32_t ino, next;
	int count = 0;
	int error;
	int has_orphans;

	has_orphans = (fs->m_last_orphan != 0) ||
	    (fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT);

	if (!has_orphans)
		return (0);

	printf("ext4fs: cleaning up orphan inodes\n");

	/* Walk classic sb_last_orphan linked list */
	ino = fs->m_last_orphan;
	while (ino != 0) {
		struct vnode *vp;
		struct inode *ip;

		error = VFS_VGET(mp, ino, &vp);
		if (error) {
			printf("ext4fs: orphan cleanup: "
			    "can't get inode %u\n", ino);
			break;
		}
		ip = VTOI(vp);
		next = letoh32(ip->i_e4din->dinode.i_dtime);
		vput(vp);

		ext4fs_process_orphan(mp, ino, &count);
		ino = next;
	}

	fs->m_last_orphan = 0;
	fs->m_sble.sb_last_orphan = htole32(0);

	/* Scan orphan file if ORPHAN_PRESENT */
	if (fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT) {
		u_int32_t ro;

		ext4fs_orphan_file_scan(mp, &count);

		ro = letoh32(fs->m_sble.sb_feature_ro_compat);
		ro &= ~EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT;
		fs->m_sble.sb_feature_ro_compat = htole32(ro);
		fs->m_feature_ro_compat = ro;
	}

	fs->m_fs_was_modified = 1;

	printf("ext4fs: orphan cleanup: %d inodes processed\n", count);
	return (0);
}
