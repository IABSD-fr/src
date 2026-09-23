/*
 * Copyright (c) 2025,2026 kmx.io.
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
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/dkio.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/vnode.h>

#include <lib/libkern/crc32c.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>

#include <ufs/ext4fs/ext4fs_dinode.h>
#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_orphan.h>

struct ext4fs_orphan_extent_ctx {
	struct m_ext4fs	*fs;
	struct vnode	*devvp;
	u_int32_t	 ino;
	u_int32_t	 generation;
	u_int64_t	 cutoff;
	u_int64_t	 nodes_left;
};

#define EXT4FS_ORPHAN_XATTR_MAGIC	0xea020000

struct ext4fs_orphan_xattr_header {
	u_int32_t	h_magic;
	u_int32_t	h_refcount;
	u_int32_t	h_blocks;
	u_int32_t	h_hash;
	u_int32_t	h_checksum;
	u_int32_t	h_reserved[3];
} __attribute__((packed));

static int	ext4fs_orphan_inode_read (struct m_ext4fs *, struct vnode *,
		    u_int32_t, struct ext4fs_dinode_256 *);
static int	ext4fs_orphan_inode_write (struct m_ext4fs *, struct vnode *,
		    u_int32_t, struct ext4fs_dinode_256 *);
static int	ext4fs_orphan_recount (struct mount *, int64_t);

static int
ext4fs_orphan_flush (struct mount *mp)
{
	struct vnode *devvp = VFSTOUFS(mp)->um_devvp;
	int error, force;

	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_FSYNC(devvp, FSCRED, MNT_WAIT, curproc);
	VOP_UNLOCK(devvp);
	if (error)
		return (error);
	force = 1;
	error = VOP_IOCTL(devvp, DIOCCACHESYNC, &force, FWRITE, FSCRED,
	    curproc);
	if (error == ENOTTY || error == EOPNOTSUPP)
		return (0);
	return (error);
}

static u_int64_t
ext4fs_orphan_bgd_block (struct m_ext4fs *fs,
    struct ext4fs_block_group_descriptor *gd, int which)
{
	u_int64_t block;

	switch (which) {
	case 0:
		block = letoh32(gd->bgd_block_bitmap_block_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			block |= (u_int64_t)
			    letoh32(gd->bgd_block_bitmap_block_hi) << 32;
		break;
	case 1:
		block = letoh32(gd->bgd_inode_bitmap_block_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			block |= (u_int64_t)
			    letoh32(gd->bgd_inode_bitmap_block_hi) << 32;
		break;
	default:
		block = letoh32(gd->bgd_inode_table_block_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			block |= (u_int64_t)
			    letoh32(gd->bgd_inode_table_block_hi) << 32;
		break;
	}
	return (block);
}

/*
 * Reject fixed filesystem metadata before trusting an orphan extent.  This
 * is deliberately stricter than the normal extent lookup path: recovery is
 * allowed to clear allocation bits and must never be directed at metadata.
 */
static int
ext4fs_orphan_data_block_valid (struct m_ext4fs *fs, u_int64_t block)
{
	struct ext4fs_block_group_descriptor *gd;
	u_int64_t group64, group_start, metadata_end, table;
	u_int32_t group, i;

	if (block < fs->m_first_data_block || block >= fs->m_blocks_count)
		return (0);
	if (fs->m_block_group_count > UINT32_MAX)
		return (0);
	group64 = (block - fs->m_first_data_block) /
	    fs->m_blocks_per_group;
	if (group64 >= fs->m_block_group_count)
		return (0);
	group = group64;
	/* flex_bg may place another group's metadata in this group. */
	for (i = 0; i < fs->m_block_group_count; i++) {
		gd = &fs->m_gd[i];
		if (block == ext4fs_orphan_bgd_block(fs, gd, 0) ||
		    block == ext4fs_orphan_bgd_block(fs, gd, 1))
			return (0);
		table = ext4fs_orphan_bgd_block(fs, gd, 2);
		if (block >= table &&
		    block - table < fs->m_inode_table_blocks_per_group)
			return (0);
	}

	group_start = fs->m_first_data_block +
	    (u_int64_t)group * fs->m_blocks_per_group;
	if (ext4fs_block_group_has_super_block(group)) {
		metadata_end = group_start + 1 +
		    fs->m_block_group_descriptor_blocks_count +
		    fs->m_reserved_bgdt_blocks;
		if (block >= group_start && block < metadata_end)
			return (0);
	}
	return (1);
}

static int
ext4fs_orphan_inode_location (struct m_ext4fs *fs, u_int32_t ino,
    u_int32_t *group, u_int32_t *offset, u_int64_t *block)
{
	struct ext4fs_block_group_descriptor *gd;
	u_int32_t index, table_index;

	if (ino == 0 || ino > fs->m_inodes_count)
		return (EINVAL);
	*group = (ino - 1) / fs->m_inodes_per_group;
	if (*group >= fs->m_block_group_count)
		return (EINVAL);
	index = (ino - 1) % fs->m_inodes_per_group;
	table_index = index / fs->m_inodes_per_block;
	*offset = (index % fs->m_inodes_per_block) * fs->m_inode_size;
	gd = &fs->m_gd[*group];
	if (letoh16(gd->bgd_flags) & EXT4FS_BGD_FLAG_INODE_UNINIT)
		return (EINVAL);
	*block = ext4fs_orphan_bgd_block(fs, gd, 2) + table_index;
	if (*block >= fs->m_blocks_count ||
	    *offset + sizeof(struct ext4fs_dinode_256) > fs->m_block_size)
		return (EINVAL);
	return (0);
}

static int
ext4fs_orphan_inode_read (struct m_ext4fs *fs, struct vnode *devvp,
    u_int32_t ino, struct ext4fs_dinode_256 *dp)
{
	struct buf *bp = NULL;
	u_int64_t block;
	u_int32_t group, offset;
	int error;

	error = ext4fs_orphan_inode_location(fs, ino, &group, &offset,
	    &block);
	if (error)
		return (error);
	error = bread(devvp, (daddr_t)EXT4FS_FSBTODB(fs, block),
	    fs->m_block_size, &bp);
	if (error) {
		if (bp != NULL)
			brelse(bp);
		return (error);
	}
	memcpy(dp, (char *)bp->b_data + offset, sizeof(*dp));
	brelse(bp);
	return (ext4fs_inode_csum_verify(fs, dp, ino));
}

