/*
 * Copyright (c) 2025 kmx.io.
 * Copyright (c) 1997 Manuel Bouyer.
 * Copyright (c) 1982, 1986, 1989, 1993
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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/proc.h>
#include <sys/ucred.h>

#include <ufs/ufs/dinode.h>
#include <ufs/ext4fs/ext4fs_crc32c.h>

struct fid;
struct inode;
struct nameidata;
struct statfs;
struct vfsconf;

#define EXT4FS_EXTENT_DEPTH_MAX		5
#define EXT4FS_FUNCTION_MAX		32
#define EXT4FS_REV_EXT2			0
#define EXT4FS_REV_DYNAMIC		1
#define EXT4FS_REV_MINOR		0
#define EXT4FS_LAST_MOUNTED_MAX		64
#define EXT4FS_LOG_MIN_BLOCK_SIZE	10
#define EXT4FS_MAGIC			0xEF53
#define EXT4FS_MOUNT_OPTS_MAX		64
#define EXT4FS_LINK_MAX			65000
#define EXT4FS_MAXNAMLEN		255
#define EXT4FS_SUPER_BLOCK_OFFSET	1024
#define EXT4FS_SUPER_BLOCK_SIZE		1024
#define EXT4FS_VOLUME_NAME_MAX		16

#define	EXT4FS_DIRECT_ADDR_IN_INODE	12
#define	EXT4FS_INDIRECT_ADDR_IN_INODE	3
#define EXT4FS_SYMLINK_LEN_MAX \
	((EXT4FS_DIRECT_ADDR_IN_INODE +				\
	  EXT4FS_INDIRECT_ADDR_IN_INODE) * sizeof(u_int32_t))

#define	EXT4FS_NINDIR(fs)	((fs)->m_block_size / sizeof(u_int32_t))

#define EXT4FS_LBLKNO(fs, offset)  ((offset) >> (fs)->m_block_size_shift)
#define EXT4FS_BLKOFF(fs, offset)  ((offset) & ((fs)->m_block_size - 1))
#define EXT4FS_FSBTODB(fs, b)      ((b) << (fs)->m_fs_block_to_disk_block)

#define EXT4FS_CHECKSUM_TYPE_NONE	0x0000
#define EXT4FS_CHECKSUM_TYPE_CRC32C	0x0001

#define EXT4FS_ENCODING_NONE	0x0000	// legacy behavior
#define EXT4FS_ENCODING_UTF8	0x0001	// UTF-8, Unicode 12.1.0

#define EXT4FS_ENCODING_FLAG_NONE	 0x0000
#define EXT4FS_ENCODING_FLAG_STRICT_MODE 0x0001 // Reject invalid encoding

#define EXT4FS_ERRORS_CONTINUE	1	// Log and keep going
#define EXT4FS_ERRORS_RO	2	// Remount read-only
#define EXT4FS_ERRORS_PANIC	3	// Kernel panic

#define EXT4FS_FEATURE_COMPAT_DIR_PREALLOC	0x0001
#define EXT4FS_FEATURE_COMPAT_IMAGIC_INODES	0x0002
#define EXT4FS_FEATURE_COMPAT_HAS_JOURNAL	0x0004
#define EXT4FS_FEATURE_COMPAT_EXT_ATTR		0x0008
#define EXT4FS_FEATURE_COMPAT_RESIZE_INODE	0x0010
#define EXT4FS_FEATURE_COMPAT_DIR_INDEX		0x0020

#define EXT4FS_FEATURE_INCOMPAT_COMPRESSION	0x00001
#define EXT4FS_FEATURE_INCOMPAT_FILETYPE	0x00002
#define EXT4FS_FEATURE_INCOMPAT_RECOVER		0x00004
#define EXT4FS_FEATURE_INCOMPAT_JOURNAL_DEV	0x00008
#define EXT4FS_FEATURE_INCOMPAT_META_BG		0x00010
#define EXT4FS_FEATURE_INCOMPAT_EXTENTS		0x00040
#define EXT4FS_FEATURE_INCOMPAT_64BIT		0x00080
#define EXT4FS_FEATURE_INCOMPAT_MMP		0x00100
#define EXT4FS_FEATURE_INCOMPAT_FLEX_BG		0x00200
#define EXT4FS_FEATURE_INCOMPAT_EA_INODE	0x00400
#define EXT4FS_FEATURE_INCOMPAT_DIRDATA		0x01000
#define EXT4FS_FEATURE_INCOMPAT_CSUM_SEED	0x02000
#define EXT4FS_FEATURE_INCOMPAT_LARGEDIR	0x04000
#define EXT4FS_FEATURE_INCOMPAT_INLINE_DATA	0x08000
#define EXT4FS_FEATURE_INCOMPAT_ENCRYPT		0x10000

#define EXT4FS_FEATURE_INCOMPAT_SUPPORTED	\
	(EXT4FS_FEATURE_INCOMPAT_FILETYPE |	\
	 EXT4FS_FEATURE_INCOMPAT_RECOVER |	\
	 EXT4FS_FEATURE_INCOMPAT_EXTENTS |	\
	 EXT4FS_FEATURE_INCOMPAT_64BIT |	\
	 EXT4FS_FEATURE_INCOMPAT_FLEX_BG |	\
	 EXT4FS_FEATURE_INCOMPAT_CSUM_SEED)

#define EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER   0x0001
#define EXT4FS_FEATURE_RO_COMPAT_LARGE_FILE     0x0002
#define EXT4FS_FEATURE_RO_COMPAT_BTREE_DIR      0x0004
#define EXT4FS_FEATURE_RO_COMPAT_HUGE_FILE      0x0008
#define EXT4FS_FEATURE_RO_COMPAT_GDT_CSUM       0x0010
#define EXT4FS_FEATURE_RO_COMPAT_DIR_NLINK      0x0020
#define EXT4FS_FEATURE_RO_COMPAT_EXTRA_ISIZE    0x0040
#define EXT4FS_FEATURE_RO_COMPAT_HAS_SNAPSHOT   0x0080
#define EXT4FS_FEATURE_RO_COMPAT_QUOTA          0x0100
#define EXT4FS_FEATURE_RO_COMPAT_BIGALLOC       0x0200
#define EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM  0x0400
#define EXT4FS_FEATURE_RO_COMPAT_REPLICA        0x0800
#define EXT4FS_FEATURE_RO_COMPAT_READONLY       0x1000
#define EXT4FS_FEATURE_RO_COMPAT_PROJECT        0x2000
#define EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x10000

#define EXT4FS_FEATURE_RO_COMPAT_SUPPORTED		\
	(EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER |	\
	 EXT4FS_FEATURE_RO_COMPAT_LARGE_FILE |		\
	 EXT4FS_FEATURE_RO_COMPAT_HUGE_FILE |		\
	 EXT4FS_FEATURE_RO_COMPAT_DIR_NLINK |		\
	 EXT4FS_FEATURE_RO_COMPAT_EXTRA_ISIZE |		\
	 EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM |	\
	 EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT)

#define EXT4FS_FLAG_SIGNED_HASH		0x0001
#define EXT4FS_FLAG_UNSIGNED_HASH	0x0002
#define EXT4FS_FLAG_TEST_FILESYS	0x0004
#define EXT4FS_FLAG_64BIT		0x0008
#define EXT4FS_FLAG_MOUNT_OPT_CHECK	0x0010

#define EXT4FS_INODE_BAD_BLOCKS		1
#define EXT4FS_INODE_ROOT_DIR		2
#define EXT4FS_INODE_USER_QUOTA		3
#define EXT4FS_INODE_GROUP_QUOTA	4
#define EXT4FS_INODE_BOOT_LOADER	5
#define EXT4FS_INODE_JOURNAL		8
#define EXT4FS_INODE_FIRST		11

#define EXTFS_INODE_FLAG_SECURE_RM			0x00000001
#define EXTFS_INODE_FLAG_UN_RM				0x00000002
#define EXTFS_INODE_FLAG_COMPRESSION			0x00000004
#define EXTFS_INODE_FLAG_SYNC				0x00000008
#define EXTFS_INODE_FLAG_IMMUTABLE			0x00000010
#define EXTFS_INODE_FLAG_APPEND				0x00000020
#define EXTFS_INODE_FLAG_NO_DUMP			0x00000040
#define EXTFS_INODE_FLAG_NO_ATIME			0x00000080
#define EXTFS_INODE_FLAG_DIRTY				0x00000100
#define EXTFS_INODE_FLAG_COMPRESSED_BLOCKS		0x00000200
#define EXTFS_INODE_FLAG_NO_COMPRESSION			0x00000400
#define EXTFS_INODE_FLAG_ENCRYPTED			0x00000800
#define EXTFS_INODE_FLAG_INDEX				0x00001000
#define EXTFS_INODE_FLAG_IMAGIC				0x00002000
#define EXTFS_INODE_FLAG_JOURNAL_DATA			0x00004000
#define EXTFS_INODE_FLAG_NO_TAIL			0x00008000
#define EXTFS_INODE_FLAG_DIR_SYNC			0x00010000
#define EXTFS_INODE_FLAG_TOP_DIR			0x00020000
#define EXTFS_INODE_FLAG_HUGE_FILE			0x00040000
#define EXTFS_INODE_FLAG_EXTENTS			0x00080000
#define EXTFS_INODE_FLAG_EXTENDED_ATTRIBUTES_INODE	0x00200000
#define EXTFS_INODE_FLAG_EOF_BLOCKS			0x00400000
#define EXTFS_INODE_FLAG_INLINE_DATA			0x10000000
#define EXTFS_INODE_FLAG_PROJECT_ID_INHERITANCE		0x20000000
#define EXTFS_INODE_FLAG_CASEFOLD			0x40000000

#define EXT4FS_MOUNT_READONLY			0x0001
#define EXT4FS_MOUNT_NO_ATIME			0x0002
#define EXT4FS_MOUNT_DIRSYNC			0x0004
#define EXT4FS_MOUNT_DATA_JOURNAL		0x0008
#define EXT4FS_MOUNT_DATA_ORDERED		0x0010
#define EXT4FS_MOUNT_DATA_WRITEBACK		0x0020
#define EXT4FS_MOUNT_ERRORS_CONTINUE		0x0040
#define EXT4FS_MOUNT_ERRORS_REMOUNT_RO		0x0080
#define EXT4FS_MOUNT_ERRORS_PANIC		0x0100
#define EXT4FS_MOUNT_DISCARD			0x0200
#define EXT4FS_MOUNT_NO_BUFFER_HEADS		0x0400
#define EXT4FS_MOUNT_SKIP_JOURNAL		0x0800
#define EXT4FS_MOUNT_NOAUTO_DELAYED_ALLOC	0x1000

#define EXT4FS_OS_LINUX		0
#define EXT4FS_OS_HURD		1
#define EXT4FS_OS_MASIX		2
#define EXT4FS_OS_FREEBSD	3
#define EXT4FS_OS_LITES		4
#define EXT4FS_OS_OPENBSD	5

#define EXT4FS_STATE_VALID	0x0001  // Clean unmount
#define EXT4FS_STATE_ERROR	0x0002  // Errors detected (fsck needed)

#define EXT4FS_BGD_FLAG_INODE_UNINIT	0x0001
#define EXT4FS_BGD_FLAG_BLOCK_UNINIT	0x0002
#define EXT4FS_BGD_FLAG_INODE_ZEROED	0x0004
#define EXT4FS_BGD_FLAG_DIRTY		0x0008
#define EXT4FS_BGD_FLAG_BLOCK_ZEROED	0x0010
#define EXT4FS_BGD_FLAG_READ_ONLY	0x0020

struct ext4fs {
	u_int32_t	sb_inodes_count;
	u_int32_t	sb_blocks_count_lo;
	u_int32_t	sb_reserved_blocks_count_lo;
	u_int32_t	sb_free_blocks_count_lo;
	// 0x10
	u_int32_t	sb_free_inodes_count;
	u_int32_t	sb_first_data_block;
	u_int32_t	sb_log_block_size;	// log2(block size) - 10
	u_int32_t	sb_log_cluster_size;	// log2(cluster size) - 10
	// 0x20
	u_int32_t	sb_blocks_per_group;
	u_int32_t	sb_clusters_per_group;
	u_int32_t	sb_inodes_per_group;
	u_int32_t	sb_mount_time_lo;
	// 0x30
	u_int32_t	sb_write_time_lo;
	u_int16_t	sb_mount_count;
	int16_t		sb_max_mount_count_before_fsck;
	u_int16_t	sb_magic;
	u_int16_t	sb_state;		// EXT4FS_STATE_*
	u_int16_t	sb_errors;		// EXT4FS_ERRORS_*
	u_int16_t	sb_revision_level_minor;
	// 0x40
	u_int32_t	sb_check_time_lo;
	u_int32_t	sb_check_interval;
	u_int32_t	sb_creator_os;		// EXT4FS_OS_*
	u_int32_t	sb_revision_level;
	// 0x50
	u_int16_t	sb_default_reserved_uid;
	u_int16_t	sb_default_reserved_gid;
	u_int32_t	sb_first_non_reserved_inode;
	u_int16_t	sb_inode_size;
	u_int16_t	sb_block_group_id;
	u_int32_t	sb_feature_compat;
	// 0x60
	u_int32_t	sb_feature_incompat;
	u_int32_t	sb_feature_ro_compat;
	u_int8_t	sb_uuid[16];
	char		sb_volume_name[EXT4FS_VOLUME_NAME_MAX];
	char		sb_last_mounted[EXT4FS_LAST_MOUNTED_MAX];
	u_int32_t	sb_algorithm_usage_bitmap;
	u_int8_t	sb_preallocate_blocks;
	u_int8_t	sb_preallocate_dir_blocks;
	u_int16_t	sb_reserved_bgdt_blocks;
	// 0xD0
	u_int8_t	sb_journal_uuid[16];     // UUID of journal superblock
	// 0xE0
	u_int32_t	sb_journal_inode_number;
	u_int32_t	sb_journal_device_number;
	u_int32_t	sb_last_orphan;
	u_int32_t	sb_hash_seed[4];
	u_int8_t	sb_default_hash_version;
	u_int8_t	sb_journal_backup_type;
	u_int16_t	sb_block_group_descriptor_size;
	// 0x100
	u_int32_t	sb_default_mount_opts;
	u_int32_t	sb_first_meta_block_group;
	u_int32_t	sb_newfs_time_lo;
	u_int32_t	sb_jnl_blocks[17];       // Backup of journal inode
	// 0x150
	u_int32_t	sb_blocks_count_hi;
	u_int32_t	sb_reserved_blocks_count_hi;
	u_int32_t	sb_free_blocks_count_hi;
	u_int16_t	sb_inode_size_extra_min;
	u_int16_t	sb_inode_size_extra_want;
	// 0x160
	u_int32_t	sb_flags;
	u_int16_t	sb_raid_stride_block_count;
	u_int16_t	sb_mmp_interval;
	u_int64_t	sb_mmp_block;
	// 0x170
	u_int32_t	sb_raid_stripe_width_block_count;
	u_int8_t	sb_log_groups_per_flex;
	u_int8_t	sb_checksum_type;
	u_int16_t	sb_reserved_176;
	u_int64_t	sb_kilobytes_written;
	// 0x180
	u_int32_t	sb_ext3_snapshot_inode;
	u_int32_t	sb_ext3_snapshot_id;
	u_int64_t	sb_ext3_snapshot_reserved_blocks_count;
	// 0x190
	u_int32_t	sb_ext3_snapshot_list;
	u_int32_t	sb_error_count;
	u_int32_t	sb_first_error_time_lo;
	u_int32_t	sb_first_error_inode;
	// 0x1A0
	u_int64_t	sb_first_error_block;
	char		sb_first_error_function[EXT4FS_FUNCTION_MAX];
	u_int32_t	sb_first_error_line;
	u_int32_t	sb_last_error_time_lo;
	// 0x1D0
	u_int32_t	sb_last_error_inode;
	u_int32_t	sb_last_error_line;
	u_int64_t	sb_last_error_block;
	// 0x1E0
	char		sb_last_error_function[EXT4FS_FUNCTION_MAX];
	// 0x200
	char		sb_mount_opts[EXT4FS_MOUNT_OPTS_MAX];
	// 0x240
	u_int32_t	sb_user_quota_inode;
	u_int32_t	sb_group_quota_inode;
	u_int32_t	sb_overhead_clusters;
	u_int32_t	sb_backup_block_groups[2];
	u_int8_t	sb_encrypt_algos[4];
	u_int8_t	sb_encrypt_pw_salt[16];
	u_int32_t	sb_lost_and_found_inode;
	u_int32_t	sb_project_quota_inode;
	// 0x270
	u_int32_t	sb_checksum_seed;
	u_int8_t	sb_write_time_hi;
	u_int8_t	sb_mount_time_hi;
	u_int8_t	sb_newfs_time_hi;
	u_int8_t	sb_check_time_hi;
	u_int8_t	sb_first_error_time_hi;
	u_int8_t	sb_last_error_time_hi;
	u_int8_t	sb_first_error_code;
	u_int8_t	sb_last_error_code;
	u_int16_t	sb_encoding;
	u_int16_t	sb_encoding_flags;
	// 0x280
	u_int32_t	sb_orphan_file_inode;
	u_int32_t	sb_reserved_288[94];
	u_int32_t	sb_checksum;
} __attribute__((packed));

struct m_ext4fs {
	/* little-endian super-block */
	struct ext4fs	m_sble;
	/* computed from little-endian super-block */
	u_int32_t	m_inodes_count;
	u_int64_t	m_blocks_count;
	u_int64_t	m_reserved_blocks_count;
	u_int64_t	m_free_blocks_count;
	u_int32_t	m_free_inodes_count;
	u_int32_t	m_first_data_block;
	u_int32_t	m_log_block_size;       // log2(block size) - 10
	u_int32_t	m_log_cluster_size;     // log2(cluster size) - 10
	u_int32_t	m_blocks_per_group;
	u_int32_t	m_clusters_per_group;
	u_int32_t	m_inodes_per_group;
	u_int64_t	m_mount_time;
	u_int32_t	m_write_time;
	u_int16_t	m_mount_count;
	int16_t		m_max_mount_count_before_fsck;
	u_int16_t	m_state;                // EXT4FS_STATE_*
	u_int16_t	m_errors;               // EXT4FS_ERRORS_*
	u_int16_t	m_revision_level_minor;
	u_int64_t	m_check_time;
	u_int32_t	m_check_interval;
	u_int32_t	m_creator_os;           // EXT4FS_OS_*
	u_int32_t	m_revision_level;
	u_int16_t	m_default_reserved_uid;
	u_int16_t	m_default_reserved_gid;
	u_int32_t	m_first_non_reserved_inode;
	u_int16_t	m_inode_size;
	u_int16_t	m_block_group_id;
	u_int32_t	m_feature_compat;
	u_int32_t	m_feature_incompat;
	u_int32_t	m_feature_ro_compat;
	u_int32_t	m_algorithm_usage_bitmap;
	u_int16_t	m_reserved_bgdt_blocks;
	u_int32_t	m_journal_inode_number;
	u_int32_t	m_journal_device_number;
	u_int32_t	m_last_orphan;
	u_int16_t	m_block_group_descriptor_size;
	u_int32_t	m_default_mount_opts;
	u_int32_t	m_first_meta_block_group;
	u_int64_t	m_newfs_time;
	u_int16_t	m_inode_size_extra_min;
	u_int16_t	m_inode_size_extra_want;
	u_int32_t	m_flags;
	u_int16_t	m_raid_stride_block_count;
	u_int16_t	m_mmp_interval;
	u_int64_t	m_mmp_block;
	u_int32_t	m_raid_stripe_width_block_count;
	u_int64_t	m_kilobytes_written;
	u_int32_t	m_error_count;
	u_int64_t	m_first_error_time;
	u_int32_t	m_first_error_inode;
	u_int64_t	m_first_error_block;
	u_int32_t	m_first_error_line;
	u_int64_t	m_last_error_time;
	u_int32_t	m_last_error_inode;
	u_int32_t	m_last_error_line;
	u_int64_t	m_last_error_block;
	u_int32_t	m_user_quota_inode;
	u_int32_t	m_group_quota_inode;
	u_int32_t	m_overhead_clusters;
	u_int32_t	m_backup_block_groups[2];
	u_int32_t	m_lost_and_found_inode;
	u_int32_t	m_project_quota_inode;
	u_int32_t	m_checksum_seed;
	u_int16_t	m_encoding;
	u_int16_t	m_encoding_flags;
	u_int32_t	m_orphan_file_inode;
	int		m_read_only;
	int		m_fs_was_modified;
	/* computed by ext4fs_sbfill */
	u_int64_t       m_block_group_descriptor_blocks_count;
	u_int64_t	m_block_group_count;
	u_int64_t	m_block_size;
	u_int64_t	m_block_size_shift;
	u_int32_t	m_fs_block_to_disk_block;
	u_int32_t	m_inodes_per_block;
	u_int32_t	m_inode_table_blocks_per_group;
	u_int32_t	m_resize_dind_block;
	struct ext4fs_block_group_descriptor *m_gd;
};

