/*
 * Copyright (c) 2025 kmx.io.
 * Copyright (c) 1997 Manuel Bouyer.
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Modified for ext4fs by kmx.io.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/pool.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/lockf.h>
#include <sys/specdev.h>
#include <sys/unistd.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/inode.h>
#include <ufs/ufs/dir.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_crc32c.h>

/* Convert ext4 directory entry file type to BSD dirent type */
static const u_int8_t ext4fs_type_to_dt[EXT4FS_FT_MAX] = {
	[EXT4FS_FT_UNKNOWN]	= DT_UNKNOWN,
	[EXT4FS_FT_REG_FILE]	= DT_REG,
	[EXT4FS_FT_DIR]		= DT_DIR,
	[EXT4FS_FT_CHRDEV]	= DT_CHR,
	[EXT4FS_FT_BLKDEV]	= DT_BLK,
	[EXT4FS_FT_FIFO]	= DT_FIFO,
	[EXT4FS_FT_SOCK]	= DT_SOCK,
	[EXT4FS_FT_SYMLINK]	= DT_LNK,
};

/*
 * Look up the physical block number for a given logical block number
 * using the extent tree in the inode.
 * Returns 0 on success with the physical block stored in *pblk.
 */
static int
ext4fs_extent_pblk(struct inode *ip, u_int64_t lbn, u_int64_t *pblk,
    u_int64_t *ncontig)
{
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_extent_header *eh;
	struct m_ext4fs *fs = ip->i_e4fs;
	struct buf *bp = NULL;
	u_int16_t entries, depth;
	int error, found, i;

	/* Start with the extent header in the inode */
	eh = &din->i_extent_header;
	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC)
		return (EIO);

	depth = letoh16(eh->eh_depth);
	entries = letoh16(eh->eh_entries);

	/* Walk down the extent tree */
	while (depth > 0) {
		struct ext4fs_extent_idx *idx;
		u_int64_t child_blk;

		/* Index node: find the child that covers lbn */
		idx = (struct ext4fs_extent_idx *)(eh + 1);
		found = -1;
		for (i = 0; i < (int)entries; i++) {
			if (letoh32(idx[i].ei_block) <= lbn)
				found = i;
			else
				break;
		}
		if (found < 0) {
			if (bp != NULL)
				brelse(bp);
			return (EIO);
		}

		/* Read the child node block */
		child_blk = letoh32(idx[found].ei_leaf_lo);
		child_blk |= (u_int64_t)letoh16(idx[found].ei_leaf_hi) << 32;

		if (bp != NULL)
			brelse(bp);

		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, child_blk),
		    fs->m_block_size, &bp);
		if (error) {
			if (bp != NULL)
				brelse(bp);
			return (error);
		}

		eh = (struct ext4fs_extent_header *)bp->b_data;
		if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC) {
			brelse(bp);
			return (EIO);
		}
		depth = letoh16(eh->eh_depth);
		entries = letoh16(eh->eh_entries);
	}

	/* Leaf node: search for the extent containing lbn */
	{
		struct ext4fs_extent *ext;
		ext = (struct ext4fs_extent *)(eh + 1);
		for (i = 0; i < (int)entries; i++) {
			u_int32_t e_block = letoh32(ext[i].e_block);
			u_int16_t e_len = letoh16(ext[i].e_len);

			/* High bit of e_len marks uninitialized extents */
			if (e_len > 32768)
				e_len -= 32768;

			if (lbn >= e_block && lbn < e_block + e_len) {
				u_int64_t start = letoh32(ext[i].e_start_lo);
				start |=
				    (u_int64_t)letoh16(ext[i].e_start_hi) << 32;
				*pblk = start + (lbn - e_block);
				if (ncontig != NULL)
					*ncontig = e_len - (lbn - e_block);
				if (bp != NULL)
					brelse(bp);
				return (0);
			}
		}
	}

	if (bp != NULL)
		brelse(bp);

	/* Block not covered by any extent — hole */
	*pblk = 0;
	if (ncontig != NULL)
		*ncontig = 1;
	return (0);
}

/*
 * Write inode back to disk with checksum update.
 */
int
ext4fs_update(struct inode *ip, int waitfor)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct buf *bp;
	u_int32_t inode_group, inode_index, block_in_table, offset_in_block;
	struct ext4fs_block_group_descriptor *gd;
	u_int64_t inode_table_block;
	daddr_t disk_block;
	u_int32_t csum;
	int error;

	if (ITOV(ip)->v_mount->mnt_flag & MNT_RDONLY)
		return (0);

	EXT4FS_ITIMES(ip);

	if ((ip->i_flag & IN_MODIFIED) == 0) {
		return (0);
	}

	ip->i_flag &= ~IN_MODIFIED;

	/* Locate inode on disk */
	inode_group = (ip->i_number - 1) / fs->m_inodes_per_group;
	inode_index = (ip->i_number - 1) % fs->m_inodes_per_group;
	block_in_table = inode_index / fs->m_inodes_per_block;
	offset_in_block = (inode_index % fs->m_inodes_per_block) *
	    fs->m_inode_size;

	gd = &fs->m_gd[inode_group];
	inode_table_block = letoh32(gd->bgd_inode_table_block_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		inode_table_block |=
		    (u_int64_t)letoh32(gd->bgd_inode_table_block_hi) << 32;

	disk_block = (inode_table_block + block_in_table) <<
	    fs->m_fs_block_to_disk_block;

	error = bread(ip->i_devvp, disk_block, fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return (error);
	}

	/*
	 * Verify extent header integrity before writing.
	 * If the inode uses extents (not a fast symlink), the magic
	 * must be valid. Refuse to persist corruption.
	 */
	{
		u_int16_t wr_mode = letoh16(ip->i_e4din->dinode.i_mode);
		u_int32_t wr_flags = letoh32(ip->i_e4din->dinode.i_flags);
		u_int16_t wr_magic =
		    letoh16(ip->i_e4din->dinode.i_extent_header.eh_magic);
		if (wr_mode != 0 &&
		    (wr_flags & EXTFS_INODE_FLAG_EXTENTS) &&
		    wr_magic != EXT4FS_EXTENT_HEADER_MAGIC) {
			printf("ext4fs_update: REFUSING to write ino=%u "
			    "with corrupt extent header! "
			    "magic=0x%x mode=0%o flags=0x%x\n",
			    ip->i_number, wr_magic, wr_mode, wr_flags);
			brelse(bp);
			return (EIO);
		}
	}

	/* Recompute inode checksum */
	csum = ext4fs_inode_csum(fs, ip->i_e4din, ip->i_number);
	ip->i_e4din->dinode.i_checksum_lo = htole16(csum & 0xFFFF);
	ip->i_e4din->dinode.i_checksum_hi = htole16((csum >> 16) & 0xFFFF);

	/* Copy inode to buffer */
	memcpy((char *)bp->b_data + offset_in_block, ip->i_e4din,
	    fs->m_inode_size);

	if (waitfor) {
		error = bwrite(bp);
		return (error);
	}

	bdwrite(bp);
	return (0);
}

/*
 * Set inode size (both low and high 32-bit fields).
 */
void
ext4fs_setsize(struct inode *ip, u_int64_t size)
{
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;

	din->i_size_lo = htole32((u_int32_t)size);
	din->i_size_hi = htole32((u_int32_t)(size >> 32));
}

/*
 * Allocate a filesystem block.
 * Tries the group of the goal block first, then scans all groups.
 */
int
ext4fs_blkalloc(struct inode *ip, u_int64_t goal, u_int32_t count,
    u_int64_t *bnp, u_int32_t *countp)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp, *dbp;
	u_int64_t bitmap_blk, grp_start, bb, ib, itb;
	u_int32_t group, ngroups, g, blk_in_group, free_blocks;
	u_int32_t it_blocks, mb, pbit, rb, bcsum;
	u_int32_t start_bit, nalloced, k;
	u_int32_t *dind;
	char *bbp;
	int error, i, j, has_sb;

	*bnp = 0;
	*countp = 0;

	if (count == 0)
		count = 1;

	if (fs->m_free_blocks_count == 0)
		return (ENOSPC);

	ngroups = fs->m_block_group_count;

	/* Pick starting group from goal */
	if (goal >= fs->m_first_data_block && goal < fs->m_blocks_count)
		group = (goal - fs->m_first_data_block) /
		    fs->m_blocks_per_group;
	else
		group = (ip->i_number - 1) / fs->m_inodes_per_group;

	for (i = 0; i < ngroups; i++) {
		g = (group + i) % ngroups;
		gd = &fs->m_gd[g];

		free_blocks = letoh16(gd->bgd_free_blocks_count_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			free_blocks |= (u_int32_t)
			    letoh16(gd->bgd_free_blocks_count_hi) << 16;
		if (free_blocks == 0)
			continue;

		/* Read block bitmap */
		bitmap_blk = letoh32(gd->bgd_block_bitmap_block_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			bitmap_blk |= (u_int64_t)
			    letoh32(gd->bgd_block_bitmap_block_hi) << 32;

		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, bitmap_blk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			continue;
		}
		bbp = (char *)bp->b_data;

		/*
		 * If BLOCK_UNINIT is set, the on-disk bitmap block
		 * may contain garbage. Zero it and mark metadata
		 * blocks (bitmaps, inode table) as used.
		 */
		if (letoh16(gd->bgd_flags) &
		    EXT4FS_BGD_FLAG_BLOCK_UNINIT) {
			grp_start = (u_int64_t)g *
			    fs->m_blocks_per_group +
			    fs->m_first_data_block;
			memset(bbp, 0, fs->m_block_size);
			/*
			 * Mark superblock, GDT, and reserved
			 * GDT blocks for groups that have them.
			 */
			has_sb = 0;
			if (!(fs->m_feature_ro_compat &
			    EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER))
				has_sb = 1;
			else if (g == 0 || g == 1)
				has_sb = 1;
			else {
				u_int64_t n;
				for (n = 3; n <= g; n *= 3)
					if (n == g) has_sb = 1;
				for (n = 5; n <= g; n *= 5)
					if (n == g) has_sb = 1;
				for (n = 7; n <= g; n *= 7)
					if (n == g) has_sb = 1;
			}
			if (has_sb) {
				u_int32_t overhead = 1 +
				    fs->m_block_group_descriptor_blocks_count +
				    fs->m_reserved_bgdt_blocks;
				for (mb = 0; mb < overhead; mb++)
					setbit(bbp, mb);
			}
			/* Block bitmap */
			bb = letoh32(gd->bgd_block_bitmap_block_lo);
			if (bb >= grp_start &&
			    bb < grp_start + fs->m_blocks_per_group)
				setbit(bbp, bb - grp_start);
			/* Inode bitmap */
			ib = letoh32(gd->bgd_inode_bitmap_block_lo);
			if (ib >= grp_start &&
			    ib < grp_start + fs->m_blocks_per_group)
				setbit(bbp, ib - grp_start);
			/* Inode table */
			itb = letoh32(gd->bgd_inode_table_block_lo);
			it_blocks = (fs->m_inodes_per_group *
			    fs->m_inode_size + fs->m_block_size - 1) /
			    fs->m_block_size;
			for (mb = 0; mb < it_blocks; mb++) {
				u_int64_t b = itb + mb;
				if (b >= grp_start &&
				    b < grp_start +
				    fs->m_blocks_per_group)
					setbit(bbp, b - grp_start);
			}
			for (pbit = fs->m_blocks_per_group;
			    pbit < fs->m_block_size * 8; pbit++)
				setbit(bbp, pbit);
			/* Mark resize inode (inode 7) blocks */
			if (fs->m_resize_dind_block != 0) {
				if (fs->m_resize_dind_block >= grp_start &&
				    fs->m_resize_dind_block <
				    grp_start + fs->m_blocks_per_group)
					setbit(bbp,
					    fs->m_resize_dind_block -
					    grp_start);

				error = bread(ip->i_devvp,
				    (daddr_t)EXT4FS_FSBTODB(fs,
				    fs->m_resize_dind_block),
				    fs->m_block_size, &dbp);
				if (!error) {
					dind = (u_int32_t *)dbp->b_data;
					for (j = 0;
					    j < fs->m_block_size / 4;
					    j++) {
						rb = letoh32(dind[j]);
						if (rb == 0)
							continue;
						if (rb >= grp_start &&
						    rb < grp_start +
						    fs->m_blocks_per_group)
							setbit(bbp,
							    rb - grp_start);
					}
					brelse(dbp);
				} else {
					brelse(dbp);
				}
			}
			gd->bgd_flags = htole16(letoh16(
			    gd->bgd_flags) &
			    ~EXT4FS_BGD_FLAG_BLOCK_UNINIT);
			ext4fs_bgd_write(fs, ip->i_devvp, g);
		}

		/* Start scan from goal bit if goal is in this group */
		start_bit = 0;
		if (goal >= fs->m_first_data_block &&
		    goal < fs->m_blocks_count) {
			u_int32_t goal_group = (goal - fs->m_first_data_block) /
			    fs->m_blocks_per_group;
			if (goal_group == g)
				start_bit = (goal - fs->m_first_data_block) %
				    fs->m_blocks_per_group;
		}

		/* Scan bitmap for free block(s) */
		for (blk_in_group = start_bit;
		    blk_in_group < fs->m_blocks_per_group;
		    blk_in_group++) {
			if (isclr(bbp, blk_in_group)) {
				/* Found first free bit; grab contiguous run */
				nalloced = 1;
				setbit(bbp, blk_in_group);
				for (k = 1; k < count &&
				    blk_in_group + k < fs->m_blocks_per_group &&
				    isclr(bbp, blk_in_group + k); k++) {
					setbit(bbp, blk_in_group + k);
					nalloced++;
				}

				bcsum = ext4fs_bitmap_csum(fs, g, bbp,
				    fs->m_block_size);
				gd->bgd_block_bitmap_checksum_lo =
				    htole16(bcsum & 0xFFFF);
				if (fs->m_feature_incompat &
				    EXT4FS_FEATURE_INCOMPAT_64BIT)
					gd->bgd_block_bitmap_checksum_hi
					    = htole16(
					    (bcsum >> 16) & 0xFFFF);

				bdwrite(bp);

				/* Update BGD */
				free_blocks -= nalloced;
				gd->bgd_free_blocks_count_lo =
				    htole16(free_blocks & 0xFFFF);
				if (fs->m_feature_incompat &
				    EXT4FS_FEATURE_INCOMPAT_64BIT)
					gd->bgd_free_blocks_count_hi =
					    htole16((free_blocks >> 16) &
					    0xFFFF);

				ext4fs_bgd_write(fs, ip->i_devvp, g);

				/* Update superblock counters */
				fs->m_free_blocks_count -= nalloced;
				fs->m_sble.sb_free_blocks_count_lo =
				    htole32((u_int32_t)
				    fs->m_free_blocks_count);
				fs->m_sble.sb_free_blocks_count_hi =
				    htole32((u_int32_t)
				    (fs->m_free_blocks_count >> 32));
				fs->m_fs_was_modified = 1;

				*bnp = (u_int64_t)g * fs->m_blocks_per_group +
				    blk_in_group + fs->m_first_data_block;
				*countp = nalloced;

				return (0);
			}
		}

		brelse(bp);
	}

	return (ENOSPC);
}