static int
ext4fs_orphan_inode_write (struct m_ext4fs *fs, struct vnode *devvp,
    u_int32_t ino, struct ext4fs_dinode_256 *dp)
{
	struct buf *bp = NULL;
	u_int64_t block;
	u_int32_t checksum, group, offset;
	int error;

	error = ext4fs_orphan_inode_location(fs, ino, &group, &offset,
	    &block);
	if (error)
		return (error);
	checksum = ext4fs_inode_csum(fs, dp, ino);
	dp->dinode.i_checksum_lo = htole16(checksum & 0xffff);
	dp->dinode.i_checksum_hi = htole16(checksum >> 16);
	error = bread(devvp, (daddr_t)EXT4FS_FSBTODB(fs, block),
	    fs->m_block_size, &bp);
	if (error) {
		if (bp != NULL)
			brelse(bp);
		return (error);
	}
	memcpy((char *)bp->b_data + offset, dp, sizeof(*dp));
	return (bwrite(bp));
}

static int
ext4fs_orphan_bgd_write (struct m_ext4fs *fs, struct vnode *devvp,
    u_int32_t group)
{
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp = NULL;
	u_int32_t per_block, block, offset;
	daddr_t dblk;
	int error;

	per_block = fs->m_block_size /
	    sizeof(struct ext4fs_block_group_descriptor);
	block = group / per_block;
	offset = (group % per_block) *
	    sizeof(struct ext4fs_block_group_descriptor);
	dblk = (fs->m_first_data_block + 1 + block) <<
	    fs->m_fs_block_to_disk_block;
	error = bread(devvp, dblk, fs->m_block_size, &bp);
	if (error) {
		if (bp != NULL)
			brelse(bp);
		return (error);
	}
	gd = &fs->m_gd[group];
	gd->bgd_checksum = htole16(ext4fs_bgd_csum(fs, gd, group));
	memcpy((char *)bp->b_data + offset, gd, sizeof(*gd));
	return (bwrite(bp));
}

static int
ext4fs_orphan_count_dirs (struct m_ext4fs *fs, struct vnode *devvp,
    u_int32_t group, const u_int8_t *bitmap, u_int32_t valid,
    u_int32_t *dirsp)
{
	struct ext4fs_block_group_descriptor *gd;
	struct ext4fs_dinode *din;
	struct buf *bp = NULL;
	u_int64_t table;
	u_int32_t base, block, i, slots;
	int error;

	gd = &fs->m_gd[group];
	table = ext4fs_orphan_bgd_block(fs, gd, 2);
	*dirsp = 0;
	base = 0;
	for (block = 0; base < valid; block++, base += slots) {
		slots = fs->m_inodes_per_block;
		if (slots > valid - base)
			slots = valid - base;
		error = bread(devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, table + block),
		    fs->m_block_size, &bp);
		if (error) {
			if (bp != NULL)
				brelse(bp);
			return (error);
		}
		for (i = 0; i < slots; i++) {
			if (isclr(bitmap, base + i))
				continue;
			din = (struct ext4fs_dinode *)((char *)bp->b_data +
			    (size_t)i * fs->m_inode_size);
			if ((letoh16(din->i_mode) & S_IFMT) == S_IFDIR)
				(*dirsp)++;
		}
		brelse(bp);
		bp = NULL;
	}
	return (0);
}

/*
 * Rebuild counters and bitmap checksums from the durable bitmaps.  Recovery
 * calls this before dropping an orphan reference, so a crash during the
 * recount merely causes the still-reachable orphan to be retried.
 */
static int
ext4fs_orphan_recount (struct mount *mp, int64_t dir_group)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp = NULL;
	u_int64_t free_blocks, group_start, bitmap_block;
	u_int64_t inode_start, free_inodes64;
	u_int32_t group, valid, free_count, checksum, i, dirs;
	u_int16_t flags;
	int error;

	if (fs->m_block_group_count > UINT32_MAX)
		return (EFBIG);
	free_blocks = 0;
	free_inodes64 = 0;
	for (group = 0; group < fs->m_block_group_count; group++) {
		gd = &fs->m_gd[group];
		flags = letoh16(gd->bgd_flags);

		group_start = fs->m_first_data_block +
		    (u_int64_t)group * fs->m_blocks_per_group;
		valid = 0;
		if (group_start < fs->m_blocks_count) {
			if (fs->m_blocks_count - group_start <
			    fs->m_blocks_per_group)
				valid = fs->m_blocks_count - group_start;
			else
				valid = fs->m_blocks_per_group;
		}
		if (flags & EXT4FS_BGD_FLAG_BLOCK_UNINIT) {
			free_count = letoh16(gd->bgd_free_blocks_count_lo);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				free_count |= (u_int32_t)letoh16(
				    gd->bgd_free_blocks_count_hi) << 16;
		} else {
			bitmap_block = ext4fs_orphan_bgd_block(fs, gd, 0);
			error = bread(ump->um_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, bitmap_block),
			    fs->m_block_size, &bp);
			if (error) {
				if (bp != NULL)
					brelse(bp);
				return (error);
			}
			free_count = 0;
			for (i = 0; i < valid; i++)
				if (isclr((u_int8_t *)bp->b_data, i))
					free_count++;
			checksum = ext4fs_bitmap_csum(fs, group,
			    bp->b_data, fs->m_block_size);
			gd->bgd_block_bitmap_checksum_lo =
			    htole16(checksum & 0xffff);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				gd->bgd_block_bitmap_checksum_hi =
				    htole16(checksum >> 16);
			brelse(bp);
			bp = NULL;
			gd->bgd_free_blocks_count_lo =
			    htole16(free_count & 0xffff);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				gd->bgd_free_blocks_count_hi =
				    htole16(free_count >> 16);
		}
		free_blocks += free_count;

		inode_start = (u_int64_t)group * fs->m_inodes_per_group;
		valid = 0;
		if (inode_start < fs->m_inodes_count) {
			if (fs->m_inodes_count - inode_start <
			    fs->m_inodes_per_group)
				valid = fs->m_inodes_count - inode_start;
			else
				valid = fs->m_inodes_per_group;
		}
		if (flags & EXT4FS_BGD_FLAG_INODE_UNINIT) {
			if ((int64_t)group == dir_group)
				return (EINVAL);
			free_count = letoh16(gd->bgd_free_inodes_count_lo);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				free_count |= (u_int32_t)letoh16(
				    gd->bgd_free_inodes_count_hi) << 16;
		} else {
			bitmap_block = ext4fs_orphan_bgd_block(fs, gd, 1);
			error = bread(ump->um_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, bitmap_block),
			    fs->m_block_size, &bp);
			if (error) {
				if (bp != NULL)
					brelse(bp);
				return (error);
			}
			free_count = 0;
			for (i = 0; i < valid; i++)
				if (isclr((u_int8_t *)bp->b_data, i))
					free_count++;
			checksum = ext4fs_bitmap_csum(fs, group,
			    bp->b_data,
			    howmany(fs->m_inodes_per_group, NBBY));
			gd->bgd_inode_bitmap_checksum_lo =
			    htole16(checksum & 0xffff);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				gd->bgd_inode_bitmap_checksum_hi =
				    htole16(checksum >> 16);
			if ((int64_t)group == dir_group) {
				error = ext4fs_orphan_count_dirs(fs,
				    ump->um_devvp, group, bp->b_data,
				    valid, &dirs);
				if (error) {
					brelse(bp);
					return (error);
				}
				gd->bgd_used_dirs_count_lo =
				    htole16(dirs & 0xffff);
				if (fs->m_feature_incompat &
				    EXT4FS_FEATURE_INCOMPAT_64BIT)
					gd->bgd_used_dirs_count_hi =
					    htole16(dirs >> 16);
			}
			brelse(bp);
			bp = NULL;
			gd->bgd_free_inodes_count_lo =
			    htole16(free_count & 0xffff);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				gd->bgd_free_inodes_count_hi =
				    htole16(free_count >> 16);
		}
		free_inodes64 += free_count;

		error = ext4fs_orphan_bgd_write(fs, ump->um_devvp, group);
		if (error)
			return (error);
	}
	if (free_inodes64 > UINT32_MAX)
		return (EOVERFLOW);
	fs->m_free_blocks_count = free_blocks;
	fs->m_free_inodes_count = free_inodes64;
	fs->m_fs_was_modified = 1;
	error = ext4fs_sbwrite(mp);
	if (error)
		return (error);
	return (ext4fs_orphan_flush(mp));
}