struct ext4fs_block_group_descriptor {
  u_int32_t bgd_block_bitmap_block_lo;
  u_int32_t bgd_inode_bitmap_block_lo;
  u_int32_t bgd_inode_table_block_lo;
  u_int16_t bgd_free_blocks_count_lo;
  u_int16_t bgd_free_inodes_count_lo;
  // 0x10
  u_int16_t bgd_used_dirs_count_lo;
  u_int16_t bgd_flags;
  u_int32_t bgd_exclude_bitmap_block_lo;
  u_int16_t bgd_block_bitmap_checksum_lo;
  u_int16_t bgd_inode_bitmap_checksum_lo;
  u_int16_t bgd_inode_table_unused_lo;
  u_int16_t bgd_checksum;
  // 0x20
  u_int32_t bgd_block_bitmap_block_hi;
  u_int32_t bgd_inode_bitmap_block_hi;
  u_int32_t bgd_inode_table_block_hi;
  u_int16_t bgd_free_blocks_count_hi;
  u_int16_t bgd_free_inodes_count_hi;
  // 0x30
  u_int16_t bgd_used_dirs_count_hi;
  u_int16_t bgd_inode_table_unused_hi;
  u_int32_t bgd_exclude_bitmap_block_hi;
  u_int16_t bgd_block_bitmap_checksum_hi;
  u_int16_t bgd_inode_bitmap_checksum_hi;
  u_int32_t bgd_reserved_3c;
  // 0x40
} __attribute__((packed));


