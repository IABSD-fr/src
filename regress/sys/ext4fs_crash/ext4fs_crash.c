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

#include <sys/types.h>
#include <sys/endian.h>
#include <sys/mount.h>
#include <sys/stat.h>

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

#define IO_BYTES	8192
#define OLD_BYTES	((off_t)(1024 * 1024 + 37))
#define NEW_BYTES	((off_t)(4 * 1024 * 1024 + 73))
#define OLD_SEED	0x35U
#define NEW_SEED	0xc9U
#define ARM_USEC	2000000
#define GATE_USEC	10000
#define GATE_PATH	"/tmp/ext4fs_crash.go"

#define DIR_ENTRY_LIMIT	128
#define DIR_ENTRY_NAME_LEN	200
#define GROW_DIR	"grow"
#define GROW_MARKER	"grow.marker"
#define PROBE_DIR	"grow.probe"
#define REMOVE_DIR	"removed"

#define EXTENT_FILE	"extent-probe"
#define EXTENT_ROOT_ENTRIES	4
#define EXTENT_NODE_HEADER_BYTES	12
#define EXTENT_NODE_ENTRY_BYTES	12
#define EXTENT_SPLIT_SEED	0x68U

#define JBD2_MAGIC		UINT32_C(0xc03b3998)
#define JBD2_SUPERBLOCK_V2	4
#define JBD2_SUPER_BYTES	256

#define JBD2_OFF_MAGIC		0x00
#define JBD2_OFF_BLOCKTYPE	0x04
#define JBD2_OFF_BLOCKSIZE	0x0c
#define JBD2_OFF_MAXLEN		0x10
#define JBD2_OFF_FIRST		0x14
#define JBD2_OFF_SEQUENCE	0x18
#define JBD2_OFF_START		0x1c
#define JBD2_OFF_HEAD		0x58

static void	make_path (char *, size_t, const char *, const char *);
static void	make_entry_name (char *, size_t, unsigned int);
static void	make_entry_path (char *, size_t, const char *,
    const char *, unsigned int);
static unsigned char pattern_byte (off_t, unsigned int);
static void	fill_pattern (unsigned char *, size_t, off_t,
    unsigned int);
static int	open_output (const char *, int);
static void	create_empty (const char *);
static unsigned int directory_entries (const char *);
static void	write_growth_marker (const char *, unsigned int, off_t,
    off_t);
static void	read_growth_marker (const char *, unsigned int *, off_t *,
    off_t *);
static size_t	filesystem_block_size (const char *);
static size_t	extent_leaf_capacity (size_t);
static off_t	extent_lbn_offset (size_t, size_t);
static void	read_extent_block (int, unsigned char *, size_t, off_t);
static void	check_extent_zero (const unsigned char *, size_t,
    const char *);
static int	check_extent_growth (const char *, size_t, size_t,
    size_t);
static void	write_extent_block (int, size_t, off_t);
static void	write_pattern (int, off_t, unsigned int);
static void	check_pattern (const char *, off_t, unsigned int);
static void	arm_cut (const char *);
static void	arm_boundary (const char *);
static void	make_old_pattern (const char *);
static uint32_t	load_be32 (const unsigned char *, size_t);
static void	store_be32 (unsigned char *, size_t, uint32_t);
static void	classify_journal (const char *);
static void	run_dir_growth (const char *, const char *, int);
static void	run_rmdir_open (const char *, const char *, int);
static void	run_extent_growth (const char *, const char *, int);
static void	run_workload (const char *, const char *, int);
static void	verify_dir_growth (const char *);
static void	verify_rmdir (const char *);
static void	verify_extent_promote (const char *);
static void	verify_extent_split (const char *);
static void	verify_workload (const char *);
static void	verify_unlink (const char *);
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