static int
ext4fs_orphan_bitmap_clear (struct m_ext4fs *fs, struct vnode *devvp,
    u_int64_t block, int inode)
{
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp = NULL;
	u_int64_t bitmap_block;
	u_int32_t group, bit;
	u_int16_t flag;
	int error;

	if (inode) {
		if (block == 0 || block > fs->m_inodes_count)
			return (EINVAL);
		group = (block - 1) / fs->m_inodes_per_group;
		bit = (block - 1) % fs->m_inodes_per_group;
		flag = EXT4FS_BGD_FLAG_INODE_UNINIT;
	} else {
		if (!ext4fs_orphan_data_block_valid(fs, block))
			return (EINVAL);
		group = (block - fs->m_first_data_block) /
		    fs->m_blocks_per_group;
		bit = (block - fs->m_first_data_block) %
		    fs->m_blocks_per_group;
		flag = EXT4FS_BGD_FLAG_BLOCK_UNINIT;
	}
	gd = &fs->m_gd[group];
	if (letoh16(gd->bgd_flags) & flag)
		return (EINVAL);
	bitmap_block = ext4fs_orphan_bgd_block(fs, gd, inode ? 1 : 0);
	error = bread(devvp, (daddr_t)EXT4FS_FSBTODB(fs, bitmap_block),
	    fs->m_block_size, &bp);
	if (error) {
		if (bp != NULL)
			brelse(bp);
		return (error);
	}
	clrbit((u_int8_t *)bp->b_data, bit);
	return (bwrite(bp));
}