/*
 * Free a filesystem block.
 */
void
ext4fs_blkfree(struct inode *ip, u_int64_t bno)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp;
	u_int64_t bitmap_blk;
	u_int32_t group, blk_in_group, free_blocks;
	char *bbp;
	int error;

	if (bno < fs->m_first_data_block || bno >= fs->m_blocks_count)
		return;

	group = (bno - fs->m_first_data_block) / fs->m_blocks_per_group;
	if (group >= fs->m_block_group_count)
		return;
	blk_in_group = (bno - fs->m_first_data_block) %
	    fs->m_blocks_per_group;
	gd = &fs->m_gd[group];

	/* Read block bitmap */
	bitmap_blk = letoh32(gd->bgd_block_bitmap_block_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		bitmap_blk |=
		    (u_int64_t)letoh32(gd->bgd_block_bitmap_block_hi) << 32;

	error = bread(ip->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, bitmap_blk),
	    fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return;
	}

	bbp = (char *)bp->b_data;
	clrbit(bbp, blk_in_group);

	/* Update block bitmap checksum in BGD */
	{
		u_int32_t bcsum = ext4fs_bitmap_csum(fs, group, bbp,
		    fs->m_block_size);
		gd->bgd_block_bitmap_checksum_lo = htole16(bcsum & 0xFFFF);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			gd->bgd_block_bitmap_checksum_hi =
			    htole16((bcsum >> 16) & 0xFFFF);
	}

	bdwrite(bp);

	/* Update BGD */
	free_blocks = letoh16(gd->bgd_free_blocks_count_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		free_blocks |=
		    (u_int32_t)letoh16(gd->bgd_free_blocks_count_hi) << 16;
	free_blocks++;
	gd->bgd_free_blocks_count_lo = htole16(free_blocks & 0xFFFF);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		gd->bgd_free_blocks_count_hi =
		    htole16((free_blocks >> 16) & 0xFFFF);

	ext4fs_bgd_write(fs, ip->i_devvp, group);

	/* Update superblock counters */
	fs->m_free_blocks_count++;
	fs->m_sble.sb_free_blocks_count_lo =
	    htole32((u_int32_t)fs->m_free_blocks_count);
	fs->m_sble.sb_free_blocks_count_hi =
	    htole32((u_int32_t)(fs->m_free_blocks_count >> 32));
	fs->m_fs_was_modified = 1;
}

/*
 * Promote a depth-0 extent tree to depth 1.
 * Called when the inline extent array is full (4/4 entries).
 * Allocates a leaf block, copies the 4 inline extents into it,
 * and converts the inode root to an index node with one entry.
 */
static int
ext4fs_extent_grow_tree(struct inode *ip)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_extent_header *eh = &din->i_extent_header;
	struct ext4fs_extent_header *leaf_eh;
	struct buf *bp;
	u_int64_t leaf_blk;
	u_int16_t maxleaf;
	u_int64_t i_blocks;
	u_int32_t got;
	int error;

	if (letoh16(eh->eh_depth) != 0)
		return (EIO);
	if (letoh16(eh->eh_entries) != 4)
		return (EIO);

	/* Allocate a block for the leaf node */
	error = ext4fs_blkalloc(ip, 0, 1, &leaf_blk, &got);
	if (error)
		return (error);

	/* Get buffer for the new leaf block */
	bp = getblk(ip->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, leaf_blk),
	    fs->m_block_size, 0, INFSLP);
	clrbuf(bp);

	/* Initialize leaf block header */
	maxleaf = (fs->m_block_size - sizeof(struct ext4fs_extent_header)) /
	    sizeof(struct ext4fs_extent);
	leaf_eh = (struct ext4fs_extent_header *)bp->b_data;
	leaf_eh->eh_magic = htole16(EXT4FS_EXTENT_HEADER_MAGIC);
	leaf_eh->eh_entries = htole16(4);
	leaf_eh->eh_max = htole16(maxleaf);
	leaf_eh->eh_depth = htole16(0);
	leaf_eh->eh_generation = htole32(0);

	/* Copy 4 inline extents into the leaf block */
	memcpy((char *)bp->b_data + sizeof(struct ext4fs_extent_header),
	    din->i_extent, 4 * sizeof(struct ext4fs_extent));

	ext4fs_extent_block_csum_set(fs, ip->i_number, din->i_nfs_generation, bp->b_data);
	bdwrite(bp);

	/* Convert inode root to index node with depth=1 */
	eh->eh_depth = htole16(1);
	eh->eh_entries = htole16(1);
	/* eh_max stays 4 (4 index entries fit in the inline area) */

	/* Set up the single index entry pointing to the leaf block */
	{
		struct ext4fs_extent_idx *idx = din->i_extent_idx;
		idx[0].ei_block = din->i_extent[0].e_block;  /* first lbn */
		idx[0].ei_leaf_lo = htole32((u_int32_t)leaf_blk);
		idx[0].ei_leaf_hi = htole16((u_int16_t)(leaf_blk >> 32));
		idx[0].ei_unused = 0;
		/* Zero remaining index slots */
		memset(&idx[1], 0, 3 * sizeof(struct ext4fs_extent_idx));
	}

	/* Update inode block count for the leaf block */
	i_blocks = letoh32(din->i_blocks_lo) |
	    ((u_int64_t)letoh16(din->i_blocks_hi) << 32);
	i_blocks += fs->m_block_size / DEV_BSIZE;
	din->i_blocks_lo = htole32((u_int32_t)i_blocks);
	din->i_blocks_hi = htole16((u_int16_t)(i_blocks >> 32));

	ip->i_flag |= IN_CHANGE | IN_MODIFIED;
	return (0);
}

/*
 * Split a full leaf block into two.
 * The old leaf keeps the first half, a new leaf gets the second half.
 * A new index entry is added to the parent (the inode root).
 * Returns ENOSPC if the parent index is also full (depth 2+ needed).
 */
static int
ext4fs_leaf_split(struct inode *ip, struct buf *old_bp,
    struct ext4fs_extent_header *old_eh)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_extent_header *root_eh = &din->i_extent_header;
	struct ext4fs_extent_idx *root_idx = din->i_extent_idx;
	struct ext4fs_extent_header *new_eh;
	struct ext4fs_extent *old_ext, *new_ext;
	struct buf *new_bp;
	u_int64_t new_blk;
	u_int32_t new_first_block;
	u_int16_t old_entries, new_entries, maxleaf;
	u_int16_t root_entries, root_max;
	u_int64_t i_blocks;
	u_int32_t got;
	int error, i;

	old_entries = letoh16(old_eh->eh_entries);
	maxleaf = letoh16(old_eh->eh_max);
	old_ext = (struct ext4fs_extent *)(old_eh + 1);
	/* Check parent has room for new index entry */
	root_entries = letoh16(root_eh->eh_entries);
	root_max = letoh16(root_eh->eh_max);
	if (root_entries >= root_max) {
		brelse(old_bp);
		return (ENOSPC);  /* Would need depth 2+, phase 4 */
	}

	/* Allocate block for new leaf */
	error = ext4fs_blkalloc(ip, 0, 1, &new_blk, &got);
	if (error) {
		brelse(old_bp);
		return (error);
	}

	/* Split ~50/50 */
	new_entries = old_entries / 2;
	old_entries = old_entries - new_entries;

	/*
	 * Save the first logical block of the new (second) half
	 * BEFORE we write any buffers, since new_ext will point
	 * into new_bp->b_data which is consumed by bwrite.
	 */
	new_first_block = letoh32(old_ext[old_entries].e_block);

	new_bp = getblk(ip->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, new_blk),
	    fs->m_block_size, 0, INFSLP);
	clrbuf(new_bp);

	/* Initialize new leaf */
	new_eh = (struct ext4fs_extent_header *)new_bp->b_data;
	new_eh->eh_magic = htole16(EXT4FS_EXTENT_HEADER_MAGIC);
	new_eh->eh_entries = htole16(new_entries);
	new_eh->eh_max = htole16(maxleaf);
	new_eh->eh_depth = htole16(0);
	new_eh->eh_generation = htole32(0);

	new_ext = (struct ext4fs_extent *)(new_eh + 1);
	memcpy(new_ext, &old_ext[old_entries],
	    new_entries * sizeof(struct ext4fs_extent));

	ext4fs_extent_block_csum_set(fs, ip->i_number, din->i_nfs_generation, new_bp->b_data);
	bdwrite(new_bp);

	/* Update old leaf */
	old_eh->eh_entries = htole16(old_entries);
	ext4fs_extent_block_csum_set(fs, ip->i_number, din->i_nfs_generation, old_bp->b_data);
	bdwrite(old_bp);

	/* Add new index entry in parent root (keep sorted by ei_block) */
	{
		struct ext4fs_extent_idx entry;

		entry.ei_block = htole32(new_first_block);
		entry.ei_leaf_lo = htole32((u_int32_t)new_blk);
		entry.ei_leaf_hi = htole16((u_int16_t)(new_blk >> 32));
		entry.ei_unused = 0;

		/* Find insertion point */
		for (i = 0; i < root_entries; i++) {
			if (letoh32(root_idx[i].ei_block) > new_first_block)
				break;
		}
		if (i < root_entries)
			memmove(&root_idx[i + 1], &root_idx[i],
			    (root_entries - i) *
			    sizeof(struct ext4fs_extent_idx));
		root_idx[i] = entry;
		root_eh->eh_entries = htole16(root_entries + 1);
	}

	/* Update inode block count for the new leaf block */
	i_blocks = letoh32(din->i_blocks_lo) |
	    ((u_int64_t)letoh16(din->i_blocks_hi) << 32);
	i_blocks += fs->m_block_size / DEV_BSIZE;
	din->i_blocks_lo = htole32((u_int32_t)i_blocks);
	din->i_blocks_hi = htole16((u_int16_t)(i_blocks >> 32));

	ip->i_flag |= IN_CHANGE | IN_MODIFIED;
	return (0);
}

/*
 * Insert an extent into a depth > 0 extent tree.
 * Walks the index to find the correct leaf, tries merge,
 * inserts if room, splits leaf if full.
 */
static int
ext4fs_extent_insert_depth(struct inode *ip, u_int32_t lbn, u_int64_t pblk,
    u_int16_t len)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_extent_header *root_eh = &din->i_extent_header;
	struct ext4fs_extent_idx *idx;
	struct ext4fs_extent_header *leaf_eh;
	struct ext4fs_extent *ext;
	struct buf *bp;
	u_int64_t leaf_blk;
	u_int16_t root_entries, leaf_entries, leaf_max;
	int error, found, i;

	root_entries = letoh16(root_eh->eh_entries);
	if (root_entries == 0)
		return (EIO);

	/* Find the index entry whose subtree covers lbn */
	idx = din->i_extent_idx;
	found = 0;
	for (i = 0; i < root_entries; i++) {
		if (letoh32(idx[i].ei_block) <= lbn)
			found = i;
		else
			break;
	}

	/* Read the leaf block */
	leaf_blk = letoh32(idx[found].ei_leaf_lo) |
	    ((u_int64_t)letoh16(idx[found].ei_leaf_hi) << 32);

	error = bread(ip->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, leaf_blk),
	    fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return (error);
	}

	leaf_eh = (struct ext4fs_extent_header *)bp->b_data;
	if (letoh16(leaf_eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC) {
		brelse(bp);
		return (EIO);
	}

	leaf_entries = letoh16(leaf_eh->eh_entries);
	leaf_max = letoh16(leaf_eh->eh_max);
	ext = (struct ext4fs_extent *)(leaf_eh + 1);

	/* Try to merge with last extent in this leaf */
	if (leaf_entries > 0) {
		struct ext4fs_extent *last = &ext[leaf_entries - 1];
		u_int32_t last_block = letoh32(last->e_block);
		u_int16_t last_len = letoh16(last->e_len);
		u_int64_t last_start = letoh32(last->e_start_lo) |
		    ((u_int64_t)letoh16(last->e_start_hi) << 32);

		if (last_block + last_len == lbn &&
		    last_start + last_len == pblk &&
		    last_len + len <= 32768) {
			last->e_len = htole16(last_len + len);
			ext4fs_extent_block_csum_set(fs, ip->i_number, din->i_nfs_generation, bp->b_data);
			bdwrite(bp);
			ip->i_flag |= IN_CHANGE | IN_MODIFIED;
			return (0);
		}
	}

	/* Room in leaf? */
	if (leaf_entries < leaf_max) {
		/* Find insertion point (keep sorted) */
		for (i = 0; i < leaf_entries; i++) {
			if (letoh32(ext[i].e_block) > lbn)
				break;
		}
		if (i < leaf_entries)
			memmove(&ext[i + 1], &ext[i],
			    (leaf_entries - i) *
			    sizeof(struct ext4fs_extent));

		ext[i].e_block = htole32(lbn);
		ext[i].e_len = htole16(len);
		ext[i].e_start_lo = htole32((u_int32_t)pblk);
		ext[i].e_start_hi = htole16((u_int16_t)(pblk >> 32));

		leaf_eh->eh_entries = htole16(leaf_entries + 1);
		ext4fs_extent_block_csum_set(fs, ip->i_number, din->i_nfs_generation, bp->b_data);
		bdwrite(bp);
		ip->i_flag |= IN_CHANGE | IN_MODIFIED;
		return (0);
	}

	/* Leaf is full - need to split */
	error = ext4fs_leaf_split(ip, bp, leaf_eh);
	if (error)
		return (error);

	/* bp was consumed by leaf_split (bwrite'd). Retry the insert. */
	return (ext4fs_extent_insert_depth(ip, lbn, pblk, len));
}

/*
 * Insert an extent into the inode's extent tree.
 * Handles depth 0 (inline) and depth > 0 (tree) cases.
 * Tries to merge with the last extent if contiguous.
 */
