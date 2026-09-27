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
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GROW_FILES	180
#define REFILL_FILES	60
#define EXTENT_WRITES	12
#define SHRINK_EXTENT_LBN	10
#define SHRINK_EXTENT_BYTES	37
#define DIR_TAIL_BYTES	12
#define FAST_SYMLINK_BYTES	60
#define IO_CHUNK	4096

#define DATA_SEED	0x31U
#define APPEND_SEED	0x52U
#define OVERWRITE_SEED	0x93U
#define EXTENT_SEED	0xb4U
#define BITMAP_SEED	0xc7U

static char root[PATH_MAX];
static size_t block_size;

static void	make_path (char *, size_t, const char *);
static void	make_indexed_path (char *, size_t, const char *, int);
static unsigned char pattern_byte (off_t, unsigned int);
static void	fill_pattern (unsigned char *, size_t, off_t, unsigned int);
static void	write_pattern_fd (int, off_t, size_t, unsigned int);
static void	check_pattern_fd (int, off_t, size_t, unsigned int);
static void	check_zero_fd (int, off_t, size_t);
static void	write_text_file (const char *, const char *);
static void	check_text_file (const char *, const char *);
static void	check_absent (const char *);
static void	check_directory (const char *);
static void	check_regular (const char *);
static void	check_symlink (const char *, const char *);
static void	check_data_file (const char *, int);
static void	check_sparse_file (const char *, int);
static void	check_large_sparse (const char *, int);
static void	check_extent_file (const char *);
static void	check_shrunk_extent_file (const char *);
static void	check_empty_file (const char *);
static void	create_link_growth_tree (void);
static void	check_link_growth_tree (int);
static void	check_grow_directory (const char *, int);
static void	fsync_path (const char *);
static void	expect_ro_failure (const char *, int);
static void	create_unix_socket (const char *);
static void	create_special_files (void);
static void	verify_special_files (void);
static void	remove_special_files (void);
static void	create_grow_directory (void);
static void	create_filesystem_tree (void);
static void	verify_created_tree (void);
static void	mutate_filesystem_tree (void);
static void	verify_final_tree (void);
static void	verify_readonly_tree (void);
static void	create_allocation_probe (void);
static void	verify_allocation_probe (void);
static void	create_extent_file (const char *);
static void	allocate_bitmap_probe (void);
static void	verify_bitmap_probe (int);
static void	free_bitmap_probe (void);
static void	reuse_bitmap_probe (void);
static void	retire_bitmap_probe (void);
static void	allocate_inode_probe (void);
static void	verify_inode_probe (void);
static void	free_inode_probe (void);

static void
make_path (char *path, size_t pathlen, const char *suffix)
{
	int n;

	n = snprintf(path, pathlen, "%s/%s", root, suffix);
	if (n < 0 || (size_t)n >= pathlen)
		errx(1, "path too long: %s", suffix);
}

static void
make_indexed_path (char *path, size_t pathlen, const char *directory,
    int index)
{
	char suffix[128];
	int n;

	n = snprintf(suffix, sizeof(suffix), "%s/entry-%03d-abcdefghijkl",
	    directory, index);
	if (n < 0 || (size_t)n >= sizeof(suffix))
		errx(1, "indexed suffix too long");
	make_path(path, pathlen, suffix);
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

static void
write_pattern_fd (int fd, off_t offset, size_t len, unsigned int seed)
{
	unsigned char buf[IO_CHUNK];
	size_t done, chunk;
	ssize_t n;

	for (done = 0; done < len; done += chunk) {
		chunk = len - done;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);
		fill_pattern(buf, chunk, offset + (off_t)done, seed);
		n = pwrite(fd, buf, chunk, offset + (off_t)done);
		if (n == -1)
			err(1, "pwrite at offset %lld",
			    (long long)(offset + (off_t)done));
		if ((size_t)n != chunk)
			errx(1, "short pwrite: %zd of %zu", n, chunk);
	}
}

static void
check_pattern_fd (int fd, off_t offset, size_t len, unsigned int seed)
{
	unsigned char actual[IO_CHUNK], expected[IO_CHUNK];
	size_t done, chunk;
	ssize_t n;

	for (done = 0; done < len; done += chunk) {
		chunk = len - done;
		if (chunk > sizeof(actual))
			chunk = sizeof(actual);
		n = pread(fd, actual, chunk, offset + (off_t)done);
		if (n == -1)
			err(1, "pread");
		if ((size_t)n != chunk)
			errx(1, "short pread: %zd of %zu", n, chunk);
		fill_pattern(expected, chunk, offset + (off_t)done, seed);
		if (memcmp(actual, expected, chunk) != 0)
			errx(1, "data mismatch at offset %lld",
			    (long long)(offset + (off_t)done));
	}
}

static void
check_zero_fd (int fd, off_t offset, size_t len)
{
	unsigned char buf[IO_CHUNK];
	size_t done, chunk, i;
	ssize_t n;

	for (done = 0; done < len; done += chunk) {
		chunk = len - done;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);
		n = pread(fd, buf, chunk, offset + (off_t)done);
		if (n == -1)
			err(1, "pread zero range");
		if ((size_t)n != chunk)
			errx(1, "short zero-range pread: %zd of %zu", n, chunk);
		for (i = 0; i < chunk; i++) {
			if (buf[i] != 0)
				errx(1, "non-zero hole byte at offset %lld",
				    (long long)(offset + (off_t)done +
				    (off_t)i));
		}
	}
}