static u_int64_t
ext4fs_orphan_xattr_block (struct m_ext4fs *fs, struct ext4fs_dinode *din)
{
	u_int64_t block;

	block = letoh32(din->i_extended_attributes_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		block |= (u_int64_t)
		    letoh16(din->i_extended_attributes_hi) << 32;
	return (block);
}

static u_int32_t
ext4fs_orphan_xattr_csum (struct m_ext4fs *fs, u_int64_t block,
    void *data)
{
	struct ext4fs_orphan_xattr_header *header = data;
	u_int64_t block_le;
	u_int32_t checksum, crc;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return (0);
	checksum = header->h_checksum;
	header->h_checksum = 0;
	block_le = htole64(block);
	crc = crc32c(ext4fs_csum_seed(fs), (const u_int8_t *)&block_le,
	    sizeof(block_le));
	crc = crc32c(crc, data, fs->m_block_size);
	header->h_checksum = checksum;
	return (~crc);
}

static int
ext4fs_orphan_xattr_read (struct ext4fs_orphan_extent_ctx *ctx,
    struct ext4fs_dinode *din, u_int64_t *blockp, struct buf **bpp)
{
	struct ext4fs_orphan_xattr_header *header;
	u_int32_t calculated;
	int error, i;

	*bpp = NULL;
	*blockp = ext4fs_orphan_xattr_block(ctx->fs, din);
	if (*blockp == 0)
		return (0);
	if (!ext4fs_orphan_data_block_valid(ctx->fs, *blockp))
		return (EINVAL);
	error = bread(ctx->devvp,
	    (daddr_t)EXT4FS_FSBTODB(ctx->fs, *blockp),
	    ctx->fs->m_block_size, bpp);
	if (error) {
		if (*bpp != NULL)
			brelse(*bpp);
		*bpp = NULL;
		return (error);
	}
	header = (struct ext4fs_orphan_xattr_header *)(*bpp)->b_data;
	if (letoh32(header->h_magic) != EXT4FS_ORPHAN_XATTR_MAGIC ||
	    letoh32(header->h_refcount) == 0 ||
	    letoh32(header->h_blocks) != 1) {
		error = EINVAL;
		goto bad;
	}
	for (i = 0; i < nitems(header->h_reserved); i++) {
		if (header->h_reserved[i] != 0) {
			error = EINVAL;
			goto bad;
		}
	}
	if (ctx->fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM) {
		calculated = ext4fs_orphan_xattr_csum(ctx->fs, *blockp,
		    (*bpp)->b_data);
		if (letoh32(header->h_checksum) != calculated) {
			error = EINVAL;
			goto bad;
		}
	}
	return (0);
bad:
	brelse(*bpp);
	*bpp = NULL;
	return (error);
}

/* Count live inode references instead of decrementing a possibly replayed
 * reference count.  This makes an xattr release idempotent after a crash. */
static int
ext4fs_orphan_xattr_references (struct ext4fs_orphan_extent_ctx *ctx,
    u_int64_t xattr_block, u_int32_t *referencesp)
{
	struct ext4fs_block_group_descriptor *gd;
	struct ext4fs_dinode_256 dp;
	struct ext4fs_dinode *din;
	struct buf *bitmap_bp = NULL, *table_bp = NULL;
	u_int64_t inode_start, table;
	u_int32_t base, group, i, ino, slots, valid;
	int error;

	if (ctx->fs->m_block_group_count > UINT32_MAX)
		return (EFBIG);
	*referencesp = 0;
	for (group = 0; group < ctx->fs->m_block_group_count; group++) {
		gd = &ctx->fs->m_gd[group];
		if (letoh16(gd->bgd_flags) & EXT4FS_BGD_FLAG_INODE_UNINIT)
			continue;
		inode_start = (u_int64_t)group *
		    ctx->fs->m_inodes_per_group;
		if (inode_start >= ctx->fs->m_inodes_count)
			continue;
		if (ctx->fs->m_inodes_count - inode_start <
		    ctx->fs->m_inodes_per_group)
			valid = ctx->fs->m_inodes_count - inode_start;
		else
			valid = ctx->fs->m_inodes_per_group;
		error = bread(ctx->devvp,
		    (daddr_t)EXT4FS_FSBTODB(ctx->fs,
		    ext4fs_orphan_bgd_block(ctx->fs, gd, 1)),
		    ctx->fs->m_block_size, &bitmap_bp);
		if (error) {
			if (bitmap_bp != NULL)
				brelse(bitmap_bp);
			return (error);
		}
		table = ext4fs_orphan_bgd_block(ctx->fs, gd, 2);
		base = 0;
		while (base < valid) {
			slots = ctx->fs->m_inodes_per_block;
			if (slots > valid - base)
				slots = valid - base;
			error = bread(ctx->devvp,
			    (daddr_t)EXT4FS_FSBTODB(ctx->fs,
			    table + base / ctx->fs->m_inodes_per_block),
			    ctx->fs->m_block_size, &table_bp);
			if (error) {
				if (table_bp != NULL)
					brelse(table_bp);
				brelse(bitmap_bp);
				return (error);
			}
			for (i = 0; i < slots; i++) {
				if (isclr((u_int8_t *)bitmap_bp->b_data,
				    base + i))
					continue;
				ino = inode_start + base + i + 1;
				if (ino == ctx->ino)
					continue;
				din = (struct ext4fs_dinode *)
				    ((char *)table_bp->b_data +
				    (size_t)i * ctx->fs->m_inode_size);
				memcpy(&dp, din, sizeof(dp));
				error = ext4fs_inode_csum_verify(ctx->fs,
				    &dp, ino);
				if (error) {
					brelse(table_bp);
					brelse(bitmap_bp);
					return (error);
				}
				if (ext4fs_orphan_xattr_block(ctx->fs,
				    &dp.dinode) == xattr_block)
					(*referencesp)++;
			}
			brelse(table_bp);
			table_bp = NULL;
			base += slots;
		}
		brelse(bitmap_bp);
		bitmap_bp = NULL;
	}
	return (0);
}

static int
ext4fs_orphan_xattr_release (struct mount *mp,
    struct ext4fs_orphan_extent_ctx *ctx, struct ext4fs_dinode *din)
{
	struct ext4fs_orphan_xattr_header *header;
	struct buf *bp;
	u_int64_t block;
	u_int32_t references;
	int error;

	error = ext4fs_orphan_xattr_read(ctx, din, &block, &bp);
	if (error || block == 0)
		return (error);
	error = ext4fs_orphan_xattr_references(ctx, block, &references);
	if (error) {
		brelse(bp);
		return (error);
	}
	if (references == 0) {
		brelse(bp);
		error = ext4fs_orphan_bitmap_clear(ctx->fs, ctx->devvp,
		    block, 0);
		if (error)
			return (error);
		error = ext4fs_orphan_recount(mp, -1);
		if (error)
			return (error);
	} else {
		header = (struct ext4fs_orphan_xattr_header *)bp->b_data;
		header->h_refcount = htole32(references);
		header->h_checksum = 0;
		header->h_checksum = htole32(ext4fs_orphan_xattr_csum(
		    ctx->fs, block, bp->b_data));
		error = bwrite(bp);
		if (error)
			return (error);
	}
	din->i_extended_attributes_lo = 0;
	din->i_extended_attributes_hi = 0;
	return (0);
}

static int
ext4fs_orphan_extent_csum_verify (struct ext4fs_orphan_extent_ctx *ctx,
    void *data)
{
	struct ext4fs_extent_header *eh = data;
	u_int32_t *tail, crc, ino_le, provided;
	size_t offset;

	if (!(ctx->fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return (0);
	offset = sizeof(*eh) + (size_t)letoh16(eh->eh_max) *
	    sizeof(struct ext4fs_extent);
	if (offset > ctx->fs->m_block_size - sizeof(*tail))
		return (EINVAL);
	tail = (u_int32_t *)((char *)data + offset);
	provided = letoh32(*tail);
	ino_le = htole32(ctx->ino);
	crc = crc32c(ext4fs_csum_seed(ctx->fs),
	    (const u_int8_t *)&ino_le, sizeof(ino_le));
	crc = crc32c(crc, (const u_int8_t *)&ctx->generation,
	    sizeof(ctx->generation));
	crc = crc32c(crc, data, offset);
	if (provided != ~crc)
		return (EINVAL);
	return (0);
}

static int
ext4fs_orphan_extent_header_verify (struct ext4fs_orphan_extent_ctx *ctx,
    struct ext4fs_extent_header *eh, u_int16_t depth, int root)
{
	u_int16_t entries, maximum, limit;

	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC ||
	    letoh16(eh->eh_depth) != depth ||
	    depth > EXT4FS_EXTENT_DEPTH_MAX)
		return (EINVAL);
	entries = letoh16(eh->eh_entries);
	maximum = letoh16(eh->eh_max);
	limit = root ? 4 :
	    (ctx->fs->m_block_size - sizeof(*eh)) /
	    sizeof(struct ext4fs_extent);
	if (maximum == 0 || maximum > limit || entries > maximum)
		return (EINVAL);
	if (depth != 0 && entries == 0)
		return (EINVAL);
	return (0);
}

static int
ext4fs_orphan_extent_block_read (struct ext4fs_orphan_extent_ctx *ctx,
    u_int64_t block, u_int16_t depth, struct buf **bpp)
{
	struct ext4fs_extent_header *eh;
	int error;

	*bpp = NULL;
	if (!ext4fs_orphan_data_block_valid(ctx->fs, block))
		return (EINVAL);
	error = bread(ctx->devvp,
	    (daddr_t)EXT4FS_FSBTODB(ctx->fs, block),
	    ctx->fs->m_block_size, bpp);
	if (error) {
		if (*bpp != NULL)
			brelse(*bpp);
		*bpp = NULL;
		return (error);
	}
	eh = (struct ext4fs_extent_header *)(*bpp)->b_data;
	error = ext4fs_orphan_extent_header_verify(ctx, eh, depth, 0);
	if (error == 0)
		error = ext4fs_orphan_extent_csum_verify(ctx, eh);
	if (error) {
		brelse(*bpp);
		*bpp = NULL;
	}
	return (error);
}

static int
ext4fs_orphan_extent_validate_node (struct ext4fs_orphan_extent_ctx *ctx,
    struct ext4fs_extent_header *eh, u_int16_t depth, int root)
{
	struct ext4fs_extent_idx *idx;
	struct ext4fs_extent *ext;
	struct buf *bp;
	u_int64_t child, end, physical, previous;
	u_int32_t logical;
	u_int16_t entries, length;
	int error, i;

	error = ext4fs_orphan_extent_header_verify(ctx, eh, depth, root);
	if (error)
		return (error);
	if (!root) {
		if (ctx->nodes_left == 0)
			return (EFBIG);
		ctx->nodes_left--;
	}
	entries = letoh16(eh->eh_entries);
	previous = 0;
	if (depth == 0) {
		ext = (struct ext4fs_extent *)(eh + 1);
		for (i = 0; i < entries; i++) {
			logical = letoh32(ext[i].e_block);
			length = letoh16(ext[i].e_len);
			if (length > 32768)
				length -= 32768;
			if (length == 0 ||
			    (i != 0 && logical < previous))
				return (EINVAL);
			end = (u_int64_t)logical + length;
			if (end > (u_int64_t)UINT32_MAX + 1)
				return (EINVAL);
			physical = letoh32(ext[i].e_start_lo) |
			    (u_int64_t)letoh16(ext[i].e_start_hi) << 32;
			if (physical >= ctx->fs->m_blocks_count ||
			    length > ctx->fs->m_blocks_count - physical)
				return (EINVAL);
			for (child = physical; child < physical + length;
			    child++)
				if (!ext4fs_orphan_data_block_valid(ctx->fs,
				    child))
					return (EINVAL);
			previous = end;
		}
		return (0);
	}

	idx = (struct ext4fs_extent_idx *)(eh + 1);
	for (i = 0; i < entries; i++) {
		logical = letoh32(idx[i].ei_block);
		if ((i != 0 && logical <= previous) ||
		    letoh16(idx[i].ei_unused) != 0)
			return (EINVAL);
		previous = logical;
		child = letoh32(idx[i].ei_leaf_lo) |
		    (u_int64_t)letoh16(idx[i].ei_leaf_hi) << 32;
		error = ext4fs_orphan_extent_block_read(ctx, child,
		    depth - 1, &bp);
		if (error)
			return (error);
		error = ext4fs_orphan_extent_validate_node(ctx,
		    (struct ext4fs_extent_header *)bp->b_data,
		    depth - 1, 0);
		brelse(bp);
		if (error)
			return (error);
	}
	return (0);
}

static int
ext4fs_orphan_extent_validate (struct ext4fs_orphan_extent_ctx *ctx,
    struct ext4fs_dinode *din)
{
	struct ext4fs_extent_header *eh = &din->i_extent_header;

	ctx->nodes_left = ctx->fs->m_blocks_count;
	return (ext4fs_orphan_extent_validate_node(ctx, eh,
	    letoh16(eh->eh_depth), 1));
}

static int
ext4fs_orphan_extent_lookup (struct ext4fs_orphan_extent_ctx *ctx,
    struct ext4fs_dinode *din, u_int32_t logical, u_int64_t *physicalp)
{
	struct ext4fs_extent_header *eh = &din->i_extent_header;
	struct ext4fs_extent_idx *idx;
	struct ext4fs_extent *ext;
	struct buf *bp = NULL, *nextbp;
	u_int64_t child, start;
	u_int32_t first;
	u_int16_t depth, entries, length;
	int error, found, i;

	depth = letoh16(eh->eh_depth);
	while (depth != 0) {
		entries = letoh16(eh->eh_entries);
		idx = (struct ext4fs_extent_idx *)(eh + 1);
		found = -1;
		for (i = 0; i < entries; i++) {
			if (letoh32(idx[i].ei_block) <= logical)
				found = i;
			else
				break;
		}
		if (found < 0) {
			error = 0;
			goto hole;
		}
		child = letoh32(idx[found].ei_leaf_lo) |
		    (u_int64_t)letoh16(idx[found].ei_leaf_hi) << 32;
		error = ext4fs_orphan_extent_block_read(ctx, child,
		    depth - 1, &nextbp);
		if (error)
			goto out;
		if (bp != NULL)
			brelse(bp);
		bp = nextbp;
		eh = (struct ext4fs_extent_header *)bp->b_data;
		depth--;
	}
	ext = (struct ext4fs_extent *)(eh + 1);
	entries = letoh16(eh->eh_entries);
	for (i = 0; i < entries; i++) {
		first = letoh32(ext[i].e_block);
		length = letoh16(ext[i].e_len);
		if (length > 32768)
			length -= 32768;
		if ((u_int64_t)logical >= first &&
		    (u_int64_t)logical < (u_int64_t)first + length) {
			start = letoh32(ext[i].e_start_lo) |
			    (u_int64_t)letoh16(ext[i].e_start_hi) << 32;
			*physicalp = start + logical - first;
			if (bp != NULL)
				brelse(bp);
			return (0);
		}
	}
	error = 0;
hole:
	*physicalp = 0;
out:
	if (bp != NULL)
		brelse(bp);
	return (error);
}

static int
ext4fs_orphan_extent_clear_data_node (
    struct ext4fs_orphan_extent_ctx *ctx, struct ext4fs_extent_header *eh,
    u_int16_t depth)
{
	struct ext4fs_extent_idx *idx;
	struct ext4fs_extent *ext;
	struct buf *bp;
	u_int64_t child, end, first, physical, start;
	u_int16_t entries, length;
	int error, i;

	entries = letoh16(eh->eh_entries);
	if (depth == 0) {
		ext = (struct ext4fs_extent *)(eh + 1);
		for (i = 0; i < entries; i++) {
			first = letoh32(ext[i].e_block);
			length = letoh16(ext[i].e_len);
			if (length > 32768)
				length -= 32768;
			end = first + length;
			if (end <= ctx->cutoff)
				continue;
			start = ctx->cutoff > first ? ctx->cutoff : first;
			physical = letoh32(ext[i].e_start_lo) |
			    (u_int64_t)letoh16(ext[i].e_start_hi) << 32;
			physical += start - first;
			while (start++ < end) {
				error = ext4fs_orphan_bitmap_clear(ctx->fs,
				    ctx->devvp, physical++, 0);
				if (error)
					return (error);
			}
		}
		return (0);
	}

	idx = (struct ext4fs_extent_idx *)(eh + 1);
	for (i = 0; i < entries; i++) {
		child = letoh32(idx[i].ei_leaf_lo) |
		    (u_int64_t)letoh16(idx[i].ei_leaf_hi) << 32;
		error = ext4fs_orphan_extent_block_read(ctx, child,
		    depth - 1, &bp);
		if (error)
			return (error);
		error = ext4fs_orphan_extent_clear_data_node(ctx,
		    (struct ext4fs_extent_header *)bp->b_data, depth - 1);
		brelse(bp);
		if (error)
			return (error);
	}
	return (0);
}

static int
ext4fs_orphan_extent_clear_tree_node (
    struct ext4fs_orphan_extent_ctx *ctx, struct ext4fs_extent_header *eh,
    u_int16_t depth)
{
	struct ext4fs_extent_idx *idx;
	struct buf *bp;
	u_int64_t child;
	u_int16_t entries;
	int error, i;

	if (depth == 0)
		return (0);
	entries = letoh16(eh->eh_entries);
	idx = (struct ext4fs_extent_idx *)(eh + 1);
	for (i = 0; i < entries; i++) {
		child = letoh32(idx[i].ei_leaf_lo) |
		    (u_int64_t)letoh16(idx[i].ei_leaf_hi) << 32;
		error = ext4fs_orphan_extent_block_read(ctx, child,
		    depth - 1, &bp);
		if (error)
			return (error);
		error = ext4fs_orphan_extent_clear_tree_node(ctx,
		    (struct ext4fs_extent_header *)bp->b_data, depth - 1);
		brelse(bp);
		if (error)
			return (error);
		error = ext4fs_orphan_bitmap_clear(ctx->fs, ctx->devvp,
		    child, 0);
		if (error)
			return (error);
	}
	return (0);
}

static int ext4fs_orphan_extent_prune_node (
    struct ext4fs_orphan_extent_ctx *, struct ext4fs_extent_header *,
    u_int16_t, int *, u_int64_t *);

static int
ext4fs_orphan_extent_prune_block (struct ext4fs_orphan_extent_ctx *ctx,
    u_int64_t block, u_int16_t depth, int *emptyp, u_int64_t *blocksp)
{
	struct ext4fs_extent_header *eh;
	struct buf *bp;
	int error;

	error = ext4fs_orphan_extent_block_read(ctx, block, depth, &bp);
	if (error)
		return (error);
	eh = (struct ext4fs_extent_header *)bp->b_data;
	error = ext4fs_orphan_extent_prune_node(ctx, eh, depth, emptyp,
	    blocksp);
	if (error) {
		brelse(bp);
		return (error);
	}
	if (*emptyp) {
		brelse(bp);
		return (0);
	}
	ext4fs_extent_block_csum_set(ctx->fs, ctx->ino, ctx->generation,
	    bp->b_data);
	error = bwrite(bp);
	if (error == 0)
		(*blocksp)++;
	return (error);
}

static int
ext4fs_orphan_extent_prune_node (struct ext4fs_orphan_extent_ctx *ctx,
    struct ext4fs_extent_header *eh, u_int16_t depth, int *emptyp,
    u_int64_t *blocksp)
{
	struct ext4fs_extent_idx *idx;
	struct ext4fs_extent *ext;
	u_int64_t child, child_blocks, end, first, kept;
	u_int16_t entries, length, raw_length, output;
	int child_empty, error, i;

	entries = letoh16(eh->eh_entries);
	output = 0;
	*blocksp = 0;
	if (depth == 0) {
		ext = (struct ext4fs_extent *)(eh + 1);
		for (i = 0; i < entries; i++) {
			first = letoh32(ext[i].e_block);
			raw_length = letoh16(ext[i].e_len);
			length = raw_length;
			if (length > 32768)
				length -= 32768;
			end = first + length;
			if (first >= ctx->cutoff)
				continue;
			kept = end > ctx->cutoff ? ctx->cutoff - first :
			    length;
			if (kept == 0 || kept > 32768)
				return (EINVAL);
			if (output != i)
				ext[output] = ext[i];
			if (kept != length) {
				if (raw_length > 32768)
					ext[output].e_len = htole16(kept + 32768);
				else
					ext[output].e_len = htole16(kept);
			}
			*blocksp += kept;
			output++;
		}
		if (output < entries)
			memset(&ext[output], 0,
			    (entries - output) * sizeof(*ext));
		eh->eh_entries = htole16(output);
		*emptyp = output == 0;
		return (0);
	}

	idx = (struct ext4fs_extent_idx *)(eh + 1);
	for (i = 0; i < entries; i++) {
		child = letoh32(idx[i].ei_leaf_lo) |
		    (u_int64_t)letoh16(idx[i].ei_leaf_hi) << 32;
		error = ext4fs_orphan_extent_prune_block(ctx, child,
		    depth - 1, &child_empty, &child_blocks);
		if (error)
			return (error);
		if (child_empty) {
			error = ext4fs_orphan_bitmap_clear(ctx->fs,
			    ctx->devvp, child, 0);
			if (error)
				return (error);
			continue;
		}
		if (output != i)
			idx[output] = idx[i];
		output++;
		*blocksp += child_blocks;
	}
	if (output < entries)
		memset(&idx[output], 0,
		    (entries - output) * sizeof(*idx));
	eh->eh_entries = htole16(output);
	*emptyp = output == 0;
	return (0);
}

static int
ext4fs_orphan_inode_blocks_set (struct ext4fs_orphan_extent_ctx *ctx,
    struct ext4fs_dinode *din, u_int64_t blocks)
{
	u_int64_t units;

	if (ext4fs_orphan_xattr_block(ctx->fs, din) != 0)
		blocks++;
	units = blocks;
	if (!(letoh32(din->i_flags) & EXTFS_INODE_FLAG_HUGE_FILE)) {
		if (units > UINT64_MAX / (ctx->fs->m_block_size / DEV_BSIZE))
			return (EOVERFLOW);
		units *= ctx->fs->m_block_size / DEV_BSIZE;
	}
	if (units >> 48)
		return (EOVERFLOW);
	din->i_blocks_lo = htole32(units & 0xffffffff);
	din->i_blocks_hi = htole16(units >> 32);
	return (0);
}

static int
ext4fs_orphan_inode_cleanup (struct mount *mp, u_int32_t ino,
    struct ext4fs_dinode_256 *dp, int *count)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	struct ext4fs_dinode *din = &dp->dinode;
	struct ext4fs_extent_header *eh = &din->i_extent_header;
	struct ext4fs_orphan_extent_ctx ctx;
	struct buf *bp = NULL;
	u_int64_t blocks, cutoff, physical, size;
	u_int32_t flags, group;
	u_int16_t mode, nlink;
	int empty, error, has_extents;

	mode = letoh16(din->i_mode);
	nlink = letoh16(din->i_links_count);
	flags = letoh32(din->i_flags);
	has_extents = (flags & EXTFS_INODE_FLAG_EXTENTS) != 0;
	size = letoh32(din->i_size_lo) |
	    (u_int64_t)letoh32(din->i_size_hi) << 32;
	if (size / fs->m_block_size > UINT32_MAX)
		return (EFBIG);
	cutoff = size / fs->m_block_size;
	if (size % fs->m_block_size != 0)
		cutoff++;

	memset(&ctx, 0, sizeof(ctx));
	ctx.fs = fs;
	ctx.devvp = ump->um_devvp;
	ctx.ino = ino;
	ctx.generation = din->i_nfs_generation;
	ctx.cutoff = nlink == 0 ? 0 : cutoff;

	if (!has_extents) {
		blocks = letoh32(din->i_blocks_lo) |
		    (u_int64_t)letoh16(din->i_blocks_hi) << 32;
		if (blocks != 0)
			return (EOPNOTSUPP);
	} else {
		error = ext4fs_orphan_extent_validate(&ctx, din);
		if (error)
			return (error);
	}

	if (nlink == 0 && ext4fs_orphan_xattr_block(fs, din) != 0) {
		u_int64_t xattr_block;

		error = ext4fs_orphan_xattr_read(&ctx, din, &xattr_block, &bp);
		if (error)
			return (error);
		brelse(bp);
		bp = NULL;
	}

	if (nlink != 0 && size % fs->m_block_size != 0 && has_extents) {
		error = ext4fs_orphan_extent_lookup(&ctx, din,
		    cutoff - 1, &physical);
		if (error)
			return (error);
		if (physical != 0) {
			error = bread(ump->um_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, physical),
			    fs->m_block_size, &bp);
			if (error) {
				if (bp != NULL)
					brelse(bp);
				return (error);
			}
			memset((char *)bp->b_data + size % fs->m_block_size, 0,
			    fs->m_block_size - size % fs->m_block_size);
			error = bwrite(bp);
			bp = NULL;
			if (error)
				return (error);
		}
	}

	if (has_extents) {
		error = ext4fs_orphan_extent_clear_data_node(&ctx, eh,
		    letoh16(eh->eh_depth));
		if (error)
			return (error);
		if (nlink == 0) {
			error = ext4fs_orphan_extent_clear_tree_node(&ctx, eh,
			    letoh16(eh->eh_depth));
			if (error)
				return (error);
			error = ext4fs_orphan_recount(mp, -1);
			if (error)
				return (error);
			memset(din->i_extent, 0,
			    sizeof(din->i_extent));
			eh->eh_magic = htole16(EXT4FS_EXTENT_HEADER_MAGIC);
			eh->eh_entries = htole16(0);
			eh->eh_max = htole16(4);
			eh->eh_depth = htole16(0);
			eh->eh_generation = 0;
			blocks = 0;
		} else {
			error = ext4fs_orphan_recount(mp, -1);
			if (error)
				return (error);
			error = ext4fs_orphan_extent_prune_node(&ctx, eh,
			    letoh16(eh->eh_depth), &empty, &blocks);
			if (error)
				return (error);
			if (empty) {
				memset(din->i_extent, 0,
				    sizeof(din->i_extent));
				eh->eh_magic =
				    htole16(EXT4FS_EXTENT_HEADER_MAGIC);
				eh->eh_entries = htole16(0);
				eh->eh_max = htole16(4);
				eh->eh_depth = htole16(0);
				eh->eh_generation = 0;
			}
		}
	} else {
		blocks = 0;
	}

	if (nlink == 0) {
		din->i_size_lo = 0;
		din->i_size_hi = 0;
	} else {
		din->i_dtime = 0;
	}
	error = ext4fs_orphan_inode_blocks_set(&ctx, din, blocks);
	if (error)
		return (error);
	error = ext4fs_orphan_inode_write(fs, ump->um_devvp, ino, dp);
	if (error)
		return (error);

	/* Persist index-block bitmap changes made while pruning. */
	error = ext4fs_orphan_recount(mp, -1);
	if (error)
		return (error);
	if (nlink == 0) {
		error = ext4fs_orphan_bitmap_clear(fs, ump->um_devvp, ino, 1);
		if (error)
			return (error);
		group = (ino - 1) / fs->m_inodes_per_group;
		error = ext4fs_orphan_recount(mp,
		    (mode & S_IFMT) == S_IFDIR ? group : -1);
		if (error)
			return (error);
		error = ext4fs_orphan_xattr_release(mp, &ctx, din);
		if (error)
			return (error);
		/* Counters are durable; the freed inode may now be retired. */
		din->i_mode = 0;
		error = ext4fs_orphan_inode_blocks_set(&ctx, din, 0);
		if (error)
			return (error);
		error = ext4fs_orphan_inode_write(fs, ump->um_devvp, ino, dp);
		if (error)
			return (error);
	}
	error = ext4fs_orphan_flush(mp);
	if (error)
		return (error);
	(*count)++;
	return (0);
}