static int
ext4fs_extent_insert(struct inode *ip, u_int32_t lbn, u_int64_t pblk,
    u_int16_t len)
{
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_extent_header *eh = &din->i_extent_header;
	struct ext4fs_extent *ext = din->i_extent;
	u_int16_t entries, maxe, depth;
	int error, i;

	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC)
		return (EIO);

	depth = letoh16(eh->eh_depth);

	/* Depth > 0: delegate to tree insert */
	if (depth > 0)
		return (ext4fs_extent_insert_depth(ip, lbn, pblk, len));

	/* Depth 0: inline extents */
	entries = letoh16(eh->eh_entries);
	maxe = letoh16(eh->eh_max);

	/* Try to merge with last extent */
	if (entries > 0) {
		struct ext4fs_extent *last = &ext[entries - 1];
		u_int32_t last_block = letoh32(last->e_block);
		u_int16_t last_len = letoh16(last->e_len);
		u_int64_t last_start = letoh32(last->e_start_lo) |
		    ((u_int64_t)letoh16(last->e_start_hi) << 32);

		if (last_block + last_len == lbn &&
		    last_start + last_len == pblk &&
		    last_len + len <= 32768) {
			last->e_len = htole16(last_len + len);
			ip->i_flag |= IN_CHANGE | IN_MODIFIED;
			return (0);
		}
	}

	/* Room for a new inline entry? */
	if (entries < maxe) {
		/* Find insertion point (keep sorted by lbn) */
		for (i = 0; i < entries; i++) {
			if (letoh32(ext[i].e_block) > lbn)
				break;
		}

		/* Shift entries to make room */
		if (i < entries)
			memmove(&ext[i + 1], &ext[i],
			    (entries - i) * sizeof(struct ext4fs_extent));

		/* Insert new extent */
		ext[i].e_block = htole32(lbn);
		ext[i].e_len = htole16(len);
		ext[i].e_start_lo = htole32((u_int32_t)pblk);
		ext[i].e_start_hi = htole16((u_int16_t)(pblk >> 32));

		eh->eh_entries = htole16(entries + 1);
		ip->i_flag |= IN_CHANGE | IN_MODIFIED;

		return (0);
	}

	/* Inline full - grow tree to depth 1, then insert */
	error = ext4fs_extent_grow_tree(ip);
	if (error)
		return (error);

	return (ext4fs_extent_insert_depth(ip, lbn, pblk, len));
}

/*
 * Allocate a buffer for a logical block.
 * If the block is already mapped, just read it.
 * Otherwise, allocate a new physical block and insert extent.
 */
static int
ext4fs_buf_alloc(struct inode *ip, u_int64_t lbn, int size,
    struct ucred *cred, struct buf **bpp, int flags)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int64_t pblk, goal, ncontig, i_blocks;
	int error;

	/* Check if already mapped */
	error = ext4fs_extent_pblk(ip, lbn, &pblk, &ncontig);
	if (error == 0 && pblk != 0) {
		/* Already mapped, just read */
		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, bpp);
		if (error)
			brelse(*bpp);
		return (error);
	}
	error = 0;

	/* Not mapped - allocate a new block */
	/* Goal: try to be contiguous with last extent */
	goal = 0;
	if (letoh16(din->i_extent_header.eh_entries) > 0) {
		u_int16_t depth = letoh16(din->i_extent_header.eh_depth);

		if (depth == 0) {
			u_int16_t ent = letoh16(din->i_extent_header.eh_entries);
			struct ext4fs_extent *last = &din->i_extent[ent - 1];
			u_int64_t last_start = letoh32(last->e_start_lo) |
			    ((u_int64_t)letoh16(last->e_start_hi) << 32);
			goal = last_start + letoh16(last->e_len);
		} else {
			/* Walk to last leaf to find last extent */
			u_int16_t ent = letoh16(din->i_extent_header.eh_entries);
			struct ext4fs_extent_idx *idx = din->i_extent_idx;
			u_int64_t leaf_blk;
			struct buf *gbp;

			leaf_blk = letoh32(idx[ent - 1].ei_leaf_lo) |
			    ((u_int64_t)letoh16(idx[ent - 1].ei_leaf_hi) << 32);
			error = bread(ip->i_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, leaf_blk),
			    fs->m_block_size, &gbp);
			if (error == 0) {
				struct ext4fs_extent_header *leh =
				    (struct ext4fs_extent_header *)gbp->b_data;
				u_int16_t lent = letoh16(leh->eh_entries);
				if (lent > 0 && letoh16(leh->eh_magic) ==
				    EXT4FS_EXTENT_HEADER_MAGIC) {
					struct ext4fs_extent *le =
					    (struct ext4fs_extent *)(leh + 1);
					u_int64_t ls =
					    letoh32(le[lent - 1].e_start_lo) |
					    ((u_int64_t)letoh16(
					    le[lent - 1].e_start_hi) << 32);
					goal = ls + letoh16(le[lent - 1].e_len);
				}
				brelse(gbp);
			} else {
				brelse(gbp);
			}
		}
	}

	{
		u_int32_t got;

		error = ext4fs_blkalloc(ip, goal, 1, &pblk, &got);
		if (error)
			return (error);
		error = ext4fs_extent_insert(ip, lbn, pblk, 1);
		if (error) {
			ext4fs_blkfree(ip, pblk);
			return (error);
		}
		/* Update inode block count (i_blocks is in 512-byte sectors) */
		i_blocks = letoh32(din->i_blocks_lo) |
		    ((u_int64_t)letoh16(din->i_blocks_hi) << 32);
		i_blocks += fs->m_block_size / DEV_BSIZE;
		din->i_blocks_lo = htole32((u_int32_t)i_blocks);
		din->i_blocks_hi = htole16((u_int16_t)(i_blocks >> 32));
	}

	/* Set extents flag */
	din->i_flags |= htole32(EXTFS_INODE_FLAG_EXTENTS);

	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	/* Get buffer for the new block */
	*bpp = getblk(ip->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
	    fs->m_block_size, 0, INFSLP);
	if (flags & B_CLRBUF)
		clrbuf(*bpp);

	return (0);
}

/*
 * Free all blocks described by an array of extents.
 * Batches frees by block group for efficiency.
 */
static void
ext4fs_free_extents(struct inode *ip, struct ext4fs_extent *ext,
    u_int16_t entries)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	int i;

	for (i = 0; i < entries; i++) {
		u_int64_t start = letoh32(ext[i].e_start_lo) |
		    ((u_int64_t)letoh16(ext[i].e_start_hi) << 32);
		u_int32_t len = letoh16(ext[i].e_len);
		u_int32_t freed = 0;

		if (len > 32768)
			len -= 32768;

		while (freed < len) {
			u_int64_t bno = start + freed;
			u_int32_t group, blk_in_group;
			struct ext4fs_block_group_descriptor *gd;

			if (bno < fs->m_first_data_block ||
			    bno >= fs->m_blocks_count) {
				freed++;
				continue;
			}
			group = (bno - fs->m_first_data_block) /
			    fs->m_blocks_per_group;
			if (group >= fs->m_block_group_count) {
				freed++;
				continue;
			}
			blk_in_group = (bno - fs->m_first_data_block) %
			    fs->m_blocks_per_group;
			gd = &fs->m_gd[group];
			u_int64_t bitmap_blk;
			struct buf *bbp;
			u_int32_t n, k, free_blocks;
			int berr;

			/* How many blocks fall in this group? */
			n = fs->m_blocks_per_group - blk_in_group;
			if (n > len - freed)
				n = len - freed;

			bitmap_blk = letoh32(gd->bgd_block_bitmap_block_lo);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				bitmap_blk |= (u_int64_t)letoh32(
				    gd->bgd_block_bitmap_block_hi) << 32;

			berr = bread(ip->i_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, bitmap_blk),
			    fs->m_block_size, &bbp);
			if (berr) {
				brelse(bbp);
				freed += n;
				continue;
			}

			for (k = 0; k < n; k++)
				clrbit((char *)bbp->b_data, blk_in_group + k);

			{
				u_int32_t bcsum = ext4fs_bitmap_csum(fs, group,
				    bbp->b_data, fs->m_block_size);
				gd->bgd_block_bitmap_checksum_lo =
				    htole16(bcsum & 0xFFFF);
				if (fs->m_feature_incompat &
				    EXT4FS_FEATURE_INCOMPAT_64BIT)
					gd->bgd_block_bitmap_checksum_hi =
					    htole16((bcsum >> 16) & 0xFFFF);
			}
			bdwrite(bbp);

			free_blocks = letoh16(gd->bgd_free_blocks_count_lo);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				free_blocks |= (u_int32_t)letoh16(
				    gd->bgd_free_blocks_count_hi) << 16;
			free_blocks += n;
			gd->bgd_free_blocks_count_lo =
			    htole16(free_blocks & 0xFFFF);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				gd->bgd_free_blocks_count_hi =
				    htole16((free_blocks >> 16) & 0xFFFF);
			ext4fs_bgd_write(fs, ip->i_devvp, group);

			fs->m_free_blocks_count += n;
			fs->m_sble.sb_free_blocks_count_lo =
			    htole32((u_int32_t)fs->m_free_blocks_count);
			fs->m_sble.sb_free_blocks_count_hi =
			    htole32((u_int32_t)(fs->m_free_blocks_count >> 32));
			fs->m_fs_was_modified = 1;

			freed += n;
		}
	}
}

/*
 * Trim extents: free blocks beyond new_nblocks, trim straddling extents.
 * Returns number of filesystem blocks freed.
 */
static u_int64_t
ext4fs_trim_extents(struct inode *ip, struct ext4fs_extent *ext,
    u_int16_t *entries_p, u_int32_t new_nblocks)
{
	u_int16_t entries = *entries_p;
	u_int64_t blocks_freed = 0;
	u_int16_t new_count = 0;
	int i;

	for (i = 0; i < entries; i++) {
		u_int32_t eblk = letoh32(ext[i].e_block);
		u_int16_t raw_len = letoh16(ext[i].e_len);
		u_int16_t elen = raw_len;
		u_int64_t estart;
		int uninit = 0;

		if (elen > 32768) {
			elen -= 32768;
			uninit = 1;
		}
		estart = letoh32(ext[i].e_start_lo) |
		    ((u_int64_t)letoh16(ext[i].e_start_hi) << 32);

		if (eblk >= new_nblocks) {
			/* Entirely past boundary — free all */
			struct ext4fs_extent tmp = ext[i];
			tmp.e_len = htole16(elen);
			ext4fs_free_extents(ip, &tmp, 1);
			blocks_freed += elen;
		} else if (eblk + elen > new_nblocks) {
			/* Straddles boundary — trim */
			u_int32_t keep = new_nblocks - eblk;
			u_int32_t discard = elen - keep;
			struct ext4fs_extent tmp;

			/* Free the tail */
			tmp.e_block = htole32(eblk + keep);
			tmp.e_start_lo = htole32(
			    (u_int32_t)(estart + keep));
			tmp.e_start_hi = htole16(
			    (u_int16_t)((estart + keep) >> 32));
			tmp.e_len = htole16(discard);
			ext4fs_free_extents(ip, &tmp, 1);
			blocks_freed += discard;

			/* Keep the trimmed extent */
			ext[new_count] = ext[i];
			ext[new_count].e_len = htole16(
			    keep | (uninit ? 32768 : 0));
			new_count++;
		} else {
			/* Entirely before boundary — keep */
			if (new_count != i)
				ext[new_count] = ext[i];
			new_count++;
		}
	}

	*entries_p = new_count;
	return (blocks_freed);
}

/*
 * Truncate inode to given length.
 * Handles grow (extend with hole), shrink to 0, and shrink to non-zero.
 * Supports both depth-0 (inline) and depth > 0 (tree) extent trees.
 */
int
ext4fs_truncate(struct inode *ip, off_t length, int flags, struct ucred *cred)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_extent_header *eh = &din->i_extent_header;
	struct vnode *vp = ITOV(ip);
	off_t cursize;
	u_int16_t entries, depth;
	u_int64_t blocks_freed;

	cursize = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	if (length == cursize)
		return (0);

	if (length < 0)
		return (EINVAL);

	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC)
		return (EIO);

	depth = letoh16(eh->eh_depth);
	entries = letoh16(eh->eh_entries);

	if (length > cursize) {
		/* Grow: just update size. Gap becomes hole. */
		ext4fs_setsize(ip, length);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		uvm_vnp_setsize(vp, length);
		return (ext4fs_update(ip, 1));
	}

	/* Shrink */
	blocks_freed = 0;

	if (length == 0) {
		/* Truncate to 0: free everything */
		if (depth == 0) {
			ext4fs_free_extents(ip, din->i_extent, entries);
		} else {
			struct ext4fs_extent_idx *idx = din->i_extent_idx;
			int i;

			for (i = 0; i < entries; i++) {
				u_int64_t leaf_blk;
				struct ext4fs_extent_header *leaf_eh;
				struct ext4fs_extent *leaf_ext;
				struct buf *bp;
				u_int16_t leaf_entries;
				int error;

				leaf_blk = letoh32(idx[i].ei_leaf_lo) |
				    ((u_int64_t)letoh16(
				    idx[i].ei_leaf_hi) << 32);

				error = bread(ip->i_devvp,
				    (daddr_t)EXT4FS_FSBTODB(fs, leaf_blk),
				    fs->m_block_size, &bp);
				if (error) {
					brelse(bp);
					continue;
				}

				leaf_eh = (struct ext4fs_extent_header *)
				    bp->b_data;
				if (letoh16(leaf_eh->eh_magic) !=
				    EXT4FS_EXTENT_HEADER_MAGIC) {
					brelse(bp);
					continue;
				}

				leaf_entries = letoh16(leaf_eh->eh_entries);
				leaf_ext = (struct ext4fs_extent *)
				    (leaf_eh + 1);

				ext4fs_free_extents(ip, leaf_ext,
				    leaf_entries);
				brelse(bp);

				ext4fs_blkfree(ip, leaf_blk);
			}
		}

		/* Reset inode root to depth 0, 0 entries */
		memset(din->i_extent, 0,
		    4 * sizeof(struct ext4fs_extent));
		eh->eh_entries = htole16(0);
		eh->eh_depth = htole16(0);

		/* Zero block count */
		din->i_blocks_lo = htole32(0);
		din->i_blocks_hi = htole16(0);
	} else {
		/* Truncate to non-zero length */
		u_int32_t new_nblocks;
		u_int64_t i_blocks, freed_512;

		new_nblocks = (length + fs->m_block_size - 1) /
		    fs->m_block_size;

		/* Zero out partial block tail */
		if (length % fs->m_block_size != 0) {
			u_int32_t offset = length % fs->m_block_size;
			u_int64_t pblk;

			if (ext4fs_extent_pblk(ip, new_nblocks - 1,
			    &pblk, NULL) == 0 && pblk != 0) {
				struct buf *bp;
				int error;

				error = bread(ip->i_devvp,
				    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
				    fs->m_block_size, &bp);
				if (error) {
					brelse(bp);
					return (error);
				}
				memset((char *)bp->b_data + offset, 0,
				    fs->m_block_size - offset);
				bdwrite(bp);
			}
		}

		/* Free/trim extents past new_nblocks */
		if (depth == 0) {
			blocks_freed = ext4fs_trim_extents(ip,
			    din->i_extent, &entries, new_nblocks);
			eh->eh_entries = htole16(entries);
		} else {
			struct ext4fs_extent_idx *idx = din->i_extent_idx;
			u_int16_t new_idx_count = 0;
			int i;

			for (i = 0; i < entries; i++) {
				u_int64_t leaf_blk;
				struct ext4fs_extent_header *leaf_eh;
				struct ext4fs_extent *leaf_ext;
				struct buf *bp;
				u_int16_t leaf_entries;
				int error;

				leaf_blk = letoh32(idx[i].ei_leaf_lo) |
				    ((u_int64_t)letoh16(
				    idx[i].ei_leaf_hi) << 32);

				error = bread(ip->i_devvp,
				    (daddr_t)EXT4FS_FSBTODB(fs, leaf_blk),
				    fs->m_block_size, &bp);
				if (error) {
					brelse(bp);
					continue;
				}

				leaf_eh = (struct ext4fs_extent_header *)
				    bp->b_data;
				if (letoh16(leaf_eh->eh_magic) !=
				    EXT4FS_EXTENT_HEADER_MAGIC) {
					brelse(bp);
					continue;
				}

				leaf_entries = letoh16(leaf_eh->eh_entries);
				leaf_ext = (struct ext4fs_extent *)
				    (leaf_eh + 1);

				blocks_freed += ext4fs_trim_extents(ip,
				    leaf_ext, &leaf_entries, new_nblocks);

				if (leaf_entries == 0) {
					/* Leaf now empty — free it */
					brelse(bp);
					ext4fs_blkfree(ip, leaf_blk);
				} else {
					leaf_eh->eh_entries =
					    htole16(leaf_entries);
					ext4fs_extent_block_csum_set(fs,
					    ip->i_number,
					    din->i_nfs_generation,
					    bp->b_data);
					bdwrite(bp);
					if (new_idx_count != i)
						idx[new_idx_count] = idx[i];
					new_idx_count++;
				}
			}

			entries = new_idx_count;
			eh->eh_entries = htole16(entries);

			/* If all index entries gone, collapse to depth 0 */
			if (entries == 0) {
				memset(din->i_extent, 0,
				    4 * sizeof(struct ext4fs_extent));
				eh->eh_depth = htole16(0);
			}
		}

		/* Update i_blocks */
		i_blocks = letoh32(din->i_blocks_lo);
		if (fs->m_feature_ro_compat &
		    EXT4FS_FEATURE_RO_COMPAT_HUGE_FILE)
			i_blocks |=
			    (u_int64_t)letoh16(din->i_blocks_hi) << 32;
		freed_512 = blocks_freed *
		    (fs->m_block_size / DEV_BSIZE);
		if (i_blocks >= freed_512)
			i_blocks -= freed_512;
		else
			i_blocks = 0;
		din->i_blocks_lo = htole32((u_int32_t)i_blocks);
		din->i_blocks_hi = htole16((u_int16_t)(i_blocks >> 32));
	}

	/* Update size */
	ext4fs_setsize(ip, length);
	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	/* Purge cached data */
	uvm_vnp_setsize(vp, length);
	vinvalbuf(vp, 0, NOCRED, curproc, 0, INFSLP);

	return (ext4fs_update(ip, 1));
}