/* Directory entry file types */
#define EXT4FS_FT_UNKNOWN	0
#define EXT4FS_FT_REG_FILE	1
#define EXT4FS_FT_DIR		2
#define EXT4FS_FT_CHRDEV	3
#define EXT4FS_FT_BLKDEV	4
#define EXT4FS_FT_FIFO		5
#define EXT4FS_FT_SOCK		6
#define EXT4FS_FT_SYMLINK	7
#define EXT4FS_FT_MAX		8

struct ext4fs_directory {
	u_int32_t e4d_ino;
	u_int16_t e4d_reclen;
	u_int8_t  e4d_namlen;
	u_int8_t  e4d_type;
	char      e4d_name[EXT4FS_MAXNAMLEN];
} __attribute__((packed));

/* Directory block checksum tail (last 12 bytes of block when metadata_csum) */
#define EXT4FS_DIR_TAIL_FT	0xDE
#define EXT4FS_DIR_TAIL_SIZE	12

struct ext4fs_directory_tail {
	u_int32_t det_reserved_zero1;	/* must be 0 (fake inode = 0) */
	u_int16_t det_rec_len;		/* always EXT4FS_DIR_TAIL_SIZE */
	u_int8_t  det_reserved_zero2;	/* must be 0 (namlen = 0) */
	u_int8_t  det_reserved_ft;	/* EXT4FS_DIR_TAIL_FT */
	u_int32_t det_checksum;
} __attribute__((packed));