static u_int32_t
ext4fs_orphan_file_seed (struct m_ext4fs *fs, u_int32_t ino,
    u_int32_t generation)
{
	u_int32_t crc, ino_le;

	ino_le = htole32(ino);
	crc = crc32c(ext4fs_csum_seed(fs), (const u_int8_t *)&ino_le,
	    sizeof(ino_le));
	return (crc32c(crc, (const u_int8_t *)&generation,
	    sizeof(generation)));
}

static u_int32_t
ext4fs_orphan_file_block_csum (struct m_ext4fs *fs, u_int32_t seed,
    u_int64_t physical, void *data)
{
	u_int64_t physical_le;
	u_int32_t crc;

	physical_le = htole64(physical);
	crc = crc32c(seed, (const u_int8_t *)&physical_le,
	    sizeof(physical_le));
	crc = crc32c(crc, data,
	    fs->m_block_size - sizeof(struct ext4fs_orphan_block_tail));
	return (~crc);
}

static int
ext4fs_orphan_file_block_read (struct ext4fs_orphan_extent_ctx *ctx,
    struct ext4fs_dinode *din, u_int32_t logical, u_int32_t seed,
    u_int64_t *physicalp, struct buf **bpp)
{
	struct ext4fs_orphan_block_tail *tail;
	u_int32_t calculated;
	int error;

	*bpp = NULL;
	error = ext4fs_orphan_extent_lookup(ctx, din, logical, physicalp);
	if (error)
		return (error);
	if (*physicalp == 0)
		return (EINVAL);
	error = bread(ctx->devvp,
	    (daddr_t)EXT4FS_FSBTODB(ctx->fs, *physicalp),
	    ctx->fs->m_block_size, bpp);
	if (error) {
		if (*bpp != NULL)
			brelse(*bpp);
		*bpp = NULL;
		return (error);
	}
	tail = (struct ext4fs_orphan_block_tail *)
	    ((char *)(*bpp)->b_data + ctx->fs->m_block_size -
	    sizeof(*tail));
	calculated = ext4fs_orphan_file_block_csum(ctx->fs, seed,
	    *physicalp, (*bpp)->b_data);
	if (letoh32(tail->ob_magic) != EXT4FS_ORPHAN_BLOCK_TAIL_MAGIC ||
	    letoh32(tail->ob_checksum) != calculated) {
		brelse(*bpp);
		*bpp = NULL;
		return (EINVAL);
	}
	return (0);
}