/* Forward declarations */

int ext4fs_access(void *);
int ext4fs_advlock(void *);
int ext4fs_bmap(void *);
int ext4fs_chmod(struct vnode *, mode_t, struct ucred *);
int ext4fs_chown(struct vnode *, uid_t, gid_t, struct ucred *);
int ext4fs_create(void *);
int ext4fs_fsync(void *);
int ext4fs_getattr(void *);
int ext4fs_inactive(void *);
int ext4fs_link(void *);
int ext4fs_lookup(void *);
int ext4fs_mkdir(void *);
int ext4fs_mknod(void *);
int ext4fs_open(void *);
int ext4fs_pathconf(void *);
int ext4fs_print(void *);
int ext4fs_read(void *);
int ext4fs_readdir(void *);
int ext4fs_readlink(void *);
int ext4fs_reclaim(void *);
int ext4fs_remove(void *);
int ext4fs_rename(void *);
int ext4fs_rmdir(void *);
int ext4fs_setattr(void *);
int ext4fs_strategy(void *);
int ext4fs_symlink(void *);
int ext4fs_write(void *);

const struct vops ext4fs_vops = {
	.vop_lookup	= ext4fs_lookup,
	.vop_create	= ext4fs_create,
	.vop_mknod	= ext4fs_mknod,
	.vop_open	= ext4fs_open,
	.vop_close	= ufs_close,
	.vop_access	= ext4fs_access,
	.vop_getattr	= ext4fs_getattr,
	.vop_setattr	= ext4fs_setattr,
	.vop_read	= ext4fs_read,
	.vop_write	= ext4fs_write,
	.vop_ioctl	= ufs_ioctl,
	.vop_kqfilter	= ufs_kqfilter,
	.vop_revoke	= NULL,
	.vop_fsync	= ext4fs_fsync,
	.vop_remove	= ext4fs_remove,
	.vop_link	= ext4fs_link,
	.vop_rename	= ext4fs_rename,
	.vop_mkdir	= ext4fs_mkdir,
	.vop_rmdir	= ext4fs_rmdir,
	.vop_symlink	= ext4fs_symlink,
	.vop_readdir	= ext4fs_readdir,
	.vop_readlink	= ext4fs_readlink,
	.vop_abortop	= NULL,
	.vop_inactive	= ext4fs_inactive,
	.vop_reclaim	= ext4fs_reclaim,
	.vop_lock	= ufs_lock,
	.vop_unlock	= ufs_unlock,
	.vop_bmap	= ext4fs_bmap,
	.vop_strategy	= ext4fs_strategy,
	.vop_print	= ext4fs_print,
	.vop_pathconf	= ext4fs_pathconf,
	.vop_advlock	= ext4fs_advlock,
	.vop_bwrite	= NULL,
};

/* Stub implementations */

int
ext4fs_lookup(void *v)
{
	struct vop_lookup_args *ap = v;
	struct vnode *vdp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	struct inode *dp = VTOI(vdp);
	struct m_ext4fs *fs = dp->i_e4fs;
	struct ext4fs_dinode *din = &dp->i_e4din->dinode;
	struct ext4fs_directory *ep;
	struct vnode *tdp;
	struct buf *bp;
	int flags = cnp->cn_flags;
	int nameiop = cnp->cn_nameiop;
	int lockparent = flags & LOCKPARENT;
	ino_t foundino = 0;
	off_t off, filesz;
	u_int64_t lbn, pblk, blkoff;
	u_int16_t reclen;
	int error;

	/* For CREATE: track free slot info */
	int slotneeded = 0;
	int slotsize = 0;
	off_t slotoffset = -1;
	off_t prevoff = -1;

	*vpp = NULL;

	/* Check accessibility of directory */
	if ((error = VOP_ACCESS(vdp, VEXEC, cnp->cn_cred, cnp->cn_proc)) != 0) {
		return (error);
	}

	if ((flags & ISLASTCN) && (vdp->v_mount->mnt_flag & MNT_RDONLY) &&
	    (nameiop == DELETE || nameiop == RENAME))
		return (EROFS);

	/* Check the name cache */
	if ((error = cache_lookup(vdp, vpp, cnp)) >= 0)
		return (error);

	/* Search directory for the name */
	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	if (nameiop == CREATE || nameiop == RENAME)
		slotneeded = EXT4FS_DIRSIZ(cnp->cn_namelen);

	for (off = 0; off < filesz; ) {
		lbn = EXT4FS_LBLKNO(fs, off);

		error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
		if (error || pblk == 0) {
			return (error ? error : EIO);
		}

		error = bread(dp->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			return (error);
		}

		blkoff = EXT4FS_BLKOFF(fs, off);
		prevoff = -1;

		while (blkoff < fs->m_block_size && off < filesz) {
			ep = (struct ext4fs_directory *)
			    ((char *)bp->b_data + blkoff);
			reclen = letoh16(ep->e4d_reclen);

			if (reclen == 0) {
				brelse(bp);
				return (EIO);
			}

			/* Skip directory checksum tail entry */
			if (letoh32(ep->e4d_ino) == 0 &&
			    ep->e4d_namlen == 0 &&
			    ep->e4d_type == EXT4FS_DIR_TAIL_FT &&
			    reclen == EXT4FS_DIR_TAIL_SIZE) {
				off += reclen;
				blkoff += reclen;
				continue;
			}

			/* Track free space for CREATE/RENAME */
			if ((nameiop == CREATE || nameiop == RENAME) &&
			    slotoffset == -1) {
				int freespace;

				if (letoh32(ep->e4d_ino) == 0) {
					freespace = reclen;
				} else {
					freespace = reclen -
					    EXT4FS_DIRSIZ(ep->e4d_namlen);
				}
				if (freespace >= slotneeded) {
					slotoffset = off;
					slotsize = reclen;
				}
			}

			if (letoh32(ep->e4d_ino) != 0 &&
			    ep->e4d_namlen == cnp->cn_namelen &&
			    memcmp(cnp->cn_nameptr, ep->e4d_name,
			    cnp->cn_namelen) == 0) {
				foundino = letoh32(ep->e4d_ino);
				dp->i_ino = foundino;
				dp->i_reclen = reclen;
				dp->i_offset = off;
				/* For DELETE: count = prev entry to this */
				if (nameiop == DELETE && prevoff != -1)
					dp->i_count = off - prevoff;
				else
					dp->i_count = 0;
				brelse(bp);
				goto found;
			}

			prevoff = off;
			off += reclen;
			blkoff += reclen;
		}

		brelse(bp);
	}

	/* Not found */
	if ((nameiop == CREATE || nameiop == RENAME) && (flags & ISLASTCN)) {
		if (vdp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if ((error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred,
		    cnp->cn_proc)) != 0)
			return (error);
		/* Save free slot info for direnter */
		if (slotoffset == -1) {
			dp->i_offset = filesz;
			dp->i_count = 0;
		} else {
			dp->i_offset = slotoffset;
			dp->i_count = slotsize;
		}
		cnp->cn_flags |= SAVENAME;
		if (!lockparent) {
			VOP_UNLOCK(vdp);
			cnp->cn_flags |= PDIRUNLOCK;
		}
		return (EJUSTRETURN);
	}

	if ((cnp->cn_flags & MAKEENTRY) && nameiop != CREATE)
		cache_enter(vdp, *vpp, cnp);
	return (ENOENT);

found:
	if ((flags & ISLASTCN) && nameiop == LOOKUP)
		dp->i_diroff = EXT4FS_LBLKNO(fs, dp->i_offset) *
		    fs->m_block_size;

	/*
	 * If deleting, and at end of pathname, return parameters
	 * which can be used to remove file.  If the wantparent flag
	 * isn't set, we return only the directory (in ndp->ni_dvp),
	 * otherwise we go on and lock the inode, being careful with ".".
	 */
	if (nameiop == DELETE && (flags & ISLASTCN)) {
		if ((error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred,
		    cnp->cn_proc)) != 0)
			return (error);
		if (dp->i_number == foundino) {
			vref(vdp);
			*vpp = vdp;
			return (0);
		}
		if ((error = VFS_VGET(vdp->v_mount, foundino, &tdp)) != 0)
			return (error);
		*vpp = tdp;
		if (!lockparent) {
			VOP_UNLOCK(vdp);
			cnp->cn_flags |= PDIRUNLOCK;
		}
		return (0);
	}

	/*
	 * If rewriting (RENAME), return the inode and the
	 * information required to rewrite the present directory
	 * Must get inode of directory entry to verify it's a
	 * regular file, or empty directory.
	 */
	if (nameiop == RENAME && (flags & ISLASTCN)) {
		if ((error = VOP_ACCESS(vdp, VWRITE, cnp->cn_cred,
		    cnp->cn_proc)) != 0)
			return (error);
		if (dp->i_number == foundino)
			return (EISDIR);
		if ((error = VFS_VGET(vdp->v_mount, foundino, &tdp)) != 0)
			return (error);
		*vpp = tdp;
		cnp->cn_flags |= SAVENAME;
		if (!lockparent) {
			VOP_UNLOCK(vdp);
			cnp->cn_flags |= PDIRUNLOCK;
		}
		return (0);
	}

	if (flags & ISDOTDOT) {
		/* ".." - unlock parent, get child, optionally relock */
		VOP_UNLOCK(vdp);
		cnp->cn_flags |= PDIRUNLOCK;
		error = VFS_VGET(vdp->v_mount, foundino, &tdp);
		if (error) {
			if (vn_lock(vdp, LK_EXCLUSIVE | LK_RETRY) == 0)
				cnp->cn_flags &= ~PDIRUNLOCK;
			return (error);
		}
		if (lockparent && (flags & ISLASTCN)) {
			if ((error = vn_lock(vdp, LK_EXCLUSIVE)) != 0) {
				vput(tdp);
				return (error);
			}
			cnp->cn_flags &= ~PDIRUNLOCK;
		}
		*vpp = tdp;
	} else if (dp->i_number == foundino) {
		/* "." - return same vnode */
		vref(vdp);
		*vpp = vdp;
	} else {
		if ((error = VFS_VGET(vdp->v_mount, foundino, &tdp)) != 0)
			return (error);
		if (!lockparent || !(flags & ISLASTCN)) {
			VOP_UNLOCK(vdp);
			cnp->cn_flags |= PDIRUNLOCK;
		}
		*vpp = tdp;
	}

	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(vdp, *vpp, cnp);
	return (0);
}

/*
 * Common code to create a new inode and enter it in a directory.
 */
static int
ext4fs_makeinode(int mode, struct vnode *dvp, struct vnode **vpp,
    struct componentname *cnp)
{
	struct inode *ip, *pdir;
	struct vnode *tvp;
	struct ext4fs_dinode *din;
	int error;

	pdir = VTOI(dvp);

	*vpp = NULL;
	if ((mode & S_IFMT) == 0)
		mode |= S_IFREG;


	error = ext4fs_inode_alloc(pdir, mode, cnp->cn_cred, &tvp);
	if (error) {
		pool_put(&namei_pool, cnp->cn_pnbuf);
		return (error);
	}

	ip = VTOI(tvp);
	din = &ip->i_e4din->dinode;
	/* Set owner from cred and parent */
	din->i_uid_lo = htole16(cnp->cn_cred->cr_uid & 0xFFFF);
	din->i_uid_hi = htole16((cnp->cn_cred->cr_uid >> 16) & 0xFFFF);
	{
		gid_t gid = letoh16(pdir->i_e4din->dinode.i_gid_lo) |
		    ((gid_t)letoh16(pdir->i_e4din->dinode.i_gid_hi) << 16);
		din->i_gid_lo = htole16(gid & 0xFFFF);
		din->i_gid_hi = htole16((gid >> 16) & 0xFFFF);
	}

	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	din->i_mode = htole16(mode);
	tvp->v_type = IFTOVT(mode);
	ip->i_effnlink = 1;
	din->i_links_count = htole16(1);

	/* Clear SGID if not group member */
	if ((mode & ISGID) &&
	    !groupmember(letoh16(din->i_gid_lo) |
	    ((gid_t)letoh16(din->i_gid_hi) << 16), cnp->cn_cred) &&
	    suser_ucred(cnp->cn_cred))
		din->i_mode = htole16(letoh16(din->i_mode) & ~ISGID);

	/* Write inode to disk before directory entry */
	if ((error = ext4fs_update(ip, 1)) != 0)
		goto bad;
	error = ext4fs_direnter(ip, dvp, cnp);
	if (error != 0)
		goto bad;

