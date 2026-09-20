/*
 * Copyright (c) 2026 kmx.io.
 *
 * Permission to use, copy, modify, and distribute this software for
 * any purpose with or without fee is hereby granted, provided that
 * the above copyright notice and this permission notice appear in
 * all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA
 * OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/disklabel.h>
#include <sys/dkio.h>
#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "io.h"

#define READFD	71
#define WRITEFD	72
#define GUARD	32

static unsigned char disk[65536], saved[sizeof(disk)];
static size_t sector_size;
static int reads, writes, fail_read, fail_write, fault;

u_short	dkcksum (const struct disklabel *);

static void	check (int, const char *);
static void	reset (void);
static void	check_range (size_t, size_t);
static void	check_errors (void);
static void	set_sector_size (const char *, const char *);

static void
check (int ok, const char *what)
{
	if (! ok)
		errx(1, "%s (sector size %zu)", what, sector_size);
}

/* These replace only io.c's pread/pwrite at link time. */
ssize_t
test_pread (int fd, void *buf, size_t size, off_t offset)
{
	check(fd == READFD, "read through wrong descriptor");
	check(offset >= 0 && offset % sector_size == 0 &&
	    size % sector_size == 0, "unaligned device read");
	check((u_int64_t)offset <= sizeof(disk) &&
	    size <= sizeof(disk) - offset, "read outside device");
	if (++reads == fail_read) {
		errno = fault == 0 ? EDOM : fault;
		return (fault == 0 ? (ssize_t)size / 2 : -1);
	}
	memcpy(buf, disk + offset, size);
	return (size);
}

ssize_t
test_pwrite (int fd, const void *buf, size_t size, off_t offset)
{
	check(fd == WRITEFD, "write through wrong descriptor");
	check(offset >= 0 && offset % sector_size == 0 &&
	    size % sector_size == 0, "unaligned device write");
	check((u_int64_t)offset <= sizeof(disk) &&
	    size <= sizeof(disk) - offset, "write outside device");
	if (++writes == fail_write) {
		errno = fault == 0 ? EDOM : fault;
		return (fault == 0 ? (ssize_t)size / 2 : -1);
	}
	memcpy(disk + offset, buf, size);
	return (size);
}

static void
reset (void)
{
	size_t i;

	for (i = 0; i < sizeof(disk); i++)
		disk[i] = (i * 37 + i / 251) & 255;
	memcpy(saved, disk, sizeof(disk));
	reads = writes = fail_read = fail_write = fault = 0;
}

static void
check_range (size_t offset, size_t size)
{
	unsigned char *buf;
	size_t i;

	reset();
	buf = malloc(size + 2 * GUARD);
	if (buf == NULL)
		err(1, "malloc");
	memset(buf, 0xa5, size + 2 * GUARD);
	check(fsck_io_read(READFD, buf + GUARD, size, offset,
	    sector_size) == 0, "read failed");
	check(memcmp(buf + GUARD, disk + offset, size) == 0,
	    "read returned wrong bytes");
	for (i = 0; i < GUARD; i++) {
		check(buf[i] == 0xa5, "read overwrote prefix guard");
		check(buf[GUARD + size + i] == 0xa5,
		    "read overwrote suffix guard");
	}
	memset(buf + GUARD, 0x3c, size);
	check(fsck_io_write(READFD, WRITEFD, buf + GUARD, size,
	    offset, sector_size) == 0, "write failed");
	for (i = 0; i < sizeof(disk); i++)
		check(disk[i] == (i >= offset && i - offset < size ?
		    0x3c : saved[i]), "write changed wrong bytes");
	for (i = 0; i < size + 2 * GUARD; i++)
		check(buf[i] == (i >= GUARD && i - GUARD < size ?
		    0x3c : 0xa5), "write changed source buffer");
	free(buf);
}

