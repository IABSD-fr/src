/*
 * Copyright (c) 2025 kmx.io.
 * Copyright (c) 1997 Manuel Bouyer.
 * Copyright (c) 1989, 1991, 1993, 1994
 * The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 * 3. Neither the name of the University nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Modified for ext4fs by kmx.io.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/fcntl.h>
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
#include <ufs/ufs/ufs_extern.h>

#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_extern.h>
#include <ufs/ext4fs/ext4fs_journal.h>
#include <ufs/ext4fs/ext4fs_orphan.h>

struct pool ext4fs_inode_pool;
struct pool ext4fs_dinode_pool;

void
ext4fs_print (struct mount *mp, const char *fmt, ...)
{
	const char *name;
	va_list ap;

	name = "unknown device";
	if (mp != NULL) {
		if (mp->mnt_stat.f_mntonname[0] != '\0')
			name = mp->mnt_stat.f_mntonname;
		else if (mp->mnt_stat.f_mntfromname[0] != '\0')
			name = mp->mnt_stat.f_mntfromname;
	}
	printf("ext4fs: %s: ", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static int	ext4fs_mark_writable (struct mount *, int);
static int	ext4fs_remount_readonly (struct mount *, struct proc *);
static int	ext4fs_remount_writable (struct mount *, struct proc *);
static int	ext4fs_mount_update (struct mount *, struct proc *);
static int	ext4fs_vget_handle (struct mount *, ino_t,
    struct ext4fs_journal_handle *, struct vnode **);
static int	ext4fs_allocator_cursors_init (struct m_ext4fs *);
static void	ext4fs_allocator_cursors_destroy (struct m_ext4fs *);

#define PRINTF_FEATURES(mask, features)				\
	for (i = 0; i < nitems(features); i++)			\
		if ((mask) & (features)[i].f_mask)		\
			printf("%s ", (features)[i].f_name)

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
ext4fs_block_group_has_super_block (int group)
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

u_int64_t
ext4fs_bgd_get_block (struct m_ext4fs *fs,
    struct ext4fs_block_group_descriptor *gd, unsigned int which)
{
	u_int64_t block;

	switch (which) {
	case EXT4FS_BGD_BLOCK_BITMAP:
		block = letoh32(gd->bgd_block_bitmap_block_lo);
		if (fs->m_feature_incompat &
		    EXT4FS_FEATURE_INCOMPAT_64BIT)
			block |= (u_int64_t)
			    letoh32(gd->bgd_block_bitmap_block_hi)
			    << 32;
		break;
	case EXT4FS_BGD_INODE_BITMAP:
		block = letoh32(gd->bgd_inode_bitmap_block_lo);
		if (fs->m_feature_incompat &
		    EXT4FS_FEATURE_INCOMPAT_64BIT)
			block |= (u_int64_t)
			    letoh32(gd->bgd_inode_bitmap_block_hi)
			    << 32;
		break;
	case EXT4FS_BGD_INODE_TABLE:
		block = letoh32(gd->bgd_inode_table_block_lo);
		if (fs->m_feature_incompat &
		    EXT4FS_FEATURE_INCOMPAT_64BIT)
			block |= (u_int64_t)
			    letoh32(gd->bgd_inode_table_block_hi) << 32;
		break;
	default:
		block = 0;
		break;
	}
	return (block);
}

u_int32_t
ext4fs_group_block_count (struct m_ext4fs *fs, u_int32_t group)
{
	u_int64_t blocks, start;

	if (group >= fs->m_block_group_count)
		return (0);
	start = fs->m_first_data_block +
	    (u_int64_t)group * fs->m_blocks_per_group;
	if (start >= fs->m_blocks_count)
		return (0);
	blocks = fs->m_blocks_count - start;
	if (blocks > fs->m_blocks_per_group)
		blocks = fs->m_blocks_per_group;
	return ((u_int32_t)blocks);
}

u_int32_t
ext4fs_group_inode_count (struct m_ext4fs *fs, u_int32_t group)
{
	u_int64_t inodes, start;

	if (group >= fs->m_block_group_count)
		return (0);
	start = (u_int64_t)group * fs->m_inodes_per_group;
	if (start >= fs->m_inodes_count)
		return (0);
	inodes = fs->m_inodes_count - start;
	if (inodes > fs->m_inodes_per_group)
		inodes = fs->m_inodes_per_group;
	return ((u_int32_t)inodes);
}

static int
ext4fs_allocator_cursors_init (struct m_ext4fs *fs)
{
	size_t size;

	if (fs->m_block_group_count == 0 ||
	    fs->m_block_group_count > (size_t)-1 /
	    sizeof(*fs->m_block_alloc_cursor))
		return (EFBIG);
	size = fs->m_block_group_count *
	    sizeof(*fs->m_block_alloc_cursor);
	fs->m_block_alloc_cursor = malloc(size, M_UFSMNT,
	    M_WAITOK | M_ZERO);
	fs->m_inode_alloc_cursor = malloc(size, M_UFSMNT,
	    M_WAITOK | M_ZERO);
	return (0);
}

static void
ext4fs_allocator_cursors_destroy (struct m_ext4fs *fs)
{
	size_t size;

	if (fs == NULL || fs->m_block_group_count == 0 ||
	    fs->m_block_group_count > (size_t)-1 /
	    sizeof(*fs->m_block_alloc_cursor))
		return;
	size = fs->m_block_group_count *
	    sizeof(*fs->m_block_alloc_cursor);
	if (fs->m_inode_alloc_cursor != NULL) {
		free(fs->m_inode_alloc_cursor, M_UFSMNT, size);
		fs->m_inode_alloc_cursor = NULL;
	}
	if (fs->m_block_alloc_cursor != NULL) {
		free(fs->m_block_alloc_cursor, M_UFSMNT, size);
		fs->m_block_alloc_cursor = NULL;
	}
}

u_int32_t
ext4fs_bgd_get_count (struct m_ext4fs *fs,
    struct ext4fs_block_group_descriptor *gd, unsigned int which)
{
	u_int32_t count;
	u_int16_t high;

	switch (which) {
	case EXT4FS_BGD_FREE_BLOCKS:
		count = letoh16(gd->bgd_free_blocks_count_lo);
		high = gd->bgd_free_blocks_count_hi;
		break;
	case EXT4FS_BGD_FREE_INODES:
		count = letoh16(gd->bgd_free_inodes_count_lo);
		high = gd->bgd_free_inodes_count_hi;
		break;
	case EXT4FS_BGD_USED_DIRS:
		count = letoh16(gd->bgd_used_dirs_count_lo);
		high = gd->bgd_used_dirs_count_hi;
		break;
	default:
		return (0);
	}
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		count |= (u_int32_t)letoh16(high) << 16;
	return (count);
}

void
ext4fs_bgd_set_count (struct m_ext4fs *fs,
    struct ext4fs_block_group_descriptor *gd, unsigned int which,
    u_int32_t count)
{
	switch (which) {
	case EXT4FS_BGD_FREE_BLOCKS:
		gd->bgd_free_blocks_count_lo =
		    htole16(count & 0xffff);
		if (fs->m_feature_incompat &
		    EXT4FS_FEATURE_INCOMPAT_64BIT)
			gd->bgd_free_blocks_count_hi =
			    htole16(count >> 16);
		else
			gd->bgd_free_blocks_count_hi = 0;
		break;
	case EXT4FS_BGD_FREE_INODES:
		gd->bgd_free_inodes_count_lo =
		    htole16(count & 0xffff);
		if (fs->m_feature_incompat &
		    EXT4FS_FEATURE_INCOMPAT_64BIT)
			gd->bgd_free_inodes_count_hi =
			    htole16(count >> 16);
		else
			gd->bgd_free_inodes_count_hi = 0;
		break;
	case EXT4FS_BGD_USED_DIRS:
		gd->bgd_used_dirs_count_lo =
		    htole16(count & 0xffff);
		if (fs->m_feature_incompat &
		    EXT4FS_FEATURE_INCOMPAT_64BIT)
			gd->bgd_used_dirs_count_hi =
			    htole16(count >> 16);
		else
			gd->bgd_used_dirs_count_hi = 0;
		break;
	default:
		return;
	}
}

static int
ext4fs_counters_check (struct m_ext4fs *fs)
{
	struct ext4fs_block_group_descriptor *gd;
	u_int64_t free_blocks, free_inodes;
	u_int32_t blocks, dirs, group, inodes;
	u_int32_t group_free_blocks, group_free_inodes;

	if (fs->m_block_group_count == 0 ||
	    fs->m_block_group_count > UINT32_MAX)
		return (EFBIG);
	free_blocks = 0;
	free_inodes = 0;
	for (group = 0; group < fs->m_block_group_count; group++) {
		gd = &fs->m_gd[group];
		blocks = ext4fs_group_block_count(fs, group);
		inodes = ext4fs_group_inode_count(fs, group);
		group_free_blocks = ext4fs_bgd_get_count(fs, gd,
		    EXT4FS_BGD_FREE_BLOCKS);
		group_free_inodes = ext4fs_bgd_get_count(fs, gd,
		    EXT4FS_BGD_FREE_INODES);
		dirs = ext4fs_bgd_get_count(fs, gd,
		    EXT4FS_BGD_USED_DIRS);
		if (blocks == 0 || inodes == 0 ||
		    group_free_blocks > blocks ||
		    group_free_inodes > inodes ||
		    dirs > inodes - group_free_inodes) {
			ext4fs_print(fs->m_mountp,
			    "invalid counters in group %u\n", group);
			return (EINVAL);
		}
		if (free_blocks > UINT64_MAX - group_free_blocks ||
		    free_inodes > UINT64_MAX - group_free_inodes)
			return (EOVERFLOW);
		free_blocks += group_free_blocks;
		free_inodes += group_free_inodes;
	}
	if (free_blocks != fs->m_free_blocks_count ||
	    free_inodes != fs->m_free_inodes_count) {
		ext4fs_print(fs->m_mountp, "group and superblock "
		    "counters differ\n");
		return (EINVAL);
	}
	return (0);
}

/*
 * Locate a group descriptor in the primary descriptor table.  The
 * in-memory array always uses the complete structure, while an ext4
 * filesystem without the 64BIT feature stores 32-byte descriptors.
 */
