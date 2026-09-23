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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/dkio.h>
#include <sys/endian.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <lib/libkern/crc32c.h>

#include "fsck.h"
#include "extern.h"
#include "fsutil.h"

#include <ufs/ext4fs/ext4fs_journal.h>

struct fsck_jbd2_ctx {
	u_int32_t	blocksize;
	u_int32_t	maxlen;
	u_int32_t	first;
	u_int32_t	sequence;
	u_int32_t	start;
	u_int32_t	features_compat;
	u_int32_t	features_incompat;
	u_int32_t	features_ro_compat;
	u_int32_t	checksum_seed;
	u_int8_t	uuid[16];
	u_int32_t	journal_ino;
	u_int32_t	journal_gen;	/* little-endian on-disk value */
	u_int64_t	*blockmap;
	u_int32_t	blockmap_count;
	u_int64_t	*blockset;
	u_int32_t	blockset_mask;
	struct jbd2_revoke_entry *revoke;
	u_int32_t	revoke_count;
	u_int32_t	revoke_alloc;
	u_int32_t	end_sequence;
};

static int
jread(u_int64_t fsblock, char *buf, long size)
{
	off_t offset;

	if (fsblock >= sblock.m_blocks_count ||
	    fsblock > (u_int64_t)LLONG_MAX / sblock.m_block_size)
		return (EINVAL);
	offset = (off_t)(fsblock * sblock.m_block_size);
	if (pread(fsreadfd, buf, size, offset) != size)
		return (errno ? errno : EIO);
	return (0);
}

static int
jwrite(u_int64_t fsblock, char *buf, long size)
{
	off_t offset;

	if (fsblock >= sblock.m_blocks_count ||
	    fsblock > (u_int64_t)LLONG_MAX / sblock.m_block_size)
		return (EINVAL);
	offset = (off_t)(fsblock * sblock.m_block_size);
	if (pwrite(fswritefd, buf, size, offset) != size)
		return (errno ? errno : EIO);
	fsmodified = 1;
	return (0);
}

static int
jbd2_flush_device(void)
{
	struct stat st;
	int force;

	if (fsync(fswritefd) == -1)
		return (errno);
	if (fstat(fswritefd, &st) == -1)
		return (errno);
	if (S_ISREG(st.st_mode))
		return (0);
	force = 1;
	if (ioctl(fswritefd, DIOCCACHESYNC, &force) == -1)
		return (errno);
	return (0);
}

