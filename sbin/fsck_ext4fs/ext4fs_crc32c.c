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
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <ufs/ext4fs/ext4fs_dinode.h>
#include <ufs/ext4fs/ext4fs.h>

/*
 * CRC32C lookup table, generated using the Castagnoli polynomial
 * 0x1EDC6F41 (bit-reversed: 0x82F63B78).
 *
 * This table is for little-endian CRC computation.
 */
static const u_int32_t crc32c_table[256] = {
	0x00000000, 0xF26B8303, 0xE13B70F7, 0x1350F3F4,
	0xC79A971F, 0x35F1141C, 0x26A1E7E8, 0xD4CA64EB,
	0x8AD958CF, 0x78B2DBCC, 0x6BE22838, 0x9989AB3B,
	0x4D43CFD0, 0xBF284CD3, 0xAC78BF27, 0x5E133C24,
	0x105EC76F, 0xE235446C, 0xF165B798, 0x030E349B,
	0xD7C45070, 0x25AFD373, 0x36FF2087, 0xC494A384,
	0x9A879FA0, 0x68EC1CA3, 0x7BBCEF57, 0x89D76C54,
	0x5D1D08BF, 0xAF768BBC, 0xBC267848, 0x4E4DFB4B,
	0x20BD8EDE, 0xD2D60DDD, 0xC186FE29, 0x33ED7D2A,
	0xE72719C1, 0x154C9AC2, 0x061C6936, 0xF477EA35,
	0xAA64D611, 0x580F5512, 0x4B5FA6E6, 0xB93425E5,
	0x6DFE410E, 0x9F95C20D, 0x8CC531F9, 0x7EAEB2FA,
	0x30E349B1, 0xC288CAB2, 0xD1D83946, 0x23B3BA45,
	0xF779DEAE, 0x05125DAD, 0x1642AE59, 0xE4292D5A,
	0xBA3A117E, 0x4851927D, 0x5B016189, 0xA96AE28A,
	0x7DA08661, 0x8FCB0562, 0x9C9BF696, 0x6EF07595,
	0x417B1DBC, 0xB3109EBF, 0xA0406D4B, 0x522BEE48,
	0x86E18AA3, 0x748A09A0, 0x67DAFA54, 0x95B17957,
	0xCBA24573, 0x39C9C670, 0x2A993584, 0xD8F2B687,
	0x0C38D26C, 0xFE53516F, 0xED03A29B, 0x1F682198,
	0x5125DAD3, 0xA34E59D0, 0xB01EAA24, 0x42752927,
	0x96BF4DCC, 0x64D4CECF, 0x77843D3B, 0x85EFBE38,
	0xDBFC821C, 0x2997011F, 0x3AC7F2EB, 0xC8AC71E8,
	0x1C661503, 0xEE0D9600, 0xFD5D65F4, 0x0F36E6F7,
	0x61C69362, 0x93AD1061, 0x80FDE395, 0x72966096,
	0xA65C047D, 0x5437877E, 0x4767748A, 0xB50CF789,
	0xEB1FCBAD, 0x197448AE, 0x0A24BB5A, 0xF84F3859,
	0x2C855CB2, 0xDEEEDFB1, 0xCDBE2C45, 0x3FD5AF46,
	0x7198540D, 0x83F3D70E, 0x90A324FA, 0x62C8A7F9,
	0xB602C312, 0x44694011, 0x5739B3E5, 0xA55230E6,
	0xFB410CC2, 0x092A8FC1, 0x1A7A7C35, 0xE811FF36,
	0x3CDB9BDD, 0xCEB018DE, 0xDDE0EB2A, 0x2F8B6829,
	0x82F63B78, 0x709DB87B, 0x63CD4B8F, 0x91A6C88C,
	0x456CAC67, 0xB7072F64, 0xA457DC90, 0x563C5F93,
	0x082F63B7, 0xFA44E0B4, 0xE9141340, 0x1B7F9043,
	0xCFB5F4A8, 0x3DDE77AB, 0x2E8E845F, 0xDCE5075C,
	0x92A8FC17, 0x60C37F14, 0x73938CE0, 0x81F80FE3,
	0x55326B08, 0xA759E80B, 0xB4091BFF, 0x466298FC,
	0x1871A4D8, 0xEA1A27DB, 0xF94AD42F, 0x0B21572C,
	0xDFEB33C7, 0x2D80B0C4, 0x3ED04330, 0xCCBBC033,
	0xA24BB5A6, 0x502036A5, 0x4370C551, 0xB11B4652,
	0x65D122B9, 0x97BAA1BA, 0x84EA524E, 0x7681D14D,
	0x2892ED69, 0xDAF96E6A, 0xC9A99D9E, 0x3BC21E9D,
	0xEF087A76, 0x1D63F975, 0x0E330A81, 0xFC588982,
	0xB21572C9, 0x407EF1CA, 0x532E023E, 0xA145813D,
	0x758FE5D6, 0x87E466D5, 0x94B49521, 0x66DF1622,
	0x38CC2A06, 0xCAA7A905, 0xD9F75AF1, 0x2B9CD9F2,
	0xFF56BD19, 0x0D3D3E1A, 0x1E6DCDEE, 0xEC064EED,
	0xC38D26C4, 0x31E6A5C7, 0x22B65633, 0xD0DDD530,
	0x0417B1DB, 0xF67C32D8, 0xE52CC12C, 0x1747422F,
	0x49547E0B, 0xBB3FFD08, 0xA86F0EFC, 0x5A048DFF,
	0x8ECEE914, 0x7CA56A17, 0x6FF599E3, 0x9D9E1AE0,
	0xD3D3E1AB, 0x21B862A8, 0x32E8915C, 0xC083125F,
	0x144976B4, 0xE622F5B7, 0xF5720643, 0x07198540,
	0x590AB964, 0xAB613A67, 0xB831C993, 0x4A5A4A90,
	0x9E902E7B, 0x6CFBAD78, 0x7FAB5E8C, 0x8DC0DD8F,
	0xE330A81A, 0x115B2B19, 0x020BD8ED, 0xF0605BEE,
	0x24AA3F05, 0xD6C1BC06, 0xC5914FF2, 0x37FACCF1,
	0x69E9F0D5, 0x9B8273D6, 0x88D28022, 0x7AB90321,
	0xAE7367CA, 0x5C18E4C9, 0x4F48173D, 0xBD23943E,
	0xF36E6F75, 0x0105EC76, 0x12551F82, 0xE03E9C81,
	0x34F4F86A, 0xC69F7B69, 0xD5CF889D, 0x27A40B9E,
	0x79B737BA, 0x8BDCB4B9, 0x988C474D, 0x6AE7C44E,
	0xBE2DA0A5, 0x4C4623A6, 0x5F16D052, 0xAD7D5351
};

