/*
 * Copyright (c) 2025 kmx.io.
 * Copyright (c) 1997 Manuel Bouyer.
 * Copyright (c) 1989, 1991, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
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
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
//#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/disk.h>
//#include <sys/mbuf.h>
#include <sys/fcntl.h>
//#include <sys/disklabel.h>
//#include <sys/ioctl.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/rwlock.h>
#include <sys/stat.h>
#include <sys/dkio.h>
#include <sys/specdev.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/inode.h>
//#include <ufs/ufs/dir.h>
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_extern.h>
#include <ufs/ext4fs/ext4fs_journal.h>

struct pool ext4fs_inode_pool;
struct pool ext4fs_dinode_pool;

#define PRINTF_FEATURES(mask, features)				\
	for (i = 0; i < nitems(features); i++)			\
		if ((mask) & (features)[i].f_mask)		\
			printf("%s ", (features)[i].f_name)

int	ext4fs_block_group_has_super_block(int);
int	ext4fs_mountfs(struct vnode *, struct mount *, struct proc *);
int	ext4fs_sbcheck(struct ext4fs *, int);
void	ext4fs_sbload(struct ext4fs *, struct m_ext4fs *);
int	ext4fs_sbfill(struct vnode *, struct m_ext4fs *);

const struct vfsops ext4fs_vfsops = {
	.vfs_mount	= ext4fs_mount,
	.vfs_start	= ufs_start,
	.vfs_unmount	= ext4fs_unmount,
	.vfs_root	= ufs_root,
	.vfs_quotactl	= ufs_quotactl,
	.vfs_statfs	= ext4fs_statfs,
	.vfs_sync	= ext4fs_sync,
	.vfs_vget	= ext4fs_vget,
	.vfs_fhtovp	= ext4fs_fhtovp,
	.vfs_vptofh	= ext4fs_vptofh,
	.vfs_init	= ext4fs_init,
	.vfs_sysctl	= ext4fs_sysctl,
	.vfs_checkexp	= ufs_check_export,
};

struct pool ext4fs_inode_pool;

int
ext4fs_block_group_has_super_block(int group)
{
	int a3, a5, a7;

	if (group == 0 || group == 1)
		return 1;
	for (a3 = 3, a5 = 5, a7 = 7;
	    a3 <= group || a5 <= group || a7 <= group;
	    a3 *= 3, a5 *= 5, a7 *= 7)
		if (group == a3 || group == a5 || group == a7)
			return 1;
	return 0;
}

int
ext4fs_fhtovp(struct mount *mp, struct fid *fhp, struct vnode **vpp)
{
	(void)mp;
	(void)fhp;
	(void)vpp;
	printf("ext4fs_fhtovp: not implemented\n");
	return (EOPNOTSUPP);
}

/*
 * Flush out all the files in a filesystem.
 */
