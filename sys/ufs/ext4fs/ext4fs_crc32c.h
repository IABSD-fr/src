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

/*
 * CRC32C uses the Castagnoli polynomial: 0x1EDC6F41
 * This is different from the standard CRC32 (ISO 3309) polynomial.
 *
 * ext4 stores checksums as the bitwise inverse of the CRC32C value.
 */

/* Compute CRC32C of a buffer, starting from an initial CRC value */
u_int32_t ext4fs_crc32c(u_int32_t crc, const void *buf, size_t len);

/* Compute CRC32C with initial value of ~0, then invert result (ext4 style) */
u_int32_t ext4fs_crc32c_le(u_int32_t crc, const void *buf, size_t len);

struct m_ext4fs;

/* Compute block or inode bitmap checksum (group number + bitmap data) */
u_int32_t ext4fs_bitmap_csum(struct m_ext4fs *fs, u_int32_t group,
    void *bitmap, size_t size);

/*
 * Write a directory block checksum tail at the end of buf.
 * ino: directory inode number, gen_le: i_nfs_generation (already LE).
 * No-op if METADATA_CSUM is not enabled.
 */
void ext4fs_dir_set_csum(struct m_ext4fs *fs, u_int32_t ino,
    u_int32_t gen_le, void *buf);

/*
 * Write the extent tree block checksum tail.
 * ino: inode number, gen_le: i_nfs_generation (already LE on disk).
 * No-op if METADATA_CSUM is not enabled.
 */
void ext4fs_extent_block_csum_set(struct m_ext4fs *fs, u_int32_t ino,
    u_int32_t gen_le, void *buf);

#endif /* _EXT4FS_CRC32C_H_ */