/*
 * Compute CRC32C of a buffer.
 *
 * crc: initial CRC value (use ~0 to start fresh, or previous CRC to continue)
 * buf: pointer to data buffer
 * len: length of data in bytes
 *
 * Returns the updated CRC32C value.
 */
u_int32_t
ext4fs_crc32c(u_int32_t crc, const void *buf, size_t len)
{
	const u_int8_t *p = buf;

	crc = ~crc;
	while (len--)
		crc = (crc >> 8) ^ crc32c_table[(crc & 0xff) ^ *p++];

	return ~crc;
}

/*
 * Compute CRC32C in the style used by ext4.
 *
 * ext4 computes CRC32C starting with ~0 (or a seed), then inverts the
 * final result before storing it.
 *
 * crc: seed value (use ~0 for standard ext4 checksum, or sb_checksum_seed)
 * buf: pointer to data buffer
 * len: length of data in bytes
 *
 * Returns the final CRC32C value (NOT inverted - caller should invert
 * if comparing against stored checksum, or pass result to next call).
 */
u_int32_t
ext4fs_crc32c_le(u_int32_t crc, const void *buf, size_t len)
{
	return ext4fs_crc32c(crc, buf, len);
}

/*
 * Compute the checksum seed for an ext4 filesystem.
 *
 * If the CSUM_SEED feature is set, use the pre-computed seed from the
 * superblock. Otherwise, compute it from the filesystem UUID.
 */