struct ext4fs_feature {
	int		f_mask;
	const char *	f_name;
};

static const struct ext4fs_feature ext4fs_feature_incompat[] = {
  {EXT4FS_FEATURE_INCOMPAT_COMPRESSION, "compression"},
  {EXT4FS_FEATURE_INCOMPAT_FILETYPE,    "filetype"},
  {EXT4FS_FEATURE_INCOMPAT_RECOVER,     "recover"},
  {EXT4FS_FEATURE_INCOMPAT_JOURNAL_DEV, "journal_dev"},
  {EXT4FS_FEATURE_INCOMPAT_META_BG,     "meta_bg"},
  {EXT4FS_FEATURE_INCOMPAT_EXTENTS,     "extents"},
  {EXT4FS_FEATURE_INCOMPAT_64BIT,       "64bit"},
  {EXT4FS_FEATURE_INCOMPAT_MMP,         "mmp"},
  {EXT4FS_FEATURE_INCOMPAT_FLEX_BG,     "flex_bg"},
  {EXT4FS_FEATURE_INCOMPAT_EA_INODE,    "ea_inode"},
  {EXT4FS_FEATURE_INCOMPAT_DIRDATA,     "dirdata"},
  {EXT4FS_FEATURE_INCOMPAT_CSUM_SEED,   "csum_seed"},
  {EXT4FS_FEATURE_INCOMPAT_LARGEDIR,    "largedir"},
  {EXT4FS_FEATURE_INCOMPAT_INLINE_DATA, "inline_data"},
  {EXT4FS_FEATURE_INCOMPAT_ENCRYPT,     "encrypt"},
};