int
ext4fs_flushfiles(struct mount *mp, int flags, struct proc *p)
{
	struct ufsmount *ump;
	int error;

	ump = VFSTOUFS(mp);
	/*
	 * Flush all the files.
	 */
	if ((error = vflush(mp, NULL, flags)) != 0)
		return (error);
	/*
	 * Flush filesystem metadata.
	 */
	vn_lock(ump->um_devvp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_FSYNC(ump->um_devvp, p->p_ucred, MNT_WAIT, p);
	VOP_UNLOCK(ump->um_devvp);
	return (error);
}

int
ext4fs_init(struct vfsconf *vfsp)
{
	int result;
	(void)vfsp;
	pool_init(&ext4fs_inode_pool, sizeof(struct inode), 0,
		  IPL_NONE, PR_WAITOK, "ext4inopl", NULL);
	pool_init(&ext4fs_dinode_pool, sizeof(struct ext4fs_dinode_256), 0,
		  IPL_NONE, PR_WAITOK, "ext4dinopl", NULL);
	if ((result = ufs_init(vfsp))) {
		return result;
	}
	return (0);
}

int
ext4fs_mount(struct mount *mp, const char *path, void *data,
	struct nameidata *ndp, struct proc *p)
{
	struct ufs_args *args;
	struct vnode *devvp;
	int error;
	struct m_ext4fs *mfs;
	char fname[MNAMELEN];
	char fspec[MNAMELEN];
	struct ufsmount *ump = NULL;

	args = data;
	error = copyinstr(args->fspec, fspec, sizeof(fspec), NULL);
	if (error)
		goto error;

	if (disk_map(fspec, fname, MNAMELEN, DM_OPENBLCK) == -1)
		memcpy(fname, fspec, sizeof(fname));

	NDINIT(ndp, LOOKUP, FOLLOW, UIO_SYSSPACE, fname, p);
	if ((error = namei(ndp)) != 0)
		goto error;
	devvp = ndp->ni_vp;

	if (devvp->v_type != VBLK) {
		error = ENOTBLK;
		goto error_devvp;
	}
	if (major(devvp->v_rdev) >= nblkdev) {
		error = ENXIO;
		goto error_devvp;
	}
	if ((mp->mnt_flag & MNT_UPDATE) == 0) {
		error = ext4fs_mountfs(devvp, mp, p);
	} else {
		ump = VFSTOUFS(mp);
		if (devvp != ump->um_devvp)
			error = EINVAL;	/* XXX needs translation */
		else
			vrele(devvp);
	}
	if (error)
		goto error_devvp;
	ump = VFSTOUFS(mp);
	mfs = ump->um_e4fs;

	strlcpy(mp->mnt_stat.f_mntfromname, fname,
		sizeof(mp->mnt_stat.f_mntfromname));
	strlcpy(mp->mnt_stat.f_mntonname, path,
		sizeof(mp->mnt_stat.f_mntonname));

	goto success;

error_devvp:
	vrele(devvp);

error:
success:
	return (error);
}

/*
 * Common code for mount and mountroot
 */
int
ext4fs_mountfs(struct vnode *devvp, struct mount *mp, struct proc *p)
{
	struct ufsmount *ump;
	struct buf *bp;
	struct ext4fs *sble;
	struct m_ext4fs *mfs;
	dev_t dev;
	int error, ronly;
	struct ucred *cred;

	dev = devvp->v_rdev;
	cred = p ? p->p_ucred : NOCRED;
	/*
	 * Disallow multiple mounts of the same device.
	 * Disallow mounting of a device that is currently in use
	 * (except for root, which might share swap device for miniroot).
	 * Flush out any old buffers remaining from a previous use.
	 */
	if ((error = vfs_mountedon(devvp)) != 0)
		return (error);
	if (vcount(devvp) > 1 && devvp != rootvp)
		return (EBUSY);
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	error = vinvalbuf(devvp, V_SAVE, cred, p, 0, INFSLP);
	VOP_UNLOCK(devvp);
	if (error != 0)
		return (error);

	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;
	error = VOP_OPEN(devvp, ronly ? FREAD : FREAD|FWRITE, FSCRED, p);
	if (error)
		return (error);

	bp = NULL;
	ump = NULL;

	/*
	 * Read the superblock from disk.
	 */
	error = bread(devvp, (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET /
				       DEV_BSIZE),
		      EXT4FS_SUPER_BLOCK_SIZE, &bp);
	if (error)
		goto out;
	sble = (struct ext4fs *)bp->b_data;
	error = ext4fs_sbcheck(sble, ronly);
	if (error)
		goto out;

	ump = malloc(sizeof *ump, M_UFSMNT, M_WAITOK | M_ZERO);
	mfs = ump->um_e4fs = malloc(sizeof(struct m_ext4fs), M_UFSMNT,
	    M_WAITOK | M_ZERO);

	/*
	 * Copy in the superblock, compute in-memory values
	 * and load group descriptors.
	 */
	ext4fs_sbload(sble, mfs);
	if ((error = ext4fs_sbfill(devvp, mfs)) != 0)
		goto out;
	brelse(bp);
	bp = NULL;
	sble = &mfs->m_sble;

	/*
	 * If the filesystem needs journal recovery, replay it now.
	 * For r/o mounts, we temporarily reopen the device r/w.
	 */
	if ((mfs->m_feature_compat & EXT4FS_FEATURE_COMPAT_HAS_JOURNAL) &&
	    (mfs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_RECOVER)) {
		int reopen_ro = 0;

		if (ronly) {
			/* Reopen device r/w for replay */
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
			VOP_CLOSE(devvp, FREAD, cred, p);
			VOP_UNLOCK(devvp);
			error = VOP_OPEN(devvp, FREAD | FWRITE, FSCRED, p);
			if (error) {
				printf("ext4fs: can't reopen device r/w "
				    "for journal replay\n");
				goto out;
			}
			reopen_ro = 1;
		}

		error = ext4fs_journal_replay(devvp, mfs);
		if (error) {
			printf("ext4fs: journal replay failed: %d\n", error);
			printf("ext4fs: use Linux e2fsck to repair\n");
			/* Leave RECOVER set, fail the mount */
			if (reopen_ro) {
				vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
				VOP_CLOSE(devvp, FREAD | FWRITE, cred, p);
				VOP_UNLOCK(devvp);
				VOP_OPEN(devvp, FREAD, FSCRED, p);
			}
			goto out;
		}

		/* Reopen device r/o if it was a r/o mount */
		if (reopen_ro) {
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
			VOP_CLOSE(devvp, FREAD | FWRITE, cred, p);
			VOP_UNLOCK(devvp);
			error = VOP_OPEN(devvp, FREAD, FSCRED, p);
			if (error) {
				printf("ext4fs: can't reopen device r/o\n");
				goto out;
			}
		}

		/*
		 * Replay may have changed group descriptors and
		 * superblock counters. Reload them.
		 */
		if (mfs->m_gd != NULL) {
			size_t gd_size = mfs->m_block_group_count *
			    sizeof(struct ext4fs_block_group_descriptor);
			free(mfs->m_gd, M_UFSMNT, gd_size);
			mfs->m_gd = NULL;
		}
		/* Re-read superblock from disk */
		error = bread(devvp,
		    (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE),
		    EXT4FS_SUPER_BLOCK_SIZE, &bp);
		if (error)
			goto out;
		ext4fs_sbload((struct ext4fs *)bp->b_data, mfs);
		brelse(bp);
		bp = NULL;

		error = ext4fs_sbfill(devvp, mfs);
		if (error)
			goto out;
		sble = &mfs->m_sble;
	}

	ump->um_e4fs->m_read_only = ronly;
	ump->um_fstype = UM_EXT4FS;

	if (ronly == 0) {
		if (mfs->m_state == EXT4FS_STATE_VALID)
			mfs->m_state = 0;
		else
			mfs->m_state = EXT4FS_STATE_ERROR;
		mfs->m_fs_was_modified = 1;
	}

	mp->mnt_data = ump;
	mp->mnt_stat.f_fsid.val[0] = (long)dev;
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	mp->mnt_stat.f_namemax = MAXNAMLEN;
	mp->mnt_flag |= MNT_LOCAL;
	ump->um_mountp = mp;

	ump->um_dev = dev;
	ump->um_devvp = devvp;
	ump->um_nindir = EXT4FS_NINDIR(mfs);
	ump->um_bptrtodb = mfs->m_fs_block_to_disk_block;
	ump->um_seqinc = 1; /* no frags */
	ump->um_maxsymlinklen = EXT4FS_SYMLINK_LEN_MAX;
	devvp->v_specmountpoint = mp;

	if (ronly == 0) {
		ext4fs_orphan_cleanup(mp);
		ext4fs_sbwrite(mp);
	}

	return (0);
out:
	if (devvp->v_specinfo)
		devvp->v_specmountpoint = NULL;
	if (bp)
		brelse(bp);
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)VOP_CLOSE(devvp, ronly ? FREAD : FREAD|FWRITE, cred, p);
	VOP_UNLOCK(devvp);
	if (ump) {
		if (mfs && mfs->m_gd != NULL) {
			size_t gd_size = mfs->m_block_group_count *
			    sizeof(struct ext4fs_block_group_descriptor);
			free(mfs->m_gd, M_UFSMNT, gd_size);
		}
		free(mfs, M_UFSMNT, sizeof *mfs);
		free(ump, M_UFSMNT, sizeof *ump);
		mp->mnt_data = NULL;
	}
	return (error);
}

