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
#ifdef _KERNEL
#include <sys/systm.h>
#else
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#endif

#include <lib/libkern/crc32c.h>

#include <ufs/ext4fs/ext4fs_dinode.h>
#include <ufs/ext4fs/ext4fs.h>

/*
 * Compute the checksum seed for an ext4 filesystem.
 *
 * If the CSUM_SEED feature is set, use the pre-computed seed from the
 * superblock. Otherwise, compute it from the filesystem UUID.
 */
u_int32_t
ext4fs_csum_seed (struct m_ext4fs *fs)
{
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_CSUM_SEED)
		return ~fs->m_checksum_seed;

	/* Compute seed from UUID */
	return crc32c(0, fs->m_sble.sb_uuid,
	    sizeof(fs->m_sble.sb_uuid));
}

/*
 * Compute the CRC32C checksum of an ext4 superblock.
 *
 * The checksum covers the entire superblock except for the checksum
 * field itself (last 4 bytes). The checksum field is treated as zero
 * during computation.
 */
u_int32_t
ext4fs_sb_csum (struct ext4fs *sb)
{
	u_int32_t crc;
	size_t offset;

	/* Offset of sb_checksum field within the superblock */
	offset = offsetof(struct ext4fs, sb_checksum);

	/* Compute CRC up to (but not including) the checksum field */
	crc = crc32c(0, (const uint8_t *)sb, offset);

	return ~crc;
}

/*
 * Compute the CRC32C checksum of a block group descriptor.
 *
 * When CSUM_SEED is set, the seed comes from sb_checksum_seed.
 * Otherwise, compute it from the UUID.
 * The block_group_id is always chained into the CRC (after the seed).
 */
u_int16_t
ext4fs_bgd_csum (struct m_ext4fs *fs,
    struct ext4fs_block_group_descriptor *bgd, u_int32_t block_group_id)
{
	u_int32_t crc;
	u_int32_t seed;
	u_int32_t block_group_id_le;
	size_t size;
	struct ext4fs_block_group_descriptor tmp;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;

	seed = ext4fs_csum_seed(fs);
	block_group_id_le = htole32(block_group_id);
	seed = crc32c(seed, (const uint8_t *)&block_group_id_le,
	    sizeof(block_group_id_le));

	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		size = fs->m_block_group_descriptor_size;
	else
		size = 32;
	if (size > sizeof(tmp))
		size = sizeof(tmp);

	memcpy(&tmp, bgd, size);
	tmp.bgd_checksum = 0;
	crc = crc32c(seed, (const uint8_t *)&tmp, size);

	return (~crc) & 0xFFFF;
}

/*
 * Verify a block group descriptor checksum.
 *
 * Returns 0 if the checksum is valid, or EINVAL if it doesn't match.
 */
int
ext4fs_bgd_csum_verify (struct m_ext4fs *fs,
    struct ext4fs_block_group_descriptor *bgd, u_int32_t block_group_id)
{
	u_int16_t provided, calculated;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;

	provided = letoh16(bgd->bgd_checksum);
	calculated = ext4fs_bgd_csum(fs, bgd, block_group_id);

	if (provided != calculated) {
		printf("ext4fs: bgd %u checksum mismatch: "
		    "stored=0x%04x calculated=0x%04x\n",
		    block_group_id, provided, calculated);
		return EINVAL;
	}

	return 0;
}

/*
 * Compute the CRC32C checksum of an inode.
 *
 * The checksum covers the inode number, generation, and the full
 * 256-byte inode with checksum fields zeroed.
 */
int
ext4fs_inode_has_csum_hi (const struct ext4fs_dinode_256 *dp)
{
	size_t end;

	/* i_extra_isize counts bytes beginning at the 128-byte boundary. */
	end = offsetof(struct ext4fs_dinode, i_checksum_hi) +
	    sizeof(dp->dinode.i_checksum_hi);
	return (letoh16(dp->dinode.i_extra_isize) >= end - 128);
}

u_int32_t
ext4fs_inode_csum (struct m_ext4fs *fs,
    struct ext4fs_dinode_256 *dp, u_int32_t ino)
{
	u_int32_t crc;
	u_int32_t seed;
	u_int32_t ino_le;
	struct ext4fs_dinode_256 tmp;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;

	seed = ext4fs_csum_seed(fs);

	ino_le = htole32(ino);
	crc = crc32c(seed, (const uint8_t *)&ino_le, sizeof(ino_le));
	crc = crc32c(crc,
	    (const uint8_t *)&dp->dinode.i_nfs_generation,
	    sizeof(dp->dinode.i_nfs_generation));

	tmp = *dp;
	tmp.dinode.i_checksum_lo = 0;
	if (ext4fs_inode_has_csum_hi(dp))
		tmp.dinode.i_checksum_hi = 0;
	crc = crc32c(crc, (const uint8_t *)&tmp, sizeof(tmp));

	return ~crc;
}

/*
 * Verify an inode checksum.
 *
 * Returns 0 if the checksum is valid, or EINVAL if it doesn't match.
 */
int
ext4fs_inode_csum_verify (struct m_ext4fs *fs,
    struct ext4fs_dinode_256 *dp, u_int32_t ino)
{
	u_int32_t provided, calculated;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;

	provided = letoh16(dp->dinode.i_checksum_lo);
	calculated = ext4fs_inode_csum(fs, dp, ino);
	if (ext4fs_inode_has_csum_hi(dp))
		provided |= (u_int32_t)letoh16(dp->dinode.i_checksum_hi) << 16;
	else
		calculated &= 0xffff;