static void
check_errors (void)
{
	unsigned char buf[8192];
	size_t j;
	int i;

	sector_size = 4096;
	reset();
	memset(buf, 0x3c, sizeof(buf));
	fail_read = 1;
	fault = ENXIO;
	check(fsck_io_read(READFD, buf, 1024, 1024, sector_size) ==
	    ENXIO, "lost read errno");
	reset();
	fail_read = 1;
	check(fsck_io_read(READFD, buf, 1024, 1024, sector_size) ==
	    EIO, "short read did not return EIO");
	reset();
	fail_read = 1;
	fault = EINTR;
	check(fsck_io_read(READFD, buf, 1024, 1024, sector_size) ==
	    0 && reads == 2, "interrupted read did not retry");
	for (i = 0; i < 2; i++) {
		reset();
		fault = i == 0 ? 0 : ENXIO;
		fail_read = 1;
		check(fsck_io_write(READFD, WRITEFD, buf, 1024,
		    1024, sector_size) == (fault == 0 ? EIO : ENXIO),
		    "read-modify-write ignored read failure");
		check(writes == 0 && memcmp(saved, disk,
		    sizeof(disk)) == 0, "failed read allowed a write");
	}
	reset();
	fail_write = 1;
	fault = ENOSPC;
	check(fsck_io_write(READFD, WRITEFD, buf, 1024, 1024,
	    sector_size) == ENOSPC, "lost write errno");
	reset();
	fail_write = 1;
	check(fsck_io_write(READFD, WRITEFD, buf, 1024, 1024,
	    sector_size) == EIO, "short write did not return EIO");
	reset();
	fail_write = 1;
	fault = EINTR;
	check(fsck_io_write(READFD, WRITEFD, buf, 1024, 1024,
	    sector_size) == 0 && writes == 2,
	    "interrupted write did not retry");
	reset();
	fail_read = 1;
	fault = ENXIO;
	check(fsck_io_write(READFD, WRITEFD, buf, 4096, 4096,
	    sector_size) == 0 && reads == 0,
	    "aligned write unnecessarily read the sector");
	reset();
	memset(buf, 0x3c, sizeof(buf));
	fail_read = 2;
	fault = ENXIO;
	check(fsck_io_read(READFD, buf, sizeof(buf), 1024,
	    sector_size) == ENXIO && reads == 2,
	    "multi-sector read ignored failure");
	check(memcmp(buf, disk + 1024, 3072) == 0,
	    "multi-sector read lost successful prefix");
	for (j = 3072; j < sizeof(buf); j++)
		check(buf[j] == 0x3c, "failed read copied bad bytes");
	reset();
	memset(buf, 0x3c, sizeof(buf));
	fail_read = 2;
	fault = ENXIO;
	check(fsck_io_write(READFD, WRITEFD, buf, 4096, 1024,
	    sector_size) == ENXIO && reads == 2 && writes == 1,
	    "multi-sector write ignored read failure");
	for (j = 0; j < sizeof(disk); j++)
		check(disk[j] == (j >= 1024 && j < 4096 ?
		    0x3c : saved[j]),
		    "failed write changed other bytes");
	reset();
	check(fsck_io_read(READFD, buf, 1024, -1, 4096) == EINVAL,
	    "negative offset accepted");
	check(fsck_io_read(READFD, buf, 1024, 0, 0) == EINVAL,
	    "zero sector size accepted");
	check(fsck_io_read(READFD, buf, 1024, 0, 513) == EINVAL,
	    "invalid sector size accepted");
	check(fsck_io_write(READFD, WRITEFD, buf, 1024, 0,
	    MAXBSIZE + 512) == EINVAL, "oversize sector accepted");
	check(fsck_io_read(READFD, buf, (size_t)SSIZE_MAX + 1,
	    0, 4096) == EOVERFLOW, "oversize transfer accepted");
	check(fsck_io_write(READFD, WRITEFD, buf, 1024,
	    LLONG_MAX - 511, 4096) == EOVERFLOW,
	    "offset addition overflow accepted");
	check(fsck_io_read(READFD, buf, 1, LLONG_MAX - 1,
	    4096) == EOVERFLOW, "sector rounding overflow accepted");
	check(fsck_io_read(READFD, buf, 0, 0, 4096) == 0 &&
	    fsck_io_write(READFD, WRITEFD, buf, 0, 0, 4096) == 0,
	    "zero-length transfer failed");
	check(reads == 0 && writes == 0,
	    "invalid or empty transfer reached device");
}

static void
set_sector_size (const char *path, const char *size)
{
	struct disklabel label;
	const char *error;
	unsigned int sector;
	int fd;

	sector = strtonum(size, 512, 4096, &error);
	if (error != NULL || (sector & (sector - 1)) != 0)
		errx(1, "invalid sector size: %s", size);
	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);
	if (ioctl(fd, DIOCGDINFO, &label) == -1)
		err(1, "DIOCGDINFO %s", path);
	label.d_secsize = sector;
	label.d_checksum = 0;
	label.d_checksum = dkcksum(&label);
	if (ioctl(fd, DIOCSDINFO, &label) == -1)
		err(1, "DIOCSDINFO %s", path);
	if (ioctl(fd, DIOCGDINFO, &label) == -1)
		err(1, "DIOCGDINFO verify %s", path);
	if (label.d_secsize != sector)
		errx(1, "%s retained %u-byte sectors", path,
		    label.d_secsize);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

int
main (int argc, char **argv)
{
	const size_t sectors[] = { 512, 1024, 2048, 4096, 8192 };
	const size_t sizes[] = { 1, 511, 512, 1024, 2048, 4096, 8192 };
	size_t i, j;

	if (argc == 4 && strcmp(argv[1], "sector-size") == 0) {
		set_sector_size(argv[2], argv[3]);
		return (0);
	}
	if (argc != 1)
		errx(1, "usage: io_test [sector-size device bytes]");
	for (i = 0; i < nitems(sectors); i++) {
		sector_size = sectors[i];
		for (j = 0; j < nitems(sizes); j++) {
			check_range(0, sizes[j]);
			check_range(1024, sizes[j]);
			check_range(sector_size - 1, sizes[j]);
			check_range(sector_size + 1, sizes[j]);
		}
		printf("aligned I/O: %zu-byte sectors ok\n",
		    sector_size);
	}
	check_errors();
	puts("I/O failure and overflow checks ok");
	return (0);
}