int
ext4fs_bgd_location (struct m_ext4fs *fs, u_int32_t group,
    u_int64_t *block, size_t *offset)
{
	u_int32_t per_block, size;

	if (group >= fs->m_block_group_count)
		return (EINVAL);
	size = fs->m_block_group_descriptor_size;
	if (size != EXT4FS_BGD_SIZE_32 &&
	    size != EXT4FS_BGD_SIZE_64)
		return (EINVAL);
	per_block = fs->m_block_size / size;
	if (per_block == 0)
		return (EINVAL);
	*block = (u_int64_t)fs->m_first_data_block + 1 +
	    group / per_block;
	*offset = (size_t)(group % per_block) * size;
	return (0);
}

int
ext4fs_fhtovp (struct mount *mp, struct fid *fhp, struct vnode **vpp)
{
	(void)mp;
	(void)fhp;
	(void)vpp;
	ext4fs_print(mp, "fhtovp is not implemented\n");
	return (EOPNOTSUPP);
}

/*
 * Flush out all the files in a filesystem.
 */
int
ext4fs_flushfiles (struct mount *mp, int flags, struct proc *p)
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
ext4fs_init (struct vfsconf *vfsp)
{
	int result;
	(void)vfsp;
	pool_init(&ext4fs_inode_pool, sizeof(struct inode), 0,
	    IPL_NONE, PR_WAITOK, "ext4inopl", NULL);
	pool_init(&ext4fs_dinode_pool,
	    sizeof(struct ext4fs_dinode_256), 0,
	    IPL_NONE, PR_WAITOK, "ext4dinopl", NULL);
	if ((result = ufs_init(vfsp))) {
		return result;
	}
	return (0);
}

static int
ext4fs_mark_writable (struct mount *mp, int recovered)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	int error;

	if (recovered)
		fs->m_state &= ~EXT4FS_STATE_VALID;
	else if (fs->m_state == EXT4FS_STATE_VALID)
		fs->m_state = 0;
	else
		fs->m_state = EXT4FS_STATE_ERROR;
	fs->m_fs_was_modified = 1;
	if (fs->m_journal != NULL)
		error = ext4fs_sbwrite_lifecycle(mp);
	else
		error = ext4fs_sbwrite_direct(mp);
	if (error == 0)
		error = jbd2_flush_device(ump->um_devvp, curproc);
	return (error);
}

static int
ext4fs_remount_readonly (struct mount *mp, struct proc *p)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	u_int16_t saved_state;
	int error, flags;

	if (fs->m_read_only)
		return (0);
	error = ext4fs_sync(mp, MNT_WAIT, 0, p->p_ucred, p);
	if (error)
		return (error);
	flags = WRITECLOSE;
	if (mp->mnt_flag & MNT_FORCE)
		flags |= FORCECLOSE;
	error = ext4fs_flushfiles(mp, flags, p);
	if (error)
		return (error);
	if (ext4fs_orphan_pending(mp))
		return (EBUSY);

	if (fs->m_journal != NULL) {
		error = ext4fs_journal_mark_clean(mp,
		    EXT4FS_JOURNAL_COMMIT_REMOUNT);
	} else {
		saved_state = fs->m_state;
		fs->m_state = EXT4FS_STATE_VALID;
		fs->m_fs_was_modified = 1;
		error = ext4fs_sbwrite_direct(mp);
		if (error == 0)
			error = jbd2_flush_device(ump->um_devvp, p);
		if (error) {
			fs->m_state = saved_state;
			fs->m_sble.sb_state = htole16(saved_state);
			fs->m_sble.sb_checksum = htole32(
			    ext4fs_sb_csum(&fs->m_sble));
		}
	}
	if (error)
		return (error);

	fs->m_read_only = 1;
	return (0);
}

static int
ext4fs_remount_writable (struct mount *mp, struct proc *p)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	int cleanup_error, error;

	if (! fs->m_read_only)
		return (0);
	error = ext4fs_sbcheck(&fs->m_sble, 0, mp);
	if (error)
		return (error);
	ext4fs_journal_destroy(mp);
	fs->m_read_only = 0;

	error = ext4fs_orphan_cleanup(mp);
	if (error == 0)
		error = ext4fs_counters_check(fs);
	if (error == 0)
		error = ext4fs_journal_init(mp);
	if (error == 0)
		error = ext4fs_mark_writable(mp, 0);
	if (error == 0)
		return (0);

	/*
	 * Recovery progress is restartable; restore a usable r/o
	 * mount.
	 */
	if (fs->m_journal != NULL &&
	    (fs->m_feature_incompat &
	    EXT4FS_FEATURE_INCOMPAT_RECOVER))
		(void)ext4fs_journal_mark_clean(mp,
		    EXT4FS_JOURNAL_COMMIT_REMOUNT);
	fs->m_read_only = 1;
	if (fs->m_journal == NULL) {
		cleanup_error = ext4fs_journal_init(mp);
		if (cleanup_error)
			ext4fs_print(mp, "remount rollback could not "
			    "reopen journal: %d\n", cleanup_error);
	}
	return (error);
}

static int
ext4fs_mount_update (struct mount *mp, struct proc *p)
{
	struct m_ext4fs *fs = VFSTOUFS(mp)->um_e4fs;
	int error;

	if (rw_enter(&fs->m_remount_lock,
	    RW_WRITE | RW_NOSLEEP) != 0)
		return (EBUSY);
	if (mp->mnt_flag & MNT_RELOAD)
		error = EOPNOTSUPP;
	else if (! fs->m_read_only &&
	    (mp->mnt_flag & MNT_RDONLY))
		error = ext4fs_remount_readonly(mp, p);
	else if (fs->m_read_only &&
	    (mp->mnt_flag & MNT_WANTRDWR))
		error = ext4fs_remount_writable(mp, p);
	else
		error = 0;
	rw_exit_write(&fs->m_remount_lock);
	return (error);
}

int
ext4fs_mount (struct mount *mp, const char *path, void *data,
    struct nameidata *ndp, struct proc *p)
{
	struct ufs_args *args = data;
	struct ufsmount *ump;
	struct vnode *devvp;
	char fname[MNAMELEN];
	char fspec[MNAMELEN];
	int error, update;

	update = (mp->mnt_flag & MNT_UPDATE) != 0;
	ump = update ? VFSTOUFS(mp) : NULL;
	if (args == NULL) {
		if (! update)
			return (EINVAL);
		return (ext4fs_mount_update(mp, p));
	}
	if (args->fspec == NULL) {
		if (! update)
			return (EINVAL);
		if (mp->mnt_flag & MNT_RELOAD)
			return (EOPNOTSUPP);
		error = vfs_export(mp, &ump->um_export,
		    &args->export_info);
		if (error)
			return (error);
		return (ext4fs_mount_update(mp, p));
	}
	error = copyinstr(args->fspec, fspec, sizeof(fspec), NULL);
	if (error)
		return (error);

	if (disk_map(fspec, fname, MNAMELEN, DM_OPENBLCK) == -1)
		memcpy(fname, fspec, sizeof(fname));
	strlcpy(mp->mnt_stat.f_mntfromname, fname,
	    sizeof(mp->mnt_stat.f_mntfromname));
	strlcpy(mp->mnt_stat.f_mntonname, path,
	    sizeof(mp->mnt_stat.f_mntonname));

	NDINIT(ndp, LOOKUP, FOLLOW, UIO_SYSSPACE, fname, p);
	if ((error = namei(ndp)) != 0)
		return (error);
	devvp = ndp->ni_vp;

	if (devvp->v_type != VBLK) {
		error = ENOTBLK;
		goto error_devvp;
	}
	if (major(devvp->v_rdev) >= nblkdev) {
		error = ENXIO;
		goto error_devvp;
	}
	if (! update) {
		error = ext4fs_mountfs(devvp, mp, p);
	} else {
		if (devvp != ump->um_devvp &&
		    devvp->v_rdev != ump->um_devvp->v_rdev)
			error = EINVAL;
		vrele(devvp);
		devvp = NULL;
		if (error == 0)
			error = ext4fs_mount_update(mp, p);
	}
	if (error)
		goto error_devvp;

	return (0);

error_devvp:
	if (devvp != NULL)
		vrele(devvp);
	return (error);
}

/*
 * Common code for mount and mountroot
 */
