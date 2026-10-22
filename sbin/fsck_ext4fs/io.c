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

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "io.h"

static int	io_range (size_t, off_t, size_t);
static int	io_read (int, void *, size_t, off_t);
static int	io_write (int, const void *, size_t, off_t);

static int
io_range (size_t size, off_t offset, size_t sector)
{
	u_int64_t end, padding;

	if (offset < 0 || sector < DEV_BSIZE || sector > MAXBSIZE ||
	    sector % DEV_BSIZE != 0)
		return (EINVAL);
	if (size > SSIZE_MAX || size > (u_int64_t)LLONG_MAX - offset)
		return (EOVERFLOW);
	if (size == 0)
		return (0);
	end = (u_int64_t)offset + size;
	padding = end % sector == 0 ? 0 : sector - end % sector;
	if (padding > (u_int64_t)LLONG_MAX - end)
		return (EOVERFLOW);
	return (0);
}

static int
io_read (int fd, void *buf, size_t size, off_t offset)
{
	ssize_t n;

	do {
		n = pread(fd, buf, size, offset);
	} while (n == -1 && errno == EINTR);
	if (n == -1)
		return (errno);
	return (n == (ssize_t)size ? 0 : EIO);
}

static int
io_write (int fd, const void *buf, size_t size, off_t offset)
{
	ssize_t n;

	do {
		n = pwrite(fd, buf, size, offset);
	} while (n == -1 && errno == EINTR);
	if (n == -1)
		return (errno);
	return (n == (ssize_t)size ? 0 : EIO);
}

int
fsck_io_read (int fd, void *buf, size_t size, off_t offset,
    size_t sector)
{
	char *bounce, *p = buf;
	size_t within, chunk;
	int error;

	error = io_range(size, offset, sector);
	if (error || size == 0)
		return (error);
	if (offset % sector == 0 && size % sector == 0)
		return (io_read(fd, buf, size, offset));
	bounce = malloc(sector);
	if (bounce == NULL)
		return (ENOMEM);
	while (size != 0) {
		within = offset % sector;
		chunk = MIN(size, sector - within);
		error = io_read(fd, bounce, sector, offset - within);
		if (error)
			break;
		memcpy(p, bounce + within, chunk);
		p += chunk;
		offset += chunk;
		size -= chunk;
	}
	free(bounce);
	return (error);
}

/*
 * fsck opens separate read-only and write-only descriptors.  Partial
 * sectors must be read successfully before modifying any of their
 * bytes; otherwise a superblock write could destroy adjacent data.
 */
int
fsck_io_write (int readfd, int writefd, const void *buf, size_t size,
    off_t offset, size_t sector)
{
	const char *p = buf;
	char *bounce;
	size_t within, chunk;
	int error;

	error = io_range(size, offset, sector);
	if (error || size == 0)
		return (error);
	if (offset % sector == 0 && size % sector == 0)
		return (io_write(writefd, buf, size, offset));
	bounce = malloc(sector);
	if (bounce == NULL)
		return (ENOMEM);
	while (size != 0) {
		within = offset % sector;
		chunk = MIN(size, sector - within);
		if (within != 0 || chunk != sector) {
			error = io_read(readfd, bounce, sector,
			    offset - within);
			if (error)
				break;
		}
		memcpy(bounce + within, p, chunk);
		error = io_write(writefd, bounce, sector,
		    offset - within);
		if (error)
			break;
		p += chunk;
		offset += chunk;
		size -= chunk;
	}
	free(bounce);
	return (error);
}