static const struct ext4fs_feature ext4fs_feature_ro_compat[] = {
  {EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER,  "sparse-super"},
  {EXT4FS_FEATURE_RO_COMPAT_LARGE_FILE,    "large-file"},
  {EXT4FS_FEATURE_RO_COMPAT_BTREE_DIR,     "btree-dir"},
  {EXT4FS_FEATURE_RO_COMPAT_HUGE_FILE,     "huge-file"},
  {EXT4FS_FEATURE_RO_COMPAT_GDT_CSUM,      "gdt-csum"},
  {EXT4FS_FEATURE_RO_COMPAT_DIR_NLINK,     "dir-nlink"},
  {EXT4FS_FEATURE_RO_COMPAT_EXTRA_ISIZE,   "extra-isize"},
  {EXT4FS_FEATURE_RO_COMPAT_HAS_SNAPSHOT,  "has-snapshot"},
  {EXT4FS_FEATURE_RO_COMPAT_QUOTA,         "quota"},
  {EXT4FS_FEATURE_RO_COMPAT_BIGALLOC,      "bigalloc"},
  {EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM, "metadata-csum"},
  {EXT4FS_FEATURE_RO_COMPAT_REPLICA,       "replica"},
  {EXT4FS_FEATURE_RO_COMPAT_READONLY,      "readonly"},
  {EXT4FS_FEATURE_RO_COMPAT_PROJECT,        "project"},
  {EXT4FS_FEATURE_RO_COMPAT_ORPHAN_PRESENT, "orphan_present"},
};