static int
ext4fs_orphan_file_scan (struct mount *mp, int *count)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	struct ext4fs_dinode_256 odp, dp;
	struct ext4fs_dinode *odin = &odp.dinode;
	struct ext4fs_orphan_extent_ctx ctx;
	struct ext4fs_orphan_block_tail *tail;
	struct buf *bp;
	u_int64_t physical, size;
	u_int32_t block, entry, entries, ino, nblocks, seed;
	int error;

	if (!(fs->m_feature_compat & EXT4FS_FEATURE_COMPAT_ORPHAN_FILE) ||
	    fs->m_orphan_file_inode == 0)
		return (EINVAL);
	error = ext4fs_orphan_inode_read(fs, ump->um_devvp,
	    fs->m_orphan_file_inode, &odp);
	if (error)
		return (error);
	if ((letoh16(odin->i_mode) & S_IFMT) != S_IFREG ||
	    !(letoh32(odin->i_flags) & EXTFS_INODE_FLAG_EXTENTS))
		return (EINVAL);
	size = letoh32(odin->i_size_lo) |
	    (u_int64_t)letoh32(odin->i_size_hi) << 32;
	if (size == 0 || size % fs->m_block_size != 0 ||
	    size / fs->m_block_size > UINT32_MAX)
		return (EINVAL);
	nblocks = size / fs->m_block_size;
	entries = (fs->m_block_size -
	    sizeof(struct ext4fs_orphan_block_tail)) / sizeof(u_int32_t);
	if (entries == 0)
		return (EINVAL);

	memset(&ctx, 0, sizeof(ctx));
	ctx.fs = fs;
	ctx.devvp = ump->um_devvp;
	ctx.ino = fs->m_orphan_file_inode;
	ctx.generation = odin->i_nfs_generation;
	error = ext4fs_orphan_extent_validate(&ctx, odin);
	if (error)
		return (error);
	seed = ext4fs_orphan_file_seed(fs, ctx.ino, ctx.generation);

	/* Authenticate every block and inode before changing any entry. */
	for (block = 0; block < nblocks; block++) {
		error = ext4fs_orphan_file_block_read(&ctx, odin, block,
		    seed, &physical, &bp);
		if (error)
			return (error);
		for (entry = 0; entry < entries; entry++) {
			ino = letoh32(((u_int32_t *)bp->b_data)[entry]);
			if (ino == 0)
				continue;
			if (ino < fs->m_first_non_reserved_inode ||
			    ino > fs->m_inodes_count ||
			    ino == fs->m_orphan_file_inode ||
			    ino == fs->m_journal_inode_number) {
				brelse(bp);
				return (EINVAL);
			}
			error = ext4fs_orphan_inode_read(fs, ump->um_devvp,
			    ino, &dp);
			if (error) {
				brelse(bp);
				return (error);
			}
		}
		brelse(bp);
	}

	for (block = 0; block < nblocks; block++) {
		error = ext4fs_orphan_file_block_read(&ctx, odin, block,
		    seed, &physical, &bp);
		if (error)
			return (error);
		for (entry = 0; entry < entries; entry++) {
			ino = letoh32(((u_int32_t *)bp->b_data)[entry]);
			if (ino == 0)
				continue;
			brelse(bp);
			error = ext4fs_orphan_inode_read(fs, ump->um_devvp,
			    ino, &dp);
			if (error)
				return (error);
			error = ext4fs_orphan_inode_cleanup(mp, ino, &dp,
			    count);
			if (error)
				return (error);

			/* Re-read after cleanup so concurrent crash progress wins. */
			error = ext4fs_orphan_file_block_read(&ctx, odin,
			    block, seed, &physical, &bp);
			if (error)
				return (error);
			if (letoh32(((u_int32_t *)bp->b_data)[entry]) != ino) {
				brelse(bp);
				return (EINVAL);
			}
			((u_int32_t *)bp->b_data)[entry] = 0;
			tail = (struct ext4fs_orphan_block_tail *)
			    ((char *)bp->b_data + fs->m_block_size -
			    sizeof(*tail));
			tail->ob_checksum = htole32(
			    ext4fs_orphan_file_block_csum(fs, seed,
			    physical, bp->b_data));
			error = bwrite(bp);
			if (error)
				return (error);
			bp = NULL;
			if (entry + 1 < entries) {
				error = ext4fs_orphan_file_block_read(&ctx,
				    odin, block, seed, &physical, &bp);
				if (error)
					return (error);
			}
		}
		if (bp != NULL)
			brelse(bp);
	}
	return (ext4fs_orphan_flush(mp));
}