	if ((cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, cnp->cn_pnbuf);
	*vpp = tvp;
	return (0);

bad:
	pool_put(&namei_pool, cnp->cn_pnbuf);
	ip->i_effnlink = 0;
	din->i_links_count = htole16(0);
	ip->i_flag |= IN_CHANGE;
	tvp->v_type = VNON;
	vput(tvp);
	return (error);
}

int
ext4fs_create(void *v)
{
	struct vop_create_args *ap = v;
	return (ext4fs_makeinode(
	    MAKEIMODE(ap->a_vap->va_type, ap->a_vap->va_mode),
	    ap->a_dvp, ap->a_vpp, ap->a_cnp));
}

int
ext4fs_mknod(void *v)
{
	struct vop_mknod_args *ap = v;
	struct vnode **vpp = ap->a_vpp;
	struct vnode *tvp;
	struct inode *ip;
	int error;

	error = ext4fs_makeinode(
	    MAKEIMODE(ap->a_vap->va_type, ap->a_vap->va_mode),
	    ap->a_dvp, &tvp, ap->a_cnp);
	if (error)
		return (error);

	ip = VTOI(tvp);

	/* Store device number */
	if (ap->a_vap->va_rdev != VNOVAL) {
		/* Old format in i_block[0], new format in i_block[1] */
		ip->i_e4din->dinode.i_block[0] =
		    htole32(ap->a_vap->va_rdev);
		ip->i_e4din->dinode.i_block[1] =
		    htole32(ap->a_vap->va_rdev);
	}

	ip->i_flag |= IN_CHANGE | IN_UPDATE;
	ext4fs_update(ip, 1);

	*vpp = tvp;
	return (0);
}

int
ext4fs_open(void *v)
{
	struct vop_open_args *ap = v;
	struct inode *ip = VTOI(ap->a_vp);
	u_int32_t iflags = letoh32(ip->i_e4din->dinode.i_flags);

	/* Deny write access to immutable files, non-append to append-only */
	if ((iflags & EXTFS_INODE_FLAG_IMMUTABLE) &&
	    (ap->a_mode & (FWRITE | O_TRUNC)))
		return (EPERM);
	if ((iflags & EXTFS_INODE_FLAG_APPEND) &&
	    (ap->a_mode & (FWRITE | O_TRUNC)) &&
	    !(ap->a_mode & O_APPEND))
		return (EPERM);

	return (0);
}

int
ext4fs_access(void *v)
{
	struct vop_access_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int32_t iflags;
	mode_t mode;
	uid_t uid;
	gid_t gid;

	/* Deny write access to immutable files */
	iflags = letoh32(din->i_flags);
	if ((ap->a_mode & VWRITE) && (iflags & EXTFS_INODE_FLAG_IMMUTABLE))
		return (EPERM);

	mode = letoh16(din->i_mode);
	uid = letoh16(din->i_uid_lo) |
	    ((uid_t)letoh16(din->i_uid_hi) << 16);
	gid = letoh16(din->i_gid_lo) |
	    ((gid_t)letoh16(din->i_gid_hi) << 16);

	return (vaccess(vp->v_type, mode, uid, gid, ap->a_mode, ap->a_cred));
}

int
ext4fs_getattr(void *v)
{
	struct vop_getattr_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);

	struct ext4fs_dinode_256 *din = ip->i_e4din;
	struct vattr *vap = ap->a_vap;

	/* Copy from inode table */
	vap->va_fsid = ip->i_dev;
	vap->va_fileid = ip->i_number;
	vap->va_mode = letoh16(din->dinode.i_mode) & ALLPERMS;
	vap->va_nlink = letoh16(din->dinode.i_links_count);
	vap->va_uid = letoh16(din->dinode.i_uid_lo);
	vap->va_uid |= (uid_t)letoh16(din->dinode.i_uid_hi) << 16;
	vap->va_gid = letoh16(din->dinode.i_gid_lo);
	vap->va_gid |= (gid_t)letoh16(din->dinode.i_gid_hi) << 16;
	vap->va_rdev = 0;
	vap->va_size = letoh32(din->dinode.i_size_lo);
	vap->va_size |= (off_t)letoh32(din->dinode.i_size_hi) << 32;

	/* Convert timestamps with nanosecond precision */
	vap->va_atime.tv_sec = letoh32(din->dinode.i_atime);
	vap->va_atime.tv_nsec = letoh32(din->dinode.i_atime_extra) >> 2;
	vap->va_mtime.tv_sec = letoh32(din->dinode.i_mtime);
	vap->va_mtime.tv_nsec = letoh32(din->dinode.i_mtime_extra) >> 2;
	vap->va_ctime.tv_sec = letoh32(din->dinode.i_ctime);
	vap->va_ctime.tv_nsec = letoh32(din->dinode.i_ctime_extra) >> 2;

	vap->va_flags = 0;
	vap->va_gen = letoh32(din->dinode.i_nfs_generation);

	/* Set appropriate block size */
	if (vp->v_type == VBLK)
		vap->va_blocksize = BLKDEV_IOSIZE;
	else if (vp->v_type == VCHR)
		vap->va_blocksize = MAXBSIZE;
	else
		vap->va_blocksize = vp->v_mount->mnt_stat.f_iosize;

	vap->va_bytes = letoh32(din->dinode.i_blocks_lo);
	vap->va_bytes |= (off_t)letoh16(din->dinode.i_blocks_hi) << 32;
	vap->va_bytes *= DEV_BSIZE;
	vap->va_type = vp->v_type;
	vap->va_filerev = 0;

	return (0);
}

int
ext4fs_chmod(struct vnode *vp, mode_t mode, struct ucred *cred)
{
	struct inode *ip = VTOI(vp);

	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	uid_t uid;
	gid_t gid;
	u_int16_t cur_mode;
	int error;

	uid = letoh16(din->i_uid_lo) |
	    ((uid_t)letoh16(din->i_uid_hi) << 16);
	gid = letoh16(din->i_gid_lo) |
	    ((gid_t)letoh16(din->i_gid_hi) << 16);

	if (cred->cr_uid != uid && (error = suser_ucred(cred)))
		return (error);
	if (cred->cr_uid) {
		if (vp->v_type != VDIR && (mode & S_ISTXT))
			return (EFTYPE);
		if (!groupmember(gid, cred) && (mode & ISGID))
			return (EPERM);
	}

	cur_mode = letoh16(din->i_mode);
	cur_mode &= ~ALLPERMS;
	cur_mode |= (mode & ALLPERMS);
	din->i_mode = htole16(cur_mode);
	ip->i_flag |= IN_CHANGE;

	if ((vp->v_flag & VTEXT) && (cur_mode & S_ISTXT) == 0)
		(void)uvm_vnp_uncache(vp);

	return (0);
}

int
ext4fs_chown(struct vnode *vp, uid_t uid, gid_t gid, struct ucred *cred)
{
	struct inode *ip = VTOI(vp);

	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	uid_t ouid;
	gid_t ogid;
	u_int16_t mode;
	int error;

	ouid = letoh16(din->i_uid_lo) |
	    ((uid_t)letoh16(din->i_uid_hi) << 16);
	ogid = letoh16(din->i_gid_lo) |
	    ((gid_t)letoh16(din->i_gid_hi) << 16);

	if (uid == (uid_t)VNOVAL)
		uid = ouid;
	if (gid == (gid_t)VNOVAL)
		gid = ogid;

	if ((cred->cr_uid != ouid || uid != ouid ||
	    (gid != ogid && !groupmember(gid, cred))) &&
	    (error = suser_ucred(cred)))
		return (error);

	din->i_uid_lo = htole16(uid & 0xFFFF);
	din->i_uid_hi = htole16((uid >> 16) & 0xFFFF);
	din->i_gid_lo = htole16(gid & 0xFFFF);
	din->i_gid_hi = htole16((gid >> 16) & 0xFFFF);

	if (ouid != uid || ogid != gid)
		ip->i_flag |= IN_CHANGE;
	if (ouid != uid && cred->cr_uid != 0) {
		mode = letoh16(din->i_mode);
		mode &= ~S_ISUID;
		din->i_mode = htole16(mode);
	}
	if (ogid != gid && cred->cr_uid != 0) {
		mode = letoh16(din->i_mode);
		mode &= ~S_ISGID;
		din->i_mode = htole16(mode);
	}

	return (0);
}