int
ext4fs_sbcheck(struct ext4fs *sble, int ronly)
{
	u_int32_t mask, tmp;
	int i;

	tmp = letoh16(sble->sb_magic);
	if (tmp != EXT4FS_MAGIC) {
		printf("ext2fs: wrong magic number 0x%x\n", tmp);
		return (EIO);		/* XXX needs translation */
	}

	if (ext4fs_sb_csum_verify(sble) != 0) {
		printf("ext4fs: superblock checksum verification failed\n");
		return (EINVAL);
	}

	tmp = letoh32(sble->sb_log_block_size);
	if (tmp > 2) {
		/* skewed log(block size): 1024 -> 0 | 2048 -> 1 | 4096 -> 2 */
		tmp += 10;
		printf("ext2fs: wrong log2(block size) %d\n", tmp);
		return (EIO);	   /* XXX needs translation */
	}

	if (letoh32(sble->sb_blocks_per_group) == 0) {
		printf("ext4fs: zero blocks per group\n");
		return (EIO);
	}

	if (letoh32(sble->sb_inodes_per_group) == 0) {
		printf("ext4fs: zero inodes per group\n");
		return (EIO);
	}

	tmp = letoh32(sble->sb_revision_level);
	if (tmp != EXT4FS_REV_DYNAMIC) {
		printf("ext2fs: wrong revision number 0x%x\n", tmp);
		return (EIO);		/* XXX needs translation */
	}

	tmp = letoh16(sble->sb_inode_size);
	if (tmp != 256) {
		printf("ext4fs: unsupported inode size: %d\n", tmp);
		return (EINVAL);
	}

	tmp = letoh32(sble->sb_first_non_reserved_inode);
	if (tmp != EXT4FS_INODE_FIRST) {
		printf("ext4fs: first inode at 0x%x\n", tmp);
		return (EINVAL);      /* XXX needs translation */
	}

	tmp = letoh32(sble->sb_block_group_descriptor_size);
	if (tmp != sizeof(struct ext4fs_block_group_descriptor)) {
		printf("ext4fs: block group descriptor size is 0x%x\n",
		       tmp);
		return (EINVAL);
	}

	tmp = letoh32(sble->sb_feature_incompat);
	mask = tmp & ~EXT4FS_FEATURE_INCOMPAT_SUPPORTED;
	if (mask) {
		printf("ext4fs: unsupported incompat features: 0x%x ",
		    mask);
		PRINTF_FEATURES(mask, ext4fs_feature_incompat);
		printf("\n");
		return (EINVAL);      /* XXX needs translation */
	}

	if (tmp & EXT4FS_FEATURE_INCOMPAT_RECOVER) {
		printf("ext4fs: file system needs journal recovery\n");
		if (!(letoh32(sble->sb_feature_compat) &
		    EXT4FS_FEATURE_COMPAT_HAS_JOURNAL)) {
			printf("ext4fs: RECOVER set but no journal\n");
			return (EINVAL);
		}
		/* Allow mount to proceed; replay happens in mountfs */
	}

	tmp = letoh32(sble->sb_feature_ro_compat) &
		~EXT4FS_FEATURE_RO_COMPAT_SUPPORTED;
	if (!ronly && tmp) {
		printf("ext4fs: unsupported R/O compat features: 0x%x ",
		    tmp);
		PRINTF_FEATURES(tmp, ext4fs_feature_ro_compat);
		printf("\n");
		return (EROFS);
	}

	if (!ronly &&
	    !(letoh32(sble->sb_feature_incompat) &
	      EXT4FS_FEATURE_INCOMPAT_RECOVER) &&
	    !(letoh16(sble->sb_state) & EXT4FS_STATE_VALID)) {
		printf("ext4fs: file system not clean, run e2fsck\n");
		return (EROFS);
	}

	return (0);
}

int
ext4fs_sbfill(struct vnode *devvp, struct m_ext4fs *mfs)
{
	struct ext4fs_dinode *rdp;
	struct buf *bp;
	daddr_t dblk;
	u_int64_t ritb, rblk;
	u_int32_t rgroup, rindex, roff;
	size_t gd_size;
	int error, i;

	mfs->m_block_group_count = howmany(mfs->m_blocks_count -
					   mfs->m_first_data_block,
					   mfs->m_blocks_per_group);

	mfs->m_block_size_shift = EXT4FS_LOG_MIN_BLOCK_SIZE +
		mfs->m_log_block_size;
	mfs->m_block_size = 1 << mfs->m_block_size_shift;
	mfs->m_block_group_descriptor_blocks_count =
		howmany(mfs->m_block_group_count,
			mfs->m_block_size /
			sizeof(struct ext4fs_block_group_descriptor));
	mfs->m_fs_block_to_disk_block = mfs->m_log_block_size + 1;
	mfs->m_inodes_per_block = mfs->m_block_size / mfs->m_inode_size;
	mfs->m_inode_table_blocks_per_group = mfs->m_inodes_per_group /
		mfs->m_inodes_per_block;

	gd_size = mfs->m_block_group_count * sizeof(struct ext4fs_block_group_descriptor);
	mfs->m_gd = malloc(gd_size, M_UFSMNT, M_WAITOK);

	dblk = (mfs->m_first_data_block + 1) << mfs->m_fs_block_to_disk_block;
	for (i = 0; i < mfs->m_block_group_descriptor_blocks_count; i++) {
		size_t off = (size_t)i * mfs->m_block_size;
		size_t n = mfs->m_block_size;

		/* Don't copy past end of m_gd allocation */
		if (off + n > gd_size)
			n = gd_size - off;

		error = bread(devvp, dblk + (i << mfs->m_fs_block_to_disk_block),
		    mfs->m_block_size, &bp);
		if (error) {
			printf("ext4fs_sbfill: failed to read block group descriptors: %d\n", error);
			free(mfs->m_gd, M_UFSMNT, gd_size);
			mfs->m_gd = NULL;
			return (error);
		}
		memcpy((char *)mfs->m_gd + off, bp->b_data, n);
		brelse(bp);
	}

	/* Verify block group descriptor checksums */
	for (i = 0; i < mfs->m_block_group_count; i++) {
		if ((error = ext4fs_bgd_csum_verify(mfs, &mfs->m_gd[i],
		    i)) != 0) {
			printf("ext4fs_sbfill: block group %d checksum "
			    "verification failed\n", i);
			free(mfs->m_gd, M_UFSMNT, gd_size);
			mfs->m_gd = NULL;
			return (error);
		}
	}

	/*
	 * Read the resize inode (inode 7) to get its doubly-indirect
	 * block pointer. Needed for BLOCK_UNINIT bitmap reconstruction.
	 */
	mfs->m_resize_dind_block = 0;
	rgroup = (7 - 1) / mfs->m_inodes_per_group;
	rindex = (7 - 1) % mfs->m_inodes_per_group;
	ritb = letoh32(mfs->m_gd[rgroup].bgd_inode_table_block_lo);
	rblk = ritb + (rindex * mfs->m_inode_size) / mfs->m_block_size;
	roff = (rindex * mfs->m_inode_size) % mfs->m_block_size;
	error = bread(devvp, (daddr_t)EXT4FS_FSBTODB(mfs, rblk),
	    mfs->m_block_size, &bp);
	if (error) {
		brelse(bp);
	} else {
		rdp = (struct ext4fs_dinode *)
		    ((char *)bp->b_data + roff);
		mfs->m_resize_dind_block = letoh32(rdp->i_block[13]);
		brelse(bp);
	}

	return (0);
}