	if (provided != calculated) {
		printf("ext4fs: inode %u checksum mismatch: "
		    "stored=0x%08x calculated=0x%08x\n",
		    ino, provided, calculated);
		return EINVAL;
	}

	return 0;
}

u_int32_t
ext4fs_bitmap_csum (struct m_ext4fs *fs, u_int32_t group,
    const void *bitmap, size_t size)
{
	u_int32_t crc, seed;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;

	seed = ext4fs_csum_seed(fs);
	crc = crc32c(seed, bitmap, size);

	return ~crc;
}

/*
 * Write the checksum tail at the end of a directory block.
 *
 * The tail is a 12-byte structure placed at block_size - 12.
 * Checksum covers: UUID seed, inode number, inode generation, block data.
 */
void
ext4fs_dir_set_csum (struct m_ext4fs *fs, u_int32_t ino, u_int32_t gen_le,
    void *buf)
{
	struct ext4fs_directory_tail *tail;
	u_int32_t crc, seed, ino_le;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return;

	tail = (struct ext4fs_directory_tail *)
	    ((char *)buf + fs->m_block_size - EXT4FS_DIR_TAIL_SIZE);
	tail->det_reserved_zero1 = 0;
	tail->det_rec_len = htole16(EXT4FS_DIR_TAIL_SIZE);
	tail->det_reserved_zero2 = 0;
	tail->det_reserved_ft = EXT4FS_DIR_TAIL_FT;
	tail->det_checksum = 0;

	seed = ext4fs_csum_seed(fs);
	ino_le = htole32(ino);
	crc = crc32c(seed, (const uint8_t *)&ino_le, sizeof(ino_le));
	crc = crc32c(crc, (const uint8_t *)&gen_le, sizeof(gen_le));
	crc = crc32c(crc, buf, fs->m_block_size - EXT4FS_DIR_TAIL_SIZE);
	tail->det_checksum = htole32(~crc);
}

/*
 * Verify the superblock checksum.
 *
 * Returns 0 if the checksum is valid, or EINVAL if it doesn't match.
 * If metadata checksums are not enabled, always returns 0.
 */
int
ext4fs_sb_csum_verify (struct ext4fs *sb)
{
	u_int32_t provided, calculated;

	/* Check if metadata checksums are enabled */
	if (!(letoh32(sb->sb_feature_ro_compat) &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;

	provided = letoh32(sb->sb_checksum);
	calculated = ext4fs_sb_csum(sb);

	if (provided != calculated) {
		printf("ext4fs: superblock checksum mismatch: "
		    "stored=0x%08x calculated=0x%08x\n",
		    provided, calculated);
		return EINVAL;
	}

	return 0;
}

/*
 * Write the checksum tail of an extent tree block.
 *
 * The tail is a 4-byte le32 checksum placed right after eh_max entries.
 * Checksum covers: UUID seed, inode number, inode generation,
 * then the block data up to and including the zeroed tail.
 */
void
ext4fs_extent_block_csum_set (struct m_ext4fs *fs, u_int32_t ino,
    u_int32_t gen_le, void *buf)
{
	u_int32_t crc, seed, ino_le;
	u_int32_t *tail;
	struct ext4fs_extent_header *eh;
	size_t tail_offset;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return;

	eh = (struct ext4fs_extent_header *)buf;
	/* Tail is right after eh_max entries */
	tail_offset = sizeof(struct ext4fs_extent_header) +
	    (size_t)letoh16(eh->eh_max) * sizeof(struct ext4fs_extent);
	tail = (u_int32_t *)((char *)buf + tail_offset);

	seed = ext4fs_csum_seed(fs);
	ino_le = htole32(ino);
	crc = crc32c(seed, (const uint8_t *)&ino_le, sizeof(ino_le));
	crc = crc32c(crc, (const uint8_t *)&gen_le, sizeof(gen_le));
	*tail = 0;
	crc = crc32c(crc, buf, tail_offset);
	*tail = htole32(~crc);
}

/*
 * Verify an external extent-tree block checksum.
 *
 * Returns 0 when checksums are disabled or the checksum is valid, and
 * EINVAL when the header cannot contain a checksum tail or it does not match.
 */
int
ext4fs_extent_block_csum_verify (struct m_ext4fs *fs, u_int32_t ino,
    u_int32_t gen_le, const void *buf)
{
	const struct ext4fs_extent_header *eh;
	const u_int32_t *tail;
	u_int32_t crc, ino_le, provided;
	size_t tail_offset;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return (0);

	eh = buf;
	tail_offset = sizeof(*eh) +
	    (size_t)letoh16(eh->eh_max) * sizeof(struct ext4fs_extent);
	if (tail_offset > fs->m_block_size - sizeof(*tail))
		return (EINVAL);
	tail = (const u_int32_t *)((const char *)buf + tail_offset);
	provided = letoh32(*tail);
	ino_le = htole32(ino);
	crc = crc32c(ext4fs_csum_seed(fs), (const uint8_t *)&ino_le,
	    sizeof(ino_le));
	crc = crc32c(crc, (const uint8_t *)&gen_le, sizeof(gen_le));
	crc = crc32c(crc, buf, tail_offset);
	if (provided != ~crc)
		return (EINVAL);
	return (0);
}
