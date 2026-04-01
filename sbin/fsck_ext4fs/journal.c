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
#include <sys/endian.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
	u_int32_t	features_incompat;
	u_int64_t	*blockmap;
	u_int32_t	blockmap_count;
	struct jbd2_revoke_entry *revoke;
	u_int32_t	revoke_count;
	u_int32_t	revoke_alloc;
	u_int32_t	end_sequence;
};

static int
jread(u_int64_t fsblock, char *buf, long size)
{
	return bread(fsreadfd, buf, EXT4FS_FSBTODB(&sblock, fsblock), size);
}

static int
jwrite(u_int64_t fsblock, char *buf, long size)
{
	bwrite(fswritefd, buf, EXT4FS_FSBTODB(&sblock, fsblock), size);
	return 0;
}

static int
jbd2_build_blockmap(struct fsck_jbd2_ctx *ctx,
    struct ext4fs_extent_header *eh)
{
	struct ext4fs_extent *ext;
	struct ext4fs_extent_idx *idx;
	u_int16_t depth, entries, i;
	u_int32_t jblock, len;
	u_int64_t pblock;

	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC)
		return (EINVAL);

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
				if (j < ctx->blockmap_count)
					ctx->blockmap[j] = pblock + jblock;
			}
		}
	} else {
		idx = (struct ext4fs_extent_idx *)(eh + 1);
		for (i = 0; i < entries; i++) {
			struct ext4fs_extent_header *leh;
			struct ext4fs_extent *lext;
			u_int16_t lentries, j;
			u_int64_t leaf_block;
			char *lbuf;

			leaf_block =
			    (u_int64_t)letoh16(idx[i].ei_leaf_hi) << 32 |
			    letoh32(idx[i].ei_leaf_lo);
			lbuf = malloc(sblock.m_block_size);
			if (lbuf == NULL)
				return (ENOMEM);
			if (jread(leaf_block, lbuf, sblock.m_block_size) != 0) {
				free(lbuf);
				return (EIO);
			}
			leh = (struct ext4fs_extent_header *)lbuf;
			if (letoh16(leh->eh_magic) !=
			    EXT4FS_EXTENT_HEADER_MAGIC ||
			    letoh16(leh->eh_depth) != 0) {
				free(lbuf);
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
					if (k < ctx->blockmap_count)
						ctx->blockmap[k] =
						    pblock + jblock;
				}
			}
			free(lbuf);
		}
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
jbd2_parse_tag(struct fsck_jbd2_ctx *ctx, char *buf, u_int32_t bufsize,
    u_int32_t *offset, u_int64_t *target, u_int32_t *flags)
{
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
		if (has_64bit)
			*target |= (u_int64_t)betoh32(tag3->t_blocknr_high) << 32;
		*offset += tag_size;
		if (!(*flags & JBD2_FLAG_SAME_UUID))
			*offset += 16;
	} else {
		struct jbd2_block_tag *tag;
		u_int32_t tag_size = 8;
		if (*offset + tag_size > bufsize)
			return (EINVAL);
		tag = (struct jbd2_block_tag *)(buf + *offset);
		*target = betoh32(tag->t_blocknr);
		*flags = betoh16(tag->t_flags);
		*offset += tag_size;
		if (has_64bit) {
			if (*offset + 4 > bufsize)
				return (EINVAL);
			*target |= (u_int64_t)betoh32(tag->t_blocknr_high) << 32;
			*offset += 4;
		}
		if (!(*flags & JBD2_FLAG_SAME_UUID))
			*offset += 16;
	}
	return (0);
}

static int
jbd2_count_tags(struct fsck_jbd2_ctx *ctx, char *buf, u_int32_t *count)
{
	u_int32_t offset = sizeof(struct jbd2_header);
	u_int32_t flags;
	u_int64_t target;

	*count = 0;
	while (offset < ctx->blocksize) {
		if (jbd2_parse_tag(ctx, buf, ctx->blocksize, &offset,
		    &target, &flags) != 0)
			break;
		(*count)++;
		if (flags & JBD2_FLAG_LAST_TAG)
			break;
	}
	return (0);
}

static void
jbd2_revoke_add(struct fsck_jbd2_ctx *ctx, u_int64_t block, u_int32_t seq)
{
	u_int32_t i;

	for (i = 0; i < ctx->revoke_count; i++) {
		if (ctx->revoke[i].re_block == block) {
			if (seq > ctx->revoke[i].re_sequence)
				ctx->revoke[i].re_sequence = seq;
			return;
		}
	}
	if (ctx->revoke_count >= ctx->revoke_alloc) {
		u_int32_t newalloc = ctx->revoke_alloc ? ctx->revoke_alloc * 2 : 64;
		struct jbd2_revoke_entry *nr;
		nr = reallocarray(ctx->revoke, newalloc, sizeof(*nr));
		if (nr == NULL)
			return;
		ctx->revoke = nr;
		ctx->revoke_alloc = newalloc;
	}
	ctx->revoke[ctx->revoke_count].re_block = block;
	ctx->revoke[ctx->revoke_count].re_sequence = seq;
	ctx->revoke_count++;
}