void
ext4fs_sbload(struct ext4fs *sble, struct m_ext4fs *dest)
{
	int feature_incompat_64bit;
	feature_incompat_64bit = letoh32(sble->sb_feature_incompat) &
		EXT4FS_FEATURE_INCOMPAT_64BIT;
	/* Keep a copy of the raw little-endian superblock */
	memcpy(&dest->m_sble, sble, sizeof(dest->m_sble));
	dest->m_inodes_count = letoh32(sble->sb_inodes_count);
	dest->m_blocks_count = letoh32(sble->sb_blocks_count_lo);
	dest->m_reserved_blocks_count =
		letoh32(sble->sb_reserved_blocks_count_lo);
	dest->m_free_blocks_count =
		letoh32(sble->sb_free_blocks_count_lo);
	dest->m_free_inodes_count = letoh32(sble->sb_free_inodes_count);
	dest->m_first_data_block = letoh32(sble->sb_first_data_block);
	dest->m_log_block_size = letoh32(sble->sb_log_block_size);
	dest->m_log_cluster_size = letoh32(sble->sb_log_cluster_size);
	dest->m_blocks_per_group = letoh32(sble->sb_blocks_per_group);
	dest->m_clusters_per_group = letoh32(sble->sb_clusters_per_group);
	dest->m_inodes_per_group = letoh32(sble->sb_inodes_per_group);
	dest->m_mount_time = letoh32(sble->sb_mount_time_lo);
	dest->m_write_time = letoh32(sble->sb_write_time_lo);
	dest->m_mount_count = letoh16(sble->sb_mount_count);
	dest->m_max_mount_count_before_fsck = (int16_t)letoh16(sble->sb_max_mount_count_before_fsck);
	dest->m_state = letoh16(sble->sb_state);
	dest->m_errors = letoh16(sble->sb_errors);
	dest->m_revision_level_minor = letoh16(sble->sb_revision_level_minor);
	dest->m_check_time = letoh32(sble->sb_check_time_lo);
	dest->m_check_interval = letoh32(sble->sb_check_interval);
	dest->m_creator_os = letoh32(sble->sb_creator_os);
	dest->m_revision_level = letoh32(sble->sb_revision_level);
	dest->m_default_reserved_uid = letoh16(sble->sb_default_reserved_uid);
	dest->m_default_reserved_gid = letoh16(sble->sb_default_reserved_gid);
	dest->m_first_non_reserved_inode = letoh32(sble->sb_first_non_reserved_inode);
	dest->m_inode_size = letoh16(sble->sb_inode_size);
	dest->m_block_group_id = letoh16(sble->sb_block_group_id);
	dest->m_feature_compat = letoh32(sble->sb_feature_compat);
	dest->m_feature_incompat = letoh32(sble->sb_feature_incompat);
	dest->m_feature_ro_compat = letoh32(sble->sb_feature_ro_compat);
	dest->m_algorithm_usage_bitmap = letoh32(sble->sb_algorithm_usage_bitmap);
	dest->m_reserved_bgdt_blocks = letoh16(sble->sb_reserved_bgdt_blocks);
	dest->m_journal_inode_number = letoh32(sble->sb_journal_inode_number);
	dest->m_journal_device_number = letoh32(sble->sb_journal_device_number);
	dest->m_last_orphan = letoh32(sble->sb_last_orphan);
	dest->m_block_group_descriptor_size = letoh16(sble->sb_block_group_descriptor_size);
	dest->m_default_mount_opts = letoh32(sble->sb_default_mount_opts);
	dest->m_first_meta_block_group = letoh32(sble->sb_first_meta_block_group);
	dest->m_newfs_time = letoh32(sble->sb_newfs_time_lo);
	dest->m_inode_size_extra_min = letoh16(sble->sb_inode_size_extra_min);
	dest->m_inode_size_extra_want = letoh16(sble->sb_inode_size_extra_want);
	dest->m_flags = letoh32(sble->sb_flags);
	dest->m_raid_stride_block_count = letoh16(sble->sb_raid_stride_block_count);
	dest->m_mmp_interval = letoh16(sble->sb_mmp_interval);
	dest->m_mmp_block = letoh64(sble->sb_mmp_block);
	dest->m_raid_stripe_width_block_count = letoh32(sble->sb_raid_stripe_width_block_count);
	dest->m_kilobytes_written = letoh64(sble->sb_kilobytes_written);
	dest->m_error_count = letoh32(sble->sb_error_count);
	dest->m_first_error_time = letoh32(sble->sb_first_error_time_lo);
	dest->m_first_error_inode = letoh32(sble->sb_first_error_inode);
	dest->m_first_error_block = letoh64(sble->sb_first_error_block);
	dest->m_first_error_line = letoh32(sble->sb_first_error_line);
	dest->m_last_error_time = letoh32(sble->sb_last_error_time_lo);
	dest->m_last_error_inode = letoh32(sble->sb_last_error_inode);
	dest->m_last_error_line = letoh32(sble->sb_last_error_line);
	dest->m_last_error_block = letoh64(sble->sb_last_error_block);
	dest->m_user_quota_inode = letoh32(sble->sb_user_quota_inode);
	dest->m_group_quota_inode = letoh32(sble->sb_group_quota_inode);
	dest->m_overhead_clusters = letoh32(sble->sb_overhead_clusters);
	dest->m_backup_block_groups[0] = letoh32(sble->sb_backup_block_groups[0]);
	dest->m_backup_block_groups[1] = letoh32(sble->sb_backup_block_groups[1]);
	dest->m_lost_and_found_inode = letoh32(sble->sb_lost_and_found_inode);
	dest->m_project_quota_inode = letoh32(sble->sb_project_quota_inode);
	dest->m_checksum_seed = letoh32(sble->sb_checksum_seed);
	dest->m_encoding = letoh16(sble->sb_encoding);
	dest->m_encoding_flags = letoh16(sble->sb_encoding_flags);
	dest->m_orphan_file_inode = letoh32(sble->sb_orphan_file_inode);
	if (feature_incompat_64bit) {
		dest->m_blocks_count |= (u_int64_t)
			letoh32(sble->sb_blocks_count_hi) << 32;
		dest->m_reserved_blocks_count |=	(u_int64_t)
			letoh32(sble->sb_reserved_blocks_count_hi)
			<< 32;
		dest->m_free_blocks_count |= (u_int64_t)
			letoh32(sble->sb_free_blocks_count_hi) << 32;
		dest->m_mount_time |= (u_int64_t)
			letoh32(sble->sb_mount_time_hi) << 32;
		dest->m_check_time |= (u_int64_t)
			letoh32(sble->sb_check_time_hi) << 32;
		dest->m_newfs_time |= (u_int64_t)
			letoh32(sble->sb_newfs_time_hi) << 32;
		dest->m_first_error_time |= (u_int64_t)
			letoh32(sble->sb_first_error_time_hi) << 32;
		dest->m_last_error_time |= (u_int64_t)
			letoh32(sble->sb_last_error_time_hi) << 32;
	}
}