static int
jbd2_extent_header_check(struct ext4fs_extent_header *eh, size_t bytes,
    int expected_depth)
{
	u_int16_t depth, entries, max;
	size_t capacity;

	if (bytes < sizeof(*eh) ||
	    letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC)
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

static int
jbd2_extent_block_csum_verify(struct fsck_jbd2_ctx *ctx, void *buf)
{
	struct ext4fs_extent_header *eh = buf;
	u_int32_t crc, ino_le, provided, *tail;
	size_t tail_offset;

	if (!(sblock.m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return (1);
	tail_offset = sizeof(*eh) +
	    (size_t)letoh16(eh->eh_max) * sizeof(struct ext4fs_extent);
	if (tail_offset > sblock.m_block_size - sizeof(*tail))
		return (0);
	tail = (u_int32_t *)((char *)buf + tail_offset);
	provided = *tail;
	*tail = 0;
	ino_le = htole32(ctx->journal_ino);
	crc = crc32c(ext4fs_csum_seed(&sblock), (const uint8_t *)&ino_le,
	    sizeof(ino_le));
	crc = crc32c(crc, (const uint8_t *)&ctx->journal_gen,
	    sizeof(ctx->journal_gen));
	crc = crc32c(crc, buf, tail_offset);
	*tail = provided;
	return (letoh32(provided) == ~crc);
}

static int
jbd2_extent_lookup(struct fsck_jbd2_ctx *ctx,
    struct ext4fs_extent_header *eh, size_t bytes, int expected_depth,
    u_int32_t logical, u_int64_t *physical)
{
	struct ext4fs_extent *ext;
	struct ext4fs_extent_idx *idx;
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
			u_int64_t pblock;

			if (rawlen == 0 || rawlen > 0x8000)
				return (EINVAL);
			pblock = (u_int64_t)letoh16(ext[i].e_start_hi) << 32 |
			    letoh32(ext[i].e_start_lo);
			if (pblock == 0 || pblock >= sblock.m_blocks_count ||
			    rawlen > sblock.m_blocks_count - pblock)
				return (EINVAL);
			if (logical >= lblk && logical - lblk < rawlen) {
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
		char *buf;
		u_int64_t child;

		child = (u_int64_t)letoh16(idx[chosen].ei_leaf_hi) << 32 |
		    letoh32(idx[chosen].ei_leaf_lo);
		if (child == 0 || child >= sblock.m_blocks_count)
			return (EINVAL);
		buf = malloc(sblock.m_block_size);
		if (buf == NULL)
			return (ENOMEM);
		if (jread(child, buf, sblock.m_block_size) != 0) {
			free(buf);
			return (EIO);
		}
		if (!jbd2_extent_block_csum_verify(ctx, buf)) {
			free(buf);
			return (EINVAL);
		}
		error = jbd2_extent_lookup(ctx,
		    (struct ext4fs_extent_header *)buf, sblock.m_block_size,
		    depth - 1, logical, physical);
		free(buf);
	}
	return (error);
}

static int
jbd2_fill_blockmap(struct fsck_jbd2_ctx *ctx,
    struct ext4fs_extent_header *eh, size_t bytes, int expected_depth)
{
	struct ext4fs_extent *ext;
	struct ext4fs_extent_idx *idx;
	u_int16_t depth, entries, i;
	int error;

	error = jbd2_extent_header_check(eh, bytes, expected_depth);
	if (error)
		return (error);
	depth = letoh16(eh->eh_depth);
	entries = letoh16(eh->eh_entries);

	if (depth == 0) {
		ext = (struct ext4fs_extent *)(eh + 1);
		for (i = 0; i < entries; i++) {
			u_int16_t rawlen = letoh16(ext[i].e_len);
			u_int32_t j, lblk = letoh32(ext[i].e_block);
			u_int64_t pblock;

			if (i > 0 && lblk <= letoh32(ext[i - 1].e_block))
				return (EINVAL);
			if (rawlen == 0 || rawlen > 0x8000)
				return (EINVAL);
			pblock = (u_int64_t)letoh16(ext[i].e_start_hi) << 32 |
			    letoh32(ext[i].e_start_lo);
			if (pblock == 0 || pblock >= sblock.m_blocks_count ||
			    rawlen > sblock.m_blocks_count - pblock)
				return (EINVAL);
			for (j = 0; j < rawlen; j++) {
				u_int32_t logical = lblk + j;

				if (logical < lblk)
					return (EINVAL);
				if (logical < ctx->blockmap_count) {
					if (ctx->blockmap[logical] != 0)
						return (EINVAL);
					ctx->blockmap[logical] = pblock + j;
				}
			}
		}
		return (0);
	}

	idx = (struct ext4fs_extent_idx *)(eh + 1);
	for (i = 0; i < entries; i++) {
		char *buf;
		u_int64_t child;

		if (i > 0 && letoh32(idx[i].ei_block) <=
		    letoh32(idx[i - 1].ei_block))
			return (EINVAL);
		child = (u_int64_t)letoh16(idx[i].ei_leaf_hi) << 32 |
		    letoh32(idx[i].ei_leaf_lo);
		if (child == 0 || child >= sblock.m_blocks_count)
			return (EINVAL);
		buf = malloc(sblock.m_block_size);
		if (buf == NULL)
			return (ENOMEM);
		if (jread(child, buf, sblock.m_block_size) != 0) {
			free(buf);
			return (EIO);
		}
		if (!jbd2_extent_block_csum_verify(ctx, buf)) {
			free(buf);
			return (EINVAL);
		}
		error = jbd2_fill_blockmap(ctx,
		    (struct ext4fs_extent_header *)buf, sblock.m_block_size,
		    depth - 1);
		free(buf);
		if (error)
			return (error);
	}
	return (0);
}

static int
jbd2_build_blockmap(struct fsck_jbd2_ctx *ctx,
    struct ext4fs_extent_header *eh)
{
	u_int32_t hash, i, mask, slots;
	u_int64_t fsblock;
	int error;

	error = jbd2_fill_blockmap(ctx, eh,
	    sizeof(((struct ext4fs_dinode *)0)->i_block), -1);
	if (error)
		return (error);
	for (i = 0; i < ctx->blockmap_count; i++)
		if (ctx->blockmap[i] == 0)
			return (EINVAL);

	/* Build an O(1) membership set and reject physical extent aliases. */
	slots = 1;
	while (slots < ctx->blockmap_count * 2)
		slots <<= 1;
	ctx->blockset = calloc(slots, sizeof(*ctx->blockset));
	if (ctx->blockset == NULL)
		return (ENOMEM);
	ctx->blockset_mask = mask = slots - 1;
	for (i = 0; i < ctx->blockmap_count; i++) {
		fsblock = ctx->blockmap[i];
		hash = ((u_int32_t)fsblock ^ (u_int32_t)(fsblock >> 32)) *
		    2654435761U;
		hash &= mask;
		while (ctx->blockset[hash] != 0) {
			if (ctx->blockset[hash] == fsblock)
				return (EINVAL);
			hash = (hash + 1) & mask;
		}
		ctx->blockset[hash] = fsblock;
	}
	return (0);
}

static int
jbd2_read_jblock(struct fsck_jbd2_ctx *ctx, u_int32_t jblock, char *buf)
{
	u_int64_t fsblock;

	if (jblock >= ctx->blockmap_count)
		return (EIO);
	fsblock = ctx->blockmap[jblock];
	if (fsblock == 0)
		return (EIO);
	return jread(fsblock, buf, ctx->blocksize);
}

static u_int32_t
jbd2_next_block(struct fsck_jbd2_ctx *ctx, u_int32_t block)
{
	block++;
	if (block >= ctx->maxlen)
		block = ctx->first;
	return block;
}

static int
jbd2_is_journal_block(struct fsck_jbd2_ctx *ctx, u_int64_t fsblock)
{
	u_int32_t hash;

	if (ctx->blockset == NULL)
		return (0);
	hash = ((u_int32_t)fsblock ^ (u_int32_t)(fsblock >> 32)) *
	    2654435761U;
	hash &= ctx->blockset_mask;
	while (ctx->blockset[hash] != 0) {
		if (ctx->blockset[hash] == fsblock)
			return (1);
		hash = (hash + 1) & ctx->blockset_mask;
	}
	return (0);
}

static int
jbd2_has_csum_v2or3(struct fsck_jbd2_ctx *ctx)
{
	return ((ctx->features_incompat &
	    (JBD2_FEATURE_INCOMPAT_CSUM_V2 |
	    JBD2_FEATURE_INCOMPAT_CSUM_V3)) != 0);
}

static u_int32_t
jbd2_block_checksum(struct fsck_jbd2_ctx *ctx, const void *data, size_t len)
{
	return (~crc32c(ctx->checksum_seed, data, len));
}

static int
jbd2_metadata_block_csum_verify(struct fsck_jbd2_ctx *ctx, void *data)
{
	struct jbd2_block_tail *tail;
	u_int32_t provided, calculated;

	if (!jbd2_has_csum_v2or3(ctx))
		return (1);
	tail = (struct jbd2_block_tail *)((char *)data + ctx->blocksize -
	    sizeof(*tail));
	provided = tail->t_checksum;
	tail->t_checksum = 0;
	calculated = jbd2_block_checksum(ctx, data, ctx->blocksize);
	tail->t_checksum = provided;
	return (betoh32(provided) == calculated);
}

static int
jbd2_commit_block_csum_verify(struct fsck_jbd2_ctx *ctx, void *data)
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
	calculated = jbd2_block_checksum(ctx, data, ctx->blocksize);
	commit->h_checksum[0] = provided;
	return (betoh32(provided) == calculated);
}

static int
jbd2_data_block_csum_verify(struct fsck_jbd2_ctx *ctx, void *data,
    u_int32_t sequence, u_int32_t provided)
{
	u_int32_t crc, sequence_be;

	if (!jbd2_has_csum_v2or3(ctx))
		return (1);
	sequence_be = htobe32(sequence);
	crc = crc32c(ctx->checksum_seed, (const uint8_t *)&sequence_be,
	    sizeof(sequence_be));
	crc = ~crc32c(crc, data, ctx->blocksize);
	if (ctx->features_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3)
		return (crc == provided);
	return ((u_int16_t)crc == (u_int16_t)provided);
}

static u_int32_t
jbd2_descriptor_limit(struct fsck_jbd2_ctx *ctx)
{
	u_int32_t limit = ctx->blocksize;

	if (jbd2_has_csum_v2or3(ctx))
		limit -= sizeof(struct jbd2_block_tail);
	return (limit);
}

static int
jbd2_parse_tag(struct fsck_jbd2_ctx *ctx, char *buf, u_int32_t bufsize,
    u_int32_t *offset, u_int64_t *target, u_int32_t *flags,
    u_int32_t *checksum, int *uuid_seen)
{
	const u_int8_t zero_uuid[16] = { 0 };
	int has_csum_v2 = ctx->features_incompat &
	    JBD2_FEATURE_INCOMPAT_CSUM_V2;
	int has_csum_v3 = ctx->features_incompat &
	    JBD2_FEATURE_INCOMPAT_CSUM_V3;
	int has_64bit = ctx->features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT;

	if (has_csum_v3) {
		struct jbd2_block_tag3 *tag3;
		u_int32_t tag_size = sizeof(struct jbd2_block_tag3);
		if (*offset + tag_size > bufsize)
			return (EINVAL);
		tag3 = (struct jbd2_block_tag3 *)(buf + *offset);
		*target = betoh32(tag3->t_blocknr);
		*flags = betoh32(tag3->t_flags);
		*checksum = betoh32(tag3->t_checksum);
		if (has_64bit)
			*target |= (u_int64_t)betoh32(tag3->t_blocknr_high) << 32;
		else if (tag3->t_blocknr_high != 0)
			return (EINVAL);
		*offset += tag_size;
	} else {
		struct jbd2_block_tag *tag;
		u_int32_t tag_size = 8;
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
			*target |= (u_int64_t)betoh32(tag->t_blocknr_high) << 32;
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
	if (*flags & ~(JBD2_FLAG_ESCAPE | JBD2_FLAG_SAME_UUID |
	    JBD2_FLAG_DELETED | JBD2_FLAG_LAST_TAG)) {
		pfatal("JOURNAL TAG HAS UNKNOWN FLAGS 0x%x\n", *flags);
		return (EINVAL);
	}
	if (*target >= sblock.m_blocks_count) {
		pfatal("JOURNAL TAG TARGET %llu IS OUT OF RANGE\n",
		    (unsigned long long)*target);
		return (EINVAL);
	}
	if (jbd2_is_journal_block(ctx, *target)) {
		pfatal("JOURNAL TAG TARGET %llu IS IN THE JOURNAL\n",
		    (unsigned long long)*target);
		return (EINVAL);
	}
	if (*flags & JBD2_FLAG_SAME_UUID) {
		if (!*uuid_seen) {
			pfatal("FIRST JOURNAL TAG OMITS ITS UUID\n");
			return (EINVAL);
		}
	} else {
		/*
		 * The open-coded UUID is present only when SAME_UUID is clear.
		 * e2fsprogs writes a zero UUID here; accept it because the journal
		 * superblock and the data checksum still bind the tag to this journal.
		 */
		if (*offset + sizeof(ctx->uuid) > bufsize ||
		    (memcmp(buf + *offset, ctx->uuid, sizeof(ctx->uuid)) != 0 &&
		    memcmp(buf + *offset, zero_uuid, sizeof(zero_uuid)) != 0))
			return (EINVAL);
		*offset += sizeof(ctx->uuid);
		*uuid_seen = 1;
	}
	return (0);
}

static int
jbd2_count_tags(struct fsck_jbd2_ctx *ctx, char *buf, u_int32_t *count)
{
	u_int32_t offset = sizeof(struct jbd2_header);
	u_int32_t checksum, flags, limit;
	u_int64_t target;
	int error, uuid_seen;

	*count = 0;
	uuid_seen = 0;
	limit = jbd2_descriptor_limit(ctx);
	while (offset < limit) {
		error = jbd2_parse_tag(ctx, buf, limit, &offset,
		    &target, &flags, &checksum, &uuid_seen);
		if (error) {
			pfatal("INVALID JOURNAL TAG NEAR OFFSET %u\n", offset);
			return (error);
		}
		if (!(flags & JBD2_FLAG_DELETED))
			(*count)++;
		if (flags & JBD2_FLAG_LAST_TAG)
			return (0);
	}
	/* A descriptor that exactly fills the tag area needs no LAST_TAG. */
	return (*count != 0 && offset == limit ? 0 : EINVAL);
}

static int
jbd2_revoke_block_check(struct fsck_jbd2_ctx *ctx, char *buf,
    u_int32_t *count)
{
	struct jbd2_revoke_header *rh;
	u_int32_t bytes, limit, record_size;

	if (!(ctx->features_incompat & JBD2_FEATURE_INCOMPAT_REVOKE))
		return (EINVAL);
	if (!jbd2_metadata_block_csum_verify(ctx, buf))
		return (EINVAL);
	rh = (struct jbd2_revoke_header *)buf;
	bytes = betoh32(rh->r_count);
	limit = jbd2_descriptor_limit(ctx);
	record_size = (ctx->features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
	if (bytes < sizeof(*rh) || bytes > limit ||
	    (bytes - sizeof(*rh)) % record_size != 0)
		return (EINVAL);
	*count = (bytes - sizeof(*rh)) / record_size;
	return (0);
}

static int
jbd2_revoke_add(struct fsck_jbd2_ctx *ctx, u_int64_t block, u_int32_t seq)
{
	u_int32_t hash, mask;
	u_int64_t key;

	if (ctx->revoke == NULL) {
		ctx->revoke_alloc = JBD2_MAX_REVOKE_ENTRIES * 2;
		ctx->revoke = calloc(ctx->revoke_alloc, sizeof(*ctx->revoke));
		if (ctx->revoke == NULL)
			return (ENOMEM);
	}
	key = block + 1;
	mask = ctx->revoke_alloc - 1;
	hash = ((u_int32_t)block ^ (u_int32_t)(block >> 32)) *
	    2654435761U;
	hash &= mask;
	while (ctx->revoke[hash].re_block != 0) {
		if (ctx->revoke[hash].re_block == key) {
			if ((int32_t)(seq - ctx->revoke[hash].re_sequence) >= 0)
				ctx->revoke[hash].re_sequence = seq;
			return (0);
		}
		hash = (hash + 1) & mask;
	}
	if (ctx->revoke_count >= JBD2_MAX_REVOKE_ENTRIES)
		return (EFBIG);
	ctx->revoke[hash].re_block = key;
	ctx->revoke[hash].re_sequence = seq;
	ctx->revoke_count++;
	return (0);
}

static int
jbd2_revoke_check(struct fsck_jbd2_ctx *ctx, u_int64_t block, u_int32_t seq)
{
	u_int32_t hash, mask;
	u_int64_t key;

	if (ctx->revoke == NULL)
		return (0);
	key = block + 1;
	mask = ctx->revoke_alloc - 1;
	hash = ((u_int32_t)block ^ (u_int32_t)(block >> 32)) *
	    2654435761U;
	hash &= mask;
	while (ctx->revoke[hash].re_block != 0) {
		if (ctx->revoke[hash].re_block == key)
			return ((int32_t)(ctx->revoke[hash].re_sequence - seq) >= 0);
		hash = (hash + 1) & mask;
	}
	return (0);
}

static int
jbd2_pass_scan(struct fsck_jbd2_ctx *ctx, char *buf)
{
	u_int32_t block, consumed, loglen, revoke_count, seq, tag_count;
	struct jbd2_header *hdr;
	int in_transaction;

	block = ctx->start;
	seq = ctx->sequence;
	ctx->end_sequence = seq;
	consumed = 0;
	in_transaction = 0;
	loglen = ctx->maxlen - ctx->first;

	printf("journal scan: start block %u sequence %u\n", block, seq);

	while (consumed < loglen) {
		if (jbd2_read_jblock(ctx, block, buf) != 0) {
			pfatal("CAN'T READ JOURNAL BLOCK %u DURING SCAN\n", block);
			return (EIO);
		}
		consumed++;
		hdr = (struct jbd2_header *)buf;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq) {
			if (in_transaction)
				return (EINVAL);
			break;
		}

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			in_transaction = 1;
			if (!jbd2_metadata_block_csum_verify(ctx, buf)) {
				pfatal("BAD JOURNAL DESCRIPTOR CHECKSUM\n");
				return (EINVAL);
			}
			if (jbd2_count_tags(ctx, buf, &tag_count) != 0) {
				pfatal("MALFORMED JOURNAL DESCRIPTOR AT BLOCK %u\n",
				    block);
				return (EINVAL);
			}
			if (tag_count > loglen - consumed) {
				pfatal("JOURNAL DESCRIPTOR DATA EXCEEDS LOG\n");
				return (EINVAL);
			}
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
			if (jbd2_revoke_block_check(ctx, buf,
			    &revoke_count) != 0) {
				pfatal("MALFORMED JOURNAL REVOKE BLOCK %u\n", block);
				return (EINVAL);
			}
			block = jbd2_next_block(ctx, block);
			break;
		case JBD2_COMMIT_BLOCK:
			if (!in_transaction)
				return (EINVAL);
			if (!jbd2_commit_block_csum_verify(ctx, buf)) {
				pfatal("BAD JOURNAL COMMIT CHECKSUM\n");
				return (EINVAL);
			}
			seq++;
			in_transaction = 0;
			ctx->end_sequence = seq;
			block = jbd2_next_block(ctx, block);
			break;
		default:
			return (EINVAL);
		}
	}
	if (in_transaction)
		return (EINVAL);
	printf("journal scan: end sequence %u (%u transactions)\n",
	    ctx->end_sequence, ctx->end_sequence - ctx->sequence);
	return 0;
}

static int
jbd2_pass_revoke(struct fsck_jbd2_ctx *ctx, char *buf)
{
	u_int32_t block, consumed, loglen, revoke_count, seq, tag_count;
	struct jbd2_header *hdr;
	int has_64bit = ctx->features_incompat & JBD2_FEATURE_INCOMPAT_64BIT;
	int error;

	block = ctx->start;
	seq = ctx->sequence;
	consumed = 0;
	loglen = ctx->maxlen - ctx->first;

	while (seq != ctx->end_sequence && consumed < loglen) {
		if (jbd2_read_jblock(ctx, block, buf) != 0)
			return (EIO);
		consumed++;
		hdr = (struct jbd2_header *)buf;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq)
			return (EINVAL);

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			if (!jbd2_metadata_block_csum_verify(ctx, buf))
				return (EINVAL);
			if (jbd2_count_tags(ctx, buf, &tag_count) != 0)
				return (EINVAL);
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
		case JBD2_REVOKE_BLOCK: {
			u_int32_t off = sizeof(struct jbd2_revoke_header);
			u_int32_t i;

			if (jbd2_revoke_block_check(ctx, buf,
			    &revoke_count) != 0)
				return (EINVAL);
			for (i = 0; i < revoke_count; i++) {
				u_int64_t revblk;
				if (has_64bit) {
					u_int32_t high, low;

					memcpy(&high, buf + off, 4);
					memcpy(&low, buf + off + 4, 4);
					revblk = (u_int64_t)betoh32(high) << 32 |
					    betoh32(low);
					off += 8;
				} else {
					u_int32_t value;

					memcpy(&value, buf + off, 4);
					revblk = betoh32(value);
					off += 4;
				}
				if (revblk >= sblock.m_blocks_count)
					return (EINVAL);
				if (jbd2_is_journal_block(ctx, revblk))
					return (EINVAL);
				error = jbd2_revoke_add(ctx, revblk, seq);
				if (error)
					return (error);
			}
			block = jbd2_next_block(ctx, block);
			break;
		}
		case JBD2_COMMIT_BLOCK:
			if (!jbd2_commit_block_csum_verify(ctx, buf))
				return (EINVAL);
			block = jbd2_next_block(ctx, block);
			seq++;
			break;
		default:
			return (EINVAL);
		}
	}
	if (seq != ctx->end_sequence)
		return (EINVAL);
	if (ctx->revoke_count > 0)
		printf("journal revoke: %u blocks revoked\n", ctx->revoke_count);
	return 0;
}