int
ext4fs_setattr(void *v)
{
	struct vop_setattr_args *ap = v;
	struct vattr *vap = ap->a_vap;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ucred *cred = ap->a_cred;

	int error = 0;

	if ((vap->va_type != VNON) || (vap->va_nlink != VNOVAL) ||
	    (vap->va_fsid != VNOVAL) || (vap->va_fileid != VNOVAL) ||
	    (vap->va_blocksize != VNOVAL) || (vap->va_rdev != VNOVAL) ||
	    ((int)vap->va_bytes != VNOVAL) || (vap->va_gen != VNOVAL))
		return (EINVAL);

	if (vap->va_flags != VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		if ((error = suser_ucred(cred)))
			return (error);
		u_int32_t iflags = letoh32(din->i_flags);
		iflags &= ~(EXTFS_INODE_FLAG_APPEND |
		    EXTFS_INODE_FLAG_IMMUTABLE);
		iflags |= (vap->va_flags & SF_APPEND) ?
		    EXTFS_INODE_FLAG_APPEND : 0;
		iflags |= (vap->va_flags & SF_IMMUTABLE) ?
		    EXTFS_INODE_FLAG_IMMUTABLE : 0;
		din->i_flags = htole32(iflags);
		ip->i_flag |= IN_CHANGE;
	}

	if (vap->va_uid != (uid_t)VNOVAL ||
	    vap->va_gid != (gid_t)VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		error = ext4fs_chown(vp, vap->va_uid, vap->va_gid, cred);
		if (error)
			return (error);
	}

	if (vap->va_size != VNOVAL) {
		switch (vp->v_type) {
		case VDIR:
			return (EISDIR);
		case VLNK:
		case VREG:
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
		error = ext4fs_truncate(ip, vap->va_size, 0, cred);
		if (error)
			return (error);
	}

	if ((vap->va_vaflags & VA_UTIMES_CHANGE) ||
	    vap->va_atime.tv_nsec != VNOVAL ||
	    vap->va_mtime.tv_nsec != VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		uid_t uid = letoh16(din->i_uid_lo) |
		    ((uid_t)letoh16(din->i_uid_hi) << 16);
		if (cred->cr_uid != uid &&
		    (error = suser_ucred(cred)) &&
		    ((vap->va_vaflags & VA_UTIMES_NULL) == 0 ||
		    (error = VOP_ACCESS(vp, VWRITE, cred, ap->a_p))))
			return (error);
		if (vap->va_mtime.tv_nsec != VNOVAL)
			ip->i_flag |= IN_CHANGE | IN_UPDATE;
		else if (vap->va_vaflags & VA_UTIMES_CHANGE)
			ip->i_flag |= IN_CHANGE;
		if (vap->va_atime.tv_nsec != VNOVAL)
			ip->i_flag |= IN_ACCESS;
		EXT4FS_ITIMES(ip);
		if (vap->va_mtime.tv_nsec != VNOVAL) {
			din->i_mtime =
			    htole32((u_int32_t)vap->va_mtime.tv_sec);
			din->i_mtime_extra =
			    htole32(vap->va_mtime.tv_nsec << 2);
		}
		if (vap->va_atime.tv_nsec != VNOVAL) {
			din->i_atime =
			    htole32((u_int32_t)vap->va_atime.tv_sec);
			din->i_atime_extra =
			    htole32(vap->va_atime.tv_nsec << 2);
		}
		ip->i_flag |= IN_MODIFIED;
		error = ext4fs_update(ip, 1);
		if (error)
			return (error);
	}

	if (vap->va_mode != (mode_t)VNOVAL) {
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return (EROFS);
		error = ext4fs_chmod(vp, vap->va_mode, cred);
	}

	return (error);
}

int
ext4fs_read(void *v)
{
	struct vop_read_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct uio *uio = ap->a_uio;
	struct buf *bp;
	off_t filesz, bytesinfile;
	daddr_t lbn, nextlbn;
	int error, blkoffset, xfersize, size;

	if (vp->v_type == VDIR)
		return (EISDIR);
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	for (error = 0, bp = NULL; uio->uio_resid > 0; bp = NULL) {
		bytesinfile = filesz - uio->uio_offset;
		if (bytesinfile <= 0)
			break;

		lbn = EXT4FS_LBLKNO(fs, uio->uio_offset);
		nextlbn = lbn + 1;
		blkoffset = EXT4FS_BLKOFF(fs, uio->uio_offset);
		size = fs->m_block_size;

		xfersize = size - blkoffset;
		if (uio->uio_resid < xfersize)
			xfersize = uio->uio_resid;
		if (bytesinfile < xfersize)
			xfersize = bytesinfile;

		if ((u_int64_t)nextlbn * fs->m_block_size >= filesz)
			error = bread(vp, lbn, size, &bp);
		else if (lbn - 1 == ip->i_ci.ci_lastr ||
		    uio->uio_resid > xfersize)
			error = bread_cluster(vp, lbn, size, &bp);
		else
			error = bread(vp, lbn, size, &bp);
		if (error)
			break;
		ip->i_ci.ci_lastr = lbn;

		/*
		 * We should only get non-zero b_resid when an I/O error
		 * has occurred, which should cause us to break above.
		 * However, if the short read did not cause an error,
		 * then we want to ensure that we do not uiomove bad
		 * or uninitialized data.
		 */
		size -= bp->b_resid;
		if (size < xfersize) {
			if (size == 0)
				break;
			xfersize = size;
		}

		error = uiomove((char *)bp->b_data + blkoffset, xfersize, uio);
		if (error)
			break;
		brelse(bp);
	}
	if (bp != NULL)
		brelse(bp);

	if (!(vp->v_mount->mnt_flag & MNT_NOATIME))
		ip->i_flag |= IN_ACCESS;

	return (error);
}

int
ext4fs_write(void *v)
{
	struct vop_write_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct inode *ip = VTOI(vp);
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct buf *bp;
	off_t filesz;
	u_int64_t lbn, pblk, ncontig, prealloc_start;
	u_int32_t prealloc_count, prealloc_got, prealloc_i;
	u_int64_t i_blocks;
	int ioflag = ap->a_ioflag;
	int blkoffset, xfersize;
	int error;
	size_t resid;
	ssize_t overrun;

	if (uio->uio_resid == 0)
		return (0);

	switch (vp->v_type) {
	case VREG:
		break;
	case VLNK:
		break;
	case VDIR:
		return (EOPNOTSUPP);
	default:
		panic("ext4fs_write: type");
	}

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	if (ioflag & IO_APPEND)
		uio->uio_offset = filesz;

	if (uio->uio_offset < 0)
		return (EINVAL);

	if ((error = vn_fsizechk(vp, uio, ioflag, &overrun)))
		return (error);

	resid = uio->uio_resid;

	for (error = 0; uio->uio_resid > 0; ) {
		lbn = EXT4FS_LBLKNO(fs, uio->uio_offset);
		blkoffset = EXT4FS_BLKOFF(fs, uio->uio_offset);
		xfersize = fs->m_block_size - blkoffset;
		if (uio->uio_resid < xfersize)
			xfersize = uio->uio_resid;

		/*
		 * For full-block writes past EOF, batch-allocate
		 * contiguous blocks for the remaining write.
		 */
		if (blkoffset == 0 && xfersize == fs->m_block_size &&
		    uio->uio_offset >= filesz &&
		    (ext4fs_extent_pblk(ip, lbn, &pblk, &ncontig) != 0 ||
		    pblk == 0)) {
			/* Count full blocks remaining in this write */
			prealloc_count = uio->uio_resid / fs->m_block_size;
			if (prealloc_count > 32768)
				prealloc_count = 32768;
			if (prealloc_count == 0)
				prealloc_count = 1;
			/* Goal: contiguous with last extent */
			pblk = 0;
			if (letoh16(din->i_extent_header.eh_entries) > 0) {
				u_int64_t dummy;
				u_int64_t nc;
				/* Use lbn-1 to find last mapped block */
				if (lbn > 0 && ext4fs_extent_pblk(ip,
				    lbn - 1, &dummy, &nc) == 0 &&
				    dummy != 0)
					pblk = dummy + 1;
			}
			error = ext4fs_blkalloc(ip, pblk, prealloc_count,
			    &prealloc_start, &prealloc_got);
			if (error)
				break;
			error = ext4fs_extent_insert(ip, lbn,
			    prealloc_start, prealloc_got);
			if (error) {
				for (prealloc_i = 0;
				    prealloc_i < prealloc_got;
				    prealloc_i++)
					ext4fs_blkfree(ip,
					    prealloc_start + prealloc_i);
				break;
			}
			i_blocks = letoh32(din->i_blocks_lo) |
			    ((u_int64_t)letoh16(din->i_blocks_hi) << 32);
			i_blocks += (u_int64_t)prealloc_got *
			    (fs->m_block_size / DEV_BSIZE);
			din->i_blocks_lo = htole32((u_int32_t)i_blocks);
			din->i_blocks_hi =
			    htole16((u_int16_t)(i_blocks >> 32));
			din->i_flags |=
			    htole32(EXTFS_INODE_FLAG_EXTENTS);
			ip->i_flag |= IN_CHANGE | IN_MODIFIED;
			/* Now use the first allocated block */
			pblk = prealloc_start;
		} else if (ext4fs_extent_pblk(ip, lbn, &pblk,
		    &ncontig) == 0 && pblk != 0) {
			/* Already mapped */
		} else {
			/* Partial block or not past EOF: single alloc */
			error = ext4fs_buf_alloc(ip, lbn, fs->m_block_size,
			    ap->a_cred, &bp, B_CLRBUF);
			if (error)
				break;
			goto do_io;
		}

		/* Full block: getblk without read; partial: bread */
		if (blkoffset == 0 && xfersize == fs->m_block_size) {
			bp = getblk(ip->i_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
			    fs->m_block_size, 0, INFSLP);
		} else {
			error = bread(ip->i_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
			    fs->m_block_size, &bp);
			if (error) {
				brelse(bp);
				break;
			}
		}
do_io:
		error = uiomove((char *)bp->b_data + blkoffset, xfersize,
		    uio);
		if (error) {
			brelse(bp);
			break;
		}

		if (ioflag & IO_SYNC)
			(void)bwrite(bp);
		else if (xfersize + blkoffset == fs->m_block_size)
			bawrite(bp);
		else
			bdwrite(bp);
		(void)uvm_vnp_uncache(vp);
		if (xfersize == 0)
			break;

		/* Update file size if we wrote past end */
		if (uio->uio_offset > filesz) {
			ext4fs_setsize(ip, uio->uio_offset);
			filesz = uio->uio_offset;
			uvm_vnp_setsize(vp, filesz);
		}

		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}

	/* Clear setuid/setgid bits on write by non-root */
	if (resid > uio->uio_resid && ap->a_cred &&
	    ap->a_cred->cr_uid != 0) {
		u_int16_t mode = letoh16(din->i_mode);
		mode &= ~(S_ISUID | S_ISGID);
		din->i_mode = htole16(mode);
	}

	if (error == 0 && resid > uio->uio_resid && (ioflag & IO_SYNC))
		error = ext4fs_update(ip, 1);

	uio->uio_resid += overrun;
	return (error);
}

int
ext4fs_fsync(void *v)
{
	struct vop_fsync_args *ap = v;
	struct vnode *vp = ap->a_vp;


	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return (0);

	vflushbuf(vp, ap->a_waitfor == MNT_WAIT);
	return (ext4fs_update(VTOI(vp), ap->a_waitfor == MNT_WAIT));
}

int
ext4fs_remove(void *v)
{
	struct vop_remove_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct inode *ip = VTOI(vp);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int16_t nlink;
	int error;

	if (vp->v_type == VDIR) {
		error = EPERM;
		goto out;
	}

	/* Cannot remove immutable or append-only files */
	if (letoh32(din->i_flags) &
	    (EXTFS_INODE_FLAG_IMMUTABLE | EXTFS_INODE_FLAG_APPEND)) {
		error = EPERM;
		goto out;
	}

	error = ext4fs_dirremove(dvp, ap->a_cnp);
	if (error)
		goto out;

	nlink = letoh16(din->i_links_count);
	if (nlink > 0)
		nlink--;
	din->i_links_count = htole16(nlink);
	ip->i_effnlink = nlink;
	ip->i_flag |= IN_CHANGE;

out:
	return (error);
}

int
ext4fs_link(void *v)
{
	struct vop_link_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vnode *vp = ap->a_vp;
	struct componentname *cnp = ap->a_cnp;
	struct inode *ip = VTOI(vp);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int16_t nlink;
	int error;

	if (vp->v_type == VDIR) {
		error = EPERM;
		goto out2;
	}
	if (dvp->v_mount != vp->v_mount) {
		error = EXDEV;
		goto out2;
	}

	nlink = letoh16(din->i_links_count);
	if (nlink >= EXT4FS_LINK_MAX) {
		error = EMLINK;
		goto out2;
	}

	if ((error = vn_lock(vp, LK_EXCLUSIVE)) != 0)
		goto out2;

	nlink++;
	din->i_links_count = htole16(nlink);
	ip->i_effnlink = nlink;
	ip->i_flag |= IN_CHANGE;
	error = ext4fs_update(ip, 1);
	if (error)
		goto out1;

	error = ext4fs_direnter(ip, dvp, cnp);
	if (error) {
		nlink--;
		din->i_links_count = htole16(nlink);
		ip->i_effnlink = nlink;
		ip->i_flag |= IN_CHANGE;
	}

out1:
	if (dvp != vp)
		VOP_UNLOCK(vp);
out2:
	vput(dvp);
	return (error);
}

/*
 * Check if source is an ancestor of target in the directory hierarchy.
 * Prevents creating directory loops via rename.
 * target vnode must be locked on entry and will be vput on exit.
 */
static int
ext4fs_checkpath(struct inode *source, struct inode *target, struct ucred *cred)
{
	struct vnode *vp;
	struct m_ext4fs *fs = source->i_e4fs;
	u_int32_t ino;
	int error = 0;

	vp = ITOV(target);
	if (target->i_number == source->i_number) {
		error = EEXIST;
		goto out;
	}
	if (target->i_number == ROOTINO)
		goto out;

	for (;;) {
		struct inode *ip = VTOI(vp);
		struct buf *bp;
		struct ext4fs_directory *dot, *dotdot;
		u_int64_t pblk;

		if (vp->v_type != VDIR) {
			error = ENOTDIR;
			break;
		}

		/* Read ".." from first directory block */
		error = ext4fs_extent_pblk(ip, 0, &pblk, NULL);
		if (error || pblk == 0) {
			if (!error) error = EIO;
			break;
		}
		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			break;
		}

		/* ".." is the second entry after "." */
		dot = (struct ext4fs_directory *)bp->b_data;
		dotdot = (struct ext4fs_directory *)
		    ((char *)bp->b_data + letoh16(dot->e4d_reclen));
		if (dotdot->e4d_namlen != 2 ||
		    dotdot->e4d_name[0] != '.' ||
		    dotdot->e4d_name[1] != '.') {
			brelse(bp);
			error = ENOTDIR;
			break;
		}
		ino = letoh32(dotdot->e4d_ino);
		brelse(bp);

		if (ino == source->i_number) {
			error = EINVAL;
			break;
		}
		if (ino == ROOTINO)
			break;

		VOP_UNLOCK(vp);
		error = VFS_VGET(vp->v_mount, ino, &vp);
		if (error) {
			vp = NULL;
			break;
		}
	}

out:
	if (error == ENOTDIR)
		printf("ext4fs_checkpath: .. not a directory\n");
	if (vp != NULL)
		vput(vp);
	return (error);
}

int
ext4fs_rename(void *v)
{
	struct vop_rename_args *ap = v;
	struct vnode *tvp = ap->a_tvp;
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *fdvp = ap->a_fdvp;
	struct componentname *tcnp = ap->a_tcnp;
	struct componentname *fcnp = ap->a_fcnp;
	struct inode *ip, *xp = NULL, *dp;
	struct ext4fs_dinode *din;
	int doingdirectory = 0, oldparent = 0, newparent = 0;
	int error = 0;
	u_int16_t nlink;

	/* Check for cross-device rename */
	if ((fvp->v_mount != tdvp->v_mount) ||
	    (tvp && (fvp->v_mount != tvp->v_mount))) {
		error = EXDEV;
abortit:
		VOP_ABORTOP(tdvp, tcnp);
		if (tdvp == tvp)
			vrele(tdvp);
		else
			vput(tdvp);
		if (tvp)
			vput(tvp);
		VOP_ABORTOP(fdvp, fcnp);
		vrele(fdvp);
		vrele(fvp);
		return (error);
	}

	/* Lock source */
	if ((error = vn_lock(fvp, LK_EXCLUSIVE)) != 0)
		goto abortit;

	dp = VTOI(fdvp);
	ip = VTOI(fvp);
	din = &ip->i_e4din->dinode;

	nlink = letoh16(din->i_links_count);
	if ((letoh32(din->i_flags) &
	    (EXTFS_INODE_FLAG_IMMUTABLE | EXTFS_INODE_FLAG_APPEND))) {
		VOP_UNLOCK(fvp);
		error = EPERM;
		goto abortit;
	}

	if ((letoh16(din->i_mode) & S_IFMT) == S_IFDIR) {
		doingdirectory = 1;
		oldparent = dp->i_number;
	}

	/* Bump link count temporarily for crash safety */
	nlink++;
	din->i_links_count = htole16(nlink);
	ip->i_effnlink = nlink;
	ip->i_flag |= IN_CHANGE;
	if ((error = ext4fs_update(ip, 1)) != 0) {
		VOP_UNLOCK(fvp);
		goto abortit;
	}

	/* Check write access for changing ".." */
	if (doingdirectory)
		error = VOP_ACCESS(fvp, VWRITE, tcnp->cn_cred,
		    tcnp->cn_proc);
	VOP_UNLOCK(fvp);
	vrele(fdvp);

	/*
	 * If ".." must be changed (ie the directory gets a new parent)
	 * then the source directory must not be in the directory
	 * hierarchy above the target.
	 */
	dp = VTOI(tdvp);
	if (oldparent != dp->i_number)
		newparent = dp->i_number;

	if (doingdirectory && newparent) {
		if (error)	/* write access check above */
			goto bad;
		if (tvp)
			vput(tvp);
		/* checkpath vput's tdvp, compensate */
		vref(tdvp);
		error = ext4fs_checkpath(ip, dp, tcnp->cn_cred);
		if (error) {
			vrele(tdvp);
			goto out;
		}
		if ((tcnp->cn_flags & SAVESTART) == 0)
			panic("ext4fs_rename: lost to startdir");
		error = vfs_relookup(tdvp, &tvp, tcnp);
		if (error) {
			vrele(tdvp);
			goto out;
		}
		vrele(tdvp);
		dp = VTOI(tdvp);
	}

	xp = NULL;
	if (tvp)
		xp = VTOI(tvp);

	/*
	 * 2) If target doesn't exist, link the target to the source
	 *    and unlink the source. Otherwise, rewrite the target
	 *    directory entry to reference the source inode.
	 */
	if (xp == NULL) {
		/*
		 * Account for ".." in new directory.
		 * When source and destination have the same
		 * parent we don't fool with the link count.
		 */
		if (doingdirectory && newparent) {
			u_int16_t pnlink = letoh16(
			    dp->i_e4din->dinode.i_links_count);
			pnlink++;
			dp->i_e4din->dinode.i_links_count = htole16(pnlink);
			dp->i_effnlink = pnlink;
			dp->i_flag |= IN_CHANGE;
			if ((error = ext4fs_update(dp, 1)) != 0)
				goto bad;
		}
		error = ext4fs_direnter(ip, tdvp, tcnp);
		if (error) {
			if (doingdirectory && newparent) {
				u_int16_t pnlink = letoh16(
				    dp->i_e4din->dinode.i_links_count);
				if (pnlink > 1)
					pnlink--;
				dp->i_e4din->dinode.i_links_count =
				    htole16(pnlink);
				dp->i_effnlink = pnlink;
				dp->i_flag |= IN_CHANGE;
				(void)ext4fs_update(dp, 1);
			}
			goto bad;
		}
		vput(tdvp);
	} else {
		/*
		 * Target exists. If replacing a directory,
		 * check that it is empty BEFORE rewriting
		 * the directory entry.
		 */
		if (doingdirectory) {
			if (!ext4fs_dirempty(xp, dp->i_number,
			    tcnp->cn_cred)) {
				error = ENOTEMPTY;
				goto bad;
			}
		}

		/* Rewrite the entry to point to source inode */
		error = ext4fs_dirrewrite(dp, ip, tcnp);
		if (error)
			goto bad;

		/*
		 * If the target directory is in the same
		 * directory as the source directory,
		 * decrement the link count on the parent
		 * of the target directory.
		 */
		if (doingdirectory && !newparent) {
			u_int16_t pnlink = letoh16(
			    dp->i_e4din->dinode.i_links_count);
			if (pnlink > 1)
				pnlink--;
			dp->i_e4din->dinode.i_links_count = htole16(pnlink);
			dp->i_effnlink = pnlink;
			dp->i_flag |= IN_CHANGE;
		}
		vput(tdvp);

		/*
		 * Adjust the link count of the target to
		 * reflect the dirrewrite above.
		 */
		{
			u_int16_t xnlink =
			    letoh16(xp->i_e4din->dinode.i_links_count);
			if (xnlink > 0)
				xnlink--;
			if (doingdirectory) {
				if (xnlink > 0)
					xnlink--;
				error = ext4fs_truncate(xp, 0, 0,
				    tcnp->cn_cred);
			}
			xp->i_e4din->dinode.i_links_count = htole16(xnlink);
			xp->i_effnlink = xnlink;
			xp->i_flag |= IN_CHANGE;
		}
		vput(tvp);
		xp = NULL;
	}

	/*
	 * 3) Unlink the source.
	 * Re-lookup the source entry to get correct i_offset/i_count,
	 * since the target lookup overwrites them (especially when
	 * fdvp == tdvp, i.e., same-directory rename).
	 */
	fcnp->cn_flags &= ~MODMASK;
	fcnp->cn_flags |= LOCKPARENT | LOCKLEAF;
	if ((fcnp->cn_flags & SAVESTART) == 0)
		panic("ext4fs_rename: lost from startdir");
	(void) vfs_relookup(fdvp, &fvp, fcnp);
	if (fvp != NULL) {
		xp = VTOI(fvp);
		dp = VTOI(fdvp);
	} else {
		if (doingdirectory)
			panic("ext4fs_rename: lost dir entry");
		vrele(ap->a_fvp);
		return (0);
	}

	if (xp != ip) {
		if (doingdirectory)
			panic("ext4fs_rename: lost dir entry");
	} else {
		/* If directory moved to new parent, update ".." */
		if (doingdirectory && newparent) {
			struct buf *dbp;
			struct ext4fs_directory *dotdot;
			u_int64_t dpblk;

			dp->i_e4din->dinode.i_links_count = htole16(
			    letoh16(dp->i_e4din->dinode.i_links_count) - 1);
			dp->i_effnlink--;
			dp->i_flag |= IN_CHANGE;

			error = ext4fs_extent_pblk(ip, 0, &dpblk, NULL);
			if (error == 0 && dpblk != 0) {
				error = bread(ip->i_devvp,
				    (daddr_t)EXT4FS_FSBTODB(ip->i_e4fs,
				    dpblk), ip->i_e4fs->m_block_size, &dbp);
				if (error == 0) {
					dotdot = (struct ext4fs_directory *)
					    ((char *)dbp->b_data +
					    letoh16(((struct ext4fs_directory *)
					    dbp->b_data)->e4d_reclen));
					dotdot->e4d_ino = htole32(newparent);
					ext4fs_dir_set_csum(ip->i_e4fs,
					    ip->i_number,
					    ip->i_e4din->dinode.
					    i_nfs_generation,
					    dbp->b_data);
					bwrite(dbp);
				} else
					brelse(dbp);
			}
		}

		error = ext4fs_dirremove(fdvp, fcnp);
		if (!error) {
			nlink = letoh16(
			    xp->i_e4din->dinode.i_links_count);
			if (nlink > 0)
				nlink--;
			xp->i_e4din->dinode.i_links_count = htole16(nlink);
			xp->i_effnlink = nlink;
			xp->i_flag |= IN_CHANGE;
		}
	}
	if (dp)
		vput(fdvp);
	if (xp)
		vput(fvp);
	vrele(ap->a_fvp);
	return (error);

bad:
	if (xp)
		vput(ITOV(xp));
	vput(ITOV(dp));
out:
	if (doingdirectory)
		ip->i_flag &= ~IN_RENAME;
	if (vn_lock(fvp, LK_EXCLUSIVE) == 0) {
		nlink = letoh16(ip->i_e4din->dinode.i_links_count);
		if (nlink > 0)
			nlink--;
		ip->i_e4din->dinode.i_links_count = htole16(nlink);
		ip->i_effnlink = nlink;
		ip->i_flag |= IN_CHANGE;
		vput(fvp);
	} else
		vrele(fvp);
	return (error);
}