u_int32_t
ext4fs_csum_seed(struct m_ext4fs *fs)
{
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_CSUM_SEED)
		return ~fs->m_checksum_seed;

	/* Compute seed from UUID */
	return ext4fs_crc32c(0, fs->m_sble.sb_uuid,
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
ext4fs_sb_csum(struct ext4fs *sb)
{
	u_int32_t crc;
	size_t offset;

	/* Offset of sb_checksum field within the superblock */
	offset = offsetof(struct ext4fs, sb_checksum);

	/* Compute CRC up to (but not including) the checksum field */
	crc = ext4fs_crc32c(0, sb, offset);

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
ext4fs_bgd_csum(struct m_ext4fs *fs,
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
	seed = ext4fs_crc32c(seed, &block_group_id_le,
	    sizeof(block_group_id_le));

	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		size = fs->m_block_group_descriptor_size;
	else
		size = 32;
	if (size > sizeof(tmp))
		size = sizeof(tmp);

	memcpy(&tmp, bgd, size);
	tmp.bgd_checksum = 0;
	crc = ext4fs_crc32c(seed, &tmp, size);

	return (~crc) & 0xFFFF;
}

/*
 * Verify a block group descriptor checksum.
 *
 * Returns 0 if the checksum is valid, or EINVAL if it doesn't match.
 */
int
ext4fs_bgd_csum_verify(struct m_ext4fs *fs,
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
u_int32_t
ext4fs_inode_csum(struct m_ext4fs *fs,
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
	crc = ext4fs_crc32c(seed, &ino_le, sizeof(ino_le));
	crc = ext4fs_crc32c(crc, &dp->dinode.i_nfs_generation,
	    sizeof(dp->dinode.i_nfs_generation));

	tmp = *dp;
	tmp.dinode.i_checksum_lo = 0;
	tmp.dinode.i_checksum_hi = 0;
	crc = ext4fs_crc32c(crc, &tmp, sizeof(tmp));

	return ~crc;
}

/*
 * Verify an inode checksum.
 *
 * Returns 0 if the checksum is valid, or EINVAL if it doesn't match.
 */
int
ext4fs_inode_csum_verify(struct m_ext4fs *fs,
    struct ext4fs_dinode_256 *dp, u_int32_t ino)
{
	u_int32_t provided, calculated;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;

	provided = letoh16(dp->dinode.i_checksum_lo);
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		provided |= (u_int32_t)letoh16(dp->dinode.i_checksum_hi) << 16;
	calculated = ext4fs_inode_csum(fs, dp, ino);

	if (provided != calculated) {
		printf("ext4fs: inode %u checksum mismatch: "
		    "stored=0x%08x calculated=0x%08x\n",
		    ino, provided, calculated);
		return EINVAL;
	}

	return 0;
}

u_int32_t
ext4fs_bitmap_csum(struct m_ext4fs *fs, u_int32_t group,
    void *bitmap, size_t size)
{
	u_int32_t crc, seed;

	if (!(fs->m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;

	seed = ext4fs_csum_seed(fs);
	crc = ext4fs_crc32c(seed, bitmap, size);

	return ~crc;
}

/*
 * Write the checksum tail at the end of a directory block.
 *
 * The tail is a 12-byte structure placed at block_size - 12.
 * Checksum covers: UUID seed, inode number, inode generation, block data.
 */
void
ext4fs_dir_set_csum(struct m_ext4fs *fs, u_int32_t ino, u_int32_t gen_le,
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
	crc = ext4fs_crc32c(seed, &ino_le, sizeof(ino_le));
	crc = ext4fs_crc32c(crc, &gen_le, sizeof(gen_le));
	crc = ext4fs_crc32c(crc, buf, fs->m_block_size - EXT4FS_DIR_TAIL_SIZE);
	tail->det_checksum = htole32(~crc);
}

/*
 * Verify the superblock checksum.
 *
 * Returns 0 if the checksum is valid, or EINVAL if it doesn't match.
 * If metadata checksums are not enabled, always returns 0.
 */
int
ext4fs_sb_csum_verify(struct ext4fs *sb)
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
ext4fs_extent_block_csum_set(struct m_ext4fs *fs, u_int32_t ino,
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
	crc = ext4fs_crc32c(seed, &ino_le, sizeof(ino_le));
	crc = ext4fs_crc32c(crc, &gen_le, sizeof(gen_le));
	*tail = 0;
	crc = ext4fs_crc32c(crc, buf, tail_offset);
	*tail = htole32(~crc);
}