static int
jbd2_pass_replay(struct fsck_jbd2_ctx *ctx, char *buf, int apply)
{
	u_int32_t block, checksum, consumed, loglen, revoke_count, seq;
	u_int32_t replayed = 0, tag_count;
	struct jbd2_header *hdr;
	char *dbuf;
	u_int32_t offset, flags, limit;
	u_int64_t target;
	int uuid_seen;

	dbuf = malloc(ctx->blocksize);
	if (dbuf == NULL) {
		free(dbuf);
		return (ENOMEM);
	}

	block = ctx->start;
	seq = ctx->sequence;
	consumed = 0;
	loglen = ctx->maxlen - ctx->first;
	limit = jbd2_descriptor_limit(ctx);

	while (seq != ctx->end_sequence && consumed < loglen) {
		if (jbd2_read_jblock(ctx, block, buf) != 0) {
			free(dbuf);
			return (EIO);
		}
		consumed++;
		hdr = (struct jbd2_header *)buf;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq) {
			free(dbuf);
			return (EINVAL);
		}

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			if (!jbd2_metadata_block_csum_verify(ctx, buf) ||
			    jbd2_count_tags(ctx, buf, &tag_count) != 0 ||
			    tag_count > loglen - consumed) {
				free(dbuf);
				return (EINVAL);
			}
			offset = sizeof(struct jbd2_header);
			uuid_seen = 0;
			while (offset < limit) {
				if (jbd2_parse_tag(ctx, buf, limit,
				    &offset, &target, &flags, &checksum,
				    &uuid_seen) != 0) {
					free(dbuf);
					return (EINVAL);
				}
				if (flags & JBD2_FLAG_DELETED) {
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
				}
				block = jbd2_next_block(ctx, block);
				consumed++;
				if (jbd2_revoke_check(ctx, target, seq)) {
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
				}
				if (jbd2_read_jblock(ctx, block, dbuf) != 0) {
					free(dbuf);
					return (EIO);
				}
				if (!jbd2_data_block_csum_verify(ctx, dbuf, seq,
				    checksum)) {
					free(dbuf);
					return (EINVAL);
				}
				if (!apply) {
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
				}
				if (flags & JBD2_FLAG_ESCAPE) {
					u_int32_t magic = htobe32(JBD2_MAGIC);
					memcpy(dbuf, &magic, 4);
				}
				if (jwrite(target, dbuf, ctx->blocksize) != 0) {
					free(dbuf);
					return (EIO);
				}
				replayed++;
				if (flags & JBD2_FLAG_LAST_TAG)
					break;
			}
			block = jbd2_next_block(ctx, block);
			break;
		case JBD2_REVOKE_BLOCK:
			if (jbd2_revoke_block_check(ctx, buf,
			    &revoke_count) != 0) {
				free(dbuf);
				return (EINVAL);
			}
			block = jbd2_next_block(ctx, block);
			break;
		case JBD2_COMMIT_BLOCK:
			if (!jbd2_commit_block_csum_verify(ctx, buf)) {
				free(dbuf);
				return (EINVAL);
			}
			block = jbd2_next_block(ctx, block);
			seq++;
			break;
		default:
			free(dbuf);
			return (EINVAL);
		}
	}

	free(dbuf);
	if (seq != ctx->end_sequence)
		return (EINVAL);
	if (apply)
		printf("journal replay: %u blocks replayed\n", replayed);
	return 0;
}