int
ext4fs_mkdir(void *v)
{
	struct vop_mkdir_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vattr *vap = ap->a_vap;
	struct componentname *cnp = ap->a_cnp;
	struct inode *dp = VTOI(dvp);
	struct inode *ip;
	struct vnode *tvp;
	struct buf *bp;
	struct ext4fs_directory *dirp;
	struct ext4fs_dinode *din;
	struct m_ext4fs *fs = dp->i_e4fs;
	int error;
	u_int16_t nlink;

	nlink = letoh16(dp->i_e4din->dinode.i_links_count);
	if (nlink >= EXT4FS_LINK_MAX) {
		error = EMLINK;
		pool_put(&namei_pool, cnp->cn_pnbuf);
		goto out;
	}

	/* Allocate inode for new directory */
	error = ext4fs_inode_alloc(dp, S_IFDIR | vap->va_mode,
	    cnp->cn_cred, &tvp);
	if (error) {
		pool_put(&namei_pool, cnp->cn_pnbuf);
		goto out;
	}

	ip = VTOI(tvp);
	din = &ip->i_e4din->dinode;

	/* Set owner */
	din->i_uid_lo = htole16(cnp->cn_cred->cr_uid & 0xFFFF);
	din->i_uid_hi = htole16((cnp->cn_cred->cr_uid >> 16) & 0xFFFF);
	{
		gid_t gid = letoh16(dp->i_e4din->dinode.i_gid_lo) |
		    ((gid_t)letoh16(dp->i_e4din->dinode.i_gid_hi) << 16);
		din->i_gid_lo = htole16(gid & 0xFFFF);
		din->i_gid_hi = htole16((gid >> 16) & 0xFFFF);
	}

	ip->i_flag |= IN_ACCESS | IN_CHANGE | IN_UPDATE;
	din->i_mode = htole16(S_IFDIR | vap->va_mode);
	tvp->v_type = VDIR;
	ip->i_effnlink = 2;
	din->i_links_count = htole16(2);

	/* Allocate first block for "." and ".." */
	error = ext4fs_buf_alloc(ip, 0, fs->m_block_size, cnp->cn_cred,
	    &bp, B_CLRBUF);
	if (error)
		goto bad;

	/* Write "." entry */
	dirp = (struct ext4fs_directory *)bp->b_data;
	dirp->e4d_ino = htole32((u_int32_t)ip->i_number);
	dirp->e4d_reclen = htole16(12);
	dirp->e4d_namlen = 1;
	dirp->e4d_type = EXT4FS_FT_DIR;
	dirp->e4d_name[0] = '.';

	/* Write ".." entry */
	dirp = (struct ext4fs_directory *)((char *)bp->b_data + 12);
	dirp->e4d_ino = htole32((u_int32_t)dp->i_number);
	dirp->e4d_reclen = htole16(fs->m_block_size - 12 -
	    ((fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM) ? EXT4FS_DIR_TAIL_SIZE : 0));
	dirp->e4d_namlen = 2;
	dirp->e4d_type = EXT4FS_FT_DIR;
	dirp->e4d_name[0] = '.';
	dirp->e4d_name[1] = '.';

	ext4fs_dir_set_csum(fs, ip->i_number,
	    ip->i_e4din->dinode.i_nfs_generation, bp->b_data);
	error = bwrite(bp);
	if (error)
		goto bad;

	/* Set directory size */
	ext4fs_setsize(ip, fs->m_block_size);
	ip->i_flag |= IN_CHANGE | IN_UPDATE;

	/* Write inode before directory entry */
	if ((error = ext4fs_update(ip, 1)) != 0)
		goto bad;

	/* Increment parent's link count for ".." */
	nlink++;
	dp->i_e4din->dinode.i_links_count = htole16(nlink);
	dp->i_effnlink = nlink;
	dp->i_flag |= IN_CHANGE;
	if ((error = ext4fs_update(dp, 1)) != 0)
		goto bad;

	/* Enter new directory in parent */
	error = ext4fs_direnter(ip, dvp, cnp);
	if (error) {
		/* Undo parent nlink */
		nlink--;
		dp->i_e4din->dinode.i_links_count = htole16(nlink);
		dp->i_effnlink = nlink;
		dp->i_flag |= IN_CHANGE;
		goto bad;
	}

	if ((cnp->cn_flags & SAVESTART) == 0)
		pool_put(&namei_pool, cnp->cn_pnbuf);
	*ap->a_vpp = tvp;

	vput(dvp);
	return (0);

bad:
	pool_put(&namei_pool, cnp->cn_pnbuf);
	ip->i_effnlink = 0;
	din->i_links_count = htole16(0);
	ip->i_flag |= IN_CHANGE;
	tvp->v_type = VNON;
	vput(tvp);
out:
	vput(dvp);
	return (error);
}

int
ext4fs_rmdir(void *v)
{
	struct vop_rmdir_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct inode *ip = VTOI(vp);
	struct inode *dp = VTOI(dvp);
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int16_t nlink;
	int error;

	/* Directory must be empty */
	if (!ext4fs_dirempty(ip, dp->i_number, cnp->cn_cred)) {
		error = ENOTEMPTY;
		goto out;
	}

	/* Remove entry from parent */
	error = ext4fs_dirremove(dvp, cnp);
	if (error)
		goto out;

	/* Decrement parent's link count ("..") */
	nlink = letoh16(dp->i_e4din->dinode.i_links_count);
	if (nlink > 1)
		nlink--;
	dp->i_e4din->dinode.i_links_count = htole16(nlink);
	dp->i_effnlink = nlink;
	dp->i_flag |= IN_CHANGE;

	cache_purge(dvp);

	/* Set target link count to 0 */
	din->i_links_count = htole16(0);
	ip->i_effnlink = 0;
	ip->i_flag |= IN_CHANGE;

	/* Truncate directory contents */
	error = ext4fs_truncate(ip, 0, 0, cnp->cn_cred);

	cache_purge(vp);

out:
	if (dvp == vp)
		vrele(vp);
	else
		vput(vp);
	vput(dvp);
	return (error);
}

int
ext4fs_symlink(void *v)
{
	struct vop_symlink_args *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vattr *vap = ap->a_vap;
	struct componentname *cnp = ap->a_cnp;
	struct vnode **vpp = ap->a_vpp;
	struct inode *ip;
	int error, len;

	error = ext4fs_makeinode(S_IFLNK | vap->va_mode, dvp, vpp, cnp);
	if (error) {
		vput(dvp);
		return (error);
	}

	ip = VTOI(*vpp);
	len = strlen(ap->a_target);

	if (len <= EXT4FS_SYMLINK_LEN_MAX) {
		/* Fast symlink: store inline in i_block[] */
		memcpy(ip->i_e4din->dinode.i_block, ap->a_target, len);
		ext4fs_setsize(ip, len);
		/* Clear EXTENTS flag for fast symlinks */
		ip->i_e4din->dinode.i_flags &=
		    ~htole32(EXTFS_INODE_FLAG_EXTENTS);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
		error = ext4fs_update(ip, 1);
	} else {
		/* Slow symlink: write to data blocks */
		struct uio auio;
		struct iovec aiov;

		aiov.iov_base = ap->a_target;
		aiov.iov_len = len;
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_offset = 0;
		auio.uio_rw = UIO_WRITE;
		auio.uio_segflg = UIO_SYSSPACE;
		auio.uio_procp = NULL;
		auio.uio_resid = len;
		error = VOP_WRITE(*vpp, &auio, IO_NODELOCKED, ap->a_cnp->cn_cred);
	}

	vput(*vpp);
	vput(dvp);
	return (error);
}

int
ext4fs_readdir(void *v)
{
	struct vop_readdir_args *ap = v;
	struct uio *uio = ap->a_uio;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);

	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_directory *ep;
	struct dirent dstd;
	struct buf *bp;
	off_t off, filesz;
	u_int64_t lbn, pblk, blkoff;
	u_int16_t reclen;
	int error = 0;

	if (vp->v_type != VDIR)
		return (ENOTDIR);

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);
	off = uio->uio_offset;

	while (off < filesz && uio->uio_resid > 0) {
		lbn = EXT4FS_LBLKNO(fs, off);

		error = ext4fs_extent_pblk(ip, lbn, &pblk, NULL);
		if (error || pblk == 0) {
			if (!error) error = EIO;
			break;
		}

		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			break;
		}

		blkoff = EXT4FS_BLKOFF(fs, off);

		while (blkoff < fs->m_block_size && off < filesz) {
			ep = (struct ext4fs_directory *)
			    ((char *)bp->b_data + blkoff);
			reclen = letoh16(ep->e4d_reclen);

			if (reclen < 8 || reclen > fs->m_block_size ||
			    blkoff + reclen > fs->m_block_size) {
				error = EIO;
				brelse(bp);
				goto done;
			}

			if (letoh32(ep->e4d_ino) != 0) {
				u_int8_t namlen = ep->e4d_namlen;

				if (namlen > reclen - 8
#if MAXNAMLEN < 255
				    || namlen > MAXNAMLEN
#endif
				    ) {
					error = EIO;
					brelse(bp);
					goto done;
				}

				memset(&dstd, 0, sizeof(dstd));
				dstd.d_fileno = letoh32(ep->e4d_ino);
				dstd.d_namlen = namlen;

				if (ep->e4d_type < EXT4FS_FT_MAX)
					dstd.d_type =
					    ext4fs_type_to_dt[ep->e4d_type];
				else
					dstd.d_type = DT_UNKNOWN;

				memcpy(dstd.d_name, ep->e4d_name,
				    namlen);
				dstd.d_name[dstd.d_namlen] = '\0';
				dstd.d_reclen = DIRENT_SIZE(&dstd);
				dstd.d_off = off + reclen;

				if (dstd.d_reclen > uio->uio_resid) {
					brelse(bp);
					goto done;
				}

				error = uiomove(&dstd, dstd.d_reclen, uio);
				if (error) {
					brelse(bp);
					goto done;
				}
			}

			off += reclen;
			blkoff += reclen;
		}

		brelse(bp);
	}

done:
	uio->uio_offset = off;
	*ap->a_eofflag = (off >= filesz);
	return (error);
}

int
ext4fs_readlink(void *v)
{
	struct vop_readlink_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);

	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	u_int64_t filesz;

	filesz = (u_int64_t)letoh32(din->i_size_lo) |
	    ((u_int64_t)letoh32(din->i_size_hi) << 32);

	/* Fast symlink: target stored inline in i_block[] area */
	if (filesz <= EXT4FS_SYMLINK_LEN_MAX &&
	    !(letoh32(din->i_flags) & EXTFS_INODE_FLAG_EXTENTS)) {
		return (uiomove((char *)din->i_block, filesz, ap->a_uio));
	}

	/* Slow symlink: target stored in data blocks */
	return (VOP_READ(vp, ap->a_uio, 0, ap->a_cred));
}

/*
 * Enter a directory entry for inode ip into directory dvp.
 */
int
ext4fs_direnter(struct inode *ip, struct vnode *dvp,
    struct componentname *cnp)
{
	struct inode *dp = VTOI(dvp);
	struct m_ext4fs *fs = dp->i_e4fs;
	struct ext4fs_dinode *ddin = &dp->i_e4din->dinode;
	struct ext4fs_directory *ep, *nep;
	struct buf *bp;
	u_int64_t pblk;
	off_t filesz;
	int entrysize, error, loc;
	u_int16_t reclen, mode;

	entrysize = EXT4FS_DIRSIZ(cnp->cn_namelen);
	mode = letoh16(ip->i_e4din->dinode.i_mode);