#define EXT4FS_ITIMES(ip) do {						\
	if ((ip)->i_flag & (IN_ACCESS | IN_CHANGE | IN_UPDATE)) {	\
		struct timespec _ts;					\
		(ip)->i_flag |= IN_MODIFIED;				\
		getnanotime(&_ts);					\
		if ((ip)->i_flag & IN_ACCESS) {				\
			(ip)->i_e4din->dinode.i_atime =			\
			    htole32((u_int32_t)_ts.tv_sec);		\
			(ip)->i_e4din->dinode.i_atime_extra =		\
			    htole32(_ts.tv_nsec << 2);			\
		}							\
		if ((ip)->i_flag & IN_UPDATE) {				\
			(ip)->i_e4din->dinode.i_mtime =			\
			    htole32((u_int32_t)_ts.tv_sec);		\
			(ip)->i_e4din->dinode.i_mtime_extra =		\
			    htole32(_ts.tv_nsec << 2);			\
		}							\
		if ((ip)->i_flag & IN_CHANGE) {				\
			(ip)->i_e4din->dinode.i_ctime =			\
			    htole32((u_int32_t)_ts.tv_sec);		\
			(ip)->i_e4din->dinode.i_ctime_extra =		\
			    htole32(_ts.tv_nsec << 2);			\
			(ip)->i_modrev++;				\
		}							\
		(ip)->i_flag &= ~(IN_ACCESS | IN_CHANGE | IN_UPDATE);	\
	}								\
} while (0)