static int
jbd2_revoke_check(struct fsck_jbd2_ctx *ctx, u_int64_t block, u_int32_t seq)
{
	u_int32_t i;
	for (i = 0; i < ctx->revoke_count; i++)
		if (ctx->revoke[i].re_block == block &&
		    ctx->revoke[i].re_sequence >= seq)
			return 1;
	return 0;
}

static int
jbd2_pass_scan(struct fsck_jbd2_ctx *ctx, char *buf)
{
	u_int32_t block, seq, next_seq, tag_count;
	struct jbd2_header *hdr;

	block = ctx->start;
	seq = ctx->sequence;
	ctx->end_sequence = seq;

	printf("journal scan: start block %u sequence %u\n", block, seq);

	while (1) {
		if (jbd2_read_jblock(ctx, block, buf) != 0)
			break;
		hdr = (struct jbd2_header *)buf;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq)
			break;

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			jbd2_count_tags(ctx, buf, &tag_count);
			{
				u_int32_t i;
				for (i = 0; i < tag_count; i++)
					block = jbd2_next_block(ctx, block);
			}
			block = jbd2_next_block(ctx, block);
			if (jbd2_read_jblock(ctx, block, buf) != 0)
				goto done;
			hdr = (struct jbd2_header *)buf;
			if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
			    betoh32(hdr->h_sequence) != seq ||
			    betoh32(hdr->h_blocktype) != JBD2_COMMIT_BLOCK) {
				printf("journal scan: bad commit for seq %u\n", seq);
				goto done;
			}
			next_seq = seq + 1;
			ctx->end_sequence = next_seq;
			seq = next_seq;
			block = jbd2_next_block(ctx, block);
			break;
		case JBD2_REVOKE_BLOCK:
			block = jbd2_next_block(ctx, block);
			break;
		case JBD2_COMMIT_BLOCK:
			goto done;
		default:
			goto done;
		}
	}
done:
	printf("journal scan: end sequence %u (%u transactions)\n",
	    ctx->end_sequence, ctx->end_sequence - ctx->sequence);
	return 0;
}

static int
jbd2_pass_revoke(struct fsck_jbd2_ctx *ctx, char *buf)
{
	u_int32_t block, seq, tag_count;
	struct jbd2_header *hdr;
	int has_64bit = ctx->features_incompat & JBD2_FEATURE_INCOMPAT_64BIT;

	block = ctx->start;
	seq = ctx->sequence;

	while (seq < ctx->end_sequence) {
		if (jbd2_read_jblock(ctx, block, buf) != 0)
			return EIO;
		hdr = (struct jbd2_header *)buf;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq)
			break;

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			jbd2_count_tags(ctx, buf, &tag_count);
			{
				u_int32_t i;
				for (i = 0; i < tag_count; i++)
					block = jbd2_next_block(ctx, block);
			}
			block = jbd2_next_block(ctx, block);
			block = jbd2_next_block(ctx, block);
			seq++;
			break;
		case JBD2_REVOKE_BLOCK: {
			struct jbd2_revoke_header *rh =
			    (struct jbd2_revoke_header *)buf;
			u_int32_t rcount = betoh32(rh->r_count);
			u_int32_t off = sizeof(struct jbd2_revoke_header);
			while (off < rcount) {
				u_int64_t revblk;
				if (has_64bit) {
					if (off + 8 > rcount)
						break;
					revblk =
					    (u_int64_t)betoh32(*(u_int32_t *)(buf + off)) << 32 |
					    betoh32(*(u_int32_t *)(buf + off + 4));
					off += 8;
				} else {
					if (off + 4 > rcount)
						break;
					revblk = betoh32(*(u_int32_t *)(buf + off));
					off += 4;
				}
				jbd2_revoke_add(ctx, revblk, seq);
			}
			block = jbd2_next_block(ctx, block);
			break;
		}
		case JBD2_COMMIT_BLOCK:
			block = jbd2_next_block(ctx, block);
			seq++;
			break;
		default:
			block = jbd2_next_block(ctx, block);
			break;
		}
	}
	if (ctx->revoke_count > 0)
		printf("journal revoke: %u blocks revoked\n", ctx->revoke_count);
	return 0;
}