	filesz = (off_t)letoh32(ddin->i_size_lo) |
	    ((off_t)letoh32(ddin->i_size_hi) << 32);

	if (dp->i_count == 0) {
		/*
		 * No free slot found - append at end of directory.
		 * Allocate a new block if needed.
		 */
		u_int64_t lbn = EXT4FS_LBLKNO(fs, filesz);
		u_int64_t blkoff = EXT4FS_BLKOFF(fs, filesz);

		if (blkoff == 0) {
			/* Need a new block */
			error = ext4fs_buf_alloc(dp, lbn, fs->m_block_size,
			    cnp->cn_cred, &bp, B_CLRBUF);
			if (error)
				return (error);
		} else {
			error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
			if (error || pblk == 0)
				return (error ? error : EIO);
			error = bread(dp->i_devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
			    fs->m_block_size, &bp);
			if (error) {
				brelse(bp);
				return (error);
			}
		}

		/* Write entry at end */
		ep = (struct ext4fs_directory *)
		    ((char *)bp->b_data + blkoff);
		ep->e4d_ino = htole32((u_int32_t)ip->i_number);
		{
			int tail = (fs->m_feature_ro_compat &
			    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM) ?
			    EXT4FS_DIR_TAIL_SIZE : 0;
			if (blkoff == 0)
				ep->e4d_reclen =
				    htole16(fs->m_block_size - tail);
			else
				ep->e4d_reclen =
				    htole16(fs->m_block_size - blkoff - tail);
		}
		ep->e4d_namlen = cnp->cn_namelen;
		ep->e4d_type = ext4fs_mode_to_ft(mode);
		memcpy(ep->e4d_name, cnp->cn_nameptr, cnp->cn_namelen);

		ext4fs_dir_set_csum(fs, dp->i_number,
		    dp->i_e4din->dinode.i_nfs_generation, bp->b_data);
		error = bwrite(bp);
		if (error)
			return (error);

		/* Update directory size */
		if (blkoff == 0)
			ext4fs_setsize(dp, filesz + fs->m_block_size);
		else
			ext4fs_setsize(dp, filesz + entrysize);
		dp->i_flag |= IN_CHANGE | IN_UPDATE;
		return (ext4fs_update(dp, 1));
	}

	/*
	 * Found a free slot at dp->i_offset with dp->i_count bytes.
	 * Read the block and compact entries to make room.
	 */
	{
		u_int64_t lbn = EXT4FS_LBLKNO(fs, dp->i_offset);

		error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
		if (error || pblk == 0)
			return (error ? error : EIO);

		error = bread(dp->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			return (error);
		}
	}

	loc = EXT4FS_BLKOFF(fs, dp->i_offset);
	ep = (struct ext4fs_directory *)((char *)bp->b_data + loc);
	reclen = letoh16(ep->e4d_reclen);

	if (letoh32(ep->e4d_ino) == 0) {
		/* Unused entry - just overwrite */
		ep->e4d_ino = htole32((u_int32_t)ip->i_number);
		/* Keep reclen as is */
		ep->e4d_namlen = cnp->cn_namelen;
		ep->e4d_type = ext4fs_mode_to_ft(mode);
		memcpy(ep->e4d_name, cnp->cn_nameptr, cnp->cn_namelen);
	} else {
		/* Compact: shrink current entry, add new one after it */
		int oldentsz = EXT4FS_DIRSIZ(ep->e4d_namlen);

		nep = (struct ext4fs_directory *)
		    ((char *)ep + oldentsz);
		nep->e4d_ino = htole32((u_int32_t)ip->i_number);
		nep->e4d_reclen = htole16(reclen - oldentsz);
		nep->e4d_namlen = cnp->cn_namelen;
		nep->e4d_type = ext4fs_mode_to_ft(mode);
		memcpy(nep->e4d_name, cnp->cn_nameptr, cnp->cn_namelen);
		ep->e4d_reclen = htole16(oldentsz);
	}

	ext4fs_dir_set_csum(fs, dp->i_number,
	    dp->i_e4din->dinode.i_nfs_generation, bp->b_data);
	error = bwrite(bp);
	dp->i_flag |= IN_CHANGE | IN_UPDATE;
	if (error == 0)
		error = ext4fs_update(dp, 1);
	return (error);
}

/*
 * Remove a directory entry.
 */
int
ext4fs_dirremove(struct vnode *dvp, struct componentname *cnp)
{
	struct inode *dp = VTOI(dvp);
	struct m_ext4fs *fs = dp->i_e4fs;
	struct ext4fs_directory *ep, *prevep;
	struct buf *bp;
	u_int64_t lbn, pblk;
	int error, loc;

	lbn = EXT4FS_LBLKNO(fs, dp->i_offset);

	error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
	if (error || pblk == 0)
		return (error ? error : EIO);

	error = bread(dp->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
	    fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return (error);
	}

	loc = EXT4FS_BLKOFF(fs, dp->i_offset);
	ep = (struct ext4fs_directory *)((char *)bp->b_data + loc);

	if (dp->i_count == 0) {
		/* First entry in block: just zero the inode field */
		ep->e4d_ino = 0;
	} else {
		/* Merge with previous entry */
		int prevloc = EXT4FS_BLKOFF(fs, dp->i_offset - dp->i_count);
		prevep = (struct ext4fs_directory *)
		    ((char *)bp->b_data + prevloc);
		prevep->e4d_reclen = htole16(
		    letoh16(prevep->e4d_reclen) + letoh16(ep->e4d_reclen));
	}

	ext4fs_dir_set_csum(fs, dp->i_number,
	    dp->i_e4din->dinode.i_nfs_generation, bp->b_data);
	error = bwrite(bp);
	dp->i_flag |= IN_CHANGE | IN_UPDATE;
	return (error);
}

/*
 * Check if a directory is empty (contains only "." and "..").
 */
int
ext4fs_dirempty(struct inode *ip, ufsino_t parentino, struct ucred *cred)
{
	struct m_ext4fs *fs = ip->i_e4fs;
	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	struct ext4fs_directory *ep;
	struct buf *bp;
	off_t off, filesz;
	u_int64_t lbn, pblk, blkoff;
	u_int16_t reclen;
	int error;

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);

	for (off = 0; off < filesz; ) {
		lbn = EXT4FS_LBLKNO(fs, off);

		error = ext4fs_extent_pblk(ip, lbn, &pblk, NULL);
		if (error || pblk == 0)
			return (0);

		error = bread(ip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			return (0);
		}

		blkoff = EXT4FS_BLKOFF(fs, off);

		while (blkoff < fs->m_block_size && off < filesz) {
			ep = (struct ext4fs_directory *)
			    ((char *)bp->b_data + blkoff);
			reclen = letoh16(ep->e4d_reclen);

			if (reclen < 8 || reclen > fs->m_block_size ||
			    blkoff + reclen > fs->m_block_size) {
				brelse(bp);
				return (0);
			}

			if (letoh32(ep->e4d_ino) != 0) {
				if (ep->e4d_namlen > 2) {
					brelse(bp);
					return (0);
				}
				if (ep->e4d_name[0] != '.') {
					brelse(bp);
					return (0);
				}
				if (ep->e4d_namlen == 1) {
					/* "." - ok */
				} else if (ep->e4d_name[1] == '.') {
					/* ".." - ok */
				} else {
					brelse(bp);
					return (0);
				}
			}

			off += reclen;
			blkoff += reclen;
		}

		brelse(bp);
	}

	return (1);
}

/*
 * Rewrite an existing directory entry to point to a new inode.
 */
int
ext4fs_dirrewrite(struct inode *dp, struct inode *ip,
    struct componentname *cnp)
{
	struct m_ext4fs *fs = dp->i_e4fs;
	struct ext4fs_directory *ep;
	struct buf *bp;
	u_int64_t lbn, pblk;
	u_int16_t mode;
	int error, loc;

	lbn = EXT4FS_LBLKNO(fs, dp->i_offset);

	error = ext4fs_extent_pblk(dp, lbn, &pblk, NULL);
	if (error || pblk == 0)
		return (error ? error : EIO);

	error = bread(dp->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, pblk),
	    fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return (error);
	}

	loc = EXT4FS_BLKOFF(fs, dp->i_offset);
	ep = (struct ext4fs_directory *)((char *)bp->b_data + loc);
	ep->e4d_ino = htole32((u_int32_t)ip->i_number);
	mode = letoh16(ip->i_e4din->dinode.i_mode);
	ep->e4d_type = ext4fs_mode_to_ft(mode);

	ext4fs_dir_set_csum(fs, dp->i_number,
	    dp->i_e4din->dinode.i_nfs_generation, bp->b_data);
	error = bwrite(bp);
	dp->i_flag |= IN_CHANGE | IN_UPDATE;
	return (error);
}

int
ext4fs_inactive(void *v)
{
	struct vop_inactive_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	u_int16_t mode, nlink = 1;
	int error = 0;
#ifdef DIAGNOSTIC
	extern int prtactive;

	if (prtactive && vp->v_usecount != 0)
		vprint("ext4fs_inactive: pushing active", vp);
#endif

	/*
	 * Ignore inodes related to stale file handles.
	 */
	if (ip->i_e4din == NULL) {
		goto out;
	}

	mode = letoh16(ip->i_e4din->dinode.i_mode);
	if (mode == 0) {
		goto out;
	}

	/*
	 * If the inode was deleted (dtime != 0), skip further processing.
	 */
	if (letoh32(ip->i_e4din->dinode.i_dtime) != 0) {
		goto out;
	}

	nlink = letoh16(ip->i_e4din->dinode.i_links_count);

	/*
	 * Handle file deletion: if nlink == 0, truncate data,
	 * free inode, and mark as deleted.
	 */
	if (nlink == 0 && (vp->v_mount->mnt_flag & MNT_RDONLY) == 0) {
		struct timespec ts;

		(void)ext4fs_truncate(ip, 0, 0, NOCRED);

		mode = letoh16(ip->i_e4din->dinode.i_mode);
		ip->i_e4din->dinode.i_mode = htole16(0);

		ext4fs_inode_free(ip, ip->i_number, mode);

		getnanotime(&ts);
		ip->i_e4din->dinode.i_dtime =
		    htole32((u_int32_t)ts.tv_sec);
		ip->i_flag |= IN_CHANGE | IN_UPDATE;
	}

	if (ip->i_flag & (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE)) {
		ext4fs_update(ip, nlink == 0 ? 1 : 0);
	}

out:
	VOP_UNLOCK(vp);

	/*
	 * If we are done with the inode, reclaim it
	 * so that it can be reused immediately.
	 * NOTE: after vrecycle, ip is freed (use-after-free danger).
	 */
	if (ip->i_e4din == NULL ||
	    letoh16(ip->i_e4din->dinode.i_mode) == 0)
		vrecycle(vp, ap->a_p);

	return (error);
}

int
ext4fs_reclaim(void *v)
{
	struct vop_reclaim_args *ap = v;
	struct vnode *vp = ap->a_vp;
	struct inode *ip = VTOI(vp);
	int error;

	if ((error = ufs_reclaim(vp)) != 0)
		return (error);

	if (ip->i_e4din != NULL)
		pool_put(&ext4fs_dinode_pool, ip->i_e4din);

	pool_put(&ext4fs_inode_pool, ip);

	vp->v_data = NULL;

	return (0);
}

int
ext4fs_bmap(void *v)
{
	struct vop_bmap_args *ap = v;
	struct inode *ip = VTOI(ap->a_vp);
	struct m_ext4fs *fs = ip->i_e4fs;
	u_int64_t pblk, ncontig;
	int error;

	if (ap->a_vpp != NULL)
		*ap->a_vpp = ip->i_devvp;
	if (ap->a_bnp == NULL)
		return (0);

	error = ext4fs_extent_pblk(ip, (u_int64_t)ap->a_bn, &pblk, &ncontig);
	if (error) {
		*ap->a_bnp = -1;
		return (error);
	}

	if (pblk == 0) {
		/* Hole — no physical block allocated */
		*ap->a_bnp = -1;
		if (ap->a_runp != NULL)
			*ap->a_runp = 0;
		return (0);
	}

	*ap->a_bnp = (daddr_t)EXT4FS_FSBTODB(fs, pblk);

	if (ap->a_runp != NULL) {
		int maxrun = MAXBSIZE / fs->m_block_size - 1;
		*ap->a_runp = MIN((int)(ncontig - 1), maxrun);
		if (*ap->a_runp < 0)
			*ap->a_runp = 0;
	}

	return (0);
}

int
ext4fs_strategy(void *v)
{
	struct vop_strategy_args *ap = v;
	struct buf *bp = ap->a_bp;
	struct vnode *vp = bp->b_vp;
	struct inode *ip;
	int error;
	int s;

	ip = VTOI(vp);
	if (vp->v_type == VBLK || vp->v_type == VCHR)
		panic("ext4fs_strategy: spec");

	if (bp->b_blkno == bp->b_lblkno) {
		error = VOP_BMAP(vp, bp->b_lblkno, NULL, &bp->b_blkno,
		    NULL);
		if (error) {
			bp->b_error = error;
			bp->b_flags |= B_ERROR;
			s = splbio();
			biodone(bp);
			splx(s);
			return (error);
		}
		if (bp->b_blkno == -1)
			clrbuf(bp);
	}
	if (bp->b_blkno == -1) {
		s = splbio();
		biodone(bp);
		splx(s);
		return (0);
	}
	vp = ip->i_devvp;
	bp->b_dev = vp->v_rdev;
	VOP_STRATEGY(vp, bp);
	return (0);
}

int
ext4fs_print(void *v)
{
	struct vop_print_args *ap = v;
	struct inode *ip = VTOI(ap->a_vp);

	printf("tag VT_EXT4FS, ino %llu, on dev %d, %d",
	    (unsigned long long)ip->i_number,
	    major(ip->i_dev), minor(ip->i_dev));
	printf(" flags 0x%x, effnlink %d\n",
	    ip->i_flag, ip->i_effnlink);
	return (0);
}

int
ext4fs_pathconf(void *v)
{
	struct vop_pathconf_args *ap = v;


	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = EXT4FS_LINK_MAX;
		break;
	case _PC_NAME_MAX:
		*ap->a_retval = EXT4FS_MAXNAMLEN;
		break;
	case _PC_PATH_MAX:
		*ap->a_retval = PATH_MAX;
		break;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		break;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		break;
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		break;
	case _PC_TIMESTAMP_RESOLUTION:
		*ap->a_retval = 1;
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

int
ext4fs_advlock(void *v)
{
	struct vop_advlock_args *ap = v;
	struct inode *ip = VTOI(ap->a_vp);

	struct ext4fs_dinode *din = &ip->i_e4din->dinode;
	off_t filesz;

	filesz = (off_t)letoh32(din->i_size_lo) |
	    ((off_t)letoh32(din->i_size_hi) << 32);
	return (lf_advlock(&ip->i_lockf, filesz, ap->a_id, ap->a_op,
	    ap->a_fl, ap->a_flags));
}