struct ext4fs_sync_args {
	int	allerror;
	int	waitfor;
	struct proc *p;
	struct ucred *cred;
};

extern struct pool ext4fs_inode_pool;
extern struct pool ext4fs_dinode_pool;

/* VFS operations */
int ext4fs_fhtovp(struct mount *, struct fid *, struct vnode **);
int ext4fs_init(struct vfsconf *);
int ext4fs_mount(struct mount *, const char *, void *,
	struct nameidata *, struct proc *);
int ext4fs_statfs(struct mount *, struct statfs *, struct proc *);
int ext4fs_sync(struct mount *, int, int, struct ucred *,
	struct proc *);
int ext4fs_sysctl(int *, u_int, void *, size_t *, void *, size_t,
	struct proc *);
int ext4fs_unmount(struct mount *, int, struct proc *);
int ext4fs_vget(struct mount *, ino_t, struct vnode **);
int ext4fs_vptofh(struct vnode *, struct fid *);

/* VNode operations */

int ext4fs_lookup(void *);
int ext4fs_create(void *);
int ext4fs_mknod(void *);
int ext4fs_open(void *);
int ext4fs_access(void *);
int ext4fs_getattr(void *);
int ext4fs_setattr(void *);
int ext4fs_read(void *);
int ext4fs_write(void *);
int ext4fs_fsync(void *);
int ext4fs_remove(void *);
int ext4fs_link(void *);
int ext4fs_rename(void *);
int ext4fs_mkdir(void *);
int ext4fs_rmdir(void *);
int ext4fs_symlink(void *);
int ext4fs_readdir(void *);
int ext4fs_readlink(void *);
int ext4fs_inactive(void *);
int ext4fs_reclaim(void *);
int ext4fs_bmap(void *);
int ext4fs_strategy(void *);
int ext4fs_print(void *);
int ext4fs_pathconf(void *);
int ext4fs_advlock(void *);