int
ext4fs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p)
{
	struct ufsmount *ump;
	struct m_ext4fs *mfs;
	const u_int32_t overhead_per_group_block_bitmap = 1;
	const u_int32_t overhead_per_group_inode_bitmap = 1;
	u_int32_t overhead, overhead_per_group;
	int ngroups;

	(void)p;
	ump = VFSTOUFS(mp);
	mfs = ump->um_e4fs;

	overhead_per_group = overhead_per_group_block_bitmap +
		overhead_per_group_inode_bitmap +
		mfs->m_inode_table_blocks_per_group;
	overhead = mfs->m_first_data_block +
		mfs->m_block_group_count * overhead_per_group;
	if (mfs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER) {
		int i;
		for (i = 0, ngroups = 0; i < mfs->m_block_group_count; i++) {
			if (ext4fs_block_group_has_super_block(i))
				ngroups++;
		}
	} else {
		ngroups = mfs->m_block_group_count;
	}
	overhead += ngroups *
		(1 + mfs->m_block_group_descriptor_blocks_count);

	sbp->f_bsize = mfs->m_block_size;
	sbp->f_iosize = mfs->m_block_size;
	sbp->f_blocks = mfs->m_blocks_count - overhead;
	sbp->f_bfree = mfs->m_free_blocks_count;
	if (sbp->f_bfree > mfs->m_reserved_blocks_count)
		sbp->f_bavail = sbp->f_bfree - mfs->m_reserved_blocks_count;
	else
		sbp->f_bavail = 0;
	sbp->f_files = mfs->m_inodes_count;
	sbp->f_favail = sbp->f_ffree = mfs->m_free_inodes_count;
	copy_statfs_info(sbp, mp);

	return (0);
}

/*
 * Write a block group descriptor back to disk with updated checksum.
 */
int
ext4fs_bgd_write(struct m_ext4fs *fs, struct vnode *devvp, u_int32_t group)
{
	struct buf *bp;
	struct ext4fs_block_group_descriptor *gd;
	u_int32_t bgds_per_block, bgd_block, bgd_off;
	daddr_t dblk;
	int error;

	bgds_per_block = fs->m_block_size /
	    sizeof(struct ext4fs_block_group_descriptor);
	bgd_block = group / bgds_per_block;
	bgd_off = (group % bgds_per_block) *
	    sizeof(struct ext4fs_block_group_descriptor);

	dblk = (fs->m_first_data_block + 1 + bgd_block) <<
	    fs->m_fs_block_to_disk_block;

	error = bread(devvp, dblk, fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return (error);
	}

	/* Update in-memory checksum */
	gd = &fs->m_gd[group];
	gd->bgd_checksum = htole16(ext4fs_bgd_csum(fs, gd, group));

	/* Copy to buffer and write */
	memcpy((char *)bp->b_data + bgd_off, gd,
	    sizeof(struct ext4fs_block_group_descriptor));

	bdwrite(bp);
	return (0);
}

/*
 * Write the superblock to disk with updated counters and checksum.
 */
int
ext4fs_sbwrite(struct mount *mp)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	struct ext4fs *sble = &fs->m_sble;
	struct buf *bp;
	struct timespec ts;
	int error;

	/* Update counters in on-disk superblock */
	sble->sb_free_blocks_count_lo =
	    htole32((u_int32_t)fs->m_free_blocks_count);
	sble->sb_free_blocks_count_hi =
	    htole32((u_int32_t)(fs->m_free_blocks_count >> 32));
	sble->sb_free_inodes_count = htole32(fs->m_free_inodes_count);

	getnanotime(&ts);
	sble->sb_write_time_lo = htole32((u_int32_t)ts.tv_sec);

	sble->sb_state = htole16(fs->m_state);

	/* Recompute checksum */
	sble->sb_checksum = htole32(ext4fs_sb_csum(sble));

	/* Write to disk at the fixed superblock offset */
	error = bread(ump->um_devvp,
	    (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE),
	    EXT4FS_SUPER_BLOCK_SIZE, &bp);
	if (error) {
		brelse(bp);
		return (error);
	}

	memcpy(bp->b_data, sble, sizeof(struct ext4fs));
	return (bwrite(bp));
}

static u_long ext4fs_gennumber;

/*
 * Allocate an inode in the file system.
 */
int
ext4fs_inode_alloc(struct inode *pip, mode_t mode, struct ucred *cred,
    struct vnode **vpp)
{
	struct m_ext4fs *fs = pip->i_e4fs;
	struct vnode *pvp = ITOV(pip);
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp, *tbp;
	struct inode *ip;
	u_int32_t group, ngroups, ino_in_group, pbit, tb, it_blocks;
	u_int32_t best, best_free, fi, g, free_inodes, icsum;
	u_int32_t itu, first_unused, dirs;
	u_int64_t bitmap_blk, itb;
	ufsino_t ino;
	char *ibp;
	int error, i;

	*vpp = NULL;

	if (fs->m_free_inodes_count == 0)
		return (ENOSPC);

	ngroups = fs->m_block_group_count;

	/* Pick starting group */
	if ((mode & S_IFMT) == S_IFDIR) {
		best = 0;
		best_free = 0;
		for (i = 0; i < ngroups; i++) {
			fi = letoh16(fs->m_gd[i].bgd_free_inodes_count_lo);
			if (fi > best_free) {
				best_free = fi;
				best = i;
			}
		}
		group = best;
	} else {
		group = (pip->i_number - 1) / fs->m_inodes_per_group;
	}

