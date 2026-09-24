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
#define EXT4_SB_CHECKSUM			0x3fc

#define EXT4_BG_CHECKSUM		0x01e

#define EXT4_SUPER_MAGIC		0xef53
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL	0x00000004U
#define EXT4_FEATURE_INCOMPAT_RECOVER	0x00000004U
#define EXT4_FEATURE_INCOMPAT_ENCRYPT	0x00010000U
#define EXT4_FEATURE_RO_METADATA_CSUM	0x00000400U

enum checksum_profile {
	CHECKSUM_REQUIRED,
	CHECKSUM_FORBIDDEN
};

struct mutation {
	const char		*name;
	enum checksum_profile	 checksum;
};

static const struct mutation mutations[] = {
	{ "bad-superblock-checksum", CHECKSUM_REQUIRED },
	{ "bad-group-descriptor-checksum", CHECKSUM_REQUIRED },
	{ "block-size-too-large", CHECKSUM_FORBIDDEN },
	{ "zero-blocks-per-group", CHECKSUM_FORBIDDEN },
	{ "zero-inodes-per-group", CHECKSUM_FORBIDDEN },
	{ "invalid-inode-size", CHECKSUM_FORBIDDEN },
	{ "invalid-first-inode", CHECKSUM_FORBIDDEN },
	{ "invalid-descriptor-size", CHECKSUM_FORBIDDEN },
	{ "unsupported-incompat-feature", CHECKSUM_FORBIDDEN },
	{ "recover-without-journal", CHECKSUM_FORBIDDEN }
};

static void	read_exact (int, void *, size_t, off_t, const char *);
static void	write_exact (int, const void *, size_t, off_t, const char *);
static uint16_t	get16 (const uint8_t *, size_t);
static uint32_t	get32 (const uint8_t *, size_t);
static void	put16 (uint8_t *, size_t, uint16_t);
static void	put32 (uint8_t *, size_t, uint32_t);
static const struct mutation *find_mutation (const char *);
static void	mutate (int, uint8_t *, const struct mutation *);

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

static const struct mutation *
find_mutation (const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(mutations) / sizeof(mutations[0]); i++)
		if (strcmp(name, mutations[i].name) == 0)
			return (&mutations[i]);
	return (NULL);
}

static void
mutate (int fd, uint8_t *sb, const struct mutation *mutation)
{
	uint64_t descriptor_offset;
	uint32_t block_size, compat, incompat, log_block_size;
	uint8_t value;

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
	uint32_t ro_compat;
	int fd, has_checksum;

	if (argc != 3)
		errx(1, "usage: %s image mutation", argv[0]);
	mutation = find_mutation(argv[2]);
	if (mutation == NULL)
		errx(1, "unknown mutation: %s", argv[2]);

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

	mutate(fd, sb, mutation);
	if (fsync(fd) == -1)
		err(1, "fsync %s", argv[1]);
	if (close(fd) == -1)
		err(1, "close %s", argv[1]);
	return (0);
}