int
ext4fs_mountfs (struct vnode *devvp, struct mount *mp, struct proc *p)
{
	struct ufsmount *ump;
	struct buf *bp;
	struct ext4fs *sble;
	struct m_ext4fs *mfs;
	const char *stage;
	dev_t dev;
	int devopen, error, openflags, recovered, ronly;
	struct ucred *cred;

	dev = devvp->v_rdev;
	cred = p ? p->p_ucred : NOCRED;
	/*
	 * Disallow multiple mounts of the same device.
	 * Disallow mounting of a device that is currently in use
	 * except root, which might share the miniroot swap device.
	 * Flush out any old buffers remaining from a previous use.
	 */
	error = vfs_mountedon(devvp);
	if (error != 0) {
		ext4fs_print(mp, "mounted-device check failed: %d\n",
		    error);
		return (error);
	}
	if (vcount(devvp) > 1 && devvp != rootvp)
		return (EBUSY);
	vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
	error = vinvalbuf(devvp, V_SAVE, cred, p, 0, INFSLP);
	VOP_UNLOCK(devvp);
	if (error != 0) {
		ext4fs_print(mp, "device cache invalidation failed: %d\n",
		    error);
		return (error);
	}

	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;
	openflags = ronly ? FREAD : FREAD | FWRITE;
	error = VOP_OPEN(devvp, openflags, FSCRED, p);
	if (error) {
		ext4fs_print(mp, "device open failed: %d\n", error);
		return (error);
	}
	devopen = 1;

	bp = NULL;
	ump = NULL;
	recovered = 0;

	/*
	 * Read the superblock from disk.
	 */
	stage = "superblock read";
	error = bread(devvp, (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET /
				       DEV_BSIZE),
		      EXT4FS_SUPER_BLOCK_SIZE, &bp);
	if (error)
		goto out;
	sble = (struct ext4fs *)bp->b_data;
	stage = "superblock validation";
	error = ext4fs_sbcheck(sble, ronly, mp);
	if (error)
		goto out;

	ump = malloc(sizeof *ump, M_UFSMNT, M_WAITOK | M_ZERO);
	mfs = ump->um_e4fs = malloc(sizeof(struct m_ext4fs), M_UFSMNT,
	    M_WAITOK | M_ZERO);
	mfs->m_mountp = mp;
	rw_init(&mfs->m_remount_lock, "e4remount");
	rw_init(&mfs->m_runtime_orphan_lock, "e4orphan");

	/*
	 * Copy in the superblock, compute in-memory values
	 * and load group descriptors.
	 */
	ext4fs_sbload(sble, mfs);
	mfs->m_read_only = ronly;
	stage = "group descriptor loading";
	if ((error = ext4fs_sbfill(devvp, mfs)) != 0)
		goto out;
	brelse(bp);
	bp = NULL;
	sble = &mfs->m_sble;

	/*
	 * If the filesystem needs journal recovery, replay it now.
	 * For r/o mounts, we temporarily reopen the device r/w.
	 */
	if ((mfs->m_feature_compat &
	    EXT4FS_FEATURE_COMPAT_HAS_JOURNAL) &&
	    (mfs->m_feature_incompat &
	    EXT4FS_FEATURE_INCOMPAT_RECOVER)) {
		if (ronly) {
			/* Reopen device r/w for replay */
			stage = "journal replay device reopen";
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
			error = VOP_CLOSE(devvp, openflags, cred, p);
			VOP_UNLOCK(devvp);
			devopen = 0;
			if (error)
				goto out;
			error = VOP_OPEN(devvp, FREAD | FWRITE,
			    FSCRED, p);
			if (error) {
				ext4fs_print(mp, "can't reopen device r/w "
				    "for journal replay\n");
				goto out;
			}
			openflags = FREAD | FWRITE;
			devopen = 1;
		}

		stage = "journal replay";
		error = ext4fs_journal_replay(devvp, mfs, p);
		if (error) {
			ext4fs_print(mp, "journal replay failed: %d\n",
			    error);
			ext4fs_print(mp, "use e2fsck to repair\n");
			/* Leave RECOVER set; close the held mode. */
			goto out;
		}
		recovered = 1;

		/* Reopen device r/o if it was a r/o mount */
		if (ronly) {
			stage = "read-only device reopen";
			vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
			error = VOP_CLOSE(devvp, openflags, cred, p);
			VOP_UNLOCK(devvp);
			devopen = 0;
			if (error)
				goto out;
			error = VOP_OPEN(devvp, FREAD, FSCRED, p);
			if (error) {
				ext4fs_print(mp, "can't reopen device "
				    "r/o\n");
				goto out;
			}
			openflags = FREAD;
			devopen = 1;
		}

		/*
		 * Replay may have changed group descriptors and
		 * superblock counters. Reload them.
		 */
		if (mfs->m_gd != NULL) {
			size_t gd_size = mfs->m_block_group_count *
			    sizeof(
			    struct ext4fs_block_group_descriptor);
			free(mfs->m_gd, M_UFSMNT, gd_size);
			mfs->m_gd = NULL;
		}
		/* Re-read superblock from disk */
		stage = "post-replay superblock read";
		error = bread(devvp,
		    (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE),
		    EXT4FS_SUPER_BLOCK_SIZE, &bp);
		if (error)
			goto out;
		stage = "post-replay superblock validation";
		error = ext4fs_sbcheck(
		    (struct ext4fs *)bp->b_data, ronly, mp);
		if (error)
			goto out;
		ext4fs_sbload((struct ext4fs *)bp->b_data, mfs);
		brelse(bp);
		bp = NULL;

		stage = "post-replay group descriptor loading";
		error = ext4fs_sbfill(devvp, mfs);
		if (error)
			goto out;
		sble = &mfs->m_sble;
	}
	stage = "allocator cursor setup";
	error = ext4fs_allocator_cursors_init(mfs);
	if (error)
		goto out;

	ump->um_e4fs->m_read_only = ronly;
	ump->um_fstype = UM_EXT4FS;

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
	/* No fragments. */
	ump->um_seqinc = 1;
	ump->um_maxsymlinklen = EXT4FS_SYMLINK_LEN_MAX;
	devvp->v_specmountpoint = mp;

	if (ronly == 0) {
		stage = "orphan recovery";
		error = ext4fs_orphan_cleanup(mp);
		if (error)
			goto out;
	}
	if (ronly == 0 || (mfs->m_last_orphan == 0 &&
	    ! (mfs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT))) {
		stage = "counter validation";
		error = ext4fs_counters_check(mfs);
		if (error)
			goto out;
	}

	stage = "journal initialization";
	error = ext4fs_journal_init(mp);
	if (error)
		goto out;

	if (ronly == 0) {
		/*
		 * Keep the filesystem clean while orphan recovery
		 * checkpoints.  Incomplete recovery retains an orphan
		 * root.  It can resume on the next writable mount.
		 * Mark the filesystem dirty only after those roots are
		 * gone.
		 */
		stage = "writable-state initialization";
		error = ext4fs_mark_writable(mp, recovered);
		if (error)
			goto out;
	}

	return (0);
out:
	ext4fs_print(mp, "mount failed during %s: error %d\n", stage,
	    error);
	if (devvp->v_specinfo)
		devvp->v_specmountpoint = NULL;
	if (bp)
		brelse(bp);
	if (devopen) {
		vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
		(void)VOP_CLOSE(devvp, openflags, cred, p);
		VOP_UNLOCK(devvp);
	}
	if (ump) {
		if (mfs != NULL && mfs->m_journal != NULL)
			ext4fs_journal_destroy(mp);
		ext4fs_allocator_cursors_destroy(mfs);
		if (mfs && mfs->m_gd != NULL) {
			size_t gd_size = mfs->m_block_group_count *
			    sizeof(
			    struct ext4fs_block_group_descriptor);
			free(mfs->m_gd, M_UFSMNT, gd_size);
		}
		free(mfs, M_UFSMNT, sizeof *mfs);
		free(ump, M_UFSMNT, sizeof *ump);
		mp->mnt_data = NULL;
	}
	return (error);
}

