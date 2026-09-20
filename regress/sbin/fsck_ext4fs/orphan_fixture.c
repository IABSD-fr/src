/*
 * Copyright (c) 2026 OpenAI
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

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <lib/libkern/crc32c.h>

#define ORPHAN_TAIL_MAGIC	0x0b10ca04

struct orphan_tail {
	uint32_t	magic;
	uint32_t	checksum;
} __attribute__((packed));

static uint64_t
number (const char *value, const char *name)
{
	char *end;
	uint64_t result;

	errno = 0;
	end = NULL;
	if (*value == '-')
		errx(1, "%s is negative", name);
	result = strtoull(value, &end, 0);
	if (errno == ERANGE)
		errx(1, "%s is too large", name);
	if (value[0] == '\0' || end == NULL || *end != '\0')
		errx(1, "%s is invalid", name);
	return (result);
}

static uint32_t
number32 (const char *value, const char *name)
{
	uint64_t result;

	result = number(value, name);
	if (result > UINT32_MAX)
		errx(1, "%s is too large", name);
	return (result);
}

int
main (int argc, char **argv)
{
	struct orphan_tail *tail;
	uint8_t *data;
	uint64_t physical, offset;
	uint32_t block_size, stored_seed, orphan_ino, generation, target;
	uint32_t seed, crc, ino_le, generation_le, target_le;
	uint64_t physical_le;
	size_t entries, i;
	ssize_t done;
	int fd;

	if (argc != 8)
		errx(1, "usage: %s image block-size physical-block "
		    "checksum-seed orphan-inode generation target-inode",
		    argv[0]);
	block_size = number32(argv[2], "block size");
	physical = number(argv[3], "physical block");
	stored_seed = number32(argv[4], "checksum seed");
	orphan_ino = number32(argv[5], "orphan inode");
	generation = number32(argv[6], "generation");
	target = number32(argv[7], "target inode");
	if (block_size < 1024 || block_size > 4096 ||
	    (block_size & (block_size - 1)) != 0)
		errx(1, "unsupported block size");
	if (physical > (uint64_t)INT64_MAX / block_size)
		errx(1, "block offset is too large");
	offset = physical * block_size;

	data = calloc(1, block_size);
	if (data == NULL)
		err(1, "calloc");
	fd = open(argv[1], O_RDWR);
	if (fd == -1)
		err(1, "%s", argv[1]);
	done = pread(fd, data, block_size, offset);
	if (done == -1)
		err(1, "pread");
	if (done != block_size)
		errx(1, "short orphan block read");

	tail = (struct orphan_tail *)(data + block_size - sizeof(*tail));
	if (letoh32(tail->magic) != ORPHAN_TAIL_MAGIC)
		errx(1, "bad orphan block magic");
	entries = (block_size - sizeof(*tail)) / sizeof(uint32_t);
	for (i = 0; i < entries; i++)
		if (((uint32_t *)data)[i] == 0)
			break;
	if (i == entries)
		errx(1, "orphan block has no free entry");
	target_le = htole32(target);
	((uint32_t *)data)[i] = target_le;

	seed = ~stored_seed;
	ino_le = htole32(orphan_ino);
	generation_le = htole32(generation);
	crc = crc32c(seed, (const uint8_t *)&ino_le, sizeof(ino_le));
	crc = crc32c(crc, (const uint8_t *)&generation_le,
	    sizeof(generation_le));
	physical_le = htole64(physical);
	crc = crc32c(crc, (const uint8_t *)&physical_le,
	    sizeof(physical_le));
	crc = crc32c(crc, data, block_size - sizeof(*tail));
	tail->checksum = htole32(~crc);

	done = pwrite(fd, data, block_size, offset);
	if (done == -1)
		err(1, "pwrite");
	if (done != block_size)
		errx(1, "short orphan block write");
	if (fsync(fd) == -1)
		err(1, "fsync");
	if (close(fd) == -1)
		err(1, "close");
	free(data);
	return (0);
}