static int
jbd2_pass_replay(struct fsck_jbd2_ctx *ctx, char *buf)
{
	u_int32_t block, seq, replayed = 0;
	struct jbd2_header *hdr;
	char *dbuf, *wbuf;
	u_int32_t offset, flags;
	u_int64_t target;

	dbuf = malloc(ctx->blocksize);
	wbuf = malloc(ctx->blocksize);
	if (dbuf == NULL || wbuf == NULL) {
		free(dbuf);
		free(wbuf);
		return ENOMEM;
	}

	block = ctx->start;
	seq = ctx->sequence;

	while (seq < ctx->end_sequence) {
		if (jbd2_read_jblock(ctx, block, buf) != 0)
			break;
		hdr = (struct jbd2_header *)buf;
		if (betoh32(hdr->h_magic) != JBD2_MAGIC ||
		    betoh32(hdr->h_sequence) != seq)
			break;

		switch (betoh32(hdr->h_blocktype)) {
		case JBD2_DESCRIPTOR_BLOCK:
			offset = sizeof(struct jbd2_header);
			while (offset < ctx->blocksize) {
				if (jbd2_parse_tag(ctx, buf, ctx->blocksize,
				    &offset, &target, &flags) != 0)
					break;
				block = jbd2_next_block(ctx, block);
				if (jbd2_revoke_check(ctx, target, seq)) {
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
				}
				if (jbd2_read_jblock(ctx, block, dbuf) != 0) {
					if (flags & JBD2_FLAG_LAST_TAG)
						break;
					continue;
				}
				if (flags & JBD2_FLAG_ESCAPE) {
					u_int32_t magic = htobe32(JBD2_MAGIC);
					memcpy(dbuf, &magic, 4);
				}
				jwrite(target, dbuf, ctx->blocksize);
				replayed++;
				if (flags & JBD2_FLAG_LAST_TAG)
					break;
			}
			block = jbd2_next_block(ctx, block);
			block = jbd2_next_block(ctx, block);
			seq++;
			break;
		case JBD2_REVOKE_BLOCK:
			block = jbd2_next_block(ctx, block);
			break;
		case JBD2_COMMIT_BLOCK:
			block = jbd2_next_block(ctx, block);
			seq++;
			break;
		default:
			block = jbd2_next_block(ctx, block);
			break;
		}
	}

	free(dbuf);
	free(wbuf);
	printf("journal replay: %u blocks replayed\n", replayed);
	return 0;
}