int
ext4fs_sbcheck (struct ext4fs *sble, int ronly, struct mount *mp)
{
	u_int16_t desc_size;
	u_int32_t incompat;
	u_int32_t mask, tmp;
	int i;

	tmp = letoh16(sble->sb_magic);
	if (tmp != EXT4FS_MAGIC) {
		ext4fs_print(mp, "wrong magic number 0x%x\n", tmp);
		/* XXX needs translation */
		return (EIO);
	}

	if (ext4fs_sb_csum_verify(sble) != 0) {
		ext4fs_print(mp, "superblock checksum verification "
		    "failed\n");
		return (EINVAL);
	}

	tmp = letoh16(sble->sb_errors);
	if (tmp != EXT4FS_ERRORS_CONTINUE &&
	    tmp != EXT4FS_ERRORS_RO &&
	    tmp != EXT4FS_ERRORS_PANIC) {
		ext4fs_print(mp, "invalid error policy: %u\n", tmp);
		return (EINVAL);
	}

	tmp = letoh32(sble->sb_log_block_size);
	if (tmp > 2) {
		/* Skewed log: 1024 -> 0, 2048 -> 1, 4096 -> 2. */
		tmp += 10;
		ext4fs_print(mp, "wrong log2(block size) %d\n", tmp);
		/* XXX needs translation */
		return (EIO);
	}

	if (letoh32(sble->sb_blocks_per_group) == 0) {
		ext4fs_print(mp, "zero blocks per group\n");
		return (EIO);
	}

	if (letoh32(sble->sb_inodes_per_group) == 0) {
		ext4fs_print(mp, "zero inodes per group\n");
		return (EIO);
	}

	tmp = letoh32(sble->sb_revision_level);
	if (tmp != EXT4FS_REV_DYNAMIC) {
		ext4fs_print(mp, "wrong revision number 0x%x\n", tmp);
		/* XXX needs translation */
		return (EIO);
	}

	tmp = letoh16(sble->sb_inode_size);
	if (tmp != 256) {
		ext4fs_print(mp, "unsupported inode size: %d\n", tmp);
		return (EINVAL);
	}

	tmp = letoh32(sble->sb_first_non_reserved_inode);
	if (tmp != EXT4FS_INODE_FIRST) {
		ext4fs_print(mp, "first inode at 0x%x\n", tmp);
		/* XXX needs translation */
		return (EINVAL);
	}

	incompat = letoh32(sble->sb_feature_incompat);
	desc_size = letoh16(sble->sb_block_group_descriptor_size);
	if (((incompat & EXT4FS_FEATURE_INCOMPAT_64BIT) &&
	    desc_size != EXT4FS_BGD_SIZE_64) ||
	    (! (incompat & EXT4FS_FEATURE_INCOMPAT_64BIT) &&
	    desc_size != 0 && desc_size != EXT4FS_BGD_SIZE_32)) {
		ext4fs_print(mp,
		    "block group descriptor size is 0x%x\n",
		    desc_size);
		return (EINVAL);
	}

	tmp = incompat;
	mask = tmp & ~EXT4FS_FEATURE_INCOMPAT_SUPPORTED;
	if (mask) {
		ext4fs_print(mp, "unsupported incompat features: 0x%x ",
		    mask);
		PRINTF_FEATURES(mask, ext4fs_feature_incompat);
		printf("\n");
		/* XXX needs translation */
		return (EINVAL);
	}

	if (tmp & EXT4FS_FEATURE_INCOMPAT_RECOVER) {
		ext4fs_print(mp, "file system needs journal recovery\n");
		if (! (letoh32(sble->sb_feature_compat) &
		    EXT4FS_FEATURE_COMPAT_HAS_JOURNAL)) {
			ext4fs_print(mp, "RECOVER set but no journal\n");
			return (EINVAL);
		}
		/* Allow mount to proceed; replay happens in mountfs */
	}

	tmp = letoh32(sble->sb_feature_ro_compat) &
		~EXT4FS_FEATURE_RO_COMPAT_SUPPORTED;
	if (! ronly && tmp) {
		ext4fs_print(mp,
		    "unsupported R/O compat features: 0x%x ",
		    tmp);
		PRINTF_FEATURES(tmp, ext4fs_feature_ro_compat);
		printf("\n");
		return (EROFS);
	}

	if (! ronly &&
	    ! (letoh32(sble->sb_feature_incompat) &
	      EXT4FS_FEATURE_INCOMPAT_RECOVER) &&
	    ! (letoh16(sble->sb_state) & EXT4FS_STATE_VALID)) {
		ext4fs_print(mp,
		    "file system not clean, run e2fsck\n");
		return (EROFS);
	}

	return (0);
}

