/*
 * Copyright (c) 2026 kmx.io.
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

#include <sys/types.h>
#include <sys/endian.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXT4_SUPER_OFFSET		1024
#define EXT4_SUPER_SIZE			1024

#define EXT4_SB_BLOCKS_COUNT_LO		0x004
#define EXT4_SB_FIRST_DATA_BLOCK		0x014
#define EXT4_SB_LOG_BLOCK_SIZE		0x018
#define EXT4_SB_BLOCKS_PER_GROUP		0x020
#define EXT4_SB_INODES_PER_GROUP		0x028
#define EXT4_SB_MAGIC			0x038
#define EXT4_SB_FIRST_INO		0x054
#define EXT4_SB_INODE_SIZE		0x058
#define EXT4_SB_FEATURE_COMPAT		0x05c
#define EXT4_SB_FEATURE_INCOMPAT		0x060
#define EXT4_SB_FEATURE_RO_COMPAT	0x064
#define EXT4_SB_DESC_SIZE		0x0fe
#define EXT4_SB_BLOCKS_COUNT_HI		0x150
#define EXT4_SB_CHECKSUM			0x3fc

#define EXT4_BG_INODE_TABLE_LO		0x008
#define EXT4_BG_CHECKSUM		0x01e
#define EXT4_BG_INODE_TABLE_HI		0x028

#define EXT4_INODE_MODE			0x000
#define EXT4_INODE_FLAGS		0x020
#define EXT4_INODE_BLOCK		0x028

#define EXT4_EXTENT_MAGIC		0x000
#define EXT4_EXTENT_ENTRIES		0x002
#define EXT4_EXTENT_MAX			0x004
#define EXT4_EXTENT_DEPTH		0x006
#define EXT4_EXTENT_HEADER_SIZE		12
#define EXT4_EXTENT_ENTRY_SIZE		12
#define EXT4_EXTENT_BLOCK		0x000
#define EXT4_EXTENT_LENGTH		0x004
#define EXT4_EXTENT_START_HI		0x006
#define EXT4_EXTENT_START_LO		0x008
#define EXT4_EXTENT_INDEX_LEAF_LO	0x004
#define EXT4_EXTENT_INDEX_LEAF_HI	0x008

#define EXT4_SUPER_MAGIC		0xef53
#define EXT4_EXTENT_HEADER_MAGIC	0xf30a
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL	0x00000004U
#define EXT4_FEATURE_INCOMPAT_RECOVER	0x00000004U
#define EXT4_FEATURE_INCOMPAT_64BIT	0x00000080U
#define EXT4_FEATURE_INCOMPAT_ENCRYPT	0x00010000U
#define EXT4_FEATURE_RO_METADATA_CSUM	0x00000400U
#define EXT4_INODE_FLAG_EXTENTS		0x00080000U

#define EXT4_MODE_TYPE			0170000
#define EXT4_MODE_REGULAR		0100000

enum checksum_profile {
	CHECKSUM_REQUIRED,
	CHECKSUM_FORBIDDEN
};

struct mutation {
	const char		*name;
	enum checksum_profile	 checksum;
	int			 needs_inode;
};

static const struct mutation mutations[] = {
	{ "bad-superblock-checksum", CHECKSUM_REQUIRED, 0 },
	{ "bad-group-descriptor-checksum", CHECKSUM_REQUIRED, 0 },
	{ "block-size-too-large", CHECKSUM_FORBIDDEN, 0 },
	{ "zero-blocks-per-group", CHECKSUM_FORBIDDEN, 0 },
	{ "zero-inodes-per-group", CHECKSUM_FORBIDDEN, 0 },
	{ "invalid-inode-size", CHECKSUM_FORBIDDEN, 0 },
	{ "invalid-first-inode", CHECKSUM_FORBIDDEN, 0 },
	{ "invalid-descriptor-size", CHECKSUM_FORBIDDEN, 0 },
	{ "unsupported-incompat-feature", CHECKSUM_FORBIDDEN, 0 },
	{ "recover-without-journal", CHECKSUM_FORBIDDEN, 0 },
	{ "extent-root-bad-magic", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-root-entries-over-max", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-root-max-too-large", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-root-unordered", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-root-overlap", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-root-zero-length", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-root-physical-out-of-range", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-root-depth-too-large", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-index-out-of-range", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-leaf-bad-magic", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-leaf-entries-over-max", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-leaf-max-too-large", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-leaf-unordered", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-leaf-overlap", CHECKSUM_FORBIDDEN, 1 },
	{ "extent-block-bad-checksum", CHECKSUM_REQUIRED, 1 }
};

static void	read_exact (int, void *, size_t, off_t, const char *);
static void	write_exact (int, const void *, size_t, off_t, const char *);
static uint16_t	get16 (const uint8_t *, size_t);
static uint32_t	get32 (const uint8_t *, size_t);
static void	put16 (uint8_t *, size_t, uint16_t);
static void	put32 (uint8_t *, size_t, uint32_t);
static uint32_t	number32 (const char *, const char *);
static const struct mutation *find_mutation (const char *);
static uint32_t	filesystem_block_size (const uint8_t *);
static uint64_t	filesystem_blocks (const uint8_t *);
static off_t	inode_offset (int, const uint8_t *, uint32_t, off_t,
		    uint32_t *);
static void	mutate_extent (int, uint8_t *, const struct mutation *,
		    uint32_t, off_t);
static void	mutate (int, uint8_t *, const struct mutation *, uint32_t,
		    off_t);

static void
read_exact (int fd, void *buf, size_t len, off_t offset, const char *what)
{
	uint8_t *p;
	ssize_t n;
	size_t done;

	p = buf;
	for (done = 0; done < len; done += (size_t)n) {
		n = pread(fd, p + done, len - done, offset + (off_t)done);
		if (n == -1)
			err(1, "pread %s", what);
		if (n == 0)
			errx(1, "short read of %s", what);
	}
}

static void
write_exact (int fd, const void *buf, size_t len, off_t offset,
    const char *what)
{
	const uint8_t *p;
	ssize_t n;
	size_t done;

	p = buf;
	for (done = 0; done < len; done += (size_t)n) {
		n = pwrite(fd, p + done, len - done, offset + (off_t)done);
		if (n == -1)
			err(1, "pwrite %s", what);
		if (n == 0)
			errx(1, "short write of %s", what);
	}
}

static uint16_t
get16 (const uint8_t *buf, size_t offset)
{
	uint16_t value;

	memcpy(&value, buf + offset, sizeof(value));
	return (letoh16(value));
}

static uint32_t
get32 (const uint8_t *buf, size_t offset)
{
	uint32_t value;

	memcpy(&value, buf + offset, sizeof(value));
	return (letoh32(value));
}

static void
put16 (uint8_t *buf, size_t offset, uint16_t value)
{
	value = htole16(value);
	memcpy(buf + offset, &value, sizeof(value));
}

static void
put32 (uint8_t *buf, size_t offset, uint32_t value)
{
	value = htole32(value);
	memcpy(buf + offset, &value, sizeof(value));
}

static uint32_t
number32 (const char *value, const char *name)
{
	char *end;
	unsigned long long result;

	if (*value == '-')
		errx(1, "%s is negative", name);
	errno = 0;
	end = NULL;
	result = strtoull(value, &end, 0);
	if (errno == ERANGE || result > UINT32_MAX)
		errx(1, "%s is too large", name);
	if (*value == '\0' || end == NULL || *end != '\0')
		errx(1, "%s is invalid", name);
	return ((uint32_t)result);
}

static const struct mutation *
find_mutation (const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++)
		if (strcmp(name, mutations[i].name) == 0)
			return (&mutations[i]);
	return (NULL);
}

static uint32_t
filesystem_block_size (const uint8_t *sb)
{
	uint32_t shift;

	shift = get32(sb, EXT4_SB_LOG_BLOCK_SIZE);
	if (shift > 2)
		errx(1, "unsupported source block-size shift");
	return (1024U << shift);
}

static uint64_t
filesystem_blocks (const uint8_t *sb)
{
	uint64_t blocks;

	blocks = get32(sb, EXT4_SB_BLOCKS_COUNT_LO);
	if (get32(sb, EXT4_SB_FEATURE_INCOMPAT) &
	    EXT4_FEATURE_INCOMPAT_64BIT)
		blocks |= (uint64_t)get32(sb, EXT4_SB_BLOCKS_COUNT_HI) << 32;
	return (blocks);
}

static off_t
inode_offset (int fd, const uint8_t *sb, uint32_t ino, off_t image_size,
    uint32_t *inode_sizep)
{
	uint8_t descriptor[64];
	uint64_t descriptor_offset, group, index, offset, table;
	uint32_t block_size, descriptor_size, inode_size, inodes_per_group;

	if (ino == 0)
		errx(1, "inode number is zero");
	block_size = filesystem_block_size(sb);
	inodes_per_group = get32(sb, EXT4_SB_INODES_PER_GROUP);
	inode_size = get16(sb, EXT4_SB_INODE_SIZE);
	descriptor_size = get16(sb, EXT4_SB_DESC_SIZE);
	if (inodes_per_group == 0)
		errx(1, "source has zero inodes per group");
	if (inode_size < 128 || inode_size > block_size)
		errx(1, "source has invalid inode size");
	if (descriptor_size != sizeof(descriptor))
		errx(1, "source does not have 64-byte descriptors");

	group = (ino - 1) / inodes_per_group;
	index = (ino - 1) % inodes_per_group;
	descriptor_offset = ((uint64_t)get32(sb,
	    EXT4_SB_FIRST_DATA_BLOCK) + 1) * block_size;
	if (group > (UINT64_MAX - descriptor_offset) / descriptor_size)
		errx(1, "group descriptor offset overflows");
	descriptor_offset += group * descriptor_size;
	if (descriptor_offset > (uint64_t)INT64_MAX || image_size < 0 ||
	    descriptor_offset > (uint64_t)image_size ||
	    sizeof(descriptor) > (uint64_t)image_size - descriptor_offset)
		errx(1, "group descriptor lies outside image");
	read_exact(fd, descriptor, sizeof(descriptor),
	    (off_t)descriptor_offset, "group descriptor");

	table = get32(descriptor, EXT4_BG_INODE_TABLE_LO);
	if (get32(sb, EXT4_SB_FEATURE_INCOMPAT) &
	    EXT4_FEATURE_INCOMPAT_64BIT)
		table |= (uint64_t)get32(descriptor,
		    EXT4_BG_INODE_TABLE_HI) << 32;
	if (table >= filesystem_blocks(sb))
		errx(1, "inode table lies outside filesystem");
	if (table > UINT64_MAX / block_size ||
	    index > (UINT64_MAX - table * block_size) / inode_size)
		errx(1, "inode offset overflows");
	offset = table * block_size + index * inode_size;
	if (offset > (uint64_t)INT64_MAX || offset > (uint64_t)image_size ||
	    inode_size > (uint64_t)image_size - offset)
		errx(1, "inode lies outside image");
	*inode_sizep = inode_size;
	return ((off_t)offset);
}

static void
mutate_extent (int fd, uint8_t *sb, const struct mutation *mutation,
    uint32_t ino, off_t image_size)
{
	uint8_t *entry, *inode, *leaf, *root;
	uint64_t blocks, leaf_block, leaf_offset;
	uint32_t block_size, inode_size;
	uint16_t depth, entries, max;
	off_t offset;
	int external, write_inode, write_leaf;

	block_size = filesystem_block_size(sb);
	blocks = filesystem_blocks(sb);
	offset = inode_offset(fd, sb, ino, image_size, &inode_size);
	inode = calloc(1, inode_size);
	if (inode == NULL)
		err(1, "calloc inode");
	read_exact(fd, inode, inode_size, offset, "inode");
	if ((get16(inode, EXT4_INODE_MODE) & EXT4_MODE_TYPE) !=
	    EXT4_MODE_REGULAR)
		errx(1, "target inode is not a regular file");
	if ((get32(inode, EXT4_INODE_FLAGS) & EXT4_INODE_FLAG_EXTENTS) == 0)
		errx(1, "target inode does not use extents");
	root = inode + EXT4_INODE_BLOCK;
	if (get16(root, EXT4_EXTENT_MAGIC) != EXT4_EXTENT_HEADER_MAGIC)
		errx(1, "source inode has bad extent magic");
	depth = get16(root, EXT4_EXTENT_DEPTH);
	entries = get16(root, EXT4_EXTENT_ENTRIES);
	max = get16(root, EXT4_EXTENT_MAX);
	if (max != 4 || entries == 0 || entries > max)
		errx(1, "source inode has an unexpected extent root");

	external = strcmp(mutation->name, "extent-root-depth-too-large") == 0 ||
	    strcmp(mutation->name, "extent-index-out-of-range") == 0 ||
	    strncmp(mutation->name, "extent-leaf-", 12) == 0 ||
	    strcmp(mutation->name, "extent-block-bad-checksum") == 0;
	if ((!external && depth != 0) || (external && depth != 1))
		errx(1, "source inode has unexpected extent depth");

	write_inode = 0;
	write_leaf = 0;
	leaf = NULL;
	if (strcmp(mutation->name, "extent-root-bad-magic") == 0) {
		put16(root, EXT4_EXTENT_MAGIC, 0);
		write_inode = 1;
	} else if (strcmp(mutation->name,
	    "extent-root-entries-over-max") == 0) {
		put16(root, EXT4_EXTENT_ENTRIES, 5);
		write_inode = 1;
	} else if (strcmp(mutation->name,
	    "extent-root-max-too-large") == 0) {
		put16(root, EXT4_EXTENT_MAX, 5);
		write_inode = 1;
	} else if (strcmp(mutation->name, "extent-root-unordered") == 0 ||
	    strcmp(mutation->name, "extent-root-overlap") == 0) {
		entry = root + EXT4_EXTENT_HEADER_SIZE;
		memcpy(entry + EXT4_EXTENT_ENTRY_SIZE, entry,
		    EXT4_EXTENT_ENTRY_SIZE);
		put16(root, EXT4_EXTENT_ENTRIES, 2);
		if (strcmp(mutation->name, "extent-root-unordered") == 0) {
			put32(entry, EXT4_EXTENT_BLOCK, 1);
			put32(entry + EXT4_EXTENT_ENTRY_SIZE,
			    EXT4_EXTENT_BLOCK, 0);
		} else {
			put32(entry + EXT4_EXTENT_ENTRY_SIZE,
			    EXT4_EXTENT_BLOCK,
			    get32(entry, EXT4_EXTENT_BLOCK));
		}
		write_inode = 1;
	} else if (strcmp(mutation->name,
	    "extent-root-zero-length") == 0) {
		entry = root + EXT4_EXTENT_HEADER_SIZE;
		put16(entry, EXT4_EXTENT_LENGTH, 0);
		write_inode = 1;
	} else if (strcmp(mutation->name,
	    "extent-root-physical-out-of-range") == 0) {
		if (blocks > UINT64_C(0xffffffffffff))
			errx(1, "filesystem block count exceeds extent encoding");
		entry = root + EXT4_EXTENT_HEADER_SIZE;
		put16(entry, EXT4_EXTENT_START_HI,
		    (uint16_t)(blocks >> 32));
		put32(entry, EXT4_EXTENT_START_LO, (uint32_t)blocks);
		write_inode = 1;
	} else if (strcmp(mutation->name,
	    "extent-root-depth-too-large") == 0) {
		put16(root, EXT4_EXTENT_DEPTH, 6);
		write_inode = 1;
	} else if (strcmp(mutation->name,
	    "extent-index-out-of-range") == 0) {
		if (blocks > UINT64_C(0xffffffffffff))
			errx(1, "filesystem block count exceeds index encoding");
		entry = root + EXT4_EXTENT_HEADER_SIZE;
		put32(entry, EXT4_EXTENT_INDEX_LEAF_LO, (uint32_t)blocks);
		put16(entry, EXT4_EXTENT_INDEX_LEAF_HI,
		    (uint16_t)(blocks >> 32));
		write_inode = 1;
	} else {
		entry = root + EXT4_EXTENT_HEADER_SIZE;
		leaf_block = get32(entry, EXT4_EXTENT_INDEX_LEAF_LO);
		leaf_block |= (uint64_t)get16(entry,
		    EXT4_EXTENT_INDEX_LEAF_HI) << 32;
		if (leaf_block == 0 || leaf_block >= blocks)
			errx(1, "source extent leaf lies outside filesystem");
		if (leaf_block > UINT64_MAX / block_size)
			errx(1, "extent leaf offset overflows");
		leaf_offset = leaf_block * block_size;
		if (leaf_offset > (uint64_t)INT64_MAX || image_size < 0 ||
		    leaf_offset > (uint64_t)image_size ||
		    block_size > (uint64_t)image_size - leaf_offset)
			errx(1, "extent leaf lies outside image");
		leaf = calloc(1, block_size);
		if (leaf == NULL)
			err(1, "calloc extent leaf");
		read_exact(fd, leaf, block_size, (off_t)leaf_offset,
		    "extent leaf");
		if (get16(leaf, EXT4_EXTENT_MAGIC) !=
		    EXT4_EXTENT_HEADER_MAGIC ||
		    get16(leaf, EXT4_EXTENT_DEPTH) != 0)
			errx(1, "source has invalid extent leaf");
		entries = get16(leaf, EXT4_EXTENT_ENTRIES);
		max = get16(leaf, EXT4_EXTENT_MAX);
		if (entries < 2 || entries > max ||
		    EXT4_EXTENT_HEADER_SIZE + (size_t)max *
		    EXT4_EXTENT_ENTRY_SIZE > block_size)
			errx(1, "source has unexpected extent leaf geometry");

		if (strcmp(mutation->name, "extent-leaf-bad-magic") == 0) {
			put16(leaf, EXT4_EXTENT_MAGIC, 0);
		} else if (strcmp(mutation->name,
		    "extent-leaf-entries-over-max") == 0) {
			put16(leaf, EXT4_EXTENT_ENTRIES, max + 1);
		} else if (strcmp(mutation->name,
		    "extent-leaf-max-too-large") == 0) {
			put16(leaf, EXT4_EXTENT_MAX, max + 1);
		} else if (strcmp(mutation->name,
		    "extent-leaf-unordered") == 0) {
			entry = leaf + EXT4_EXTENT_HEADER_SIZE;
			put32(entry, EXT4_EXTENT_BLOCK,
			    get32(entry + EXT4_EXTENT_ENTRY_SIZE,
			    EXT4_EXTENT_BLOCK) + 1);
		} else if (strcmp(mutation->name,
		    "extent-leaf-overlap") == 0) {
			entry = leaf + EXT4_EXTENT_HEADER_SIZE;
			put32(entry + EXT4_EXTENT_ENTRY_SIZE,
			    EXT4_EXTENT_BLOCK,
			    get32(entry, EXT4_EXTENT_BLOCK));
		} else if (strcmp(mutation->name,
		    "extent-block-bad-checksum") == 0) {
			leaf_offset = EXT4_EXTENT_HEADER_SIZE +
			    (uint64_t)max * EXT4_EXTENT_ENTRY_SIZE;
			if (leaf_offset > block_size - sizeof(uint32_t))
				errx(1, "extent checksum tail lies outside block");
			leaf[leaf_offset] ^= 0x01;
		} else {
			errx(1, "unimplemented extent mutation: %s",
			    mutation->name);
		}
		write_leaf = 1;
		write_exact(fd, leaf, block_size,
		    (off_t)(leaf_block * block_size), "extent leaf");
	}

	if (write_inode)
		write_exact(fd, inode, inode_size, offset, "inode");
	if (!write_inode && !write_leaf)
		errx(1, "extent mutation changed no metadata");
	free(leaf);
	free(inode);
}

static void
mutate (int fd, uint8_t *sb, const struct mutation *mutation, uint32_t ino,
    off_t image_size)
{
	uint64_t descriptor_offset;
	uint32_t block_size, compat, incompat, log_block_size;
	uint8_t value;

	if (mutation->needs_inode) {
		mutate_extent(fd, sb, mutation, ino, image_size);
		return;
	}
	if (strcmp(mutation->name, "bad-superblock-checksum") == 0) {
		sb[EXT4_SB_CHECKSUM] ^= 0x01;
	} else if (strcmp(mutation->name,
	    "bad-group-descriptor-checksum") == 0) {
		log_block_size = get32(sb, EXT4_SB_LOG_BLOCK_SIZE);
		if (log_block_size > 2)
			errx(1, "unsupported source block-size shift");
		block_size = 1024U << log_block_size;
		descriptor_offset = (uint64_t)(get32(sb,
		    EXT4_SB_FIRST_DATA_BLOCK) + 1) * block_size;
		if (descriptor_offset > INT64_MAX - EXT4_BG_CHECKSUM)
			errx(1, "group descriptor offset is too large");
		read_exact(fd, &value, sizeof(value),
		    (off_t)(descriptor_offset + EXT4_BG_CHECKSUM),
		    "group descriptor checksum");
		value ^= 0x01;
		write_exact(fd, &value, sizeof(value),
		    (off_t)(descriptor_offset + EXT4_BG_CHECKSUM),
		    "group descriptor checksum");
		return;
	} else if (strcmp(mutation->name, "block-size-too-large") == 0) {
		if (get32(sb, EXT4_SB_LOG_BLOCK_SIZE) > 2)
			errx(1, "source already has an invalid block size");
		put32(sb, EXT4_SB_LOG_BLOCK_SIZE, 3);
	} else if (strcmp(mutation->name, "zero-blocks-per-group") == 0) {
		if (get32(sb, EXT4_SB_BLOCKS_PER_GROUP) == 0)
			errx(1, "source already has zero blocks per group");
		put32(sb, EXT4_SB_BLOCKS_PER_GROUP, 0);
	} else if (strcmp(mutation->name, "zero-inodes-per-group") == 0) {
		if (get32(sb, EXT4_SB_INODES_PER_GROUP) == 0)
			errx(1, "source already has zero inodes per group");
		put32(sb, EXT4_SB_INODES_PER_GROUP, 0);
	} else if (strcmp(mutation->name, "invalid-inode-size") == 0) {
		if (get16(sb, EXT4_SB_INODE_SIZE) != 256)
			errx(1, "source does not have 256-byte inodes");
		put16(sb, EXT4_SB_INODE_SIZE, 128);
	} else if (strcmp(mutation->name, "invalid-first-inode") == 0) {
		if (get32(sb, EXT4_SB_FIRST_INO) != 11)
			errx(1, "source has an unexpected first inode");
		put32(sb, EXT4_SB_FIRST_INO, 12);
	} else if (strcmp(mutation->name,
	    "invalid-descriptor-size") == 0) {
		if (get16(sb, EXT4_SB_DESC_SIZE) != 64)
			errx(1, "source does not have 64-byte descriptors");
		put16(sb, EXT4_SB_DESC_SIZE, 32);
	} else if (strcmp(mutation->name,
	    "unsupported-incompat-feature") == 0) {
		incompat = get32(sb, EXT4_SB_FEATURE_INCOMPAT);
		if (incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT)
			errx(1, "source already has encrypt incompat feature");
		put32(sb, EXT4_SB_FEATURE_INCOMPAT,
		    incompat | EXT4_FEATURE_INCOMPAT_ENCRYPT);
	} else if (strcmp(mutation->name, "recover-without-journal") == 0) {
		compat = get32(sb, EXT4_SB_FEATURE_COMPAT);
		incompat = get32(sb, EXT4_SB_FEATURE_INCOMPAT);
		if ((compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) == 0)
			errx(1, "source has no journal");
		if (incompat & EXT4_FEATURE_INCOMPAT_RECOVER)
			errx(1, "source already needs recovery");
		put32(sb, EXT4_SB_FEATURE_COMPAT,
		    compat & ~EXT4_FEATURE_COMPAT_HAS_JOURNAL);
		put32(sb, EXT4_SB_FEATURE_INCOMPAT,
		    incompat | EXT4_FEATURE_INCOMPAT_RECOVER);
	} else {
		errx(1, "unimplemented mutation: %s", mutation->name);
	}

	write_exact(fd, sb, EXT4_SUPER_SIZE, EXT4_SUPER_OFFSET,
	    "superblock");
}

int
main (int argc, char **argv)
{
	const struct mutation *mutation;
	struct stat st;
	uint8_t sb[EXT4_SUPER_SIZE];
	uint32_t ino, ro_compat;
	int fd, has_checksum;

	if (argc < 3 || argc > 4)
		errx(1, "usage: %s image mutation [inode]", argv[0]);
	mutation = find_mutation(argv[2]);
	if (mutation == NULL)
		errx(1, "unknown mutation: %s", argv[2]);
	if ((!mutation->needs_inode && argc != 3) ||
	    (mutation->needs_inode && argc != 4))
		errx(1, "%s %s an inode argument", mutation->name,
		    mutation->needs_inode ? "requires" : "does not accept");
	ino = mutation->needs_inode ? number32(argv[3], "inode") : 0;

	fd = open(argv[1], O_RDWR);
	if (fd == -1)
		err(1, "%s", argv[1]);
	if (fstat(fd, &st) == -1)
		err(1, "fstat %s", argv[1]);
	if (!S_ISREG(st.st_mode))
		errx(1, "%s is not a regular file", argv[1]);
	if (st.st_size < EXT4_SUPER_OFFSET + EXT4_SUPER_SIZE)
		errx(1, "%s is too small", argv[1]);

	read_exact(fd, sb, sizeof(sb), EXT4_SUPER_OFFSET, "superblock");
	if (get16(sb, EXT4_SB_MAGIC) != EXT4_SUPER_MAGIC)
		errx(1, "%s has invalid ext4 magic", argv[1]);
	if (get32(sb, EXT4_SB_BLOCKS_COUNT_LO) == 0)
		errx(1, "%s has no filesystem blocks", argv[1]);
	ro_compat = get32(sb, EXT4_SB_FEATURE_RO_COMPAT);
	has_checksum = (ro_compat & EXT4_FEATURE_RO_METADATA_CSUM) != 0;
	if (mutation->checksum == CHECKSUM_REQUIRED && !has_checksum)
		errx(1, "%s requires metadata_csum", mutation->name);
	if (mutation->checksum == CHECKSUM_FORBIDDEN && has_checksum)
		errx(1, "%s requires metadata_csum to be disabled",
		    mutation->name);

	mutate(fd, sb, mutation, ino, st.st_size);
	if (fsync(fd) == -1)
		err(1, "fsync %s", argv[1]);
	if (close(fd) == -1)
		err(1, "close %s", argv[1]);
	return (0);
}
