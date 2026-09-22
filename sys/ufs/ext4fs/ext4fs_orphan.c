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
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/mount.h>
#include <sys/vnode.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>

#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_orphan.h>

int
ext4fs_orphan_cleanup_inode (struct mount *mp, u_int32_t ino, int *count)
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
		if (error) {
			printf("ext4fs: orphan inode %u: "
			    "truncate failed: %d\n", ino, error);
			vput(vp);
			return (error);
		}
		error = ext4fs_update(ip, 1);
		if (error) {
			vput(vp);
			return (error);
		}
	} else {
		printf("ext4fs: orphan inode %u: deleting\n", ino);
	}

	vput(vp);
	(*count)++;
	return (0);
}

int
ext4fs_orphan_file_scan (struct mount *mp, int *count)
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

	/*
	 * The orphan-file block checksum recipe is not yet implemented in
	 * IABSD.  Processing entries without authenticating the entire file
	 * could free blocks named by corrupt metadata, so fail closed.
	 */
	printf("ext4fs: orphan-file recovery is not yet supported\n");
	vput(ovp);
	return (EOPNOTSUPP);

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
			ext4fs_orphan_cleanup_inode(mp, ino, count);
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
ext4fs_orphan_cleanup (struct mount *mp)
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

	/*
	 * Orphan recovery must be journaled or otherwise restartable before it
	 * can safely mutate an image during mount.  Fail closed until that path
	 * is implemented; do not clear either orphan root.
	 */
	printf("ext4fs: orphan cleanup is not yet supported safely\n");
	return (EOPNOTSUPP);

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
			return (error);
		}
		ip = VTOI(vp);
		next = letoh32(ip->i_e4din->dinode.i_dtime);
		vput(vp);

		error = ext4fs_orphan_cleanup_inode(mp, ino, &count);
		if (error)
			return (error);
		if (next > fs->m_inodes_count || count > fs->m_inodes_count) {
			printf("ext4fs: corrupt orphan inode list\n");
			return (EINVAL);
		}
		ino = next;
	}

	fs->m_last_orphan = 0;
	fs->m_sble.sb_last_orphan = htole32(0);

	/* Scan orphan file if ORPHAN_PRESENT */
	if (fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT) {
		u_int32_t ro;

		error = ext4fs_orphan_file_scan(mp, &count);
		if (error)
			return (error);

		ro = letoh32(fs->m_sble.sb_feature_ro_compat);
		ro &= ~EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT;
		fs->m_sble.sb_feature_ro_compat = htole32(ro);
		fs->m_feature_ro_compat = ro;
	}

	fs->m_fs_was_modified = 1;

	printf("ext4fs: orphan cleanup: %d inodes\n", count);
	return (0);
}
