/*
 * Copyright (c) 2026 kmx.io.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IO_BYTES	8192
#define OLD_BYTES	((off_t)(1024 * 1024 + 37))
#define NEW_BYTES	((off_t)(4 * 1024 * 1024 + 73))
#define OLD_SEED	0x35U
#define NEW_SEED	0xc9U
#define ARM_USEC	2000000

static void	make_path (char *, size_t, const char *, const char *);
static unsigned char pattern_byte (off_t, unsigned int);
static void	fill_pattern (unsigned char *, size_t, off_t,
    unsigned int);
static int	open_output (const char *, int);
static void	write_pattern (int, off_t, unsigned int);
static void	check_pattern (const char *, off_t, unsigned int);
static void	arm_cut (const char *);
static void	make_old_pattern (const char *);
static void	run_workload (const char *, const char *);
static void	verify_workload (const char *);
static void	selftest (void);

static void
make_path (char *path, size_t pathlen, const char *root,
    const char *name)
{
	int n;

	n = snprintf(path, pathlen, "%s/%s", root, name);
	if (n < 0 || (size_t)n >= pathlen)
		errx(1, "path too long: %s", name);
}

static unsigned char
pattern_byte (off_t offset, unsigned int seed)
{
	uint64_t value;

	value = (uint64_t)offset;
	value ^= value >> 17;
	value *= UINT64_C(0x9e3779b185ebca87);
	value ^= value >> 29;
	return ((unsigned char)(value + seed));
}

static void
fill_pattern (unsigned char *buf, size_t len, off_t offset,
    unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = pattern_byte(offset + (off_t)i, seed);
}

static int
open_output (const char *path, int exclusive)
{
	int flags, fd;

	flags = O_WRONLY | O_CREAT | O_CLOEXEC;
	flags |= exclusive ? O_EXCL : O_TRUNC;
	fd = open(path, flags, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	return (fd);
}

static void
write_pattern (int fd, off_t len, unsigned int seed)
{
	unsigned char buf[IO_BYTES];
	off_t offset;
	size_t chunk, done;
	ssize_t n;

	for (offset = 0; offset < len; offset += (off_t)done) {
		chunk = sizeof(buf);
		if ((off_t)chunk > len - offset)
			chunk = (size_t)(len - offset);
		fill_pattern(buf, chunk, offset, seed);
		done = 0;
		while (done < chunk) {
			n = write(fd, buf + done, chunk - done);
			if (n == -1)
				err(1, "write at offset %lld",
				    (long long)(offset + (off_t)done));
			if (n == 0)
				errx(1, "zero-length write");
			done += (size_t)n;
		}
	}
}

static void
check_pattern (const char *path, off_t maximum, unsigned int seed)
{
	unsigned char actual[IO_BYTES], expected[IO_BYTES];
	struct stat st;
	off_t offset;
	size_t chunk, done;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		err(1, "open %s", path);
	if (fstat(fd, &st) == -1)
		err(1, "fstat %s", path);
	if (! S_ISREG(st.st_mode) || st.st_size < 0 ||
	    st.st_size > maximum)
		errx(1, "%s has invalid shape", path);
	for (offset = 0; offset < st.st_size;
	    offset += (off_t)chunk) {
		chunk = sizeof(actual);
		if ((off_t)chunk > st.st_size - offset)
			chunk = (size_t)(st.st_size - offset);
		done = 0;
		while (done < chunk) {
			n = pread(fd, actual + done, chunk - done,
			    offset + (off_t)done);
			if (n == -1)
				err(1, "pread %s", path);
			if (n == 0)
				errx(1, "short read from %s", path);
			done += (size_t)n;
		}
		fill_pattern(expected, chunk, offset, seed);
		if (memcmp(actual, expected, chunk) != 0)
			errx(1, "%s has invalid data at offset %lld",
			    path, (long long)offset);
	}
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
arm_cut (const char *stage)
{
	if (printf("READY %s\n", stage) < 0 || fflush(stdout) == EOF)
		err(1, "publish crash marker");
	if (usleep(ARM_USEC) == -1)
		err(1, "arm delay");
}

static void
make_old_pattern (const char *path)
{
	int fd;

	fd = open_output(path, 0);
	write_pattern(fd, OLD_BYTES, OLD_SEED);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
run_workload (const char *stage, const char *root)
{
	char current[PATH_MAX], next[PATH_MAX];
	int fd;

	make_path(current, sizeof(current), root, "current");
	make_path(next, sizeof(next), root, "next");
	fd = open_output(next, 1);
	if (strcmp(stage, "write") == 0) {
		arm_cut(stage);
		write_pattern(fd, NEW_BYTES, NEW_SEED);
	} else if (strcmp(stage, "fsync") == 0) {
		write_pattern(fd, NEW_BYTES, NEW_SEED);
		arm_cut(stage);
		if (fsync(fd) == -1)
			err(1, "fsync %s", next);
	} else if (strcmp(stage, "rename") == 0) {
		write_pattern(fd, NEW_BYTES, NEW_SEED);
		if (fsync(fd) == -1)
			err(1, "fsync %s", next);
		if (close(fd) == -1)
			err(1, "close %s", next);
		fd = -1;
		arm_cut(stage);
		if (rename(next, current) == -1)
			err(1, "rename %s", next);
	} else
		errx(1, "unknown workload stage: %s", stage);
	if (fd != -1 && close(fd) == -1)
		err(1, "close %s", next);
	if (printf("DONE %s\n", stage) < 0 || fflush(stdout) == EOF)
		err(1, "publish completion marker");
	for (;;)
		(void)pause();
}

static void
verify_workload (const char *root)
{
	struct stat current_st, next_st;
	char current[PATH_MAX], next[PATH_MAX];
	const char *state;
	off_t expected;
	unsigned int seed;

	make_path(current, sizeof(current), root, "current");
	make_path(next, sizeof(next), root, "next");
	if (lstat(current, &current_st) == -1)
		err(1, "lstat %s", current);
	if (! S_ISREG(current_st.st_mode))
		errx(1, "%s is not a regular file", current);
	if (current_st.st_size == OLD_BYTES) {
		state = "old";
		expected = OLD_BYTES;
		seed = OLD_SEED;
	} else if (current_st.st_size == NEW_BYTES) {
		state = "new";
		expected = NEW_BYTES;
		seed = NEW_SEED;
	} else
		errx(1, "%s has unexpected size %lld", current,
		    (long long)current_st.st_size);
	check_pattern(current, expected, seed);
	if (lstat(next, &next_st) == -1) {
		if (errno != ENOENT)
			err(1, "lstat %s", next);
		if (strcmp(state, "old") == 0)
			printf("state=old next=absent\n");
		else
			printf("state=new next=absent\n");
	} else {
		if (strcmp(state, "new") == 0)
			errx(1, "new current and next both exist");
		check_pattern(next, NEW_BYTES, NEW_SEED);
		printf("state=old next=%lld\n",
		    (long long)next_st.st_size);
	}
	if (fflush(stdout) == EOF)
		err(1, "publish verification state");
}

static void
selftest (void)
{
	char current[PATH_MAX], next[PATH_MAX];
	char root[] = "/tmp/ext4fs_crash.XXXXXXXX";
	int fd;

	if (mkdtemp(root) == NULL)
		err(1, "mkdtemp");
	make_path(current, sizeof(current), root, "current");
	make_path(next, sizeof(next), root, "next");
	make_old_pattern(current);
	verify_workload(root);

	fd = open_output(next, 1);
	write_pattern(fd, 12345, NEW_SEED);
	if (fsync(fd) == -1)
		err(1, "fsync %s", next);
	if (close(fd) == -1)
		err(1, "close %s", next);
	verify_workload(root);
	if (unlink(next) == -1)
		err(1, "unlink %s", next);

	fd = open_output(next, 1);
	write_pattern(fd, NEW_BYTES, NEW_SEED);
	if (fsync(fd) == -1)
		err(1, "fsync %s", next);
	if (close(fd) == -1)
		err(1, "close %s", next);
	if (rename(next, current) == -1)
		err(1, "rename %s", next);
	verify_workload(root);
	if (unlink(current) == -1)
		err(1, "unlink %s", current);
	if (rmdir(root) == -1)
		err(1, "rmdir %s", root);
}

int
main (int argc, char **argv)
{
	if (argc == 1)
		selftest();
	else if (argc == 3 && strcmp(argv[1], "pattern-old") == 0)
		make_old_pattern(argv[2]);
	else if (argc == 4 && strcmp(argv[1], "workload") == 0)
		run_workload(argv[2], argv[3]);
	else if (argc == 3 && strcmp(argv[1], "verify") == 0)
		verify_workload(argv[2]);
	else
		errx(1, "usage: ext4fs_crash pattern-old path | "
		    "workload stage root | verify root");
	return (0);
}