static void
make_entry_name (char *name, size_t namelen, unsigned int index)
{
	size_t i;
	int n;

	n = snprintf(name, namelen, "%06u-", index);
	if (n < 0 || (size_t)n >= namelen ||
	    DIR_ENTRY_NAME_LEN >= namelen ||
	    (size_t)n >= DIR_ENTRY_NAME_LEN)
		errx(1, "directory entry name is too long");
	for (i = (size_t)n; i < DIR_ENTRY_NAME_LEN; i++)
		name[i] = (char)('a' + i % 26);
	name[DIR_ENTRY_NAME_LEN] = '\0';
}

static void
make_entry_path (char *path, size_t pathlen, const char *root,
    const char *directory, unsigned int index)
{
	char name[NAME_MAX + 1];
	int n;

	make_entry_name(name, sizeof(name), index);
	n = snprintf(path, pathlen, "%s/%s/%s", root, directory,
	    name);
	if (n < 0 || (size_t)n >= pathlen)
		errx(1, "directory entry path is too long");
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
create_empty (const char *path)
{
	int fd;

	fd = open_output(path, 1);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static unsigned int
directory_entries (const char *path)
{
	struct dirent *entry;
	DIR *dir;
	unsigned int count;

	dir = opendir(path);
	if (dir == NULL)
		err(1, "opendir %s", path);
	count = 0;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;
		if (count == UINT_MAX)
			errx(1, "%s has too many entries", path);
		count++;
	}
	if (closedir(dir) == -1)
		err(1, "closedir %s", path);
	return (count);
}

static void
write_growth_marker (const char *path, unsigned int count,
    off_t initial, off_t grown)
{
	char buf[128];
	ssize_t written;
	int fd, len;

	len = snprintf(buf, sizeof(buf), "%u %lld %lld\n", count,
	    (long long)initial, (long long)grown);
	if (len < 0 || (size_t)len >= sizeof(buf))
		errx(1, "growth marker is too long");
	fd = open_output(path, 0);
	written = write(fd, buf, (size_t)len);
	if (written == -1)
		err(1, "write %s", path);
	if (written != len)
		errx(1, "short write to %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
read_growth_marker (const char *path, unsigned int *countp,
    off_t *initialp, off_t *grownp)
{
	long long initial, grown;
	FILE *file;

	file = fopen(path, "r");
	if (file == NULL)
		err(1, "fopen %s", path);
	if (fscanf(file, "%u %lld %lld", countp, &initial,
	    &grown) != 3)
		errx(1, "%s has an invalid growth marker", path);
	if (fclose(file) == EOF)
		err(1, "fclose %s", path);
	if (*countp == 0 || *countp > DIR_ENTRY_LIMIT ||
	    initial <= 0 || grown < initial)
		errx(1, "%s has invalid growth values", path);
	*initialp = (off_t)initial;
	*grownp = (off_t)grown;
	if ((long long)*initialp != initial ||
	    (long long)*grownp != grown)
		errx(1, "%s growth values exceed off_t", path);
}

static size_t
filesystem_block_size (const char *root)
{
	struct statfs sfs;

	if (statfs(root, &sfs) == -1)
		err(1, "statfs %s", root);
	if (sfs.f_bsize != 1024 && sfs.f_bsize != 2048 &&
	    sfs.f_bsize != 4096)
		errx(1, "%s has unsupported block size %u", root,
		    (unsigned int)sfs.f_bsize);
	return ((size_t)sfs.f_bsize);
}

static size_t
extent_leaf_capacity (size_t block_size)
{
	return ((block_size - EXTENT_NODE_HEADER_BYTES) /
	    EXTENT_NODE_ENTRY_BYTES);
}

static off_t
extent_lbn_offset (size_t block_size, size_t lbn)
{
	return ((off_t)block_size * (off_t)lbn);
}

static void
read_extent_block (int fd, unsigned char *buf, size_t block_size,
    off_t offset)
{
	ssize_t n;

	n = pread(fd, buf, block_size, offset);
	if (n == -1)
		err(1, "pread extent block at %lld", (long long)offset);
	if ((size_t)n != block_size)
		errx(1, "short extent read at %lld", (long long)offset);
}

static void
check_extent_zero (const unsigned char *buf, size_t block_size,
    const char *description)
{
	size_t i;

	for (i = 0; i < block_size; i++) {
		if (buf[i] != 0)
			errx(1, "%s is not zero at byte %zu", description,
			    i);
	}
}

static int
check_extent_growth (const char *root, size_t entries,
    size_t old_metadata, size_t new_metadata)
{
	unsigned char buf[IO_BYTES];
	struct stat st;
	char path[PATH_MAX];
	blkcnt_t expected_sectors;
	off_t new_size, old_size, offset;
	size_t block_size, expected_blocks, i, target_lbn;
	int fd, state;

	block_size = filesystem_block_size(root);
	if (block_size > sizeof(buf))
		errx(1, "extent block exceeds input buffer");
	target_lbn = 2 * entries;
	old_size = extent_lbn_offset(block_size, target_lbn - 1);
	new_size = extent_lbn_offset(block_size, target_lbn + 1);
	make_path(path, sizeof(path), root, EXTENT_FILE);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		err(1, "open %s", path);
	if (fstat(fd, &st) == -1)
		err(1, "fstat %s", path);
	if (! S_ISREG(st.st_mode) || st.st_nlink != 1)
		errx(1, "%s has invalid inode shape", path);
	if (st.st_size == old_size) {
		state = 0;
		expected_blocks = entries + old_metadata;
	} else if (st.st_size == new_size) {
		state = 1;
		expected_blocks = entries + 1 + new_metadata;
	} else
		errx(1, "%s has unexpected size %lld", path,
		    (long long)st.st_size);
	expected_sectors = (blkcnt_t)expected_blocks *
	    (blkcnt_t)(block_size / 512);
	if (st.st_blocks != expected_sectors)
		errx(1, "%s has unexpected block count %lld", path,
		    (long long)st.st_blocks);
	for (i = 0; i < entries; i++) {
		offset = extent_lbn_offset(block_size, 2 * i);
		read_extent_block(fd, buf, block_size, offset);
		if (buf[0] != 'x')
			errx(1, "%s source extent %zu is corrupt", path,
			    i);
		buf[0] = 0;
		check_extent_zero(buf, block_size, "source extent");
		if (i + 1 < entries) {
			offset += (off_t)block_size;
			read_extent_block(fd, buf, block_size, offset);
			check_extent_zero(buf, block_size, "sparse hole");
		}
	}
	if (state != 0) {
		offset = extent_lbn_offset(block_size, target_lbn - 1);
		read_extent_block(fd, buf, block_size, offset);
		check_extent_zero(buf, block_size, "growth hole");
		offset = extent_lbn_offset(block_size, target_lbn);
		read_extent_block(fd, buf, block_size, offset);
		for (i = 0; i < block_size; i++) {
			if (buf[i] != pattern_byte(offset + (off_t)i,
			    EXTENT_SPLIT_SEED))
				errx(1, "%s growth data is corrupt", path);
		}
	}
	if (close(fd) == -1)
		err(1, "close %s", path);
	return (state);
}

static void
write_extent_block (int fd, size_t block_size, off_t offset)
{
	unsigned char buf[IO_BYTES];
	ssize_t n;

	if (block_size > sizeof(buf))
		errx(1, "extent block exceeds output buffer");
	fill_pattern(buf, block_size, offset, EXTENT_SPLIT_SEED);
	n = pwrite(fd, buf, block_size, offset);
	if (n == -1)
		err(1, "pwrite extent block at %lld", (long long)offset);
	if ((size_t)n != block_size)
		errx(1, "short extent write at %lld", (long long)offset);
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
arm_boundary (const char *stage)
{
	if (unlink(GATE_PATH) == -1 && errno != ENOENT)
		err(1, "unlink %s", GATE_PATH);
	if (printf("READY %s\n", stage) < 0 || fflush(stdout) == EOF)
		err(1, "publish boundary marker");
	while (access(GATE_PATH, F_OK) == -1) {
		if (errno != ENOENT)
			err(1, "access %s", GATE_PATH);
		if (usleep(GATE_USEC) == -1 && errno != EINTR)
			err(1, "boundary wait");
	}
	if (unlink(GATE_PATH) == -1)
		err(1, "unlink %s", GATE_PATH);
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

static uint32_t
load_be32 (const unsigned char *buf, size_t offset)
{
	uint32_t value;

	memcpy(&value, buf + offset, sizeof(value));
	return (be32toh(value));
}

static void
store_be32 (unsigned char *buf, size_t offset, uint32_t value)
{
	value = htobe32(value);
	memcpy(buf + offset, &value, sizeof(value));
}

static void
classify_journal (const char *path)
{
	unsigned char buf[JBD2_SUPER_BYTES];
	struct stat st;
	uint64_t blocks;
	uint32_t blocksize, first, head, maxlen, sequence, start;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		err(1, "open %s", path);
	if (fstat(fd, &st) == -1)
		err(1, "fstat %s", path);
	if (! S_ISREG(st.st_mode) || st.st_size < JBD2_SUPER_BYTES)
		errx(1, "%s has invalid journal size", path);
	n = pread(fd, buf, sizeof(buf), 0);
	if (n == -1)
		err(1, "pread %s", path);
	if ((size_t)n != sizeof(buf))
		errx(1, "short read from %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);

	if (load_be32(buf, JBD2_OFF_MAGIC) != JBD2_MAGIC)
		errx(1, "%s has invalid journal magic", path);
	if (load_be32(buf, JBD2_OFF_BLOCKTYPE) !=
	    JBD2_SUPERBLOCK_V2)
		errx(1, "%s has unsupported journal version", path);
	blocksize = load_be32(buf, JBD2_OFF_BLOCKSIZE);
	if (blocksize != 1024 && blocksize != 2048 &&
	    blocksize != 4096)
		errx(1, "%s has invalid journal block size", path);
	if ((uint64_t)st.st_size % blocksize != 0)
		errx(1, "%s is not journal-block aligned", path);
	blocks = (uint64_t)st.st_size / blocksize;
	maxlen = load_be32(buf, JBD2_OFF_MAXLEN);
	first = load_be32(buf, JBD2_OFF_FIRST);
	sequence = load_be32(buf, JBD2_OFF_SEQUENCE);
	start = load_be32(buf, JBD2_OFF_START);
	head = load_be32(buf, JBD2_OFF_HEAD);
	if (first == 0 || first >= maxlen || maxlen > blocks ||
	    (start != 0 && (start < first || start >= maxlen)) ||
	    (head != 0 && (head < first || head >= maxlen)))
		errx(1, "%s has invalid journal geometry", path);

	printf("journal=%s sequence=%u start=%u head=%u "
	    "first=%u maxlen=%u\n", start == 0 ? "clean" : "recover",
	    sequence, start, head, first, maxlen);
	if (fflush(stdout) == EOF)
		err(1, "publish journal state");
}

static void
run_dir_growth (const char *stage, const char *root, int boundary)
{
	struct stat st;
	char entry[PATH_MAX], grow[PATH_MAX], marker[PATH_MAX];
	char probe[PATH_MAX];
	off_t grown, initial;
	unsigned int count, i;

	make_path(probe, sizeof(probe), root, PROBE_DIR);
	make_path(grow, sizeof(grow), root, GROW_DIR);
	make_path(marker, sizeof(marker), root, GROW_MARKER);
	/*
	 * Probe the filesystem itself so dot entries, record rounding,
	 * checksum tails, and block size are included in the boundary.
	 */
	if (mkdir(probe, 0755) == -1)
		err(1, "mkdir %s", probe);
	if (stat(probe, &st) == -1)
		err(1, "stat %s", probe);
	initial = st.st_size;
	grown = 0;
	for (count = 1; count <= DIR_ENTRY_LIMIT; count++) {
		make_entry_path(entry, sizeof(entry), root, PROBE_DIR,
		    count - 1);
		create_empty(entry);
		if (stat(probe, &st) == -1)
			err(1, "stat %s", probe);
		if (st.st_size < initial)
			errx(1, "%s shrank during growth probe", probe);
		if (st.st_size > initial) {
			grown = st.st_size;
			break;
		}
	}
	if (grown == 0)
		errx(1, "%s did not grow", probe);
	for (i = 0; i < count; i++) {
		make_entry_path(entry, sizeof(entry), root, PROBE_DIR, i);
		if (unlink(entry) == -1)
			err(1, "unlink %s", entry);
	}
	if (rmdir(probe) == -1)
		err(1, "rmdir %s", probe);

	if (mkdir(grow, 0755) == -1)
		err(1, "mkdir %s", grow);
	for (i = 0; i + 1 < count; i++) {
		make_entry_path(entry, sizeof(entry), root, GROW_DIR, i);
		create_empty(entry);
	}
	if (stat(grow, &st) == -1)
		err(1, "stat %s", grow);
	if (st.st_size != initial)
		errx(1, "%s grew before the boundary", grow);
	write_growth_marker(marker, count, initial, grown);

	if (boundary)
		arm_boundary(stage);
	else
		arm_cut(stage);
	make_entry_path(entry, sizeof(entry), root, GROW_DIR,
	    count - 1);
	create_empty(entry);
	if (printf("DONE %s\n", stage) < 0 ||
	    fflush(stdout) == EOF)
		err(1, "publish completion marker");
	for (;;)
		(void)pause();
}

static void
run_rmdir_open (const char *stage, const char *root, int boundary)
{
	char path[PATH_MAX];
	int fd;

	make_path(path, sizeof(path), root, REMOVE_DIR);
	if (mkdir(path, 0755) == -1)
		err(1, "mkdir %s", path);
	/* Keep the removed directory active until the VM is stopped. */
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		err(1, "open %s", path);
	if (boundary)
		arm_boundary(stage);
	else
		arm_cut(stage);
	if (rmdir(path) == -1)
		err(1, "rmdir %s", path);
	if (printf("DONE %s\n", stage) < 0 ||
	    fflush(stdout) == EOF)
		err(1, "publish completion marker");
	for (;;)
		(void)pause();
}

static void
run_extent_growth (const char *stage, const char *root, int boundary)
{
	char path[PATH_MAX];
	off_t offset;
	size_t block_size, entries, new_metadata, old_metadata;
	int fd;

	block_size = filesystem_block_size(root);
	if (strcmp(stage, "extent-promote") == 0) {
		entries = EXTENT_ROOT_ENTRIES;
		old_metadata = 0;
		new_metadata = 1;
	} else if (strcmp(stage, "extent-split") == 0) {
		entries = extent_leaf_capacity(block_size);
		old_metadata = 1;
		new_metadata = 2;
	} else
		errx(1, "unknown extent stage: %s", stage);
	if (check_extent_growth(root, entries, old_metadata,
	    new_metadata) != 0)
		errx(1, "%s fixture has already grown", stage);
	make_path(path, sizeof(path), root, EXTENT_FILE);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd == -1)
		err(1, "open %s", path);
	if (boundary)
		arm_boundary(stage);
	else
		arm_cut(stage);
	offset = extent_lbn_offset(block_size, 2 * entries);
	write_extent_block(fd, block_size, offset);
	if (close(fd) == -1)
		err(1, "close %s", path);
	if (printf("DONE %s\n", stage) < 0 ||
	    fflush(stdout) == EOF)
		err(1, "publish completion marker");
	for (;;)
		(void)pause();
}

static void
run_workload (const char *stage, const char *root, int boundary)
{
	char current[PATH_MAX], next[PATH_MAX];
	int fd;

	make_path(current, sizeof(current), root, "current");
	make_path(next, sizeof(next), root, "next");
	if (strcmp(stage, "extent-promote") == 0 ||
	    strcmp(stage, "extent-split") == 0) {
		run_extent_growth(stage, root, boundary);
		return;
	}
	if (strcmp(stage, "dir-grow") == 0) {
		run_dir_growth(stage, root, boundary);
		return;
	}
	if (strcmp(stage, "rmdir-open") == 0) {
		run_rmdir_open(stage, root, boundary);
		return;
	}
	if (strcmp(stage, "unlink-open") == 0) {
		/* Keep the unlinked inode active until the VM is stopped. */
		fd = open(current, O_RDWR | O_CLOEXEC);
		if (fd == -1)
			err(1, "open %s", current);
		if (boundary)
			arm_boundary(stage);
		else
			arm_cut(stage);
		if (unlink(current) == -1)
			err(1, "unlink %s", current);
		if (printf("DONE %s\n", stage) < 0 ||
		    fflush(stdout) == EOF)
			err(1, "publish completion marker");
		for (;;)
			(void)pause();
	}
	fd = open_output(next, 1);
	if (strcmp(stage, "write") == 0) {
		if (boundary)
			arm_boundary(stage);
		else
			arm_cut(stage);
		write_pattern(fd, NEW_BYTES, NEW_SEED);
	} else if (strcmp(stage, "fsync") == 0) {
		write_pattern(fd, NEW_BYTES, NEW_SEED);
		if (boundary)
			arm_boundary(stage);
		else
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
		if (boundary)
			arm_boundary(stage);
		else
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
verify_dir_growth (const char *root)
{
	struct stat dir_st, entry_st;
	char entry[PATH_MAX], grow[PATH_MAX], marker[PATH_MAX];
	char probe[PATH_MAX];
	off_t grown, initial;
	unsigned int count, entries, expected, i;
	int present;

	make_path(grow, sizeof(grow), root, GROW_DIR);
	make_path(marker, sizeof(marker), root, GROW_MARKER);
	make_path(probe, sizeof(probe), root, PROBE_DIR);
	read_growth_marker(marker, &count, &initial, &grown);
	if (stat(grow, &dir_st) == -1)
		err(1, "stat %s", grow);
	if (! S_ISDIR(dir_st.st_mode))
		errx(1, "%s is not a directory", grow);
	for (i = 0; i + 1 < count; i++) {
		make_entry_path(entry, sizeof(entry), root, GROW_DIR, i);
		if (lstat(entry, &entry_st) == -1)
			err(1, "lstat %s", entry);
		if (! S_ISREG(entry_st.st_mode) || entry_st.st_size != 0)
			errx(1, "%s has invalid shape", entry);
	}
	make_entry_path(entry, sizeof(entry), root, GROW_DIR,
	    count - 1);
	present = 1;
	if (lstat(entry, &entry_st) == -1) {
		if (errno != ENOENT)
			err(1, "lstat %s", entry);
		present = 0;
	} else if (! S_ISREG(entry_st.st_mode) ||
	    entry_st.st_size != 0)
		errx(1, "%s has invalid shape", entry);
	expected = present ? count : count - 1;
	entries = directory_entries(grow);
	if (entries != expected)
		errx(1, "%s has %u entries, expected %u", grow,
		    entries, expected);
	if (dir_st.st_size != (present ? grown : initial))
		errx(1, "%s has unexpected size %lld", grow,
		    (long long)dir_st.st_size);
	if (lstat(probe, &entry_st) != -1)
		errx(1, "%s unexpectedly exists", probe);
	if (errno != ENOENT)
		err(1, "lstat %s", probe);
	printf("state=directory-%s\n", present ? "grown" : "old");
	if (fflush(stdout) == EOF)
		err(1, "publish directory-growth state");
}

static void
verify_rmdir (const char *root)
{
	struct stat st;
	char path[PATH_MAX];
	int present;

	make_path(path, sizeof(path), root, REMOVE_DIR);
	present = 1;
	if (lstat(path, &st) == -1) {
		if (errno != ENOENT)
			err(1, "lstat %s", path);
		present = 0;
	} else {
		if (! S_ISDIR(st.st_mode))
			errx(1, "%s is not a directory", path);
		if (directory_entries(path) != 0)
			errx(1, "%s is not empty", path);
	}
	printf("state=directory-%s\n",
	    present ? "linked" : "absent");
	if (fflush(stdout) == EOF)
		err(1, "publish directory-removal state");
}

static void
verify_extent_promote (const char *root)
{
	int state;

	state = check_extent_growth(root, EXTENT_ROOT_ENTRIES, 0, 1);
	printf("state=extent-%s\n", state ? "promoted" : "inline");
	if (fflush(stdout) == EOF)
		err(1, "publish extent-promotion state");
}

static void
verify_extent_split (const char *root)
{
	size_t entries;
	int state;

	entries = extent_leaf_capacity(filesystem_block_size(root));
	state = check_extent_growth(root, entries, 1, 2);
	printf("state=extent-%s\n",
	    state ? "two-leaves" : "one-leaf");
	if (fflush(stdout) == EOF)
		err(1, "publish extent-split state");
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
		if (next_st.st_size == NEW_BYTES)
			printf("state=old next=full\n");
		else
			printf("state=old next=%lld\n",
			    (long long)next_st.st_size);
	}
	if (fflush(stdout) == EOF)
		err(1, "publish verification state");
}

static void
verify_unlink (const char *root)
{
	struct stat st;
	char current[PATH_MAX], next[PATH_MAX];

	make_path(current, sizeof(current), root, "current");
	make_path(next, sizeof(next), root, "next");
	if (lstat(current, &st) == -1) {
		if (errno != ENOENT)
			err(1, "lstat %s", current);
		printf("state=absent\n");
	} else {
		if (! S_ISREG(st.st_mode) || st.st_size != OLD_BYTES)
			errx(1, "%s has invalid shape", current);
		check_pattern(current, OLD_BYTES, OLD_SEED);
		printf("state=linked\n");
	}
	if (lstat(next, &st) != -1)
		errx(1, "%s unexpectedly exists", next);
	if (errno != ENOENT)
		err(1, "lstat %s", next);
	if (fflush(stdout) == EOF)
		err(1, "publish unlink state");
}

static void
selftest (void)
{
	unsigned char journal[JBD2_SUPER_BYTES];
	struct stat st;
	char current[PATH_MAX], entry[PATH_MAX], grow[PATH_MAX];
	char journal_path[PATH_MAX];
	char marker[PATH_MAX], next[PATH_MAX], removed[PATH_MAX];
	char root[] = "/tmp/ext4fs_crash.XXXXXXXX";
	off_t grown, initial;
	int fd;

	if (mkdtemp(root) == NULL)
		err(1, "mkdtemp");
	make_path(current, sizeof(current), root, "current");
	make_path(next, sizeof(next), root, "next");
	make_old_pattern(current);
	verify_workload(root);
	verify_unlink(root);

	make_path(grow, sizeof(grow), root, GROW_DIR);
	make_path(marker, sizeof(marker), root, GROW_MARKER);
	if (mkdir(grow, 0755) == -1)
		err(1, "mkdir %s", grow);
	if (stat(grow, &st) == -1)
		err(1, "stat %s", grow);
	initial = st.st_size;
	write_growth_marker(marker, 1, initial, initial);
	verify_dir_growth(root);
	make_entry_path(entry, sizeof(entry), root, GROW_DIR, 0);
	create_empty(entry);
	if (stat(grow, &st) == -1)
		err(1, "stat %s", grow);
	grown = st.st_size;
	write_growth_marker(marker, 1, initial, grown);
	verify_dir_growth(root);
	if (unlink(entry) == -1)
		err(1, "unlink %s", entry);
	if (unlink(marker) == -1)
		err(1, "unlink %s", marker);
	if (rmdir(grow) == -1)
		err(1, "rmdir %s", grow);

	make_path(removed, sizeof(removed), root, REMOVE_DIR);
	if (mkdir(removed, 0755) == -1)
		err(1, "mkdir %s", removed);
	verify_rmdir(root);
	if (rmdir(removed) == -1)
		err(1, "rmdir %s", removed);
	verify_rmdir(root);

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
	verify_workload(root);
	if (rename(next, current) == -1)
		err(1, "rename %s", next);
	verify_workload(root);

	make_path(journal_path, sizeof(journal_path), root, "journal");
	memset(journal, 0, sizeof(journal));
	store_be32(journal, JBD2_OFF_MAGIC, JBD2_MAGIC);
	store_be32(journal, JBD2_OFF_BLOCKTYPE,
	    JBD2_SUPERBLOCK_V2);
	store_be32(journal, JBD2_OFF_BLOCKSIZE, 1024);
	store_be32(journal, JBD2_OFF_MAXLEN, 4);
	store_be32(journal, JBD2_OFF_FIRST, 1);
	store_be32(journal, JBD2_OFF_SEQUENCE, 7);
	store_be32(journal, JBD2_OFF_HEAD, 3);
	fd = open_output(journal_path, 1);
	if (ftruncate(fd, 4096) == -1)
		err(1, "ftruncate %s", journal_path);
	if (pwrite(fd, journal, sizeof(journal), 0) !=
	    (ssize_t)sizeof(journal))
		err(1, "pwrite %s", journal_path);
	if (close(fd) == -1)
		err(1, "close %s", journal_path);
	classify_journal(journal_path);
	store_be32(journal, JBD2_OFF_START, 2);
	fd = open(journal_path, O_WRONLY | O_CLOEXEC);
	if (fd == -1)
		err(1, "open %s", journal_path);
	if (pwrite(fd, journal, sizeof(journal), 0) !=
	    (ssize_t)sizeof(journal))
		err(1, "pwrite %s", journal_path);
	if (close(fd) == -1)
		err(1, "close %s", journal_path);
	classify_journal(journal_path);
	if (unlink(journal_path) == -1)
		err(1, "unlink %s", journal_path);
	if (unlink(current) == -1)
		err(1, "unlink %s", current);
	verify_unlink(root);
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
	else if (argc == 3 && strcmp(argv[1], "journal-state") == 0)
		classify_journal(argv[2]);
	else if (argc == 4 && strcmp(argv[1], "workload") == 0)
		run_workload(argv[2], argv[3], 0);
	else if (argc == 4 && strcmp(argv[1], "boundary") == 0)
		run_workload(argv[2], argv[3], 1);
	else if (argc == 3 && strcmp(argv[1], "verify") == 0)
		verify_workload(argv[2]);
	else if (argc == 3 && strcmp(argv[1], "verify-unlink") == 0)
		verify_unlink(argv[2]);
	else if (argc == 3 && strcmp(argv[1], "verify-dir-growth") == 0)
		verify_dir_growth(argv[2]);
	else if (argc == 3 && strcmp(argv[1], "verify-rmdir") == 0)
		verify_rmdir(argv[2]);
	else if (argc == 3 &&
	    strcmp(argv[1], "verify-extent-promote") == 0)
		verify_extent_promote(argv[2]);
	else if (argc == 3 &&
	    strcmp(argv[1], "verify-extent-split") == 0)
		verify_extent_split(argv[2]);
	else
		errx(1, "usage: ext4fs_crash pattern-old path | "
		    "journal-state path | workload stage root | boundary "
		    "stage root | verify root | verify-unlink root | "
		    "verify-dir-growth root | verify-rmdir root | "
		    "verify-extent-promote root | "
		    "verify-extent-split root");
	return (0);
}