int
ext4fs_sbfill (struct vnode *devvp, struct m_ext4fs *mfs)
{
	struct ext4fs_dinode *rdp;
	struct buf *bp;
	daddr_t dblk;
	u_int64_t first, ritb, rblk;
	u_int32_t desc_size, descs_per_block, i, j, ndesc;
	u_int32_t rgroup, rindex, roff;
	size_t gd_size;
	int error;

	mfs->m_block_group_count = howmany(mfs->m_blocks_count -
					   mfs->m_first_data_block,
					   mfs->m_blocks_per_group);

	mfs->m_block_size_shift = EXT4FS_LOG_MIN_BLOCK_SIZE +
		mfs->m_log_block_size;
	mfs->m_block_size = 1 << mfs->m_block_size_shift;
	desc_size = mfs->m_block_group_descriptor_size;
	if (desc_size != EXT4FS_BGD_SIZE_32 &&
	    desc_size != EXT4FS_BGD_SIZE_64)
		return (EINVAL);
	descs_per_block = mfs->m_block_size / desc_size;
	if (mfs->m_block_group_count == 0 ||
	    mfs->m_block_group_count > UINT32_MAX ||
	    descs_per_block == 0 ||
	    mfs->m_block_group_count > (size_t)-1 /
	    sizeof(struct ext4fs_block_group_descriptor))
		return (EFBIG);
	mfs->m_block_group_descriptor_blocks_count =
		howmany(mfs->m_block_group_count,
		    descs_per_block);
	mfs->m_fs_block_to_disk_block = mfs->m_log_block_size + 1;
	mfs->m_inodes_per_block = mfs->m_block_size / mfs->m_inode_size;
	mfs->m_inode_table_blocks_per_group = mfs->m_inodes_per_group /
		mfs->m_inodes_per_block;

	gd_size = mfs->m_block_group_count *
	    sizeof(struct ext4fs_block_group_descriptor);
	mfs->m_gd = malloc(gd_size, M_UFSMNT, M_WAITOK);
	memset(mfs->m_gd, 0, gd_size);

	dblk = (daddr_t)EXT4FS_FSBTODB(mfs,
	    (u_int64_t)mfs->m_first_data_block + 1);
	for (i = 0;
	    i < mfs->m_block_group_descriptor_blocks_count; i++) {
		error = bread(devvp,
		    dblk + ((daddr_t)i <<
		    mfs->m_fs_block_to_disk_block),
		    mfs->m_block_size, &bp);
		if (error) {
			ext4fs_print(mfs->m_mountp, "failed to read "
			    "block group "
			    "descriptors: %d\n", error);
			free(mfs->m_gd, M_UFSMNT, gd_size);
			mfs->m_gd = NULL;
			return (error);
		}
		first = (u_int64_t)i * descs_per_block;
		ndesc = descs_per_block;
		if (ndesc > mfs->m_block_group_count - first)
			ndesc = mfs->m_block_group_count - first;
		for (j = 0; j < ndesc; j++)
			memcpy(&mfs->m_gd[first + j],
			    (char *)bp->b_data + j * desc_size,
			    desc_size);
		brelse(bp);
	}

	/* Verify block group descriptor checksums */
	for (i = 0; i < mfs->m_block_group_count; i++) {
		if ((error = ext4fs_bgd_csum_verify(mfs, &mfs->m_gd[i],
		    i)) != 0) {
			ext4fs_print(mfs->m_mountp,
			    "block group %d checksum "
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
ext4fs_sbload (struct ext4fs *sble, struct m_ext4fs *dest)
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
	dest->m_clusters_per_group =
	    letoh32(sble->sb_clusters_per_group);
	dest->m_inodes_per_group = letoh32(sble->sb_inodes_per_group);
	dest->m_mount_time = letoh32(sble->sb_mount_time_lo);
	dest->m_write_time = letoh32(sble->sb_write_time_lo);
	dest->m_mount_count = letoh16(sble->sb_mount_count);
	dest->m_max_mount_count_before_fsck =
	    (int16_t)letoh16(sble->sb_max_mount_count_before_fsck);
	dest->m_state = letoh16(sble->sb_state);
	dest->m_errors = letoh16(sble->sb_errors);
	dest->m_revision_level_minor =
	    letoh16(sble->sb_revision_level_minor);
	dest->m_check_time = letoh32(sble->sb_check_time_lo);
	dest->m_check_interval = letoh32(sble->sb_check_interval);
	dest->m_creator_os = letoh32(sble->sb_creator_os);
	dest->m_revision_level = letoh32(sble->sb_revision_level);
	dest->m_default_reserved_uid =
	    letoh16(sble->sb_default_reserved_uid);
	dest->m_default_reserved_gid =
	    letoh16(sble->sb_default_reserved_gid);
	dest->m_first_non_reserved_inode =
	    letoh32(sble->sb_first_non_reserved_inode);
	dest->m_inode_size = letoh16(sble->sb_inode_size);
	dest->m_block_group_id = letoh16(sble->sb_block_group_id);
	dest->m_feature_compat = letoh32(sble->sb_feature_compat);
	dest->m_feature_incompat = letoh32(sble->sb_feature_incompat);
	dest->m_feature_ro_compat = letoh32(sble->sb_feature_ro_compat);
	dest->m_algorithm_usage_bitmap =
	    letoh32(sble->sb_algorithm_usage_bitmap);
	dest->m_reserved_bgdt_blocks =
	    letoh16(sble->sb_reserved_bgdt_blocks);
	dest->m_journal_inode_number =
	    letoh32(sble->sb_journal_inode_number);
	dest->m_journal_device_number =
	    letoh32(sble->sb_journal_device_number);
	dest->m_last_orphan = letoh32(sble->sb_last_orphan);
	if (feature_incompat_64bit)
		dest->m_block_group_descriptor_size =
		    letoh16(sble->sb_block_group_descriptor_size);
	else
		dest->m_block_group_descriptor_size =
		    EXT4FS_BGD_SIZE_32;
	dest->m_default_mount_opts =
	    letoh32(sble->sb_default_mount_opts);
	dest->m_first_meta_block_group =
	    letoh32(sble->sb_first_meta_block_group);
	dest->m_newfs_time = letoh32(sble->sb_newfs_time_lo);
	dest->m_inode_size_extra_min =
	    letoh16(sble->sb_inode_size_extra_min);
	dest->m_inode_size_extra_want =
	    letoh16(sble->sb_inode_size_extra_want);
	dest->m_flags = letoh32(sble->sb_flags);
	dest->m_raid_stride_block_count =
	    letoh16(sble->sb_raid_stride_block_count);
	dest->m_mmp_interval = letoh16(sble->sb_mmp_interval);
	dest->m_mmp_block = letoh64(sble->sb_mmp_block);
	dest->m_raid_stripe_width_block_count =
	    letoh32(sble->sb_raid_stripe_width_block_count);
	dest->m_kilobytes_written = letoh64(sble->sb_kilobytes_written);
	dest->m_error_count = letoh32(sble->sb_error_count);
	dest->m_first_error_time =
	    letoh32(sble->sb_first_error_time_lo);
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
	dest->m_backup_block_groups[0] =
	    letoh32(sble->sb_backup_block_groups[0]);
	dest->m_backup_block_groups[1] =
	    letoh32(sble->sb_backup_block_groups[1]);
	dest->m_lost_and_found_inode =
	    letoh32(sble->sb_lost_and_found_inode);
	dest->m_project_quota_inode =
	    letoh32(sble->sb_project_quota_inode);
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
ext4fs_statfs (struct mount *mp, struct statfs *sbp, struct proc *p)
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
		for (i = 0, ngroups = 0;
		    i < mfs->m_block_group_count; i++) {
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
		sbp->f_bavail = sbp->f_bfree -
		    mfs->m_reserved_blocks_count;
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
ext4fs_bgd_write_direct (struct m_ext4fs *fs, struct vnode *devvp,
    u_int32_t group)
{
	struct buf *bp = NULL;
	struct ext4fs_block_group_descriptor *gd;
	u_int64_t fsblock;
	size_t bgd_off, size;
	daddr_t dblk;
	int error;

	if (fs->m_journal != NULL)
		return (EIO);
	error = ext4fs_bgd_location(fs, group, &fsblock, &bgd_off);
	if (error)
		return (error);
	size = fs->m_block_group_descriptor_size;
	dblk = (daddr_t)EXT4FS_FSBTODB(fs, fsblock);

	error = bread(devvp, dblk, fs->m_block_size, &bp);
	if (error) {
		if (bp != NULL)
			brelse(bp);
		return (error);
	}

	/* Update in-memory checksum */
	gd = &fs->m_gd[group];
	gd->bgd_checksum = htole16(ext4fs_bgd_csum(fs, gd, group));

	/* Copy to buffer and write */
	memcpy((char *)bp->b_data + bgd_off, gd, size);

	bdwrite(bp);
	return (0);
}

int
ext4fs_bgd_write_handle (struct m_ext4fs *fs, struct vnode *devvp,
    u_int32_t group, struct ext4fs_journal_handle *handle)
{
	struct ext4fs_block_group_descriptor saved, *gd;
	struct buf *bp;
	u_int64_t fsblock;
	u_int16_t saved_checksum;
	size_t bgd_off, size;
	int error;

	if (handle == NULL)
		return (EINVAL);
	error = ext4fs_bgd_location(fs, group, &fsblock, &bgd_off);
	if (error)
		return (error);
	size = fs->m_block_group_descriptor_size;

	error = ext4fs_journal_get_metadata(handle, devvp, fsblock,
	    &bp);
	if (error)
		return (error);
	memcpy(&saved, (char *)bp->b_data + bgd_off, size);
	gd = &fs->m_gd[group];
	saved_checksum = gd->bgd_checksum;
	gd->bgd_checksum = htole16(ext4fs_bgd_csum(fs, gd, group));
	memcpy((char *)bp->b_data + bgd_off, gd, size);
	error = ext4fs_journal_dirty_metadata(handle, bp);
	if (error) {
		memcpy((char *)bp->b_data + bgd_off, &saved,
		    size);
		gd->bgd_checksum = saved_checksum;
	}
	return (error);
}

static void
ext4fs_sbprepare (struct m_ext4fs *fs, struct ext4fs *sble)
{
	struct timespec ts;

	sble->sb_free_blocks_count_lo =
	    htole32((u_int32_t)fs->m_free_blocks_count);
	sble->sb_free_blocks_count_hi =
	    htole32((u_int32_t)(fs->m_free_blocks_count >> 32));
	sble->sb_free_inodes_count = htole32(fs->m_free_inodes_count);
	sble->sb_feature_compat = htole32(fs->m_feature_compat);
	sble->sb_feature_incompat = htole32(fs->m_feature_incompat);
	sble->sb_feature_ro_compat = htole32(fs->m_feature_ro_compat);
	sble->sb_last_orphan = htole32(fs->m_last_orphan);
	getnanotime(&ts);
	sble->sb_write_time_lo = htole32((u_int32_t)ts.tv_sec);
	sble->sb_state = htole16(fs->m_state);
	sble->sb_checksum = htole32(ext4fs_sb_csum(sble));
}

static int
ext4fs_sbwrite_raw (struct mount *mp)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	struct ext4fs *sble = &fs->m_sble;
	struct buf *bp;
	int error;

	ext4fs_sbprepare(fs, sble);

	/* Write to disk at the fixed superblock offset */
	error = bread(ump->um_devvp,
	    (daddr_t)(EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE),
	    EXT4FS_SUPER_BLOCK_SIZE, &bp);
	if (error) {
		if (bp != NULL)
			brelse(bp);
		return (error);
	}

	memcpy(bp->b_data, sble, sizeof(struct ext4fs));
	return (bwrite(bp));
}

/*
 * Write the superblock without a journal.  This path is limited to a
 * journal-less mount or restartable recovery before journal startup.
 */
int
ext4fs_sbwrite_direct (struct mount *mp)
{
	struct m_ext4fs *fs;

	fs = VFSTOUFS(mp)->um_e4fs;
	if (fs->m_journal != NULL)
		return (EIO);
	return (ext4fs_sbwrite_raw(mp));
}

/*
 * RECOVER and clean/dirty mount state surround the journal itself and
 * therefore cannot be journaled.  Callers must serialize these writes
 * outside any runtime transaction.
 */
int
ext4fs_sbwrite_lifecycle (struct mount *mp)
{
	struct m_ext4fs *fs;

	fs = VFSTOUFS(mp)->um_e4fs;
	if (fs->m_journal == NULL)
		return (EINVAL);
	if (fs->m_read_only)
		return (EROFS);
	return (ext4fs_sbwrite_raw(mp));
}

int
ext4fs_sbwrite_handle (struct mount *mp,
    struct ext4fs_journal_handle *handle)
{
	struct ufsmount *ump = VFSTOUFS(mp);
	struct m_ext4fs *fs = ump->um_e4fs;
	struct ext4fs saved_disk;
	struct ext4fs *sble;
	struct buf *bp;
	u_int64_t fsblock;
	u_int32_t offset;
	int error;

	if (handle == NULL || fs->m_journal == NULL)
		return (EINVAL);
	fsblock = EXT4FS_SUPER_BLOCK_OFFSET / fs->m_block_size;
	offset = EXT4FS_SUPER_BLOCK_OFFSET % fs->m_block_size;
	if (offset > fs->m_block_size ||
	    sizeof(struct ext4fs) > fs->m_block_size - offset)
		return (EINVAL);
	error = ext4fs_journal_get_metadata(handle, ump->um_devvp,
	    fsblock, &bp);
	if (error)
		return (error);
	sble = (struct ext4fs *)((char *)bp->b_data + offset);
	memcpy(&saved_disk, sble, sizeof(saved_disk));
	memcpy(sble, &fs->m_sble, sizeof(*sble));
	ext4fs_sbprepare(fs, sble);
	error = ext4fs_journal_dirty_metadata(handle, bp);
	if (error)
		memcpy(sble, &saved_disk, sizeof(saved_disk));
	else
		memcpy(&fs->m_sble, sble, sizeof(fs->m_sble));
	return (error);
}

static u_long ext4fs_gennumber;

/*
 * Allocate an inode on journal-less ext4.  Journal-bearing mounts use
 * ext4fs_inode_alloc_handle() through their compound operation.
 */
int
ext4fs_inode_alloc (struct inode *pip, mode_t mode, struct ucred *cred,
    struct vnode **vpp)
{
	struct m_ext4fs *fs = pip->i_e4fs;
	struct vnode *pvp = ITOV(pip);
	struct ext4fs_block_group_descriptor *gd;
	struct ext4fs_dinode *din;
	struct ext4fs_extent_header *eh;
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
	if (fs->m_journal != NULL)
		return (EIO);

	if (fs->m_free_inodes_count == 0)
		return (ENOSPC);

	ngroups = fs->m_block_group_count;

	/* Pick starting group */
	if ((mode & S_IFMT) == S_IFDIR) {
		best = 0;
		best_free = 0;
		for (i = 0; i < ngroups; i++) {
			fi = ext4fs_bgd_get_count(fs, &fs->m_gd[i],
			    EXT4FS_BGD_FREE_INODES);
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
		free_inodes = ext4fs_bgd_get_count(fs, gd,
		    EXT4FS_BGD_FREE_INODES);
		if (free_inodes == 0)
			continue;

		/* Read inode bitmap */
		bitmap_blk = letoh32(gd->bgd_inode_bitmap_block_lo);
		if (fs->m_feature_incompat &
		    EXT4FS_FEATURE_INCOMPAT_64BIT)
			bitmap_blk |= (u_int64_t)
			    letoh32(gd->bgd_inode_bitmap_block_hi)
			    << 32;

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
		for (ino_in_group = 0;
		    ino_in_group < fs->m_inodes_per_group;
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
				error = VFS_VGET(pvp->v_mount, ino,
				    vpp);
				if (error) {
					ext4fs_inode_free(pip, ino,
					    mode);
					return (error);
				}

				/* Clear INODE_UNINIT flag if set */
				gd->bgd_flags = htole16(
				    letoh16(gd->bgd_flags) &
				    ~EXT4FS_BGD_FLAG_INODE_UNINIT);

				/* Update BGD free count */
				free_inodes--;
				ext4fs_bgd_set_count(fs, gd,
				    EXT4FS_BGD_FREE_INODES,
				    free_inodes);

				if ((mode & S_IFMT) ==
				    S_IFDIR) {
					dirs =
					    ext4fs_bgd_get_count(fs, gd,
					    EXT4FS_BGD_USED_DIRS);
					dirs++;
					ext4fs_bgd_set_count(fs, gd,
					    EXT4FS_BGD_USED_DIRS, dirs);
				}

				itu = letoh16(
				    gd->bgd_inode_table_unused_lo);
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
						    htole16(
					    (itu >> 16) &
					    0xFFFF);
				}

				ext4fs_bgd_write_direct(fs,
				    pip->i_devvp, g);

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
				din = &ip->i_e4din->dinode;
				eh = &din->i_extent_header;
				eh->eh_magic = htole16(
				    EXT4FS_EXTENT_HEADER_MAGIC);
				eh->eh_entries = htole16(0);
				eh->eh_max = htole16(4);
				eh->eh_depth = htole16(0);
				ip->i_e4din->dinode.i_flags =
				    htole32(EXTFS_INODE_FLAG_EXTENTS);

				/* Set extra_isize */
				ip->i_e4din->dinode.i_extra_isize =
				    htole16(
				    sizeof(struct ext4fs_dinode) -
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

int
ext4fs_inode_alloc_handle (struct inode *pip, mode_t mode,
    struct ext4fs_journal_handle *handle, struct vnode **vpp)
{
	struct m_ext4fs *fs = pip->i_e4fs;
	struct vnode *pvp = ITOV(pip);
	struct ext4fs_block_group_descriptor saved_gd, *gd;
	struct ext4fs saved_sb;
	struct buf *bp;
	struct inode *ip;
	u_int8_t *bitmap, *saved_bitmap, *scan;
	u_int64_t bitmap_blk, inode_start;
	u_int32_t best, best_free, bitmap_csum, dirs, first_unused;
	u_int32_t free_bits, free_inodes, g, group, i, ino_in_group;
	u_int32_t itu, ngroups, pbit, saved_free_inodes, start, valid;
	ufsino_t ino;
	int error, saved_modified, transaction_dirty, uninit;
	int validated;

	if (handle == NULL || vpp == NULL)
		return (EINVAL);
	*vpp = NULL;
	if (fs->m_free_inodes_count == 0)
		return (ENOSPC);
	if (fs->m_block_group_count == 0 ||
	    fs->m_block_group_count > UINT32_MAX ||
	    fs->m_inode_alloc_cursor == NULL ||
	    fs->m_inodes_per_group == 0 ||
	    fs->m_inodes_per_group > fs->m_block_size * NBBY)
		return (EFBIG);

	ngroups = (u_int32_t)fs->m_block_group_count;
	if ((mode & S_IFMT) == S_IFDIR) {
		best = 0;
		best_free = 0;
		for (i = 0; i < ngroups; i++) {
			gd = &fs->m_gd[i];
			free_inodes = ext4fs_bgd_get_count(fs, gd,
			    EXT4FS_BGD_FREE_INODES);
			if (free_inodes > best_free) {
				best_free = free_inodes;
				best = i;
			}
		}
		group = best;
	} else
		group = (pip->i_number - 1) / fs->m_inodes_per_group;
	if (group >= ngroups)
		return (EINVAL);

	bitmap = malloc(fs->m_block_size, M_UFSMNT, M_WAITOK);
	saved_bitmap = malloc(fs->m_block_size, M_UFSMNT, M_WAITOK);
	bp = NULL;
	error = ENOSPC;
	for (i = 0; i < ngroups; i++) {
		g = (group + i) % ngroups;
		gd = &fs->m_gd[g];
		inode_start = (u_int64_t)g * fs->m_inodes_per_group;
		if (inode_start >= fs->m_inodes_count)
			continue;
		valid = fs->m_inodes_count - inode_start;
		if (valid > fs->m_inodes_per_group)
			valid = fs->m_inodes_per_group;
		free_inodes = ext4fs_bgd_get_count(fs, gd,
		    EXT4FS_BGD_FREE_INODES);
		if (free_inodes == 0)
			continue;
		if (free_inodes > valid ||
		    free_inodes > fs->m_free_inodes_count) {
			error = EIO;
			goto out;
		}

		bitmap_blk = ext4fs_bgd_get_block(fs, gd,
		    EXT4FS_BGD_INODE_BITMAP);
		if (bitmap_blk < fs->m_first_data_block ||
		    bitmap_blk >= fs->m_blocks_count) {
			error = EIO;
			goto out;
		}
		error = ext4fs_journal_get_metadata(handle,
		    pip->i_devvp, bitmap_blk, &bp);
		if (error)
			goto out;
		error = ext4fs_journal_metadata_validated(handle, bp,
		    EXT4FS_JOURNAL_VALID_INODE_BITMAP, &validated);
		if (error)
			goto out;
		uninit = letoh16(gd->bgd_flags) &
		    EXT4FS_BGD_FLAG_INODE_UNINIT;
		if (uninit) {
			memset(bitmap, 0, fs->m_block_size);
			for (pbit = valid;
			    pbit < fs->m_block_size * NBBY;
			    pbit++)
				setbit(bitmap, pbit);
			for (pbit = 0; pbit < valid &&
			    inode_start + pbit + 1 <
			    fs->m_first_non_reserved_inode; pbit++)
				setbit(bitmap, pbit);
			scan = bitmap;
		} else if (! validated) {
			error = ext4fs_inode_bitmap_csum_verify(fs, g,
			    gd, bp->b_data);
			if (error)
				goto out;
		}
		if (! uninit)
			scan = bp->b_data;
		if (! validated) {
			free_bits = 0;
			for (pbit = 0; pbit < valid; pbit++)
				if (isclr(scan, pbit))
					free_bits++;
			if (free_bits != free_inodes) {
				error = EIO;
				goto out;
			}
			error = ext4fs_journal_metadata_mark_validated(
			    handle, bp,
			    EXT4FS_JOURNAL_VALID_INODE_BITMAP);
			if (error)
				goto out;
		}

		start = fs->m_inode_alloc_cursor[g];
		if (start >= valid)
			start = 0;
		ino_in_group = valid;
		for (pbit = start; pbit < valid; pbit++) {
			if (isclr(scan, pbit)) {
				ino_in_group = pbit;
				break;
			}
		}
		if (ino_in_group == valid && start != 0) {
			for (pbit = 0; pbit < start; pbit++) {
				if (isclr(scan, pbit)) {
					ino_in_group = pbit;
					break;
				}
			}
		}
		if (ino_in_group == valid) {
			error = EIO;
			goto out;
		}
		ino = inode_start + ino_in_group + 1;
		if (ino < fs->m_first_non_reserved_inode) {
			error = EIO;
			goto out;
		}

		memcpy(saved_bitmap, bp->b_data, fs->m_block_size);
		saved_gd = *gd;
		saved_sb = fs->m_sble;
		saved_free_inodes = fs->m_free_inodes_count;
		saved_modified = fs->m_fs_was_modified;
		transaction_dirty = 0;
		if (uninit)
			memcpy(bp->b_data, bitmap, fs->m_block_size);
		setbit((u_int8_t *)bp->b_data, ino_in_group);

		/*
		 * Load the vnode while the unused-table boundary makes
		 * the inode reader return a zeroed inode.  Pass the
		 * handle in case its table block is already busy here.
		 */
		error = ext4fs_vget_handle(pvp->v_mount, ino, handle,
		    vpp);
		if (error) {
			memcpy(bp->b_data, saved_bitmap,
			    fs->m_block_size);
			goto out;
		}

		if (uninit)
			gd->bgd_flags = htole16(letoh16(gd->bgd_flags) &
			    ~EXT4FS_BGD_FLAG_INODE_UNINIT);
		bitmap_csum = ext4fs_bitmap_csum(fs, g, bp->b_data,
		    howmany(fs->m_inodes_per_group, NBBY));
		gd->bgd_inode_bitmap_checksum_lo =
		    htole16(bitmap_csum & 0xffff);
		if (fs->m_feature_incompat &
		    EXT4FS_FEATURE_INCOMPAT_64BIT)
			gd->bgd_inode_bitmap_checksum_hi =
			    htole16(bitmap_csum >> 16);
		free_inodes--;
		ext4fs_bgd_set_count(fs, gd, EXT4FS_BGD_FREE_INODES,
		    free_inodes);
		dirs = ext4fs_bgd_get_count(fs, gd,
		    EXT4FS_BGD_USED_DIRS);
		if ((mode & S_IFMT) == S_IFDIR) {
			if (dirs == UINT32_MAX) {
				error = EIO;
				goto restore;
			}
			dirs++;
			ext4fs_bgd_set_count(fs, gd,
			    EXT4FS_BGD_USED_DIRS, dirs);
		}
		itu = letoh16(gd->bgd_inode_table_unused_lo);
		if (fs->m_feature_incompat &
		    EXT4FS_FEATURE_INCOMPAT_64BIT)
			itu |= (u_int32_t)letoh16(
			    gd->bgd_inode_table_unused_hi) << 16;
		if (itu > fs->m_inodes_per_group) {
			error = EIO;
			goto restore;
		}
		first_unused = fs->m_inodes_per_group - itu;
		if (ino_in_group >= first_unused) {
			itu = fs->m_inodes_per_group - ino_in_group - 1;
			gd->bgd_inode_table_unused_lo =
			    htole16(itu & 0xffff);
			if (fs->m_feature_incompat &
			    EXT4FS_FEATURE_INCOMPAT_64BIT)
				gd->bgd_inode_table_unused_hi =
				    htole16(itu >> 16);
		}
		fs->m_free_inodes_count--;
		fs->m_fs_was_modified = 1;

		error = ext4fs_journal_dirty_metadata(handle, bp);
		if (error)
			goto restore;
		transaction_dirty = 1;
		error = ext4fs_bgd_write_handle(fs, pip->i_devvp, g,
		    handle);
		if (error)
			goto restore;
		error = ext4fs_sbwrite_handle(pvp->v_mount, handle);
		if (error)
			goto restore;

		ip = VTOI(*vpp);
		memset(ip->i_e4din, 0,
		    sizeof(struct ext4fs_dinode_256));
		ip->i_e4din->dinode.i_extent_header.eh_magic =
		    htole16(EXT4FS_EXTENT_HEADER_MAGIC);
		ip->i_e4din->dinode.i_extent_header.eh_max = htole16(4);
		ip->i_e4din->dinode.i_flags =
		    htole32(EXTFS_INODE_FLAG_EXTENTS);
		ip->i_e4din->dinode.i_extra_isize =
		    htole16(sizeof(struct ext4fs_dinode) - 128);
		if (++ext4fs_gennumber < (u_long)gettime())
			ext4fs_gennumber = gettime();
		ip->i_e4din->dinode.i_nfs_generation =
		    htole32(ext4fs_gennumber);
		fs->m_inode_alloc_cursor[g] = ino_in_group + 1;
		if (fs->m_inode_alloc_cursor[g] >= valid)
			fs->m_inode_alloc_cursor[g] = 0;
		error = 0;
		goto out;

restore:
		memcpy(bp->b_data, saved_bitmap, fs->m_block_size);
		*gd = saved_gd;
		fs->m_sble = saved_sb;
		fs->m_free_inodes_count = saved_free_inodes;
		fs->m_fs_was_modified = saved_modified;
		if (transaction_dirty)
			ext4fs_journal_abort(pvp->v_mount, error);
		if (*vpp != NULL) {
			ip = VTOI(*vpp);
			ip->i_e4din->dinode.i_mode = htole16(0);
			ip->i_e4din->dinode.i_links_count = htole16(0);
			ip->i_e4din->dinode.i_dtime = htole32(1);
			ip->i_effnlink = 0;
			(*vpp)->v_type = VNON;
			vput(*vpp);
			*vpp = NULL;
		}
		goto out;
	}

out:
	free(saved_bitmap, M_UFSMNT, fs->m_block_size);
	free(bitmap, M_UFSMNT, fs->m_block_size);
	return (error);
}

int
ext4fs_inode_bitmap_csum_verify (struct m_ext4fs *fs, u_int32_t group,
    struct ext4fs_block_group_descriptor *gd, const void *bitmap)
{
	u_int32_t calculated, provided;

	if (! (fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return (0);
	provided = letoh16(gd->bgd_inode_bitmap_checksum_lo);
	calculated = ext4fs_bitmap_csum(fs, group, bitmap,
	    howmany(fs->m_inodes_per_group, NBBY));
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		provided |= (u_int32_t)
		    letoh16(gd->bgd_inode_bitmap_checksum_hi) << 16;
	else
		calculated &= 0xffff;
	if (provided != calculated) {
		ext4fs_print(fs->m_mountp,
		    "inode bitmap %u checksum mismatch: stored=0x%08x "
		    "calculated=0x%08x\n", group, provided,
		    calculated);
		return (EINVAL);
	}
	return (0);
}

int
ext4fs_inode_free_handle (struct inode *pip, ufsino_t ino, mode_t mode,
    struct ext4fs_journal_handle *handle)
{
	struct m_ext4fs *fs = pip->i_e4fs;
	struct ext4fs_block_group_descriptor saved_gd, *gd;
	struct ext4fs saved_sb;
	struct buf *bp;
	u_int8_t *saved_bitmap;
	u_int64_t bitmap_blk, inode_start;
	u_int32_t bitmap_csum, dirs, free_bits, free_inodes, group;
	u_int32_t i, ino_in_group, saved_free_inodes, valid;
	int error, saved_modified, transaction_changed, validated;

	if (handle == NULL || ino < fs->m_first_non_reserved_inode ||
	    ino > fs->m_inodes_count || fs->m_inodes_per_group == 0 ||
	    fs->m_inodes_per_group > fs->m_block_size * NBBY ||
	    fs->m_block_group_count == 0 ||
	    fs->m_block_group_count > UINT32_MAX ||
	    fs->m_inode_alloc_cursor == NULL)
		return (EINVAL);
	group = (ino - 1) / fs->m_inodes_per_group;
	ino_in_group = (ino - 1) % fs->m_inodes_per_group;
	if (group >= fs->m_block_group_count)
		return (EINVAL);
	inode_start = (u_int64_t)group * fs->m_inodes_per_group;
	if (inode_start >= fs->m_inodes_count)
		return (EINVAL);
	valid = fs->m_inodes_count - inode_start;
	if (valid > fs->m_inodes_per_group)
		valid = fs->m_inodes_per_group;
	if (ino_in_group >= valid)
		return (EINVAL);

	gd = &fs->m_gd[group];
	if (letoh16(gd->bgd_flags) & EXT4FS_BGD_FLAG_INODE_UNINIT)
		return (EINVAL);
	bitmap_blk = ext4fs_bgd_get_block(fs, gd,
	    EXT4FS_BGD_INODE_BITMAP);
	if (bitmap_blk < fs->m_first_data_block ||
	    bitmap_blk >= fs->m_blocks_count)
		return (EIO);
	error = ext4fs_journal_get_metadata(handle, pip->i_devvp,
	    bitmap_blk, &bp);
	if (error)
		return (error);
	error = ext4fs_journal_metadata_validated(handle, bp,
	    EXT4FS_JOURNAL_VALID_INODE_BITMAP, &validated);
	if (error)
		return (error);
	free_inodes = ext4fs_bgd_get_count(fs, gd,
	    EXT4FS_BGD_FREE_INODES);
	if (! validated) {
		error = ext4fs_inode_bitmap_csum_verify(fs, group, gd,
		    bp->b_data);
		if (error)
			return (error);
		free_bits = 0;
		for (i = 0; i < valid; i++)
			if (isclr((u_int8_t *)bp->b_data, i))
				free_bits++;
		if (free_bits != free_inodes)
			return (EIO);
		error = ext4fs_journal_metadata_mark_validated(handle,
		    bp, EXT4FS_JOURNAL_VALID_INODE_BITMAP);
		if (error)
			return (error);
	}
	if (isclr((u_int8_t *)bp->b_data, ino_in_group))
		return (EINVAL);
	if (free_inodes >= valid ||
	    fs->m_free_inodes_count >= fs->m_inodes_count)
		return (EIO);
	dirs = ext4fs_bgd_get_count(fs, gd, EXT4FS_BGD_USED_DIRS);
	if ((mode & S_IFMT) == S_IFDIR && dirs == 0)
		return (EIO);

	saved_bitmap = malloc(fs->m_block_size, M_UFSMNT, M_WAITOK);
	memcpy(saved_bitmap, bp->b_data, fs->m_block_size);
	saved_gd = *gd;
	saved_sb = fs->m_sble;
	saved_free_inodes = fs->m_free_inodes_count;
	saved_modified = fs->m_fs_was_modified;
	transaction_changed = 0;

	clrbit((u_int8_t *)bp->b_data, ino_in_group);
	bitmap_csum = ext4fs_bitmap_csum(fs, group, bp->b_data,
	    howmany(fs->m_inodes_per_group, NBBY));
	gd->bgd_inode_bitmap_checksum_lo =
	    htole16(bitmap_csum & 0xffff);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		gd->bgd_inode_bitmap_checksum_hi =
		    htole16(bitmap_csum >> 16);
	free_inodes++;
	ext4fs_bgd_set_count(fs, gd, EXT4FS_BGD_FREE_INODES,
	    free_inodes);
	if ((mode & S_IFMT) == S_IFDIR) {
		dirs--;
		ext4fs_bgd_set_count(fs, gd, EXT4FS_BGD_USED_DIRS,
		    dirs);
	}
	fs->m_free_inodes_count++;
	fs->m_fs_was_modified = 1;

	error = ext4fs_journal_dirty_metadata(handle, bp);
	if (error)
		goto restore;
	transaction_changed = 1;
	error = ext4fs_bgd_write_handle(fs, pip->i_devvp, group,
	    handle);
	if (error)
		goto restore;
	error = ext4fs_sbwrite_handle(ITOV(pip)->v_mount, handle);
	if (error)
		goto restore;
	fs->m_inode_alloc_cursor[group] = ino_in_group;
	free(saved_bitmap, M_UFSMNT, fs->m_block_size);
	return (0);

restore:
	memcpy(bp->b_data, saved_bitmap, fs->m_block_size);
	*gd = saved_gd;
	fs->m_sble = saved_sb;
	fs->m_free_inodes_count = saved_free_inodes;
	fs->m_fs_was_modified = saved_modified;
	free(saved_bitmap, M_UFSMNT, fs->m_block_size);
	if (transaction_changed)
		ext4fs_journal_abort(ITOV(pip)->v_mount, error);
	return (error);
}

/*
 * Free an inode on journal-less ext4.  Journal-bearing mounts retire
 * inodes through ext4fs_inode_free_handle().
 */
void
ext4fs_inode_free (struct inode *pip, ufsino_t ino, mode_t mode)
{
	struct m_ext4fs *fs = pip->i_e4fs;
	struct ext4fs_block_group_descriptor *gd;
	struct buf *bp;
	u_int64_t bitmap_blk;
	u_int32_t group, ino_in_group, free_inodes, icsum;
	char *ibp;
	int error;

	if (fs->m_journal != NULL) {
		ext4fs_journal_abort(ITOV(pip)->v_mount, EIO);
		return;
	}
	group = (ino - 1) / fs->m_inodes_per_group;
	ino_in_group = (ino - 1) % fs->m_inodes_per_group;
	gd = &fs->m_gd[group];

	bitmap_blk = letoh32(gd->bgd_inode_bitmap_block_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		bitmap_blk |= (u_int64_t)
		    letoh32(gd->bgd_inode_bitmap_block_hi) << 32;

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
	free_inodes = ext4fs_bgd_get_count(fs, gd,
	    EXT4FS_BGD_FREE_INODES);
	free_inodes++;
	ext4fs_bgd_set_count(fs, gd, EXT4FS_BGD_FREE_INODES,
	    free_inodes);

	if ((mode & S_IFMT) == S_IFDIR) {
		u_int32_t dirs;
		dirs = ext4fs_bgd_get_count(fs, gd,
		    EXT4FS_BGD_USED_DIRS);
		dirs--;
		ext4fs_bgd_set_count(fs, gd, EXT4FS_BGD_USED_DIRS,
		    dirs);
	}

	ext4fs_bgd_write_direct(fs, pip->i_devvp, group);

	/* Update superblock counters */
	fs->m_free_inodes_count++;
	fs->m_sble.sb_free_inodes_count =
	    htole32(fs->m_free_inodes_count);
	fs->m_fs_was_modified = 1;
}

static int
ext4fs_sync_vnode (struct vnode *vp, void *arg)
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

	if ((error = VOP_FSYNC(vp, esa->cred, esa->waitfor,
	    esa->p)) != 0)
		esa->allerror = error;

	vput(vp);
	return (0);
}

int
ext4fs_sync (struct mount *mp, int waitfor, int stall,
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

	if (waitfor != MNT_LAZY)
		vfs_mount_foreach_vnode(mp, ext4fs_sync_vnode, &esa);

	if (fs->m_journal != NULL) {
		error = ext4fs_journal_commit(mp,
		    waitfor == MNT_WAIT ?
		    EXT4FS_JOURNAL_COMMIT_VFS_SYNC :
		    EXT4FS_JOURNAL_COMMIT_ORDINARY);
		if (error != 0)
			esa.allerror = error;
	} else if (fs->m_fs_was_modified) {
		if ((error = ext4fs_sbwrite_direct(mp)))
			esa.allerror = error;
	}

	if (waitfor != MNT_LAZY) {
		/* Flush checkpointed and journal-less metadata. */
		vn_lock(ump->um_devvp, LK_EXCLUSIVE | LK_RETRY);
		if ((error = VOP_FSYNC(ump->um_devvp, cred,
		    waitfor, p)))
			esa.allerror = error;
		VOP_UNLOCK(ump->um_devvp);
	}

	return (esa.allerror);
}

int
ext4fs_sysctl (int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen, struct proc *p)
{
	(void)name;
	(void)namelen;
	(void)oldp;
	(void)oldlenp;
	(void)newp;
	(void)newlen;
	(void)p;
	return (EOPNOTSUPP);
}

int
ext4fs_unmount (struct mount *mp, int mntflags, struct proc *p)
{
	struct ufsmount *ump;
	struct m_ext4fs *mfs;
	int error, flags, journal_error;

	flags = 0;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	if ((error = ext4fs_flushfiles(mp, flags, p)) != 0)
		return (error);
	ump = VFSTOUFS(mp);
	mfs = ump->um_e4fs;
	journal_error = ext4fs_journal_error(mp);
	if (ext4fs_orphan_pending(mp)) {
		if (journal_error == 0)
			return (EBUSY);
		/* Durable orphan roots remain for recovery. */
		ext4fs_orphan_runtime_discard(mp);
	}
	if (mfs->m_journal != NULL) {
		if (journal_error == 0 &&
		    (error = ext4fs_journal_mark_clean(mp,
		    EXT4FS_JOURNAL_COMMIT_UNMOUNT)) != 0)
			return (error);
	} else if (! mfs->m_read_only && mfs->m_fs_was_modified) {
		mfs->m_state = EXT4FS_STATE_VALID;
		if ((error = ext4fs_sbwrite_direct(mp)) != 0)
			return (error);
	}
	ext4fs_journal_destroy(mp);
	ext4fs_allocator_cursors_destroy(mfs);

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
ext4fs_vinit (struct mount *mp, struct vnode **vpp)
{
	struct inode *ip;
	struct vnode *nvp, *vp;
	dev_t rdev;

	vp = *vpp;
	ip = VTOI(vp);
	vp->v_type = IFTOVT(letoh16(ip->i_e4din->dinode.i_mode));

	switch (vp->v_type) {
	case VCHR:
	case VBLK:
		vp->v_op = &ext4fs_specvops;
		rdev = letoh32(ip->i_e4din->dinode.i_block[0]);
		if (rdev == 0)
			rdev = letoh32(ip->i_e4din->dinode.i_block[1]);
		nvp = checkalias(vp, rdev, mp);
		if (nvp != NULL) {
			nvp->v_data = vp->v_data;
			vp->v_data = NULL;
			vp->v_op = &spec_vops;
#ifdef VFSLCKDEBUG
			vp->v_flag &= ~VLOCKSWORK;
#endif
			vrele(vp);
			vgone(vp);
			vp = nvp;
			ip->i_vnode = vp;
		}
		break;
	case VFIFO:
#ifdef FIFO
		vp->v_op = &ext4fs_fifovops;
		break;
#else
		return (EOPNOTSUPP);
#endif
	default:
		break;
	}

	if (ip->i_number == EXT4FS_INODE_ROOT_DIR)
		vp->v_flag |= VROOT;
	*vpp = vp;
	return (0);
}

int
ext4fs_vget (struct mount *mp, ino_t ino, struct vnode **vpp)
{
	return (ext4fs_vget_handle(mp, ino, NULL, vpp));
}

static int
ext4fs_vget_handle (struct mount *mp, ino_t ino,
    struct ext4fs_journal_handle *handle, struct vnode **vpp)
{
	struct m_ext4fs *fs;
	struct inode *ip;
	struct ufsmount *ump;
	struct buf *bp;
	struct vnode *vp;
	struct ext4fs_block_group_descriptor *gd;
	struct ext4fs_dinode *dp;
	dev_t dev;
	u_int64_t fsblock, inode_table_block;
	u_int32_t inode_group, inode_index, block_in_table;
	u_int32_t offset_in_block;
	u_int32_t itable_unused;
	u_int16_t bgd_flags;
	int error;

	if (ino > (ufsino_t)-1)
		panic("ext4fs: %s: vget: alien ino_t %llu",
		    mp->mnt_stat.f_mntonname,
		    (unsigned long long)ino);

	ump = VFSTOUFS(mp);
	dev = ump->um_dev;
	fs = ump->um_e4fs;

retry:
	if ((*vpp = ufs_ihashget(dev, ino)) != NULL) {
		return (0);
	}

	/* Allocate a new vnode/inode. */
	if ((error = getnewvnode(VT_EXT4FS, mp, &ext4fs_vops,
	    &vp)) != 0) {
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
	 * Put it on its hash chain and lock it so other requests for
	 * this inode block if they arrive while we wait for old
	 * data structures to be purged or for the contents of the
	 * disk portion of this inode to be read.
	 */
	error = ufs_ihashins(ip);

	if (error) {
		/*
		 * ufs_ihashins locked and then unlocked the vnode on
		 * error.  We need to clean up the inode and vnode.
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
	if (fs->m_feature_incompat &
	    EXT4FS_FEATURE_INCOMPAT_64BIT)
		inode_table_block |= (u_int64_t)
		    letoh32(gd->bgd_inode_table_block_hi) << 32;

	/* Read the block containing this inode */
	fsblock = inode_table_block + block_in_table;
	bp = NULL;
	if (handle != NULL)
		error = ext4fs_journal_get_metadata(handle,
		    ump->um_devvp, fsblock, &bp);
	else
		error = ext4fs_journal_read_metadata(mp,
		    ump->um_devvp, fsblock, &bp);
	if (error) {
		vput(vp);
		if (bp != NULL && handle == NULL)
			brelse(bp);
		*vpp = NULL;
		return (error);
	}

	dp = (struct ext4fs_dinode *)
	    ((char *)bp->b_data + offset_in_block);

	/* Allocate space for on-disk inode */
	ip->i_e4din = pool_get(&ext4fs_dinode_pool, PR_WAITOK|PR_ZERO);

	/*
	 * If the group has INODE_UNINIT set, or the inode is in the
	 * unused portion of the inode table, the on-disk data is
	 * garbage. Keep zeroed data and skip checksum verification.
	 */
	bgd_flags = letoh16(gd->bgd_flags);
	itable_unused = letoh16(gd->bgd_inode_table_unused_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		itable_unused |= (u_int32_t)
		    letoh16(gd->bgd_inode_table_unused_hi) << 16;
	if ((bgd_flags & EXT4FS_BGD_FLAG_INODE_UNINIT) ||
	    inode_index >= fs->m_inodes_per_group - itable_unused) {
		if (handle == NULL)
			brelse(bp);
	} else {
		memcpy(ip->i_e4din, dp, fs->m_inode_size);
		if (handle == NULL)
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

	/* Set effective link count */
	ip->i_effnlink = letoh16(ip->i_e4din->dinode.i_links_count);

	/* If the inode was deleted, reset all fields */
	if (letoh32(ip->i_e4din->dinode.i_dtime) != 0) {
		vp->v_type = VNON;
		ip->i_effnlink = 0;
	} else {
		error = ext4fs_vinit(mp, &vp);
		if (error) {
			vput(vp);
			*vpp = NULL;
			return (error);
		}
	}

	*vpp = vp;
	return (0);
}

int
ext4fs_vptofh (struct vnode *vp, struct fid *fhp)
{
	(void)fhp;
	ext4fs_print(vp->v_mount, "vptofh is not implemented\n");
	return (EOPNOTSUPP);
}