static void
write_text_file (const char *path, const char *text)
{
	size_t len;
	ssize_t n;
	int fd;

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	len = strlen(text);
	n = write(fd, text, len);
	if (n == -1)
		err(1, "write %s", path);
	if ((size_t)n != len)
		errx(1, "short write to %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
check_text_file (const char *path, const char *text)
{
	char buf[128];
	size_t len;
	ssize_t n;
	int fd;

	len = strlen(text);
	if (len >= sizeof(buf))
		errx(1, "test text too long");
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", path);
	n = read(fd, buf, sizeof(buf));
	if (n == -1)
		err(1, "read %s", path);
	if ((size_t)n != len || memcmp(buf, text, len) != 0)
		errx(1, "text mismatch in %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
check_absent (const char *path)
{
	struct stat st;

	errno = 0;
	if (lstat(path, &st) != -1 || errno != ENOENT)
		errx(1, "%s unexpectedly exists", path);
}

static void
check_directory (const char *path)
{
	struct stat st;

	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (!S_ISDIR(st.st_mode))
		errx(1, "%s is not a directory", path);
}

static void
check_regular (const char *path)
{
	struct stat st;

	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (!S_ISREG(st.st_mode))
		errx(1, "%s is not a regular file", path);
}

static void
check_symlink (const char *path, const char *target)
{
	char buf[PATH_MAX];
	struct stat st;
	blkcnt_t blocks;
	ssize_t n;
	size_t target_len;

	target_len = strlen(target);
	if (lstat(path, &st) == -1)
		err(1, "lstat %s", path);
	blocks = target_len <= FAST_SYMLINK_BYTES ? 0 :
	    (blkcnt_t)(block_size / 512);
	if (!S_ISLNK(st.st_mode) || st.st_size != (off_t)target_len ||
	    st.st_blocks != blocks)
		errx(1, "symlink inode shape mismatch for %s", path);
	n = readlink(path, buf, sizeof(buf));
	if (n == -1)
		err(1, "readlink %s", path);
	if ((size_t)n != target_len ||
	    memcmp(buf, target, (size_t)n) != 0)
		errx(1, "symlink target mismatch for %s", path);
}

static void
check_data_file (const char *path, int mutated)
{
	struct stat st;
	off_t base, append, overwrite;
	int fd;

	base = (off_t)(3 * block_size + 257);
	append = 73;
	overwrite = (off_t)block_size - 17;
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (!S_ISREG(st.st_mode) || st.st_size != base + append)
		errx(1, "wrong type or size for %s", path);
	if ((st.st_mode & 07777) != (mutated ? 0604 : 0640))
		errx(1, "wrong mode for %s: %04o", path,
		    st.st_mode & 07777);
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", path);
	if (mutated) {
		check_pattern_fd(fd, 0, (size_t)overwrite, DATA_SEED);
		check_pattern_fd(fd, overwrite, 91, OVERWRITE_SEED);
		check_pattern_fd(fd, overwrite + 91,
		    (size_t)(base - overwrite - 91), DATA_SEED);
	} else {
		check_pattern_fd(fd, 0, (size_t)base, DATA_SEED);
	}
	check_pattern_fd(fd, base, (size_t)append, APPEND_SEED);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
check_sparse_file (const char *path, int mutated)
{
	struct stat st;
	off_t marker;
	int fd;

	marker = (off_t)(8 * block_size + 31);
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", path);
	if (!mutated) {
		if (st.st_size != marker + 47)
			errx(1, "wrong sparse-file size");
		check_zero_fd(fd, 0, (size_t)marker);
		check_pattern_fd(fd, marker, 47, 0xd5U);
	} else {
		if (st.st_size != (off_t)(5 * block_size + 9))
			errx(1, "wrong regrown sparse-file size");
		check_zero_fd(fd, 0, (size_t)st.st_size);
	}
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
check_large_sparse (const char *path, int mutated)
{
	struct stat st;
	off_t marker;
	int fd;

	marker = mutated ? ((off_t)3 * 1024 * 1024 * 1024 + 211) :
	    ((off_t)5 * 1024 * 1024 * 1024 + 123);
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (st.st_size != marker + 17)
		errx(1, "wrong large sparse-file size: %lld",
		    (long long)st.st_size);
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", path);
	check_zero_fd(fd, marker - 31, 31);
	check_pattern_fd(fd, marker, 17, mutated ? 0xf6U : 0xe5U);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
check_extent_file (const char *path)
{
	struct stat st;
	off_t offset;
	int fd, i;

	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (st.st_size != (off_t)((EXTENT_WRITES * 2 - 1) * block_size))
		errx(1, "wrong extent test size");
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", path);
	for (i = 0; i < EXTENT_WRITES; i++) {
		offset = (off_t)i * 2 * (off_t)block_size;
		check_pattern_fd(fd, offset, block_size, EXTENT_SEED + i);
		if (i != EXTENT_WRITES - 1)
			check_zero_fd(fd, offset + (off_t)block_size,
			    block_size);
	}
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
check_shrunk_extent_file (const char *path)
{
	struct stat st;
	off_t offset;
	int fd, i;

	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (st.st_size != (off_t)(SHRINK_EXTENT_LBN + 1) *
	    (off_t)block_size)
		errx(1, "wrong shrunk extent-file size");
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", path);
	for (i = 0; i < SHRINK_EXTENT_LBN / 2; i++) {
		offset = (off_t)i * 2 * (off_t)block_size;
		check_pattern_fd(fd, offset, block_size, EXTENT_SEED + i);
		check_zero_fd(fd, offset + (off_t)block_size, block_size);
	}
	offset = (off_t)SHRINK_EXTENT_LBN * (off_t)block_size;
	check_pattern_fd(fd, offset, SHRINK_EXTENT_BYTES,
	    EXTENT_SEED + SHRINK_EXTENT_LBN / 2);
	check_zero_fd(fd, offset + SHRINK_EXTENT_BYTES,
	    block_size - SHRINK_EXTENT_BYTES);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
check_empty_file (const char *path)
{
	struct stat st;

	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (!S_ISREG(st.st_mode) || st.st_size != 0 || st.st_blocks != 0)
		errx(1, "%s retained data or allocated blocks", path);
}

static void
create_link_growth_tree (void)
{
	struct stat st;
	char directory[PATH_MAX], linkpath[PATH_MAX], name[256];
	char suffix[320], target[PATH_MAX];
	size_t consume, namelen, record;
	int fd, index, n;

	make_path(directory, sizeof(directory), "link-grow");
	if (mkdir(directory, 0755) == -1)
		err(1, "mkdir %s", directory);
	make_path(target, sizeof(target), "link-target");
	write_text_file(target, "journaled-link-data");

	/*
	 * The regression images use metadata_csum.  Leave eight bytes of slack
	 * in the first linear directory block, less than any valid dirent, so
	 * the hard link below must allocate the next block itself.
	 */
	consume = block_size - DIR_TAIL_BYTES - 24 - 8;
	index = 0;
	while (consume != 0) {
		record = consume > 264 ? 264 : consume;
		if (consume > record && consume - record < 12)
			record -= 12 - (consume - record);
		if (record < 12 || (record & 3) != 0)
			errx(1, "invalid directory packing record");
		namelen = record == 264 ? 255 : record - 8;
		memset(name, 'f', namelen);
		n = snprintf(name, namelen + 1, "%03d-", index);
		if (n < 0 || (size_t)n >= namelen)
			errx(1, "directory filler name too short");
		memset(name + n, 'f', namelen - (size_t)n);
		name[namelen] = '\0';
		n = snprintf(suffix, sizeof(suffix), "link-grow/%s", name);
		if (n < 0 || (size_t)n >= sizeof(suffix))
			errx(1, "directory filler path too long");
		make_path(linkpath, sizeof(linkpath), suffix);
		fd = open(linkpath, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd == -1)
			err(1, "open %s", linkpath);
		if (close(fd) == -1)
			err(1, "close %s", linkpath);
		consume -= record;
		index++;
	}
	if (stat(directory, &st) == -1)
		err(1, "stat %s", directory);
	if (st.st_size != (off_t)block_size)
		errx(1, "directory packing unexpectedly grew the directory");
	make_path(linkpath, sizeof(linkpath),
	    "link-grow/journal-growth-link");
	if (link(target, linkpath) == -1)
		err(1, "link %s", linkpath);
	if (stat(directory, &st) == -1)
		err(1, "stat %s", directory);
	if (st.st_size != (off_t)(2 * block_size))
		errx(1, "hard link did not grow its directory");
}

static void
check_link_growth_tree (int removed)
{
	struct stat directory_st, link_st, target_st;
	char directory[PATH_MAX], linkpath[PATH_MAX], target[PATH_MAX];

	make_path(directory, sizeof(directory), "link-grow");
	make_path(target, sizeof(target), "link-target");
	make_path(linkpath, sizeof(linkpath),
	    "link-grow/journal-growth-link");
	if (stat(directory, &directory_st) == -1)
		err(1, "stat %s", directory);
	if (!S_ISDIR(directory_st.st_mode) ||
	    directory_st.st_size != (off_t)(2 * block_size))
		errx(1, "link-growth directory has wrong type or size");
	if (stat(target, &target_st) == -1)
		err(1, "stat %s", target);
	if (removed) {
		check_absent(linkpath);
		if (target_st.st_nlink != 1)
			errx(1, "journaled link removal count mismatch");
	} else {
		if (stat(linkpath, &link_st) == -1)
			err(1, "stat %s", linkpath);
		if (target_st.st_ino != link_st.st_ino ||
		    target_st.st_nlink != 2 || link_st.st_nlink != 2)
			errx(1, "journaled growth link identity or count mismatch");
		check_text_file(linkpath, "journaled-link-data");
	}
	check_text_file(target, "journaled-link-data");
}

static void
create_extent_file (const char *path)
{
	int fd, i;

	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	for (i = 0; i < EXTENT_WRITES; i++)
		write_pattern_fd(fd, (off_t)i * 2 * (off_t)block_size,
		    block_size, EXTENT_SEED + i);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
check_grow_directory (const char *path, int mutated)
{
	struct dirent *de;
	DIR *dir;
	int count, expected;

	dir = opendir(path);
	if (dir == NULL)
		err(1, "opendir %s", path);
	count = 0;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		if (de->d_type != DT_REG)
			errx(1, "wrong directory-entry type for %s", de->d_name);
		count++;
	}
	if (closedir(dir) == -1)
		err(1, "closedir %s", path);
	expected = mutated ? GROW_FILES / 2 + REFILL_FILES : GROW_FILES;
	if (count != expected)
		errx(1, "%s contains %d entries, expected %d", path, count,
		    expected);
}

static void
fsync_path (const char *path)
{
	int fd;

	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open for fsync %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
expect_ro_failure (const char *operation, int result)
{
	if (result != -1)
		errx(1, "%s unexpectedly succeeded on a read-only mount",
		    operation);
	if (errno != EROFS)
		errx(1, "%s failed with %s instead of EROFS", operation,
		    strerror(errno));
}

static void
create_unix_socket (const char *path)
{
	struct sockaddr_un sun;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
		err(1, "socket");
	memset(&sun, 0, sizeof(sun));
	sun.sun_len = sizeof(sun);
	sun.sun_family = AF_UNIX;
	if (strlcpy(sun.sun_path, path, sizeof(sun.sun_path)) >=
	    sizeof(sun.sun_path))
		errx(1, "UNIX socket path too long: %s", path);
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) == -1)
		err(1, "bind %s", path);
	if (close(fd) == -1)
		err(1, "close UNIX socket");
}

static void
create_special_files (void)
{
	struct statfs before, after;
	char path[PATH_MAX];

	if (statfs(root, &before) == -1)
		err(1, "statfs before special-file creation");
	make_path(path, sizeof(path), "char-device");
	if (mknod(path, S_IFCHR | 0600, makedev(1, 7)) == -1)
		err(1, "mknod %s", path);
	make_path(path, sizeof(path), "block-device");
	if (mknod(path, S_IFBLK | 0600, makedev(2, 3)) == -1)
		err(1, "mknod %s", path);
	make_path(path, sizeof(path), "fifo");
	if (mkfifo(path, 0600) == -1)
		err(1, "mkfifo %s", path);
	make_path(path, sizeof(path), "unix-socket");
	create_unix_socket(path);
	if (statfs(root, &after) == -1)
		err(1, "statfs after special-file creation");
	if (after.f_ffree != before.f_ffree - 4)
		errx(1, "special-file creation did not allocate four inodes");
	if (after.f_bfree != before.f_bfree)
		errx(1, "blockless special-file creation allocated a block");
	fsync_path(root);
}

static void
verify_special_files (void)
{
	struct stat st;
	char path[PATH_MAX];

	make_path(path, sizeof(path), "fifo");
	if (lstat(path, &st) == -1)
		err(1, "lstat %s", path);
	if (!S_ISFIFO(st.st_mode))
		errx(1, "%s is not a fifo", path);
	make_path(path, sizeof(path), "char-device");
	if (lstat(path, &st) == -1)
		err(1, "lstat %s", path);
	if (!S_ISCHR(st.st_mode))
		errx(1, "%s is not a character device", path);
	if (st.st_rdev != makedev(1, 7))
		errx(1, "%s has wrong device number", path);
	make_path(path, sizeof(path), "block-device");
	if (lstat(path, &st) == -1)
		err(1, "lstat %s", path);
	if (!S_ISBLK(st.st_mode))
		errx(1, "%s is not a block device", path);
	if (st.st_rdev != makedev(2, 3))
		errx(1, "%s has wrong device number", path);
	make_path(path, sizeof(path), "unix-socket");
	if (lstat(path, &st) == -1)
		err(1, "lstat %s", path);
	if (!S_ISSOCK(st.st_mode))
		errx(1, "%s is not a socket", path);
}

static void
remove_special_files (void)
{
	struct statfs before, after;
	char path[PATH_MAX];
	const char *names[] = {
		"char-device",
		"block-device",
		"fifo",
		"unix-socket",
	};
	size_t i;

	if (statfs(root, &before) == -1)
		err(1, "statfs before special-file removal");
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		make_path(path, sizeof(path), names[i]);
		if (unlink(path) == -1)
			err(1, "unlink %s", path);
	}
	if (statfs(root, &after) == -1)
		err(1, "statfs after special-file removal");
	if (after.f_ffree != before.f_ffree + 4)
		errx(1, "special-file removal did not free four inodes");
	if (after.f_bfree != before.f_bfree)
		errx(1, "blockless special-file removal freed a block");
	fsync_path(root);
}

static void
create_grow_directory (void)
{
	char path[PATH_MAX];
	int i;

	make_path(path, sizeof(path), "growdir");
	if (mkdir(path, 0755) == -1)
		err(1, "mkdir %s", path);
	for (i = 0; i < GROW_FILES; i++) {
		make_indexed_path(path, sizeof(path), "growdir", i);
		write_text_file(path, "");
	}
	make_path(path, sizeof(path), "growdir");
	fsync_path(path);
	fsync_path(root);
}

static void
create_filesystem_tree (void)
{
	struct statfs before, after;
	char path[PATH_MAX], other[PATH_MAX], longname[256], toolong[257];
	char slow_target[97];
	off_t base, marker;
	unsigned char append_buf[73];
	int fd;
	ssize_t n;

	if (statfs(root, &before) == -1 && errno != ENOENT)
		err(1, "statfs %s", root);
	if (mkdir(root, 0755) == -1)
		err(1, "mkdir %s", root);
	if (statfs(root, &before) == -1)
		err(1, "statfs %s", root);

	make_path(path, sizeof(path), "a");
	if (mkdir(path, 0755) == -1)
		err(1, "mkdir %s", path);
	make_path(path, sizeof(path), "b");
	if (mkdir(path, 0755) == -1)
		err(1, "mkdir %s", path);
	make_path(path, sizeof(path), "a/nested");
	if (mkdir(path, 0711) == -1)
		err(1, "mkdir %s", path);
	make_path(path, sizeof(path), "a/nested/leaf");
	write_text_file(path, "nested-data");
	make_path(path, sizeof(path), "a/same-old");
	write_text_file(path, "same-directory-rename");

	make_path(path, sizeof(path), "data");
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0640);
	if (fd == -1)
		err(1, "open %s", path);
	base = (off_t)(3 * block_size + 257);
	write_pattern_fd(fd, 0, (size_t)base, DATA_SEED);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
	fd = open(path, O_WRONLY | O_APPEND);
	if (fd == -1)
		err(1, "open append %s", path);
	fill_pattern(append_buf, sizeof(append_buf), base, APPEND_SEED);
	n = write(fd, append_buf, sizeof(append_buf));
	if (n == -1)
		err(1, "append %s", path);
	if ((size_t)n != sizeof(append_buf))
		errx(1, "short append to %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync append %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
	make_path(other, sizeof(other), "data.link");
	if (link(path, other) == -1)
		err(1, "link %s", other);

	make_path(path, sizeof(path), "sparse");
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	marker = (off_t)(8 * block_size + 31);
	write_pattern_fd(fd, marker, 47, 0xd5U);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);

	make_path(path, sizeof(path), "large-sparse");
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	marker = (off_t)5 * 1024 * 1024 * 1024 + 123;
	write_pattern_fd(fd, marker, 17, 0xe5U);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);

	make_path(path, sizeof(path), "extents");
	create_extent_file(path);
	make_path(path, sizeof(path), "free-extents");
	create_extent_file(path);
	make_path(path, sizeof(path), "shrink-extents");
	create_extent_file(path);
	create_link_growth_tree();

	make_path(path, sizeof(path), "empty");
	write_text_file(path, "");
	create_special_files();
	make_path(path, sizeof(path), "fast-link");
	if (symlink("data", path) == -1)
		err(1, "symlink %s", path);
	memset(slow_target, 's', sizeof(slow_target) - 1);
	slow_target[sizeof(slow_target) - 1] = '\0';
	make_path(path, sizeof(path), "slow-link");
	if (symlink(slow_target, path) == -1)
		err(1, "symlink %s", path);
	make_path(path, sizeof(path), "xattr-unique");
	write_text_file(path, "");
	make_path(path, sizeof(path), "xattr-shared-a");
	write_text_file(path, "");
	make_path(path, sizeof(path), "xattr-shared-b");
	write_text_file(path, "");

	memset(longname, 'n', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	make_path(path, sizeof(path), longname);
	write_text_file(path, "max-name");
	memset(toolong, 'x', sizeof(toolong) - 1);
	toolong[sizeof(toolong) - 1] = '\0';
	make_path(path, sizeof(path), toolong);
	errno = 0;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd != -1 || errno != ENAMETOOLONG)
		errx(1, "256-byte component did not fail with ENAMETOOLONG");

	fsync_path(root);
	if (statfs(root, &after) == -1)
		err(1, "statfs %s", root);
	if (after.f_bfree >= before.f_bfree || after.f_ffree >= before.f_ffree)
		errx(1, "free-space counters did not decrease after creation");
}

static void
verify_created_tree (void)
{
	struct stat first, second;
	char path[PATH_MAX], other[PATH_MAX], longname[256];
	char slow_target[97];

	check_directory(root);
	make_path(path, sizeof(path), "a");
	check_directory(path);
	make_path(path, sizeof(path), "b");
	check_directory(path);
	make_path(path, sizeof(path), "a/nested/leaf");
	check_text_file(path, "nested-data");
	make_path(path, sizeof(path), "a/same-old");
	check_text_file(path, "same-directory-rename");

	make_path(path, sizeof(path), "data");
	check_data_file(path, 0);
	if (stat(path, &first) == -1)
		err(1, "stat %s", path);
	make_path(other, sizeof(other), "data.link");
	if (stat(other, &second) == -1)
		err(1, "stat %s", other);
	if (first.st_ino != second.st_ino || first.st_nlink != 2 ||
	    second.st_nlink != 2)
		errx(1, "hard-link identity or count mismatch");

	make_path(path, sizeof(path), "sparse");
	check_sparse_file(path, 0);
	make_path(path, sizeof(path), "large-sparse");
	check_large_sparse(path, 0);
	make_path(path, sizeof(path), "extents");
	check_extent_file(path);
	make_path(path, sizeof(path), "free-extents");
	check_extent_file(path);
	make_path(path, sizeof(path), "shrink-extents");
	check_extent_file(path);
	check_link_growth_tree(0);
	make_path(path, sizeof(path), "empty");
	check_regular(path);

	verify_special_files();
	make_path(path, sizeof(path), "fast-link");
	check_symlink(path, "data");
	memset(slow_target, 's', sizeof(slow_target) - 1);
	slow_target[sizeof(slow_target) - 1] = '\0';
	make_path(path, sizeof(path), "slow-link");
	check_symlink(path, slow_target);
	make_path(path, sizeof(path), "xattr-unique");
	check_regular(path);
	make_path(path, sizeof(path), "xattr-shared-a");
	check_regular(path);
	make_path(path, sizeof(path), "xattr-shared-b");
	check_regular(path);
	make_path(path, sizeof(path), "growdir");
	check_grow_directory(path, 0);

	memset(longname, 'n', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	make_path(path, sizeof(path), longname);
	check_text_file(path, "max-name");
}

static void
mutate_filesystem_tree (void)
{
	struct statfs before, after;
	struct statfs orphan_before, orphan_during, orphan_after;
	struct stat directory, parent_after, parent_before, parent_during;
	struct timeval times[2];
	char path[PATH_MAX], other[PATH_MAX];
	off_t marker, overwrite;
	int fd, fd2, i;

	/* Exercise journaled retirement with and without extent data. */
	if (statfs(root, &before) == -1)
		err(1, "statfs before non-regular unlink");
	make_path(path, sizeof(path), "fast-link");
	if (unlink(path) == -1)
		err(1, "unlink fast-link");
	make_path(path, sizeof(path), "fifo");
	if (unlink(path) == -1)
		err(1, "unlink fifo");
	make_path(path, sizeof(path), "char-device");
	if (unlink(path) == -1)
		err(1, "unlink char-device");
	make_path(path, sizeof(path), "block-device");
	if (unlink(path) == -1)
		err(1, "unlink block-device");
	make_path(path, sizeof(path), "unix-socket");
	if (unlink(path) == -1)
		err(1, "unlink unix-socket");
	make_path(path, sizeof(path), "slow-link");
	if (unlink(path) == -1)
		err(1, "unlink slow-link");
	if (statfs(root, &after) == -1)
		err(1, "statfs after non-regular unlink");
	if (after.f_ffree != before.f_ffree + 6)
		errx(1, "non-regular unlink did not free exactly six inodes");
	if (after.f_bfree != before.f_bfree + 1)
		errx(1, "slow symlink unlink did not free exactly one block");

	/* Release unique and shared external-xattr blocks exactly once. */
	if (statfs(root, &before) == -1)
		err(1, "statfs before unique xattr unlink");
	make_path(path, sizeof(path), "xattr-unique");
	if (unlink(path) == -1)
		err(1, "unlink xattr-unique");
	if (statfs(root, &after) == -1)
		err(1, "statfs after unique xattr unlink");
	if (after.f_ffree != before.f_ffree + 1 ||
	    after.f_bfree != before.f_bfree + 1)
		errx(1, "unique xattr unlink accounting mismatch");
	before = after;
	make_path(path, sizeof(path), "xattr-shared-a");
	if (unlink(path) == -1)
		err(1, "unlink xattr-shared-a");
	if (statfs(root, &after) == -1)
		err(1, "statfs after first shared xattr unlink");
	if (after.f_ffree != before.f_ffree + 1 ||
	    after.f_bfree != before.f_bfree)
		errx(1, "shared xattr was freed while still referenced");
	before = after;
	make_path(path, sizeof(path), "xattr-shared-b");
	if (unlink(path) == -1)
		err(1, "unlink xattr-shared-b");
	if (statfs(root, &after) == -1)
		err(1, "statfs after last shared xattr unlink");
	if (after.f_ffree != before.f_ffree + 1 ||
	    after.f_bfree != before.f_bfree + 1)
		errx(1, "last shared xattr unlink accounting mismatch");

	/* Exercise non-final unlink independently of rename replacement. */
	make_path(path, sizeof(path),
	    "link-grow/journal-growth-link");
	if (unlink(path) == -1)
		err(1, "unlink journal-growth-link");

	make_path(path, sizeof(path), "data");
	make_path(other, sizeof(other), "a/renamed");
	if (statfs(root, &before) == -1)
		err(1, "statfs before cross-directory file rename");
	if (rename(path, other) == -1)
		err(1, "rename data");
	if (statfs(root, &after) == -1)
		err(1, "statfs after cross-directory file rename");
	if (after.f_ffree != before.f_ffree ||
	    after.f_bfree != before.f_bfree)
		errx(1, "cross-directory file rename changed accounting");
	make_path(path, sizeof(path), "data.link");
	if (unlink(path) == -1)
		err(1, "unlink data.link");
	make_path(path, sizeof(path), "a/renamed");
	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);
	overwrite = (off_t)block_size - 17;
	write_pattern_fd(fd, overwrite, 91, OVERWRITE_SEED);
	if (fchmod(fd, 0604) == -1)
		err(1, "fchmod %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
	times[0].tv_sec = 1700000000;
	times[0].tv_usec = 123456;
	times[1].tv_sec = 1700000001;
	times[1].tv_usec = 654321;
	if (utimes(path, times) == -1)
		err(1, "utimes %s", path);

	make_path(path, sizeof(path), "a/replacement.new");
	write_text_file(path, "replacement-data");
	make_path(other, sizeof(other), "b/replaced");
	write_text_file(other, "obsolete-data");
	if (statfs(root, &before) == -1)
		err(1, "statfs before replacement rename");
	if (rename(path, other) == -1)
		err(1, "replacement rename");
	if (statfs(root, &after) == -1)
		err(1, "statfs after replacement rename");
	if (after.f_ffree != before.f_ffree + 1 ||
	    after.f_bfree != before.f_bfree + 1)
		errx(1, "replacement rename retirement accounting mismatch");
	check_absent(path);
	check_text_file(other, "replacement-data");

	/* Replacing one of several target names must not orphan its inode. */
	make_path(path, sizeof(path), "a/multilink-source");
	write_text_file(path, "new-multilink-data");
	make_path(other, sizeof(other), "b/multilink-target");
	write_text_file(other, "old-multilink-data");
	make_path(path, sizeof(path), "b/multilink-alias");
	if (link(other, path) == -1)
		err(1, "link multilink replacement target");
	if (statfs(root, &before) == -1)
		err(1, "statfs before multilink replacement rename");
	make_path(path, sizeof(path), "a/multilink-source");
	if (rename(path, other) == -1)
		err(1, "multilink replacement rename");
	if (statfs(root, &after) == -1)
		err(1, "statfs after multilink replacement rename");
	if (after.f_ffree != before.f_ffree ||
	    after.f_bfree != before.f_bfree)
		errx(1, "multilink replacement rename changed accounting");
	check_absent(path);
	check_text_file(other, "new-multilink-data");
	make_path(path, sizeof(path), "b/multilink-alias");
	check_text_file(path, "old-multilink-data");

	/* POSIX same-inode rename is a no-op which preserves both names. */
	make_path(path, sizeof(path), "a/same-inode-source");
	write_text_file(path, "same-inode-rename");
	make_path(other, sizeof(other), "b/same-inode-target");
	if (link(path, other) == -1)
		err(1, "link same-inode rename target");
	if (statfs(root, &before) == -1)
		err(1, "statfs before same-inode rename");
	if (rename(path, other) == -1)
		err(1, "same-inode rename");
	if (statfs(root, &after) == -1)
		err(1, "statfs after same-inode rename");
	if (after.f_ffree != before.f_ffree ||
	    after.f_bfree != before.f_bfree)
		errx(1, "same-inode rename changed accounting");
	if (stat(path, &parent_before) == -1 ||
	    stat(other, &parent_after) == -1)
		err(1, "stat same-inode rename names");
	if (parent_before.st_ino != parent_after.st_ino ||
	    parent_before.st_nlink != 2 || parent_after.st_nlink != 2)
		errx(1, "same-inode rename changed link identity");

	make_path(path, sizeof(path), "a/same-old");
	make_path(other, sizeof(other), "a/same-new");
	if (statfs(root, &before) == -1)
		err(1, "statfs before same-directory rename");
	if (rename(path, other) == -1)
		err(1, "same-directory rename");
	if (statfs(root, &after) == -1)
		err(1, "statfs after same-directory rename");
	if (after.f_ffree != before.f_ffree ||
	    after.f_bfree != before.f_bfree)
		errx(1, "same-directory rename changed accounting");

	make_path(path, sizeof(path), "a/move");
	if (mkdir(path, 0750) == -1)
		err(1, "mkdir %s", path);
	make_path(path, sizeof(path), "a/move/child");
	write_text_file(path, "moved-child");
	make_path(path, sizeof(path), "a/move");
	make_path(other, sizeof(other), "b/moved");
	make_path(path, sizeof(path), "a");
	if (stat(path, &parent_before) == -1)
		err(1, "stat old parent before directory rename");
	make_path(path, sizeof(path), "b");
	if (stat(path, &parent_during) == -1)
		err(1, "stat new parent before directory rename");
	if (statfs(root, &before) == -1)
		err(1, "statfs before cross-directory rename");
	make_path(path, sizeof(path), "a/move");
	if (rename(path, other) == -1)
		err(1, "cross-directory rename");
	if (statfs(root, &after) == -1)
		err(1, "statfs after cross-directory rename");
	if (after.f_ffree != before.f_ffree ||
	    after.f_bfree != before.f_bfree)
		errx(1, "cross-directory rename changed accounting");
	make_path(path, sizeof(path), "a");
	if (stat(path, &parent_after) == -1)
		err(1, "stat old parent after directory rename");
	if (parent_after.st_nlink != parent_before.st_nlink - 1)
		errx(1, "directory rename did not decrement old parent links");
	make_path(path, sizeof(path), "b");
	if (stat(path, &parent_after) == -1)
		err(1, "stat new parent after directory rename");
	if (parent_after.st_nlink != parent_during.st_nlink + 1)
		errx(1, "directory rename did not increment new parent links");

	/* Same-parent directory replacement retires exactly one directory. */
	make_path(path, sizeof(path), "a/dir-replace-source");
	if (mkdir(path, 0710) == -1)
		err(1, "mkdir %s", path);
	make_path(path, sizeof(path), "a/dir-replace-source/child");
	write_text_file(path, "same-parent-directory-replacement");
	make_path(path, sizeof(path), "a/dir-replace-target");
	if (mkdir(path, 0700) == -1)
		err(1, "mkdir %s", path);
	make_path(path, sizeof(path), "a");
	if (stat(path, &parent_before) == -1)
		err(1, "stat parent before directory replacement");
	if (statfs(root, &before) == -1)
		err(1, "statfs before directory replacement");
	make_path(path, sizeof(path), "a/dir-replace-source");
	make_path(other, sizeof(other), "a/dir-replace-target");
	if (rename(path, other) == -1)
		err(1, "same-parent directory replacement");
	if (statfs(root, &after) == -1)
		err(1, "statfs after directory replacement");
	if (after.f_ffree != before.f_ffree + 1 ||
	    after.f_bfree != before.f_bfree + 1)
		errx(1, "directory replacement retirement accounting mismatch");
	make_path(path, sizeof(path), "a");
	if (stat(path, &parent_after) == -1)
		err(1, "stat parent after directory replacement");
	if (parent_after.st_nlink != parent_before.st_nlink - 1)
		errx(1, "directory replacement parent link mismatch");
	make_path(path, sizeof(path), "a/dir-replace-source");
	check_absent(path);
	make_path(path, sizeof(path), "a/dir-replace-target/child");
	check_text_file(path, "same-parent-directory-replacement");

	/* Cross-parent replacement changes '..' but not the new parent's links. */
	make_path(path, sizeof(path), "a/cross-replace-source");
	if (mkdir(path, 0751) == -1)
		err(1, "mkdir %s", path);
	make_path(path, sizeof(path), "a/cross-replace-source/child");
	write_text_file(path, "cross-parent-directory-replacement");
	make_path(path, sizeof(path), "b/cross-replace-target");
	if (mkdir(path, 0701) == -1)
		err(1, "mkdir %s", path);
	make_path(path, sizeof(path), "a");
	if (stat(path, &parent_before) == -1)
		err(1, "stat old parent before cross replacement");
	make_path(path, sizeof(path), "b");
	if (stat(path, &parent_during) == -1)
		err(1, "stat new parent before cross replacement");
	if (statfs(root, &before) == -1)
		err(1, "statfs before cross-parent directory replacement");
	make_path(path, sizeof(path), "a/cross-replace-source");
	make_path(other, sizeof(other), "b/cross-replace-target");
	if (rename(path, other) == -1)
		err(1, "cross-parent directory replacement");
	if (statfs(root, &after) == -1)
		err(1, "statfs after cross-parent directory replacement");
	if (after.f_ffree != before.f_ffree + 1 ||
	    after.f_bfree != before.f_bfree + 1)
		errx(1, "cross-parent replacement accounting mismatch");
	make_path(path, sizeof(path), "a");
	if (stat(path, &parent_after) == -1)
		err(1, "stat old parent after cross replacement");
	if (parent_after.st_nlink != parent_before.st_nlink - 1)
		errx(1, "cross replacement old-parent link mismatch");
	make_path(path, sizeof(path), "b");
	if (stat(path, &parent_after) == -1)
		err(1, "stat new parent after cross replacement");
	if (parent_after.st_nlink != parent_during.st_nlink)
		errx(1, "cross replacement new-parent link mismatch");
	make_path(path, sizeof(path), "a/cross-replace-source");
	check_absent(path);
	make_path(path, sizeof(path), "b/cross-replace-target/child");
	check_text_file(path, "cross-parent-directory-replacement");

	/* A non-empty target and an ancestor move must leave both trees intact. */
	make_path(path, sizeof(path), "a/rejected-directory");
	if (mkdir(path, 0755) == -1)
		err(1, "mkdir %s", path);
	make_path(other, sizeof(other), "b/cross-replace-target");
	errno = 0;
	if (rename(path, other) != -1 || errno != ENOTEMPTY)
		errx(1, "non-empty directory replacement did not fail");
	check_directory(path);
	make_path(other, sizeof(other), "b/cross-replace-target/child");
	check_text_file(other, "cross-parent-directory-replacement");
	if (rmdir(path) == -1)
		err(1, "rmdir %s", path);

	make_path(path, sizeof(path), "b");
	make_path(other, sizeof(other), "b/moved/loop");
	errno = 0;
	if (rename(path, other) != -1 || errno != EINVAL)
		errx(1, "directory-loop rename did not fail with EINVAL");
	make_path(path, sizeof(path), "b/moved");
	errno = 0;
	if (rmdir(path) != -1 || errno != ENOTEMPTY)
		errx(1, "rmdir of non-empty directory did not fail with ENOTEMPTY");

	make_path(path, sizeof(path), "open-unlinked");
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	write_pattern_fd(fd, 0, block_size + 19, 0xc5U);
	if (statfs(root, &orphan_before) == -1)
		err(1, "statfs before open-file unlink");
	if (unlink(path) == -1)
		err(1, "unlink open file");
	if (statfs(root, &orphan_during) == -1)
		err(1, "statfs during open-file unlink");
	if (orphan_during.f_ffree != orphan_before.f_ffree)
		errx(1, "open-file unlink freed its inode before last close");
	check_pattern_fd(fd, 0, block_size + 19, 0xc5U);
	if (fsync(fd) == -1)
		err(1, "fsync unlinked file");
	if (close(fd) == -1)
		err(1, "close unlinked file");
	check_absent(path);
	if (statfs(root, &orphan_after) == -1)
		err(1, "statfs after open-file close");
	if (orphan_after.f_ffree != orphan_before.f_ffree + 1)
		errx(1, "last close did not free exactly one orphan inode");
	make_path(path, sizeof(path), "orphan-reuse");
	write_text_file(path, "orphan-inode-reused");
	if (statfs(root, &orphan_during) == -1)
		err(1, "statfs after orphan inode reuse");
	if (orphan_during.f_ffree != orphan_before.f_ffree)
		errx(1, "freed orphan inode was not reusable exactly once");

	/* Close two unlinked files out of orphan insertion order. */
	make_path(path, sizeof(path), "orphan-order-a");
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	make_path(other, sizeof(other), "orphan-order-b");
	fd2 = open(other, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd2 == -1)
		err(1, "open %s", other);
	write_pattern_fd(fd, 0, block_size + 7, 0xd6U);
	write_pattern_fd(fd2, 0, block_size + 11, 0xe7U);
	if (statfs(root, &orphan_before) == -1)
		err(1, "statfs before ordered orphan closes");
	if (unlink(path) == -1)
		err(1, "unlink %s", path);
	if (unlink(other) == -1)
		err(1, "unlink %s", other);
	if (statfs(root, &orphan_during) == -1)
		err(1, "statfs during ordered orphan closes");
	if (orphan_during.f_ffree != orphan_before.f_ffree)
		errx(1, "multiple open-file unlinks freed an inode early");
	if (close(fd) == -1)
		err(1, "close first inserted orphan");
	if (statfs(root, &orphan_after) == -1)
		err(1, "statfs after non-head orphan close");
	if (orphan_after.f_ffree != orphan_before.f_ffree + 1)
		errx(1, "non-head orphan close did not free exactly one inode");
	check_pattern_fd(fd2, 0, block_size + 11, 0xe7U);
	if (close(fd2) == -1)
		err(1, "close second inserted orphan");
	if (statfs(root, &orphan_after) == -1)
		err(1, "statfs after final ordered orphan close");
	if (orphan_after.f_ffree != orphan_before.f_ffree + 2)
		errx(1, "ordered orphan closes did not free exactly two inodes");
	check_absent(path);
	check_absent(other);

	make_path(path, sizeof(path), "sparse");
	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);
	if (ftruncate(fd, (off_t)(2 * block_size + 13)) == -1)
		err(1, "shrink %s", path);
	if (ftruncate(fd, (off_t)(5 * block_size + 9)) == -1)
		err(1, "regrow %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);

	make_path(path, sizeof(path), "large-sparse");
	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);
	if (ftruncate(fd, 0) == -1)
		err(1, "truncate %s", path);
	marker = (off_t)3 * 1024 * 1024 * 1024 + 211;
	write_pattern_fd(fd, marker, 17, 0xf6U);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);

	/* Free all data extents and the external depth-1 extent block. */
	make_path(path, sizeof(path), "free-extents");
	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);
	if (ftruncate(fd, 0) == -1)
		err(1, "truncate %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);

	/* Keep the depth-1 tree while freeing its tail and zeroing partial EOF. */
	make_path(path, sizeof(path), "shrink-extents");
	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);
	marker = (off_t)SHRINK_EXTENT_LBN * (off_t)block_size +
	    SHRINK_EXTENT_BYTES;
	if (ftruncate(fd, marker) == -1)
		err(1, "shrink %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync shrunk %s", path);
	marker = (off_t)(SHRINK_EXTENT_LBN + 1) * (off_t)block_size;
	if (ftruncate(fd, marker) == -1)
		err(1, "regrow %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync regrown %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);

	if (statfs(root, &before) == -1)
		err(1, "statfs before directory reuse");
	for (i = 0; i < GROW_FILES; i += 2) {
		make_indexed_path(path, sizeof(path), "growdir", i);
		if (unlink(path) == -1)
			err(1, "unlink %s", path);
	}
	for (i = 0; i < REFILL_FILES; i++) {
		int n;
		char suffix[64];

		n = snprintf(suffix, sizeof(suffix), "growdir/refill-%03d", i);
		if (n < 0 || (size_t)n >= sizeof(suffix))
			errx(1, "refill suffix too long");
		make_path(path, sizeof(path), suffix);
		write_text_file(path, "");
	}
	if (statfs(root, &after) == -1)
		err(1, "statfs after directory reuse");
	if (after.f_ffree <= before.f_ffree)
		errx(1, "inode counter did not reflect directory-entry churn");

	if (statfs(root, &before) == -1)
		err(1, "statfs before mkdir");
	if (stat(root, &parent_before) == -1)
		err(1, "stat parent before mkdir");
	make_path(path, sizeof(path), "temporary-directory");
	if (mkdir(path, 0700) == -1)
		err(1, "mkdir %s", path);
	if (stat(path, &directory) == -1)
		err(1, "stat %s", path);
	if (!S_ISDIR(directory.st_mode) || directory.st_nlink != 2 ||
	    directory.st_size != (off_t)block_size ||
	    (directory.st_mode & 0777) != 0700)
		errx(1, "new directory metadata mismatch");
	if (statfs(root, &after) == -1)
		err(1, "statfs after mkdir");
	if (stat(root, &parent_during) == -1)
		err(1, "stat parent after mkdir");
	if (after.f_ffree + 1 != before.f_ffree ||
	    after.f_bfree + 1 != before.f_bfree)
		errx(1, "mkdir did not consume exactly one inode and one block");
	if (parent_during.st_nlink != parent_before.st_nlink + 1)
		errx(1, "mkdir did not increment the parent link count");
	if (rmdir(path) == -1)
		err(1, "rmdir %s", path);
	check_absent(path);
	if (statfs(root, &after) == -1)
		err(1, "statfs after rmdir");
	if (stat(root, &parent_after) == -1)
		err(1, "stat parent after rmdir");
	if (after.f_ffree != before.f_ffree ||
	    after.f_bfree != before.f_bfree)
		errx(1, "rmdir did not restore inode and block accounting");
	if (parent_after.st_nlink != parent_before.st_nlink)
		errx(1, "rmdir did not restore the parent link count");
	fsync_path(root);
}

static void
verify_final_tree (void)
{
	struct stat first, second, st;
	char path[PATH_MAX], other[PATH_MAX], longname[256];

	make_path(path, sizeof(path), "data");
	check_absent(path);
	make_path(path, sizeof(path), "data.link");
	check_absent(path);
	make_path(path, sizeof(path), "a/renamed");
	check_data_file(path, 1);
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (st.st_nlink != 1 || st.st_mtime != 1700000001)
		errx(1, "link count or timestamp mismatch for %s", path);

	make_path(path, sizeof(path), "b/replaced");
	check_text_file(path, "replacement-data");
	make_path(path, sizeof(path), "a/multilink-source");
	check_absent(path);
	make_path(path, sizeof(path), "b/multilink-target");
	check_text_file(path, "new-multilink-data");
	if (stat(path, &first) == -1)
		err(1, "stat %s", path);
	make_path(other, sizeof(other), "b/multilink-alias");
	check_text_file(other, "old-multilink-data");
	if (stat(other, &second) == -1)
		err(1, "stat %s", other);
	if (first.st_ino == second.st_ino || first.st_nlink != 1 ||
	    second.st_nlink != 1)
		errx(1, "multilink replacement identity mismatch");
	make_path(path, sizeof(path), "a/same-inode-source");
	make_path(other, sizeof(other), "b/same-inode-target");
	check_text_file(path, "same-inode-rename");
	check_text_file(other, "same-inode-rename");
	if (stat(path, &first) == -1 || stat(other, &second) == -1)
		err(1, "stat remounted same-inode rename names");
	if (first.st_ino != second.st_ino || first.st_nlink != 2 ||
	    second.st_nlink != 2)
		errx(1, "remounted same-inode rename identity mismatch");
	make_path(path, sizeof(path), "a/same-old");
	check_absent(path);
	make_path(path, sizeof(path), "a/same-new");
	check_text_file(path, "same-directory-rename");
	make_path(path, sizeof(path), "b/moved/child");
	check_text_file(path, "moved-child");
	make_path(path, sizeof(path), "b/replaced");
	if (stat(path, &first) == -1)
		err(1, "stat %s", path);
	make_path(other, sizeof(other), "b/moved/../replaced");
	if (stat(other, &second) == -1)
		err(1, "stat %s", other);
	if (first.st_ino != second.st_ino)
		errx(1, "moved directory has incorrect parent");
	make_path(path, sizeof(path), "a/dir-replace-source");
	check_absent(path);
	make_path(path, sizeof(path), "a/dir-replace-target/child");
	check_text_file(path, "same-parent-directory-replacement");
	make_path(path, sizeof(path), "a/dir-replace-target");
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (!S_ISDIR(st.st_mode) || (st.st_mode & 0777) != 0710)
		errx(1, "same-parent replacement lost source directory mode");
	make_path(path, sizeof(path), "a/cross-replace-source");
	check_absent(path);
	make_path(path, sizeof(path), "b/cross-replace-target/child");
	check_text_file(path, "cross-parent-directory-replacement");
	make_path(path, sizeof(path), "b/cross-replace-target");
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (!S_ISDIR(st.st_mode) || (st.st_mode & 0777) != 0751)
		errx(1, "cross-parent replacement lost source directory mode");
	make_path(path, sizeof(path), "b/replaced");
	if (stat(path, &first) == -1)
		err(1, "stat %s", path);
	make_path(other, sizeof(other),
	    "b/cross-replace-target/../replaced");
	if (stat(other, &second) == -1)
		err(1, "stat %s", other);
	if (first.st_ino != second.st_ino)
		errx(1, "replaced directory has incorrect parent");

	make_path(path, sizeof(path), "open-unlinked");
	check_absent(path);
	make_path(path, sizeof(path), "orphan-reuse");
	check_text_file(path, "orphan-inode-reused");
	make_path(path, sizeof(path), "orphan-order-a");
	check_absent(path);
	make_path(path, sizeof(path), "orphan-order-b");
	check_absent(path);
	make_path(path, sizeof(path), "temporary-directory");
	check_absent(path);
	make_path(path, sizeof(path), "sparse");
	check_sparse_file(path, 1);
	make_path(path, sizeof(path), "large-sparse");
	check_large_sparse(path, 1);
	make_path(path, sizeof(path), "extents");
	check_extent_file(path);
	make_path(path, sizeof(path), "free-extents");
	check_empty_file(path);
	make_path(path, sizeof(path), "shrink-extents");
	check_shrunk_extent_file(path);
	check_link_growth_tree(1);
	make_path(path, sizeof(path), "growdir");
	check_grow_directory(path, 1);

	make_path(path, sizeof(path), "fast-link");
	check_absent(path);
	make_path(path, sizeof(path), "slow-link");
	check_absent(path);
	make_path(path, sizeof(path), "fifo");
	check_absent(path);
	make_path(path, sizeof(path), "char-device");
	check_absent(path);
	make_path(path, sizeof(path), "block-device");
	check_absent(path);
	make_path(path, sizeof(path), "unix-socket");
	check_absent(path);
	make_path(path, sizeof(path), "xattr-unique");
	check_absent(path);
	make_path(path, sizeof(path), "xattr-shared-a");
	check_absent(path);
	make_path(path, sizeof(path), "xattr-shared-b");
	check_absent(path);

	memset(longname, 'n', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	make_path(path, sizeof(path), longname);
	check_text_file(path, "max-name");
}

static void
verify_readonly_tree (void)
{
	char path[PATH_MAX], other[PATH_MAX];
	int fd;

	verify_final_tree();
	make_path(path, sizeof(path), "a/renamed");
	errno = 0;
	fd = open(path, O_WRONLY);
	expect_ro_failure("open for write", fd);
	make_path(path, sizeof(path), "readonly-create");
	errno = 0;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	expect_ro_failure("create", fd);
	check_absent(path);
	make_path(path, sizeof(path), "readonly-directory");
	errno = 0;
	expect_ro_failure("mkdir", mkdir(path, 0700));
	check_absent(path);
	make_path(path, sizeof(path), "b/replaced");
	errno = 0;
	expect_ro_failure("unlink", unlink(path));
	check_text_file(path, "replacement-data");
	make_path(other, sizeof(other), "b/replaced-new");
	errno = 0;
	expect_ro_failure("rename", rename(path, other));
	check_absent(other);
	errno = 0;
	expect_ro_failure("chmod", chmod(path, 0600));
	errno = 0;
	expect_ro_failure("truncate", truncate(path, 0));
	check_text_file(path, "replacement-data");
}

static void
create_allocation_probe (void)
{
	char path[PATH_MAX];
	int fd;

	make_path(path, sizeof(path), "allocation-probe");
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	write_pattern_fd(fd, 0, block_size + 31, 0xa7U);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
verify_allocation_probe (void)
{
	char path[PATH_MAX];
	struct stat st;
	int fd;

	make_path(path, sizeof(path), "allocation-probe");
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (!S_ISREG(st.st_mode) || st.st_size != (off_t)block_size + 31)
		errx(1, "allocation probe has wrong type or size");
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", path);
	check_pattern_fd(fd, 0, block_size + 31, 0xa7U);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
allocate_bitmap_probe (void)
{
	struct statfs before, after;
	char path[PATH_MAX];
	int fd;

	if (statfs(root, &before) == -1)
		err(1, "statfs before block allocation");
	make_path(path, sizeof(path), "bitmap-probe");
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	write_pattern_fd(fd, 0, block_size, BITMAP_SEED);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
	if (statfs(root, &after) == -1)
		err(1, "statfs after block allocation");
	if (after.f_bfree + 1 != before.f_bfree ||
	    after.f_ffree + 1 != before.f_ffree)
		errx(1, "block allocation accounting mismatch");
	verify_bitmap_probe(1);
}

static void
verify_bitmap_probe (int allocated)
{
	struct stat st;
	char path[PATH_MAX];
	int fd;

	make_path(path, sizeof(path), "bitmap-probe");
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (!S_ISREG(st.st_mode))
		errx(1, "bitmap probe is not a regular file");
	if (!allocated) {
		if (st.st_size != 0 || st.st_blocks != 0)
			errx(1, "freed bitmap probe retains blocks");
		return;
	}
	if (st.st_size != (off_t)block_size ||
	    st.st_blocks != (blkcnt_t)(block_size / 512))
		errx(1, "allocated bitmap probe has wrong shape");
	fd = open(path, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", path);
	check_pattern_fd(fd, 0, block_size, BITMAP_SEED);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
free_bitmap_probe (void)
{
	struct statfs before, after;
	char path[PATH_MAX];
	int fd;

	verify_bitmap_probe(1);
	if (statfs(root, &before) == -1)
		err(1, "statfs before block free");
	make_path(path, sizeof(path), "bitmap-probe");
	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);
	if (ftruncate(fd, 0) == -1)
		err(1, "ftruncate %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
	if (statfs(root, &after) == -1)
		err(1, "statfs after block free");
	if (after.f_bfree != before.f_bfree + 1 ||
	    after.f_ffree != before.f_ffree)
		errx(1, "block free accounting mismatch");
	verify_bitmap_probe(0);
}

static void
reuse_bitmap_probe (void)
{
	struct statfs before, after;
	char path[PATH_MAX];
	int fd;

	verify_bitmap_probe(0);
	if (statfs(root, &before) == -1)
		err(1, "statfs before block reuse");
	make_path(path, sizeof(path), "bitmap-probe");
	fd = open(path, O_RDWR);
	if (fd == -1)
		err(1, "open %s", path);
	write_pattern_fd(fd, 0, block_size, BITMAP_SEED);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
	if (statfs(root, &after) == -1)
		err(1, "statfs after block reuse");
	if (after.f_bfree + 1 != before.f_bfree ||
	    after.f_ffree != before.f_ffree)
		errx(1, "block reuse accounting mismatch");
	verify_bitmap_probe(1);
}

static void
retire_bitmap_probe (void)
{
	struct statfs before, after;
	char path[PATH_MAX];

	verify_bitmap_probe(1);
	if (statfs(root, &before) == -1)
		err(1, "statfs before block retirement");
	make_path(path, sizeof(path), "bitmap-probe");
	if (unlink(path) == -1)
		err(1, "unlink %s", path);
	if (statfs(root, &after) == -1)
		err(1, "statfs after block retirement");
	if (after.f_bfree != before.f_bfree + 1 ||
	    after.f_ffree != before.f_ffree + 1)
		errx(1, "block retirement accounting mismatch");
	check_absent(path);
}

static void
allocate_inode_probe (void)
{
	struct statfs before, after;
	char path[PATH_MAX];
	int fd;

	if (statfs(root, &before) == -1)
		err(1, "statfs before inode allocation");
	make_path(path, sizeof(path), "inode-probe");
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
	if (statfs(root, &after) == -1)
		err(1, "statfs after inode allocation");
	if (after.f_bfree != before.f_bfree ||
	    after.f_ffree + 1 != before.f_ffree)
		errx(1, "inode allocation accounting mismatch");
	verify_inode_probe();
}

static void
verify_inode_probe (void)
{
	struct stat st;
	char path[PATH_MAX];

	make_path(path, sizeof(path), "inode-probe");
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (!S_ISREG(st.st_mode) || st.st_size != 0 ||
	    st.st_blocks != 0 || st.st_nlink != 1)
		errx(1, "inode probe has wrong shape");
}

static void
free_inode_probe (void)
{
	struct statfs before, after;
	char path[PATH_MAX];

	verify_inode_probe();
	if (statfs(root, &before) == -1)
		err(1, "statfs before inode free");
	make_path(path, sizeof(path), "inode-probe");
	if (unlink(path) == -1)
		err(1, "unlink %s", path);
	if (statfs(root, &after) == -1)
		err(1, "statfs after inode free");
	if (after.f_bfree != before.f_bfree ||
	    after.f_ffree != before.f_ffree + 1)
		errx(1, "inode free accounting mismatch");
	check_absent(path);
}

int
main (int argc, char **argv)
{
	struct statfs sfs;

	if (argc != 3)
		errx(1, "usage: ext4fsops mode root");
	if (strlcpy(root, argv[2], sizeof(root)) >= sizeof(root))
		errx(1, "root path too long");

	if (strcmp(argv[1], "create") == 0) {
		/* The root does not exist until this mode creates it. */
		block_size = 0;
		if (mkdir(root, 0755) == -1)
			err(1, "mkdir %s", root);
		if (statfs(root, &sfs) == -1)
			err(1, "statfs %s", root);
		if (rmdir(root) == -1)
			err(1, "rmdir %s", root);
		block_size = (size_t)sfs.f_bsize;
		create_filesystem_tree();
	} else if (strcmp(argv[1], "bitmap-allocate") == 0) {
		block_size = 0;
		if (mkdir(root, 0755) == -1)
			err(1, "mkdir %s", root);
		if (statfs(root, &sfs) == -1)
			err(1, "statfs %s", root);
		block_size = (size_t)sfs.f_bsize;
		allocate_bitmap_probe();
	} else if (strcmp(argv[1], "inode-allocate") == 0) {
		block_size = 0;
		if (mkdir(root, 0755) == -1)
			err(1, "mkdir %s", root);
		if (statfs(root, &sfs) == -1)
			err(1, "statfs %s", root);
		block_size = (size_t)sfs.f_bsize;
		allocate_inode_probe();
	} else if (strcmp(argv[1], "create-special") == 0) {
		block_size = 0;
		if (mkdir(root, 0755) == -1)
			err(1, "mkdir %s", root);
		if (statfs(root, &sfs) == -1)
			err(1, "statfs %s", root);
		block_size = (size_t)sfs.f_bsize;
		create_special_files();
	} else {
		if (statfs(root, &sfs) == -1)
			err(1, "statfs %s", root);
		block_size = (size_t)sfs.f_bsize;
		if (strcmp(argv[1], "verify-create") == 0)
			verify_created_tree();
		else if (strcmp(argv[1], "create-growdir") == 0)
			create_grow_directory();
		else if (strcmp(argv[1], "mutate") == 0)
			mutate_filesystem_tree();
		else if (strcmp(argv[1], "verify-final") == 0)
			verify_final_tree();
		else if (strcmp(argv[1], "verify-readonly") == 0)
			verify_readonly_tree();
		else if (strcmp(argv[1], "create-allocation-probe") == 0)
			create_allocation_probe();
		else if (strcmp(argv[1], "verify-allocation-probe") == 0)
			verify_allocation_probe();
		else if (strcmp(argv[1], "bitmap-verify") == 0)
			verify_bitmap_probe(1);
		else if (strcmp(argv[1], "bitmap-free") == 0)
			free_bitmap_probe();
		else if (strcmp(argv[1], "bitmap-verify-free") == 0)
			verify_bitmap_probe(0);
		else if (strcmp(argv[1], "bitmap-reuse") == 0)
			reuse_bitmap_probe();
		else if (strcmp(argv[1], "bitmap-retire") == 0)
			retire_bitmap_probe();
		else if (strcmp(argv[1],
		    "bitmap-verify-retired") == 0) {
			char path[PATH_MAX];

			make_path(path, sizeof(path), "bitmap-probe");
			check_absent(path);
		} else if (strcmp(argv[1], "inode-verify") == 0)
			verify_inode_probe();
		else if (strcmp(argv[1], "inode-free") == 0)
			free_inode_probe();
		else if (strcmp(argv[1], "inode-reuse") == 0)
			allocate_inode_probe();
		else if (strcmp(argv[1],
		    "inode-verify-retired") == 0) {
			char path[PATH_MAX];

			make_path(path, sizeof(path), "inode-probe");
			check_absent(path);
		} else if (strcmp(argv[1], "verify-special") == 0)
			verify_special_files();
		else if (strcmp(argv[1], "remove-special") == 0)
			remove_special_files();
		else
			errx(1, "unknown mode: %s", argv[1]);
	}

	if (block_size != 1024 && block_size != 2048 && block_size != 4096)
		errx(1, "unexpected filesystem block size: %zu", block_size);
	return (0);
}