int
fsck_journal_replay(void)
{
	struct fsck_jbd2_ctx ctx;
	struct jbd2_superblock *jsb;
	struct ext4fs_dinode *jdi;
	char *ibuf, *buf;
	u_int64_t jblock0;
	u_int32_t group, index;
	u_int64_t itb, blk;
	u_int32_t off;
	int error;

	if (!(sblock.m_feature_compat & EXT4FS_FEATURE_COMPAT_HAS_JOURNAL))
		return 0;
	if (!(sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_RECOVER))
		return 0;

	memset(&ctx, 0, sizeof(ctx));

	ibuf = malloc(sblock.m_block_size);
	if (ibuf == NULL)
		return ENOMEM;

	group = (EXT4FS_INODE_JOURNAL - 1) / sblock.m_inodes_per_group;
	index = (EXT4FS_INODE_JOURNAL - 1) % sblock.m_inodes_per_group;

	itb = letoh32(sblock.m_gd[group].bgd_inode_table_block_lo);
	if (sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		itb |= (u_int64_t)letoh32(sblock.m_gd[group].bgd_inode_table_block_hi) << 32;

	blk = itb + (index * sblock.m_inode_size) / sblock.m_block_size;
	off = (index * sblock.m_inode_size) % sblock.m_block_size;

	if (jread(blk, ibuf, sblock.m_block_size) != 0) {
		free(ibuf);
		pfatal("CAN'T READ JOURNAL INODE\n");
		return EIO;
	}

	jdi = (struct ext4fs_dinode *)(ibuf + off);

	if (letoh16(jdi->i_extent_header.eh_magic) !=
	    EXT4FS_EXTENT_HEADER_MAGIC ||
	    letoh16(jdi->i_extent_header.eh_entries) == 0) {
		free(ibuf);
		pfatal("JOURNAL INODE HAS NO EXTENTS\n");
		return EINVAL;
	}

	if (letoh16(jdi->i_extent_header.eh_depth) == 0) {
		struct ext4fs_extent *ext = jdi->i_extent;
		jblock0 = (u_int64_t)letoh16(ext->e_start_hi) << 32 |
		    letoh32(ext->e_start_lo);
	} else {
		struct ext4fs_extent_idx *idx = jdi->i_extent_idx;
		struct ext4fs_extent_header *leh;
		struct ext4fs_extent *lext;
		char *lbuf;
		u_int64_t leaf_block;

		leaf_block = (u_int64_t)letoh16(idx->ei_leaf_hi) << 32 |
		    letoh32(idx->ei_leaf_lo);
		lbuf = malloc(sblock.m_block_size);
		if (lbuf == NULL) {
			free(ibuf);
			return ENOMEM;
		}
		if (jread(leaf_block, lbuf, sblock.m_block_size) != 0) {
			free(lbuf);
			free(ibuf);
			return EIO;
		}
		leh = (struct ext4fs_extent_header *)lbuf;
		lext = (struct ext4fs_extent *)(leh + 1);
		jblock0 = (u_int64_t)letoh16(lext->e_start_hi) << 32 |
		    letoh32(lext->e_start_lo);
		free(lbuf);
	}

	buf = malloc(sblock.m_block_size);
	if (buf == NULL) {
		free(ibuf);
		return ENOMEM;
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
	{
		u_int32_t btype = betoh32(jsb->s_header.h_blocktype);
		if (btype != JBD2_SUPERBLOCK_V1 && btype != JBD2_SUPERBLOCK_V2) {
			pfatal("BAD JOURNAL SUPERBLOCK VERSION %u\n", btype);
			error = EINVAL;
			goto out;
		}
		if (btype == JBD2_SUPERBLOCK_V2)
			ctx.features_incompat = betoh32(jsb->s_feature_incompat);
	}

	ctx.blocksize = betoh32(jsb->s_blocksize);
	ctx.maxlen = betoh32(jsb->s_maxlen);
	ctx.first = betoh32(jsb->s_first);
	ctx.sequence = betoh32(jsb->s_sequence);
	ctx.start = betoh32(jsb->s_start);

	if (ctx.blocksize != sblock.m_block_size) {
		pfatal("JOURNAL BLOCKSIZE %u != FS BLOCKSIZE %llu\n",
		    ctx.blocksize, (unsigned long long)sblock.m_block_size);
		error = EINVAL;
		goto out;
	}

	if (ctx.start == 0) {
		printf("journal is clean, no replay needed\n");
		error = 0;
		goto clear_recover;
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

	printf("replaying journal (sequence %u, start block %u, %u blocks)\n",
	    ctx.sequence, ctx.start, ctx.maxlen);

	error = jbd2_pass_scan(&ctx, buf);
	if (error)
		goto out;

	if (ctx.end_sequence == ctx.sequence) {
		printf("journal has no valid transactions\n");
		goto clear;
	}

	error = jbd2_pass_revoke(&ctx, buf);
	if (error)
		goto out;

	error = jbd2_pass_replay(&ctx, buf);
	if (error)
		goto out;

clear:
	if (jread(jblock0, buf, ctx.blocksize) != 0) {
		error = EIO;
		goto out;
	}
	jsb = (struct jbd2_superblock *)buf;
	jsb->s_start = htobe32(0);
	jsb->s_sequence = htobe32(ctx.end_sequence);
	jwrite(jblock0, buf, ctx.blocksize);

clear_recover:
	{
		u_int32_t incompat;
		char *sbbuf;

		incompat = letoh32(sblock.m_sble.sb_feature_incompat);
		incompat &= ~EXT4FS_FEATURE_INCOMPAT_RECOVER;
		sblock.m_sble.sb_feature_incompat = htole32(incompat);
		sblock.m_feature_incompat = incompat;
		sblock.m_sble.sb_state = htole16(EXT4FS_STATE_VALID);
		sblock.m_state = EXT4FS_STATE_VALID;
		sblock.m_sble.sb_checksum =
		    htole32(ext4fs_sb_csum(&sblock.m_sble));

		sbbuf = malloc(EXT4FS_SUPER_BLOCK_SIZE);
		if (sbbuf != NULL) {
			if (bread(fsreadfd, sbbuf,
			    EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE,
			    EXT4FS_SUPER_BLOCK_SIZE) == 0) {
				memcpy(sbbuf, &sblock.m_sble,
				    sizeof(struct ext4fs));
				bwrite(fswritefd, sbbuf,
				    EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE,
				    EXT4FS_SUPER_BLOCK_SIZE);
			}
			free(sbbuf);
		}
	}

	printf("journal replay complete\n");
	error = 0;

out:
	free(ibuf);
	free(buf);
	free(ctx.blockmap);
	free(ctx.revoke);
	return error;
}