static int
jbd2_superblock_csum_verify(struct fsck_jbd2_ctx *ctx,
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

static void
jbd2_superblock_csum_set(struct fsck_jbd2_ctx *ctx,
    struct jbd2_superblock *jsb)
{
	u_int32_t checksum;

	if (!jbd2_has_csum_v2or3(ctx))
		return;
	jsb->s_checksum = 0;
	checksum = ~crc32c(0, (const uint8_t *)jsb, sizeof(*jsb));
	jsb->s_checksum = htobe32(checksum);
}

static int
jbd2_recovered_super_check(void)
{
	struct ext4fs *sble;
	char buf[EXT4FS_SUPER_BLOCK_SIZE];
	u_int32_t incompat, ro_compat;

	if (pread(fsreadfd, buf, sizeof(buf), EXT4FS_SUPER_BLOCK_OFFSET) !=
	    sizeof(buf))
		return (errno ? errno : EIO);
	sble = (struct ext4fs *)buf;
	if (letoh16(sble->sb_magic) != EXT4FS_MAGIC ||
	    memcmp(sble->sb_uuid, sblock.m_sble.sb_uuid,
	    sizeof(sble->sb_uuid)) != 0 ||
	    ext4fs_sb_csum_verify(sble) != 0)
		return (EINVAL);
	if (sble->sb_log_block_size != sblock.m_sble.sb_log_block_size ||
	    sble->sb_blocks_count_lo != sblock.m_sble.sb_blocks_count_lo ||
	    sble->sb_blocks_count_hi != sblock.m_sble.sb_blocks_count_hi ||
	    sble->sb_inodes_count != sblock.m_sble.sb_inodes_count ||
	    sble->sb_first_data_block != sblock.m_sble.sb_first_data_block ||
	    sble->sb_blocks_per_group != sblock.m_sble.sb_blocks_per_group ||
	    sble->sb_inodes_per_group != sblock.m_sble.sb_inodes_per_group ||
	    sble->sb_inode_size != sblock.m_sble.sb_inode_size ||
	    sble->sb_block_group_descriptor_size !=
	    sblock.m_sble.sb_block_group_descriptor_size ||
	    sble->sb_journal_inode_number !=
	    sblock.m_sble.sb_journal_inode_number)
		return (EINVAL);
	incompat = letoh32(sble->sb_feature_incompat);
	ro_compat = letoh32(sble->sb_feature_ro_compat);
	if ((incompat & ~EXT4FS_FEATURE_INCOMPAT_SUPPORTED) != 0 ||
	    (ro_compat & ~EXT4FS_FEATURE_RO_COMPAT_SUPPORTED) != 0 ||
	    !(incompat & EXT4FS_FEATURE_INCOMPAT_RECOVER) ||
	    !(letoh32(sble->sb_feature_compat) &
	    EXT4FS_FEATURE_COMPAT_HAS_JOURNAL))
		return (EINVAL);
	return (0);
}

int
fsck_journal_replay(int apply)
{
	struct fsck_jbd2_ctx ctx;
	struct jbd2_superblock *jsb;
	struct ext4fs_dinode *jdi;
	char *ibuf, *buf;
	u_int64_t jblock0, journal_blocks, journal_bytes;
	u_int32_t btype, group, index, journal_ino, unsupported;
	u_int64_t inode_offset, itb, blk;
	u_int32_t off;
	int error;

	if (!(sblock.m_feature_compat & EXT4FS_FEATURE_COMPAT_HAS_JOURNAL))
		return 0;
	if (!(sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_RECOVER))
		return 0;

	memset(&ctx, 0, sizeof(ctx));
	ibuf = NULL;
	buf = NULL;
	error = 0;

	journal_ino = sblock.m_journal_inode_number;
	if (journal_ino == 0 || journal_ino > sblock.m_inodes_count ||
	    sblock.m_inodes_per_group == 0) {
		pfatal("INVALID JOURNAL INODE %u\n", journal_ino);
		return (EINVAL);
	}

	ibuf = malloc(sblock.m_block_size);
	if (ibuf == NULL)
		return (ENOMEM);

	group = (journal_ino - 1) / sblock.m_inodes_per_group;
	index = (journal_ino - 1) % sblock.m_inodes_per_group;
	if (group >= sblock.m_block_group_count ||
	    sblock.m_inode_size < sizeof(struct ext4fs_dinode) ||
	    ((sblock.m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM) &&
	    sblock.m_inode_size < sizeof(struct ext4fs_dinode_256))) {
		pfatal("INVALID JOURNAL INODE LOCATION\n");
		error = EINVAL;
		goto out;
	}

	itb = letoh32(sblock.m_gd[group].bgd_inode_table_block_lo);
	if (sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		itb |= (u_int64_t)letoh32(sblock.m_gd[group].bgd_inode_table_block_hi) << 32;

	inode_offset = (u_int64_t)index * sblock.m_inode_size;
	if (itb >= sblock.m_blocks_count ||
	    inode_offset / sblock.m_block_size >= sblock.m_blocks_count - itb) {
		pfatal("INVALID JOURNAL INODE TABLE\n");
		error = EINVAL;
		goto out;
	}
	blk = itb + inode_offset / sblock.m_block_size;
	off = inode_offset % sblock.m_block_size;
	if (off + sblock.m_inode_size > sblock.m_block_size) {
		pfatal("JOURNAL INODE CROSSES A BLOCK BOUNDARY\n");
		error = EINVAL;
		goto out;
	}

	if (jread(blk, ibuf, sblock.m_block_size) != 0) {
		pfatal("CAN'T READ JOURNAL INODE\n");
		error = EIO;
		goto out;
	}

	jdi = (struct ext4fs_dinode *)(ibuf + off);
	ctx.journal_ino = journal_ino;
	ctx.journal_gen = jdi->i_nfs_generation;
	if (ext4fs_inode_csum_verify(&sblock,
	    (struct ext4fs_dinode_256 *)jdi, journal_ino) != 0) {
		pfatal("JOURNAL INODE CHECKSUM IS INVALID\n");
		error = EINVAL;
		goto out;
	}
	if ((letoh16(jdi->i_mode) & IFMT) != IFREG) {
		pfatal("JOURNAL INODE IS NOT A REGULAR FILE\n");
		error = EINVAL;
		goto out;
	}
	if (!(letoh32(jdi->i_flags) & EXTFS_INODE_FLAG_EXTENTS)) {
		pfatal("JOURNAL INODE DOES NOT USE EXTENTS\n");
		error = EINVAL;
		goto out;
	}
	journal_bytes = letoh32(jdi->i_size_lo) |
	    (u_int64_t)letoh32(jdi->i_size_hi) << 32;
	if (journal_bytes < sblock.m_block_size) {
		pfatal("JOURNAL INODE IS TOO SMALL\n");
		error = EINVAL;
		goto out;
	}
	error = jbd2_extent_lookup(&ctx, &jdi->i_extent_header,
	    sizeof(jdi->i_block), -1, 0, &jblock0);
	if (error || jblock0 == 0) {
		pfatal("CAN'T LOCATE JOURNAL BLOCK 0\n");
		error = error ? error : EINVAL;
		goto out;
	}

	buf = malloc(sblock.m_block_size);
	if (buf == NULL) {
		error = ENOMEM;
		goto out;
	}

	if (jread(jblock0, buf, sblock.m_block_size) != 0) {
		pfatal("CAN'T READ JOURNAL SUPERBLOCK\n");
		error = EIO;
		goto out;
	}

	jsb = (struct jbd2_superblock *)buf;
	if (betoh32(jsb->s_header.h_magic) != JBD2_MAGIC) {
		pfatal("BAD JOURNAL MAGIC 0x%x\n",
		    betoh32(jsb->s_header.h_magic));
		error = EINVAL;
		goto out;
	}
	btype = betoh32(jsb->s_header.h_blocktype);
	if (btype != JBD2_SUPERBLOCK_V2) {
		pfatal("UNSUPPORTED JOURNAL SUPERBLOCK VERSION %u\n", btype);
		error = EINVAL;
		goto out;
	}
	if (betoh32(jsb->s_errno) != 0) {
		pfatal("JOURNAL RECORDS ERROR %u\n", betoh32(jsb->s_errno));
		error = EIO;
		goto out;
	}

	ctx.blocksize = betoh32(jsb->s_blocksize);
	ctx.maxlen = betoh32(jsb->s_maxlen);
	ctx.first = betoh32(jsb->s_first);
	ctx.sequence = betoh32(jsb->s_sequence);
	ctx.start = betoh32(jsb->s_start);
	if (btype == JBD2_SUPERBLOCK_V2) {
		ctx.features_compat = betoh32(jsb->s_feature_compat);
		ctx.features_incompat = betoh32(jsb->s_feature_incompat);
		ctx.features_ro_compat = betoh32(jsb->s_feature_ro_compat);
		if (betoh32(jsb->s_nr_users) != 1) {
			pfatal("SHARED JOURNAL IS NOT SUPPORTED\n");
			error = EINVAL;
			goto out;
		}
	}

	if (ctx.blocksize != sblock.m_block_size) {
		pfatal("JOURNAL BLOCKSIZE %u != FS BLOCKSIZE %llu\n",
		    ctx.blocksize, (unsigned long long)sblock.m_block_size);
		error = EINVAL;
		goto out;
	}
	if (journal_bytes % ctx.blocksize != 0) {
		pfatal("JOURNAL INODE SIZE IS NOT BLOCK ALIGNED\n");
		error = EINVAL;
		goto out;
	}
	journal_blocks = journal_bytes / ctx.blocksize;
	if (ctx.first == 0 || ctx.first >= ctx.maxlen ||
	    ctx.maxlen > journal_blocks ||
	    ctx.maxlen > JBD2_MAX_BLOCKMAP_ENTRIES ||
	    (ctx.start != 0 && (ctx.start < ctx.first ||
	    ctx.start >= ctx.maxlen))) {
		pfatal("INVALID JOURNAL GEOMETRY\n");
		error = EINVAL;
		goto out;
	}
	unsupported = ctx.features_compat & ~JBD2_FEATURE_COMPAT_SUPPORTED;
	if (unsupported != 0) {
		pfatal("UNSUPPORTED JOURNAL COMPAT FEATURES 0x%x\n",
		    unsupported);
		error = EINVAL;
		goto out;
	}
	unsupported = ctx.features_incompat &
	    ~JBD2_FEATURE_INCOMPAT_SUPPORTED;
	if (unsupported != 0) {
		pfatal("UNSUPPORTED JOURNAL INCOMPAT FEATURES 0x%x\n",
		    unsupported);
		error = EINVAL;
		goto out;
	}
	unsupported = ctx.features_ro_compat &
	    ~JBD2_FEATURE_RO_COMPAT_SUPPORTED;
	if (unsupported != 0) {
		pfatal("UNSUPPORTED JOURNAL RO-COMPAT FEATURES 0x%x\n",
		    unsupported);
		error = EINVAL;
		goto out;
	}
	if ((ctx.features_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) &&
	    (ctx.features_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3)) {
		pfatal("JOURNAL ENABLES BOTH CHECKSUM V2 AND V3\n");
		error = EINVAL;
		goto out;
	}
	if (btype == JBD2_SUPERBLOCK_V2) {
		const u_int8_t zero_uuid[16] = { 0 };
		const u_int8_t *expected_uuid;

		expected_uuid = sblock.m_sble.sb_journal_uuid;
		if (memcmp(expected_uuid, zero_uuid, sizeof(zero_uuid)) == 0)
			expected_uuid = sblock.m_sble.sb_uuid;
		if (memcmp(jsb->s_uuid, expected_uuid,
		    sizeof(jsb->s_uuid)) != 0) {
			pfatal("JOURNAL UUID DOES NOT MATCH FILESYSTEM\n");
			error = EINVAL;
			goto out;
		}
	}
	memcpy(ctx.uuid, jsb->s_uuid, sizeof(ctx.uuid));
	ctx.checksum_seed = crc32c(0, jsb->s_uuid,
	    sizeof(jsb->s_uuid));
	if (jbd2_has_csum_v2or3(&ctx) &&
	    jsb->s_checksum_type != JBD2_CHECKSUM_CRC32C) {
		pfatal("UNSUPPORTED JOURNAL CHECKSUM TYPE %u\n",
		    jsb->s_checksum_type);
		error = EINVAL;
		goto out;
	}
	if (!jbd2_superblock_csum_verify(&ctx, jsb)) {
		pfatal("BAD JOURNAL SUPERBLOCK CHECKSUM\n");
		error = EINVAL;
		goto out;
	}

	ctx.blockmap = calloc(ctx.maxlen, sizeof(u_int64_t));
	if (ctx.blockmap == NULL) {
		error = ENOMEM;
		goto out;
	}
	ctx.blockmap_count = ctx.maxlen;

	error = jbd2_build_blockmap(&ctx, &jdi->i_extent_header);
	free(ibuf);
	ibuf = NULL;
	if (error)
		goto out;
	if (ctx.start == 0) {
		/*
		 * This is the restart point after replay and the clean journal
		 * marker became durable, but before RECOVER was cleared.
		 */
		printf("journal is already clean; completing recovery\n");
		ctx.end_sequence = ctx.sequence;
		if (!apply) {
			printf("RECOVER must be cleared with write access\n");
			error = EROFS;
			goto out;
		}
		goto clear_recover;
	}

	printf("replaying journal (sequence %u, start block %u, %u blocks)\n",
	    ctx.sequence, ctx.start, ctx.maxlen);

	error = jbd2_pass_scan(&ctx, buf);
	if (error)
		goto out;

	if (ctx.end_sequence == ctx.sequence) {
		printf("journal has no valid transactions\n");
		if (!apply) {
			error = EROFS;
			goto out;
		}
		goto clear;
	}

	error = jbd2_pass_revoke(&ctx, buf);
	if (error)
		goto out;

	/* Validate every payload before allowing the first home-block write. */
	error = jbd2_pass_replay(&ctx, buf, 0);
	if (error)
		goto out;
	if (!apply) {
		printf("journal validates but must be replayed with write access\n");
		error = EROFS;
		goto out;
	}

	error = jbd2_pass_replay(&ctx, buf, 1);
	if (error)
		goto out;
	error = jbd2_flush_device();
	if (error)
		goto out;
	error = jbd2_recovered_super_check();
	if (error) {
		pfatal("REPLAY PRODUCED AN INVALID SUPERBLOCK\n");
		goto out;
	}

clear:
	if (jread(jblock0, buf, ctx.blocksize) != 0) {
		error = EIO;
		goto out;
	}
	jsb = (struct jbd2_superblock *)buf;
	jsb->s_start = htobe32(0);
	jsb->s_sequence = htobe32(ctx.end_sequence);
	jbd2_superblock_csum_set(&ctx, jsb);
	error = jwrite(jblock0, buf, ctx.blocksize);
	if (error)
		goto out;
	error = jbd2_flush_device();
	if (error)
		goto out;

clear_recover:
	{
		struct ext4fs *sble;
		u_int32_t incompat;
		char *sbbuf;

		sbbuf = malloc(EXT4FS_SUPER_BLOCK_SIZE);
		if (sbbuf == NULL) {
			error = ENOMEM;
			goto out;
		}
		if (pread(fsreadfd, sbbuf, EXT4FS_SUPER_BLOCK_SIZE,
		    EXT4FS_SUPER_BLOCK_OFFSET) != EXT4FS_SUPER_BLOCK_SIZE) {
			error = errno ? errno : EIO;
			free(sbbuf);
			goto out;
		}
		sble = (struct ext4fs *)sbbuf;
		if (letoh16(sble->sb_magic) != EXT4FS_MAGIC ||
		    memcmp(sble->sb_uuid, sblock.m_sble.sb_uuid,
		    sizeof(sble->sb_uuid)) != 0 ||
		    ext4fs_sb_csum_verify(sble) != 0) {
			error = EINVAL;
			free(sbbuf);
			goto out;
		}
		incompat = letoh32(sble->sb_feature_incompat);
		incompat &= ~EXT4FS_FEATURE_INCOMPAT_RECOVER;
		sble->sb_feature_incompat = htole32(incompat);
		/* Final clean-state marking belongs to a successful full fsck. */
		sble->sb_checksum = htole32(ext4fs_sb_csum(sble));
		if (pwrite(fswritefd, sbbuf, EXT4FS_SUPER_BLOCK_SIZE,
		    EXT4FS_SUPER_BLOCK_OFFSET) != EXT4FS_SUPER_BLOCK_SIZE) {
			error = errno ? errno : EIO;
			free(sbbuf);
			goto out;
		}
		fsmodified = 1;
		free(sbbuf);
		error = jbd2_flush_device();
		if (error)
			goto out;
	}

	printf("journal replay complete\n");
	error = 0;

out:
	free(ibuf);
	free(buf);
	free(ctx.blockset);
	free(ctx.blockmap);
	free(ctx.revoke);
	return error;
}
