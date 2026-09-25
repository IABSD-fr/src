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
#ifndef _EXT4FS_CRC32C_H_
#define _EXT4FS_CRC32C_H_

#include <sys/types.h>

struct ext4fs;
struct ext4fs_block_group_descriptor;
struct ext4fs_dinode_256;
struct m_ext4fs;

u_int32_t ext4fs_sb_csum (struct ext4fs *);
int ext4fs_sb_csum_verify (struct ext4fs *);
u_int32_t ext4fs_csum_seed (struct m_ext4fs *);

/* Compute block or inode bitmap checksum (group number + bitmap data) */
u_int32_t ext4fs_bitmap_csum (struct m_ext4fs *, u_int32_t, const void *,
    size_t);
u_int16_t ext4fs_bgd_csum (struct m_ext4fs *,
    struct ext4fs_block_group_descriptor *, u_int32_t);
int ext4fs_bgd_csum_verify (struct m_ext4fs *,
    struct ext4fs_block_group_descriptor *, u_int32_t);
u_int32_t ext4fs_inode_csum (struct m_ext4fs *,
    struct ext4fs_dinode_256 *, u_int32_t);
int ext4fs_inode_has_csum_hi (const struct ext4fs_dinode_256 *);
int ext4fs_inode_csum_verify (struct m_ext4fs *,
    struct ext4fs_dinode_256 *, u_int32_t);

/*
 * Write a directory block checksum tail at the end of buf.
 * ino: directory inode number, gen_le: i_nfs_generation (already LE).
 * No-op if METADATA_CSUM is not enabled.
 */
void ext4fs_dir_set_csum (struct m_ext4fs *fs, u_int32_t ino,
    u_int32_t gen_le, void *buf);

/*
 * Write the extent tree block checksum tail.
 * ino: inode number, gen_le: i_nfs_generation (already LE on disk).
 * No-op if METADATA_CSUM is not enabled.
 */
void ext4fs_extent_block_csum_set (struct m_ext4fs *fs, u_int32_t ino,
    u_int32_t gen_le, void *buf);
int ext4fs_extent_block_csum_verify (struct m_ext4fs *, u_int32_t,
    u_int32_t, const void *);

#endif /* _EXT4FS_CRC32C_H_ */