int ext4fs_update(struct inode *, int);

u_int32_t ext4fs_sb_csum(struct ext4fs *);
int ext4fs_sb_csum_verify(struct ext4fs *);
u_int32_t ext4fs_csum_seed(struct m_ext4fs *);
u_int32_t ext4fs_bitmap_csum(struct m_ext4fs *, u_int32_t, void *, size_t);
u_int16_t ext4fs_bgd_csum(struct m_ext4fs *,
	struct ext4fs_block_group_descriptor *, u_int32_t);
int ext4fs_bgd_csum_verify(struct m_ext4fs *,
	struct ext4fs_block_group_descriptor *, u_int32_t);
u_int32_t ext4fs_inode_csum(struct m_ext4fs *,
	struct ext4fs_dinode_256 *, u_int32_t);
int ext4fs_inode_csum_verify(struct m_ext4fs *,
	struct ext4fs_dinode_256 *, u_int32_t);

/* Directory entry size: 8 bytes header + name, rounded up to 4 */
#define EXT4FS_DIRSIZ(namlen)	(((8 + (namlen)) + 3) & ~3)

/* Convert inode mode to directory file type */
static inline u_int8_t
ext4fs_mode_to_ft(u_int16_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:	return EXT4FS_FT_REG_FILE;
	case S_IFDIR:	return EXT4FS_FT_DIR;
	case S_IFCHR:	return EXT4FS_FT_CHRDEV;
	case S_IFBLK:	return EXT4FS_FT_BLKDEV;
	case S_IFIFO:	return EXT4FS_FT_FIFO;
	case S_IFSOCK:	return EXT4FS_FT_SOCK;
	case S_IFLNK:	return EXT4FS_FT_SYMLINK;
	default:	return EXT4FS_FT_UNKNOWN;
	}
}

/* Block allocation / free */
int ext4fs_blkalloc(struct inode *, u_int64_t, u_int32_t, u_int64_t *,
    u_int32_t *);
void ext4fs_blkfree(struct inode *, u_int64_t);

/* Inode allocation / free */
int ext4fs_inode_alloc(struct inode *, mode_t, struct ucred *,
	struct vnode **);
void ext4fs_inode_free(struct inode *, ufsino_t, mode_t);

/* Directory operations */
int ext4fs_direnter(struct inode *, struct vnode *,
	struct componentname *);
int ext4fs_dirremove(struct vnode *, struct componentname *);
int ext4fs_dirempty(struct inode *, ufsino_t, struct ucred *);
int ext4fs_dirrewrite(struct inode *, struct inode *,
	struct componentname *);

/* Truncation */
int ext4fs_truncate(struct inode *, off_t, int, struct ucred *);

/* Size update */
void ext4fs_setsize(struct inode *, u_int64_t);

/* Superblock / BGD write-back */
int ext4fs_bgd_write(struct m_ext4fs *, struct vnode *, u_int32_t);
int ext4fs_sbwrite(struct mount *);