	/* Scan groups starting from preferred */
	for (i = 0; i < ngroups; i++) {
		g = (group + i) % ngroups;
		gd = &fs->m_gd[g];
		free_inodes = letoh16(gd->bgd_free_inodes_count_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			free_inodes |=
			    (u_int32_t)letoh16(gd->bgd_free_inodes_count_hi)
			    << 16;
		if (free_inodes == 0)
			continue;

		/* Read inode bitmap */
		bitmap_blk = letoh32(gd->bgd_inode_bitmap_block_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			bitmap_blk |= (u_int64_t)
			    letoh32(gd->bgd_inode_bitmap_block_hi) << 32;

		error = bread(pip->i_devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, bitmap_blk),
		    fs->m_block_size, &bp);
		if (error) {
			brelse(bp);
			continue;
		}

		ibp = (char *)bp->b_data;

		/*
		 * If INODE_UNINIT, the bitmap is stale. Zero it,
		 * set padding bits, and zero all inode table blocks.
		 */
		if (letoh16(gd->bgd_flags) &
		    EXT4FS_BGD_FLAG_INODE_UNINIT) {
			memset(ibp, 0, fs->m_block_size);
			for (pbit = fs->m_inodes_per_group;
			    pbit < fs->m_block_size * 8; pbit++)
				setbit(ibp, pbit);

			itb = letoh32(gd->bgd_inode_table_block_lo);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				itb |= (u_int64_t)letoh32(
				    gd->bgd_inode_table_block_hi) << 32;
			it_blocks = fs->m_inode_table_blocks_per_group;
			for (tb = 0; tb < it_blocks; tb++) {
				error = bread(pip->i_devvp,
				    (daddr_t)EXT4FS_FSBTODB(fs,
				    itb + tb),
				    fs->m_block_size, &tbp);
				if (error) {
					brelse(tbp);
					continue;
				}
				memset(tbp->b_data, 0,
				    fs->m_block_size);
				error = bwrite(tbp);
			}
		}

		/* Find free inode bit */
		for (ino_in_group = 0; ino_in_group < fs->m_inodes_per_group;
		    ino_in_group++) {
			if (isclr(ibp, ino_in_group)) {
				setbit(ibp, ino_in_group);

				icsum = ext4fs_bitmap_csum(fs, g, ibp,
				    fs->m_inodes_per_group / 8);
				gd->bgd_inode_bitmap_checksum_lo =
				    htole16(icsum & 0xFFFF);
				if (fs->m_feature_incompat &
				    EXT4FS_FEATURE_INCOMPAT_64BIT)
					gd->bgd_inode_bitmap_checksum_hi
					    = htole16(
					    (icsum >> 16) & 0xFFFF);

				error = bwrite(bp);
				if (error)
					return (error);

				/* Compute inode number (1-based) */
				ino = g * fs->m_inodes_per_group +
				    ino_in_group + 1;

				/* Get vnode for new inode */
				error = VFS_VGET(pvp->v_mount, ino, vpp);
				if (error) {
					ext4fs_inode_free(pip, ino, mode);
					return (error);
				}

				/* Clear INODE_UNINIT flag if set */
				gd->bgd_flags = htole16(letoh16(gd->bgd_flags) &
				    ~EXT4FS_BGD_FLAG_INODE_UNINIT);

				/* Update BGD free count */
				free_inodes--;
				gd->bgd_free_inodes_count_lo =
				    htole16(free_inodes & 0xFFFF);
				if (fs->m_feature_incompat &
				    EXT4FS_FEATURE_INCOMPAT_64BIT)
					gd->bgd_free_inodes_count_hi =
					    htole16((free_inodes >> 16) &
					    0xFFFF);

				if ((mode & S_IFMT) == S_IFDIR) {
					dirs = letoh16(
					    gd->bgd_used_dirs_count_lo);
					if (fs->m_feature_incompat &
					    EXT4FS_FEATURE_INCOMPAT_64BIT)
						dirs |= (u_int32_t)letoh16(
						    gd->bgd_used_dirs_count_hi)
						    << 16;
					dirs++;
					gd->bgd_used_dirs_count_lo =
					    htole16(dirs & 0xFFFF);
					if (fs->m_feature_incompat &
					    EXT4FS_FEATURE_INCOMPAT_64BIT)
						gd->bgd_used_dirs_count_hi =
						    htole16((dirs >> 16) &
						    0xFFFF);
				}

				itu = letoh16(gd->bgd_inode_table_unused_lo);
				first_unused =
				    fs->m_inodes_per_group - itu;
				if (ino_in_group >= first_unused) {
					itu = fs->m_inodes_per_group -
					    ino_in_group - 1;
					gd->bgd_inode_table_unused_lo =
					    htole16(itu & 0xFFFF);
					if (fs->m_feature_incompat &
					    EXT4FS_FEATURE_INCOMPAT_64BIT)
						gd->bgd_inode_table_unused_hi =
						    htole16((itu >> 16) &
						    0xFFFF);
				}

				ext4fs_bgd_write(fs, pip->i_devvp, g);

				/* Update superblock counters */
				fs->m_free_inodes_count--;
				fs->m_sble.sb_free_inodes_count =
				    htole32(fs->m_free_inodes_count);
				fs->m_fs_was_modified = 1;

				ip = VTOI(*vpp);

				/* Zero the dinode */
				memset(ip->i_e4din, 0,
				    sizeof(struct ext4fs_dinode_256));

				/* Initialize extent header */
				ip->i_e4din->dinode.i_extent_header.eh_magic =
				    htole16(EXT4FS_EXTENT_HEADER_MAGIC);
				ip->i_e4din->dinode.i_extent_header.eh_entries =
				    htole16(0);
				ip->i_e4din->dinode.i_extent_header.eh_max =
				    htole16(4);
				ip->i_e4din->dinode.i_extent_header.eh_depth =
				    htole16(0);
				ip->i_e4din->dinode.i_flags =
				    htole32(EXTFS_INODE_FLAG_EXTENTS);

				/* Set extra_isize */
				ip->i_e4din->dinode.i_extra_isize =
				    htole16(sizeof(struct ext4fs_dinode) -
				    128);

				/* Set generation number */
				if (++ext4fs_gennumber <
				    (u_long)gettime())
					ext4fs_gennumber = gettime();
				ip->i_e4din->dinode.i_nfs_generation =
				    htole32(ext4fs_gennumber);

				return (0);
			}
		}

		brelse(bp);
	}

	return (ENOSPC);
}

/*
 * Free an inode.
 */
void
ext4fs_inode_free(struct inode *pip, ufsino_t ino, mode_t mode)
{
	struct m_ext4fs *fs = pip->i_e4fs;
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp;
	u_int64_t bitmap_blk;
	u_int32_t group, ino_in_group, free_inodes, icsum;
	char *ibp;
	int error;

	group = (ino - 1) / fs->m_inodes_per_group;
	ino_in_group = (ino - 1) % fs->m_inodes_per_group;
	gd = &fs->m_gd[group];

	bitmap_blk = letoh32(gd->bgd_inode_bitmap_block_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		bitmap_blk |=
		    (u_int64_t)letoh32(gd->bgd_inode_bitmap_block_hi) << 32;

	error = bread(pip->i_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, bitmap_blk),
	    fs->m_block_size, &bp);
	if (error) {
		brelse(bp);
		return;
	}

	ibp = (char *)bp->b_data;
	clrbit(ibp, ino_in_group);

	icsum = ext4fs_bitmap_csum(fs, group, ibp,
	    fs->m_inodes_per_group / 8);
	gd->bgd_inode_bitmap_checksum_lo = htole16(icsum & 0xFFFF);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		gd->bgd_inode_bitmap_checksum_hi =
		    htole16((icsum >> 16) & 0xFFFF);

	error = bwrite(bp);
	if (error)
		return;

	/* Update BGD */
	free_inodes = letoh16(gd->bgd_free_inodes_count_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		free_inodes |=
		    (u_int32_t)letoh16(gd->bgd_free_inodes_count_hi) << 16;
	free_inodes++;
	gd->bgd_free_inodes_count_lo = htole16(free_inodes & 0xFFFF);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		gd->bgd_free_inodes_count_hi =
		    htole16((free_inodes >> 16) & 0xFFFF);

	if ((mode & S_IFMT) == S_IFDIR) {
		u_int32_t dirs;
		dirs = letoh16(gd->bgd_used_dirs_count_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			dirs |= (u_int32_t)
			    letoh16(gd->bgd_used_dirs_count_hi) << 16;
		dirs--;
		gd->bgd_used_dirs_count_lo = htole16(dirs & 0xFFFF);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			gd->bgd_used_dirs_count_hi =
			    htole16((dirs >> 16) & 0xFFFF);
	}

	ext4fs_bgd_write(fs, pip->i_devvp, group);

	/* Update superblock counters */
	fs->m_free_inodes_count++;
	fs->m_sble.sb_free_inodes_count =
	    htole32(fs->m_free_inodes_count);
	fs->m_fs_was_modified = 1;
}

static int
ext4fs_sync_vnode(struct vnode *vp, void *arg)
{
	struct ext4fs_sync_args *esa = arg;
	struct inode *ip;
	int error, s, skip;

	if (vp->v_type == VNON)
		return (0);

	ip = VTOI(vp);
	if (ip == NULL || ip->i_e4din == NULL)
		return (0);

	s = splbio();
	skip = (ip->i_flag &
	    (IN_ACCESS | IN_CHANGE | IN_MODIFIED | IN_UPDATE)) == 0 &&
	    LIST_EMPTY(&vp->v_dirtyblkhd);
	splx(s);

	if (skip)
		return (0);

	if (vget(vp, LK_EXCLUSIVE | LK_NOWAIT))
		return (0);

	if ((error = VOP_FSYNC(vp, esa->cred, esa->waitfor, esa->p)) != 0)
		esa->allerror = error;

	vput(vp);
	return (0);
}

int
ext4fs_sync(struct mount *mp, int waitfor, int stall,
    struct ucred *cred, struct proc *p)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	struct ext4fs_sync_args esa;
	int error;

	if (fs->m_read_only)
		return (0);

	esa.p = p;
	esa.cred = cred;
	esa.allerror = 0;
	esa.waitfor = waitfor;

	vfs_mount_foreach_vnode(mp, ext4fs_sync_vnode, &esa);

	if (waitfor != MNT_LAZY) {
		vn_lock(ump->um_devvp, LK_EXCLUSIVE | LK_RETRY);
		if ((error = VOP_FSYNC(ump->um_devvp, cred, waitfor, p)))
			esa.allerror = error;
		VOP_UNLOCK(ump->um_devvp);
	}

	/* Write superblock if modified */
	if (fs->m_fs_was_modified) {
		if ((error = ext4fs_sbwrite(mp)))
			esa.allerror = error;
	}

	return (esa.allerror);
}

int
ext4fs_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p)
{
	(void)name;
	(void)namelen;
	(void)oldp;
	(void)oldlenp;
	(void)newp;
	(void)newlen;
	(void)p;
	printf("ext4fs_sysctl: not implemented\n");
	return (EOPNOTSUPP);
}

int
ext4fs_unmount(struct mount *mp, int mntflags, struct proc *p)
{
	struct ufsmount *ump;
	struct m_ext4fs *mfs;
	int error, flags;

	flags = 0;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	if ((error = ext4fs_flushfiles(mp, flags, p)) != 0)
		return (error);
	ump = VFSTOUFS(mp);
	mfs = ump->um_e4fs;

	if (!mfs->m_read_only && mfs->m_fs_was_modified) {
		mfs->m_state = EXT4FS_STATE_VALID;
		ext4fs_sbwrite(mp);
	}

	if (ump->um_devvp->v_type != VBAD)
		ump->um_devvp->v_specmountpoint = NULL;
	vn_lock(ump->um_devvp, LK_EXCLUSIVE | LK_RETRY);
	(void)VOP_CLOSE(ump->um_devvp, mfs->m_read_only ? FREAD :
	    FREAD|FWRITE, NOCRED, p);
	vput(ump->um_devvp);
	if (mfs->m_gd != NULL) {
		size_t gd_size = mfs->m_block_group_count *
		    sizeof(struct ext4fs_block_group_descriptor);
		free(mfs->m_gd, M_UFSMNT, gd_size);
	}
	free(mfs, M_UFSMNT, sizeof *mfs);
	free(ump, M_UFSMNT, sizeof *ump);
	mp->mnt_data = NULL;
	mp->mnt_flag &= ~MNT_LOCAL;
	return (0);
}

int
ext4fs_vget(struct mount *mp, ino_t ino, struct vnode **vpp)
{
	struct m_ext4fs *fs;
	struct inode *ip;
	struct ufsmount *ump;
	struct buf *bp;
	struct vnode *vp;
	struct ext4fs_block_group_descriptor *gd;
	struct ext4fs_dinode *dp;
	dev_t dev;
	daddr_t disk_block;
	u_int64_t inode_table_block;
	u_int32_t inode_group, inode_index, block_in_table, offset_in_block;
	u_int32_t itable_unused;
	u_int16_t bgd_flags, imode;
	int error;

	if (ino > (ufsino_t)-1)
		panic("ext4fs_vget: alien ino_t %llu",
		    (unsigned long long)ino);

	ump = VFSTOUFS(mp);
	dev = ump->um_dev;
	fs = ump->um_e4fs;

 retry:
	if ((*vpp = ufs_ihashget(dev, ino)) != NULL) {
		return (0);
	}

	/* Allocate a new vnode/inode. */
	if ((error = getnewvnode(VT_EXT4FS, mp, &ext4fs_vops, &vp)) != 0) {
		*vpp = NULL;
		return (error);
	}
	ip = pool_get(&ext4fs_inode_pool, PR_WAITOK|PR_ZERO);
	rrw_init_flags(&ip->i_lock, "inode", RWL_DUPOK | RWL_IS_VNODE);
	vp->v_data = ip;
	ip->i_vnode = vp;
	ip->i_ump = ump;
	ip->i_e4fs = fs;
	ip->i_dev = dev;
	ip->i_number = ino;

	/*
	 * Put it onto its hash chain and lock it so that other requests for
	 * this inode will block if they arrive while we are sleeping waiting
	 * for old data structures to be purged or for the contents of the
	 * disk portion of this inode to be read.
	 */
	error = ufs_ihashins(ip);

	if (error) {
		/*
		 * The vnode was locked by ufs_ihashins, then unlocked on error.
		 * We need to properly clean up the inode and vnode.
		 * vrele will trigger reclaim which will free the inode.
		 */
		vrele(vp);

		if (error == EEXIST)
			goto retry;

		return (error);
	}

	vref(ip->i_devvp);

	/* Calculate inode location on disk */
	if (ino == 0 || ino > fs->m_inodes_count) {
		vput(vp);
		*vpp = NULL;
		return (ESTALE);
	}
	inode_group = (ino - 1) / fs->m_inodes_per_group;
	if (inode_group >= fs->m_block_group_count) {
		vput(vp);
		*vpp = NULL;
		return (ESTALE);
	}
	inode_index = (ino - 1) % fs->m_inodes_per_group;
	block_in_table = inode_index / fs->m_inodes_per_block;
	offset_in_block = (inode_index % fs->m_inodes_per_block) *
	    fs->m_inode_size;

	gd = &fs->m_gd[inode_group];
	inode_table_block = letoh32(gd->bgd_inode_table_block_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		inode_table_block |= (u_int64_t)
		    letoh32(gd->bgd_inode_table_block_hi) << 32;

	/* Read the block containing this inode */
	disk_block = (inode_table_block + block_in_table) <<
	    fs->m_fs_block_to_disk_block;
	error = bread(ump->um_devvp, disk_block, fs->m_block_size, &bp);
	if (error) {
		vput(vp);
		brelse(bp);
		*vpp = NULL;
		return (error);
	}

	dp = (struct ext4fs_dinode *)((char *)bp->b_data + offset_in_block);

	/* Allocate space for on-disk inode */
	ip->i_e4din = pool_get(&ext4fs_dinode_pool, PR_WAITOK|PR_ZERO);

	/*
	 * If the group has INODE_UNINIT set, or the inode is in the
	 * unused portion of the inode table, the on-disk data is
	 * garbage. Keep the zeroed buffer and skip checksum verification.
	 */
	bgd_flags = letoh16(gd->bgd_flags);
	itable_unused = letoh16(gd->bgd_inode_table_unused_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		itable_unused |= (u_int32_t)
		    letoh16(gd->bgd_inode_table_unused_hi) << 16;
	if ((bgd_flags & EXT4FS_BGD_FLAG_INODE_UNINIT) ||
	    inode_index >= fs->m_inodes_per_group - itable_unused) {
		memset(dp, 0, fs->m_inode_size);
		error = bwrite(bp);
		if (error) {
			pool_put(&ext4fs_dinode_pool, ip->i_e4din);
			ip->i_e4din = NULL;
			vput(vp);
			*vpp = NULL;
			return (error);
		}
	} else {
		memcpy(ip->i_e4din, dp, fs->m_inode_size);
		brelse(bp);

		/* Verify inode checksum for initialized slots */
		if (letoh16(ip->i_e4din->dinode.i_mode) != 0 ||
		    letoh16(ip->i_e4din->dinode.i_links_count) != 0 ||
		    letoh32(ip->i_e4din->dinode.i_dtime) != 0) {
			error = ext4fs_inode_csum_verify(fs,
			    ip->i_e4din, ino);
			if (error) {
				pool_put(&ext4fs_dinode_pool,
				    ip->i_e4din);
				ip->i_e4din = NULL;
				vput(vp);
				*vpp = NULL;
				return (error);
			}
		}
	}

	/* Set vnode type based on inode mode */
	imode = letoh16(ip->i_e4din->dinode.i_mode);
	switch (imode & S_IFMT) {
	case S_IFDIR:
		vp->v_type = VDIR;
		break;
	case S_IFREG:
		vp->v_type = VREG;
		break;
	case S_IFLNK:
		vp->v_type = VLNK;
		break;
	case S_IFBLK:
		vp->v_type = VBLK;
		break;
	case S_IFCHR:
		vp->v_type = VCHR;
		break;
	case S_IFIFO:
		vp->v_type = VFIFO;
		break;
	case S_IFSOCK:
		vp->v_type = VSOCK;
		break;
	default:
		vp->v_type = VNON;
		break;
	}

	/* Set effective link count */
	ip->i_effnlink = letoh16(ip->i_e4din->dinode.i_links_count);

	/* Set VROOT flag for root inode */
	if (ip->i_number == EXT4FS_INODE_ROOT_DIR)
		vp->v_flag |= VROOT;

	/* If the inode was deleted, reset all fields */
	if (letoh32(ip->i_e4din->dinode.i_dtime) != 0) {
		vp->v_type = VNON;
		ip->i_effnlink = 0;
	}

	*vpp = vp;
	return (0);
}

int
ext4fs_vptofh(struct vnode *vp, struct fid *fhp)
{
	(void)vp;
	(void)fhp;
	printf("ext4fs_vptofh: not implemented\n");
	return (EOPNOTSUPP);
}