static int
ext4fs_orphan_classic_cleanup (struct mount *mp, int *count)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	struct ext4fs_dinode_256 dp;
	u_int32_t ino, next, previous, tail;
	u_int64_t seen;
	int error;

	while (fs->m_last_orphan != 0) {
		ino = fs->m_last_orphan;
		previous = 0;
		seen = 0;
		for (;;) {
			if (ino < fs->m_first_non_reserved_inode ||
			    ino > fs->m_inodes_count ||
			    ino == fs->m_orphan_file_inode ||
			    ino == fs->m_journal_inode_number ||
			    ++seen > fs->m_inodes_count) {
				printf("ext4fs: corrupt classic orphan list\n");
				return (EINVAL);
			}
			error = ext4fs_orphan_inode_read(fs, ump->um_devvp,
			    ino, &dp);
			if (error)
				return (error);
			next = letoh32(dp.dinode.i_dtime);
			if (next == 0)
				break;
			previous = ino;
			ino = next;
		}
		tail = ino;
		error = ext4fs_orphan_inode_cleanup(mp, tail, &dp, count);
		if (error)
			return (error);

		if (previous == 0) {
			fs->m_last_orphan = 0;
			fs->m_sble.sb_last_orphan = 0;
			fs->m_fs_was_modified = 1;
			error = ext4fs_sbwrite(mp);
		} else {
			error = ext4fs_orphan_inode_read(fs, ump->um_devvp,
			    previous, &dp);
			if (error)
				return (error);
			if (letoh32(dp.dinode.i_dtime) != tail)
				return (EINVAL);
			dp.dinode.i_dtime = 0;
			error = ext4fs_orphan_inode_write(fs, ump->um_devvp,
			    previous, &dp);
		}
		if (error)
			return (error);
		error = ext4fs_orphan_flush(mp);
		if (error)
			return (error);
	}
	return (0);
}

int
ext4fs_orphan_cleanup (struct mount *mp)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	u_int32_t ro;
	int count, error;

	if (fs->m_last_orphan == 0 &&
	    !(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT))
		return (0);
	if (fs->m_read_only)
		return (EROFS);

	printf("ext4fs: cleaning up orphan inodes\n");
	count = 0;
	error = ext4fs_orphan_classic_cleanup(mp, &count);
	if (error)
		return (error);
	if (fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT) {
		error = ext4fs_orphan_file_scan(mp, &count);
		if (error)
			return (error);
		ro = fs->m_feature_ro_compat &
		    ~EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT;
		fs->m_feature_ro_compat = ro;
		fs->m_sble.sb_feature_ro_compat = htole32(ro);
		fs->m_fs_was_modified = 1;
		error = ext4fs_sbwrite(mp);
		if (error)
			return (error);
		error = ext4fs_orphan_flush(mp);
		if (error)
			return (error);
	}
	printf("ext4fs: orphan cleanup: %d inode%s\n", count,
	    count == 1 ? "" : "s");
	return (0);
}
