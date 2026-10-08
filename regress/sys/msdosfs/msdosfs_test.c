/*
 * Copyright (c) 2025,2026 kmx.io <contact@kmx.io>
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

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BOOT_SIZE	512
#define DIRENT_SIZE	32

#define BPB_BPS		11
#define BPB_SPC		13
#define BPB_RES		14
#define BPB_FATS	16
#define BPB_ROOTENTS	17
#define BPB_SECTORS	19
#define BPB_FATSECS	22
#define BPB_HUGESECTORS	32
#define BPB_BIGFATSECS	36
#define BPB_EXTFLAGS	40
#define BPB_ROOTCLUST	44
#define BPB_FSINFO	48

#define DE_NAME		0
#define DE_ATTR		11
#define DE_MTIME	22
#define DE_MDATE	24
#define DE_CLUSTER_LO	26
#define DE_SIZE		28
#define DE_CLUSTER_HI	20

#define ATTR_DIRECTORY	0x10
#define ATTR_ARCHIVE	0x20
#define FAT32_EOF	0x0fffffffU
#define SAME_SIZE_TRUNCATE_SIZE	239709

struct fat_image {
	int		 fd;
	uint8_t		 boot[BOOT_SIZE];
	uint32_t	 bps;
	uint32_t	 spc;
	uint32_t	 res;
	uint32_t	 fats;
	uint32_t	 rootents;
	uint64_t	 sectors;
	uint32_t	 fatsecs;
	uint32_t	 rootclust;
	uint64_t	 rootsectors;
	uint64_t	 firstdata;
	uint64_t	 clusters;
	int		 fat32;
};

struct fat_fid {
	uint16_t len;
	uint16_t pad;
	uint32_t dirclust;
	uint32_t diroffset;
	uint32_t generation;
};

static void	read_exact(int, void *, size_t, off_t, const char *);
static void	write_exact(int, const void *, size_t, off_t, const char *);
static uint16_t	get16(const uint8_t *, size_t);
static uint32_t	get32(const uint8_t *, size_t);
static void	put16(uint8_t *, size_t, uint16_t);
static void	put32(uint8_t *, size_t, uint32_t);
static uint32_t	number(const char *, const char *);
static void	open_image(struct fat_image *, const char *);
static void	write_boot(struct fat_image *);
static off_t	cluster_offset(const struct fat_image *, uint32_t);
static off_t	root_offset(const struct fat_image *);
static void	set_fat(struct fat_image *, uint32_t, uint32_t);
static void	make_entry(uint8_t *, const char *, uint8_t, uint32_t,
		    uint32_t, uint16_t);
static void	write_dir_cluster(struct fat_image *, uint32_t, uint32_t);
static void	make_runtime_fixture(struct fat_image *, const char *);
static void	mutate(struct fat_image *, const char *);
static void	expect_fh_error(const fhandle_t *, int, const char *);
static void	test_fh(const char *);
static void	test_read_error(const char *);
static void	test_bad_dir(const char *);
static void	test_parent_cycle(const char *, const char *);
static void	test_free_cycle(const char *, const char *, uint64_t);
static void	test_dates(const char *);
static void	test_full_root(const char *);
static void	test_same_size_truncate(const char *);

static void
read_exact(int fd, void *buf, size_t len, off_t offset, const char *what)
{
	uint8_t *p = buf;
	ssize_t n;
	size_t done;

	for (done = 0; done < len; done += (size_t)n) {
		n = pread(fd, p + done, len - done, offset + (off_t)done);
		if (n == -1)
			err(1, "pread %s", what);
		if (n == 0)
			errx(1, "short read of %s", what);
	}
}

static void
write_exact(int fd, const void *buf, size_t len, off_t offset,
    const char *what)
{
	const uint8_t *p = buf;
	ssize_t n;
	size_t done;

	for (done = 0; done < len; done += (size_t)n) {
		n = pwrite(fd, p + done, len - done, offset + (off_t)done);
		if (n == -1)
			err(1, "pwrite %s", what);
		if (n == 0)
			errx(1, "short write of %s", what);
	}
}

static uint16_t
get16(const uint8_t *buf, size_t off)
{
	uint16_t value;

	memcpy(&value, buf + off, sizeof(value));
	return letoh16(value);
}

static uint32_t
get32(const uint8_t *buf, size_t off)
{
	uint32_t value;

	memcpy(&value, buf + off, sizeof(value));
	return letoh32(value);
}

static void
put16(uint8_t *buf, size_t off, uint16_t value)
{
	value = htole16(value);
	memcpy(buf + off, &value, sizeof(value));
}

static void
put32(uint8_t *buf, size_t off, uint32_t value)
{
	value = htole32(value);
	memcpy(buf + off, &value, sizeof(value));
}

static uint32_t
number(const char *value, const char *name)
{
	char *end = NULL;
	unsigned long long result;

	errno = 0;
	result = strtoull(value, &end, 0);
	if (*value == '\0' || *value == '-' || end == NULL || *end != '\0' ||
	    errno == ERANGE || result > UINT32_MAX)
		errx(1, "invalid %s: %s", name, value);
	return (uint32_t)result;
}

static void
open_image(struct fat_image *fs, const char *path)
{
	uint64_t metadata;

	memset(fs, 0, sizeof(*fs));
	if ((fs->fd = open(path, O_RDWR)) == -1)
		err(1, "%s", path);
	read_exact(fs->fd, fs->boot, sizeof(fs->boot), 0, "boot sector");
	fs->bps = get16(fs->boot, BPB_BPS);
	fs->spc = fs->boot[BPB_SPC];
	fs->res = get16(fs->boot, BPB_RES);
	fs->fats = fs->boot[BPB_FATS];
	fs->rootents = get16(fs->boot, BPB_ROOTENTS);
	fs->fat32 = fs->rootents == 0;
	fs->sectors = get16(fs->boot, BPB_SECTORS);
	if (fs->sectors == 0)
		fs->sectors = get32(fs->boot, BPB_HUGESECTORS);
	fs->fatsecs = fs->fat32 ? get32(fs->boot, BPB_BIGFATSECS) :
	    get16(fs->boot, BPB_FATSECS);
	fs->rootclust = fs->fat32 ? get32(fs->boot, BPB_ROOTCLUST) : 0;
	if (fs->bps < 512 || fs->spc == 0 || fs->fats == 0 ||
	    fs->fatsecs == 0 || fs->sectors == 0)
		errx(1, "%s has an invalid source BPB", path);
	fs->rootsectors = ((uint64_t)fs->rootents * DIRENT_SIZE +
	    fs->bps - 1) / fs->bps;
	metadata = fs->res + (uint64_t)fs->fats * fs->fatsecs +
	    fs->rootsectors;
	if (metadata >= fs->sectors)
		errx(1, "%s has no data area", path);
	fs->firstdata = metadata;
	fs->clusters = (fs->sectors - metadata) / fs->spc;
}

static void
write_boot(struct fat_image *fs)
{
	write_exact(fs->fd, fs->boot, sizeof(fs->boot), 0, "boot sector");
}

static off_t
cluster_offset(const struct fat_image *fs, uint32_t cluster)
{
	uint64_t sector, offset;

	if (cluster < 2)
		errx(1, "invalid fixture cluster %u", cluster);
	sector = fs->firstdata + (uint64_t)(cluster - 2) * fs->spc;
	offset = sector * fs->bps;
	if (offset > INT64_MAX)
		errx(1, "fixture cluster offset is too large");
	return (off_t)offset;
}

static off_t
root_offset(const struct fat_image *fs)
{
	uint64_t offset;

	if (fs->fat32)
		return cluster_offset(fs, fs->rootclust);
	offset = ((uint64_t)fs->res + (uint64_t)fs->fats * fs->fatsecs) *
	    fs->bps;
	if (offset > INT64_MAX)
		errx(1, "fixture root offset is too large");
	return (off_t)offset;
}

static void
set_fat(struct fat_image *fs, uint32_t cluster, uint32_t value)
{
	uint8_t entry[4];
	uint64_t fatbase, fatoff;
	uint32_t copy;

	if (!fs->fat32)
		errx(1, "runtime FAT mutation requires FAT32");
	put32(entry, 0, value);
	for (copy = 0; copy < fs->fats; copy++) {
		fatbase = ((uint64_t)fs->res +
		    (uint64_t)copy * fs->fatsecs) * fs->bps;
		fatoff = fatbase + (uint64_t)cluster * sizeof(entry);
		if (fatoff > INT64_MAX)
			errx(1, "fixture FAT offset is too large");
		write_exact(fs->fd, entry, sizeof(entry), (off_t)fatoff,
		    "FAT entry");
	}
}

static void
make_entry(uint8_t *entry, const char *name, uint8_t attr, uint32_t cluster,
    uint32_t size, uint16_t date)
{
	if (strlen(name) != 11)
		errx(1, "fixture name must contain exactly 11 bytes: %s", name);
	memset(entry, 0, DIRENT_SIZE);
	memcpy(entry + DE_NAME, name, 11);
	entry[DE_ATTR] = attr;
	put16(entry, DE_MTIME, 0);
	put16(entry, DE_MDATE, date);
	put16(entry, DE_CLUSTER_LO, cluster & 0xffff);
	put16(entry, DE_CLUSTER_HI, cluster >> 16);
	put32(entry, DE_SIZE, size);
}

static void
write_dir_cluster(struct fat_image *fs, uint32_t cluster, uint32_t parent)
{
	uint8_t *buf;
	size_t size;

	size = (size_t)fs->bps * fs->spc;
	buf = calloc(1, size);
	if (buf == NULL)
		err(1, "calloc directory cluster");
	make_entry(buf, ".          ", ATTR_DIRECTORY, cluster, 0, 0x0021);
	make_entry(buf + DIRENT_SIZE, "..         ", ATTR_DIRECTORY,
	    parent, 0, 0x0021);
	write_exact(fs->fd, buf, size, cluster_offset(fs, cluster),
	    "directory cluster");
	free(buf);
}

static void
make_runtime_fixture(struct fat_image *fs, const char *name)
{
	uint8_t entries[8 * DIRENT_SIZE];
	uint32_t maxcluster, clusterbytes;

	if (!fs->fat32)
		errx(1, "%s requires FAT32", name);
	memset(entries, 0, sizeof(entries));
	maxcluster = (uint32_t)fs->clusters + 1;
	clusterbytes = fs->bps * fs->spc;

	if (strcmp(name, "file-self-cycle") == 0) {
		make_entry(entries, "SELFCYCLBIN", ATTR_ARCHIVE, 3,
		    clusterbytes * 2, 0x0021);
		set_fat(fs, 3, 3);
	} else if (strcmp(name, "file-two-cycle") == 0) {
		make_entry(entries, "TWOCYCLEBIN", ATTR_ARCHIVE, 3,
		    clusterbytes * 4, 0x0021);
		set_fat(fs, 3, 4);
		set_fat(fs, 4, 3);
	} else if (strcmp(name, "file-out-of-range") == 0) {
		make_entry(entries, "BADRANGEBIN", ATTR_ARCHIVE, 3,
		    clusterbytes * 2, 0x0021);
		set_fat(fs, 3, maxcluster + 1);
	} else if (strcmp(name, "bad-directory") == 0) {
		make_entry(entries, "BADDIR     ", ATTR_DIRECTORY, 3, 0,
		    0x0021);
		make_entry(entries + DIRENT_SIZE, "GOOD    TXT", ATTR_ARCHIVE,
		    0, 0, 0x0021);
		set_fat(fs, 3, 3);
		write_dir_cluster(fs, 3, fs->rootclust);
	} else if (strcmp(name, "parent-self-cycle") == 0 ||
	    strcmp(name, "parent-two-cycle") == 0) {
		make_entry(entries, "SOURCE     ", ATTR_DIRECTORY, 3, 0,
		    0x0021);
		make_entry(entries + DIRENT_SIZE, "TARGET     ",
		    ATTR_DIRECTORY, 4, 0, 0x0021);
		set_fat(fs, 3, FAT32_EOF);
		set_fat(fs, 4, FAT32_EOF);
		write_dir_cluster(fs, 3, fs->rootclust);
		if (strcmp(name, "parent-self-cycle") == 0)
			write_dir_cluster(fs, 4, 4);
		else {
			set_fat(fs, 5, FAT32_EOF);
			write_dir_cluster(fs, 4, 5);
			write_dir_cluster(fs, 5, 4);
		}
	} else if (strcmp(name, "free-self-cycle") == 0) {
		make_entry(entries, "FREESELFBIN", ATTR_ARCHIVE, 3,
		    clusterbytes, 0x0021);
		set_fat(fs, 3, 3);
	} else if (strcmp(name, "free-two-cycle") == 0) {
		make_entry(entries, "FREETWO BIN", ATTR_ARCHIVE, 3,
		    clusterbytes * 2, 0x0021);
		set_fat(fs, 3, 4);
		set_fat(fs, 4, 3);
	} else if (strcmp(name, "dates") == 0) {
		make_entry(entries, "MONTH0  TXT", ATTR_ARCHIVE, 0, 0,
		    (0 << 5) | 1);
		make_entry(entries + DIRENT_SIZE, "MONTH13 TXT", ATTR_ARCHIVE,
		    0, 0, (13 << 5) | 1);
		make_entry(entries + 2 * DIRENT_SIZE, "MONTH15 TXT", ATTR_ARCHIVE,
		    0, 0, (15 << 5) | 1);
		make_entry(entries + 3 * DIRENT_SIZE, "DAY0    TXT", ATTR_ARCHIVE,
		    0, 0, (1 << 5));
		make_entry(entries + 4 * DIRENT_SIZE, "APRIL31 TXT", ATTR_ARCHIVE,
		    0, 0, (4 << 5) | 31);
		make_entry(entries + 5 * DIRENT_SIZE, "FEB29N  TXT", ATTR_ARCHIVE,
		    0, 0, (1 << 9) | (2 << 5) | 29);
		make_entry(entries + 6 * DIRENT_SIZE, "LEAP29  TXT", ATTR_ARCHIVE,
		    0, 0, (2 << 5) | 29);
	} else if (strcmp(name, "fh") == 0) {
		make_entry(entries, "HANDLE  TXT", ATTR_ARCHIVE, 0, 0,
		    0x0021);
	} else {
		errx(1, "unknown runtime fixture: %s", name);
	}
	write_exact(fs->fd, entries, sizeof(entries), root_offset(fs),
	    "root entries");
}

static void
mutate(struct fat_image *fs, const char *name)
{
	uint64_t metadata;
	uint32_t maxcluster;
	uint16_t extflags;

	maxcluster = (uint32_t)fs->clusters + 1;
	metadata = fs->res + (uint64_t)fs->fats * fs->fatsecs +
	    fs->rootsectors;

	if (strcmp(name, "bad-boot-signature") == 0) {
		fs->boot[510] = 0;
		fs->boot[511] = 0;
	} else if (strcmp(name, "zero-fats") == 0) {
		fs->boot[BPB_FATS] = 0;
	} else if (strcmp(name, "active-fat-out-of-range") == 0) {
		if (!fs->fat32)
			errx(1, "%s requires FAT32", name);
		extflags = 0x0080 | fs->fats;
		put16(fs->boot, BPB_EXTFLAGS, extflags);
	} else if (strcmp(name, "root-cluster-out-of-range") == 0) {
		if (!fs->fat32)
			errx(1, "%s requires FAT32", name);
		put32(fs->boot, BPB_ROOTCLUST, maxcluster + 1);
	} else if (strcmp(name, "fsinfo-outside-reserved") == 0) {
		if (!fs->fat32)
			errx(1, "%s requires FAT32", name);
		put16(fs->boot, BPB_FSINFO, fs->res);
	} else if (strcmp(name, "first-data-at-volume-end") == 0) {
		if (metadata > UINT32_MAX)
			errx(1, "fixture metadata is too large");
		put16(fs->boot, BPB_SECTORS, 0);
		put32(fs->boot, BPB_HUGESECTORS, (uint32_t)metadata);
	} else if (strcmp(name, "fat-past-volume") == 0) {
		if (!fs->fat32)
			errx(1, "%s requires FAT32", name);
		put32(fs->boot, BPB_BIGFATSECS, (uint32_t)fs->sectors);
	} else if (strcmp(name, "fat-too-small") == 0) {
		if (!fs->fat32)
			errx(1, "%s requires FAT32", name);
		if (fs->fatsecs <= 1)
			errx(1, "%s source FAT is too small", name);
		put32(fs->boot, BPB_BIGFATSECS, fs->fatsecs - 1);
	} else if (strcmp(name, "declared-volume-too-large") == 0) {
		if (fs->sectors >= UINT32_MAX)
			errx(1, "source volume is too large");
		put16(fs->boot, BPB_SECTORS, 0);
		put32(fs->boot, BPB_HUGESECTORS,
		    (uint32_t)fs->sectors + 1);
	} else if (strcmp(name, "misaligned-fixed-root") == 0) {
		if (fs->fat32)
			errx(1, "%s requires FAT12/16", name);
		put16(fs->boot, BPB_ROOTENTS, 1);
	} else if (strcmp(name, "invalid-fsinfo-signature") == 0) {
		uint8_t zero[4] = { 0, 0, 0, 0 };
		uint32_t sector;

		if (!fs->fat32)
			errx(1, "%s requires FAT32", name);
		sector = get16(fs->boot, BPB_FSINFO);
		write_exact(fs->fd, zero, sizeof(zero),
		    (off_t)((uint64_t)sector * fs->bps), "FSInfo signature");
		return;
	} else if (strcmp(name, "root-self-cycle") == 0) {
		if (!fs->fat32)
			errx(1, "%s requires FAT32", name);
		set_fat(fs, fs->rootclust, fs->rootclust);
		return;
	} else if (strcmp(name, "full-fixed-root") == 0) {
		uint8_t entry[DIRENT_SIZE];
		char dosname[12];
		uint32_t i;

		if (fs->fat32)
			errx(1, "%s requires FAT12/16", name);
		for (i = 0; i < fs->rootents; i++) {
			if (snprintf(dosname, sizeof(dosname), "F%07uTXT", i) != 11)
				errx(1, "could not make fixed-root name");
			make_entry(entry, dosname, ATTR_ARCHIVE, 0, 0, 0x0021);
			write_exact(fs->fd, entry, sizeof(entry),
			    root_offset(fs) + (off_t)i * DIRENT_SIZE,
			    "fixed-root entry");
		}
		return;
	} else {
		make_runtime_fixture(fs, name);
		return;
	}
	write_boot(fs);
}

static void
expect_fh_error(const fhandle_t *fh, int expected, const char *what)
{
	struct stat st;

	errno = 0;
	if (fhstat(fh, &st) != -1)
		errx(1, "%s unexpectedly succeeded", what);
	if (errno != expected)
		errx(1, "%s returned %s, expected %s", what,
		    strerror(errno), strerror(expected));
}

static void
test_fh(const char *path)
{
	fhandle_t fh, bad, dirfh, replacement;
	struct fat_fid fid;
	struct stat st;
	char dirpath[PATH_MAX], *slash;
	int fd;

	if (getfh(path, &fh) == -1)
		err(1, "getfh %s", path);
	if (fhstat(&fh, &st) == -1)
		err(1, "fhstat valid handle");

	bad = fh;
	bad.fh_fid.fid_len = 2;
	expect_fh_error(&bad, EINVAL, "short file handle");
	bad = fh;
	bad.fh_fid.fid_len = MAXFIDSZ + 1;
	expect_fh_error(&bad, EINVAL, "oversized file handle");

	memcpy(&fid, &fh.fh_fid, sizeof(fid));
	bad = fh;
	fid.generation ^= 1;
	memcpy(&bad.fh_fid, &fid, sizeof(fid));
	expect_fh_error(&bad, ESTALE, "wrong file-handle generation");

	memcpy(&fid, &fh.fh_fid, sizeof(fid));
	bad = fh;
	fid.diroffset++;
	memcpy(&bad.fh_fid, &fid, sizeof(fid));
	expect_fh_error(&bad, ESTALE, "unaligned file-handle offset");

	if (unlink(path) == -1)
		err(1, "unlink handle fixture");
	if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644)) == -1)
		err(1, "recreate handle fixture");
	if (write(fd, "new\n", 4) != 4)
		err(1, "write recreated handle fixture");
	if (close(fd) == -1)
		err(1, "close recreated handle fixture");
	expect_fh_error(&fh, ESTALE, "reused directory-slot handle");
	if (getfh(path, &replacement) == -1 || fhstat(&replacement, &st) == -1)
		err(1, "replacement file handle");

	if (strlcpy(dirpath, path, sizeof(dirpath)) >= sizeof(dirpath) ||
	    (slash = strrchr(dirpath, '/')) == NULL)
		errx(1, "handle fixture path is invalid");
	if (strlcpy(slash + 1, "DIRHANDLE", sizeof(dirpath) -
	    (size_t)(slash + 1 - dirpath)) >= sizeof(dirpath) -
	    (size_t)(slash + 1 - dirpath))
		errx(1, "directory handle path is too long");
	if (mkdir(dirpath, 0755) == -1)
		err(1, "mkdir directory handle fixture");
	if (getfh(dirpath, &dirfh) == -1)
		err(1, "getfh directory handle fixture");
	if (rmdir(dirpath) == -1)
		err(1, "rmdir directory handle fixture");
	expect_fh_error(&dirfh, ESTALE, "removed directory handle");
	if (mkdir(dirpath, 0755) == -1)
		err(1, "recreate directory handle fixture");
	expect_fh_error(&dirfh, ESTALE, "recreated directory handle");
}

static void
test_read_error(const char *path)
{
	struct stat st;
	uint8_t byte;
	ssize_t n;
	int fd;

	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (st.st_size < 2)
		errx(1, "%s is too small for a traversal test", path);
	if ((fd = open(path, O_RDONLY)) == -1)
		err(1, "open %s", path);
	errno = 0;
	n = pread(fd, &byte, sizeof(byte), st.st_size - 1);
	if (n != -1 || errno != EIO)
		errx(1, "corrupt chain read returned %zd/%s, expected EIO",
		    n, strerror(errno));
	(void)close(fd);
}

static void
test_bad_dir(const char *mountpoint)
{
	char bad[PATH_MAX], good[PATH_MAX];
	struct stat st;
	unsigned int i;

	if (snprintf(bad, sizeof(bad), "%s/BADDIR", mountpoint) >=
	    (int)sizeof(bad) ||
	    snprintf(good, sizeof(good), "%s/GOOD.TXT", mountpoint) >=
	    (int)sizeof(good))
		errx(1, "bad-directory path is too long");
	for (i = 0; i < 100; i++) {
		errno = 0;
		if (stat(bad, &st) != -1 || errno != EIO)
			errx(1, "bad directory lookup %u returned %s",
			    i, strerror(errno));
	}
	if (stat(good, &st) == -1)
		err(1, "valid lookup after failed deget");
}

static void
test_parent_cycle(const char *source, const char *target)
{
	char destination[PATH_MAX];

	if (snprintf(destination, sizeof(destination), "%s/CHILD", target) >=
	    (int)sizeof(destination))
		errx(1, "rename destination is too long");
	errno = 0;
	if (rename(source, destination) != -1 || errno != EIO)
		errx(1, "cyclic parent rename returned %s, expected EIO",
		    strerror(errno));
}

static void
test_free_cycle(const char *path, const char *probe, uint64_t expected)
{
	struct statfs before, after;
	uint64_t delta;
	int fd;

	if (statfs(path, &before) == -1)
		err(1, "statfs before truncate");
	errno = 0;
	if (truncate(path, 0) != -1 || errno != EIO)
		errx(1, "cyclic truncate returned %s, expected EIO",
		    strerror(errno));
	if (statfs(path, &after) == -1)
		err(1, "statfs after truncate");
	delta = after.f_bfree - before.f_bfree;
	if (delta != expected)
		errx(1, "cyclic truncate freed %llu clusters, expected %llu",
		    (unsigned long long)delta, (unsigned long long)expected);
	if ((fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0644)) == -1)
		err(1, "create allocation probe");
	errno = 0;
	if (write(fd, "x", 1) != -1 || errno != EIO)
		errx(1, "allocation after corrupt free returned %s, expected EIO",
		    strerror(errno));
	(void)close(fd);
}

static void
test_dates(const char *mountpoint)
{
	static const char *invalid[] = {
		"MONTH0.TXT", "MONTH13.TXT", "MONTH15.TXT", "DAY0.TXT",
		"APRIL31.TXT", "FEB29N.TXT"
	};
	char path[PATH_MAX];
	struct stat st;
	size_t i;

	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		if (snprintf(path, sizeof(path), "%s/%s", mountpoint,
		    invalid[i]) >= (int)sizeof(path))
			errx(1, "date fixture path is too long");
		if (stat(path, &st) == -1)
			err(1, "stat %s", path);
		if (st.st_mtime != 0)
			errx(1, "%s did not map an invalid date to the epoch", path);
	}
	if (snprintf(path, sizeof(path), "%s/LEAP29.TXT", mountpoint) >=
	    (int)sizeof(path))
		errx(1, "leap date fixture path is too long");
	if (stat(path, &st) == -1)
		err(1, "stat %s", path);
	if (st.st_mtime == 0)
		errx(1, "valid 1980 leap day mapped to the epoch");
}

static void
test_full_root(const char *path)
{
	int fd;

	errno = 0;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd != -1) {
		(void)close(fd);
		errx(1, "create in a full fixed root unexpectedly succeeded");
	}
	if (errno != ENOSPC)
		errx(1, "create in a full fixed root returned %s, expected ENOSPC",
		    strerror(errno));
}

static void
test_same_size_truncate(const char *path)
{
	struct stat st;
	uint8_t buf[4096];
	off_t offset;
	ssize_t n;
	size_t i, len;
	int fd;

	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd == -1)
		err(1, "open %s", path);
	for (offset = 0; offset < SAME_SIZE_TRUNCATE_SIZE;
	    offset += (off_t)len) {
		len = sizeof(buf);
		if (len > SAME_SIZE_TRUNCATE_SIZE - (size_t)offset)
			len = SAME_SIZE_TRUNCATE_SIZE - (size_t)offset;
		for (i = 0; i < len; i++)
			buf[i] = (uint8_t)((size_t)offset + i);
		n = write(fd, buf, len);
		if (n == -1)
			err(1, "write %s", path);
		if ((size_t)n != len)
			errx(1, "short write to %s", path);
	}
	if (fstat(fd, &st) == -1)
		err(1, "fstat %s", path);
	if (st.st_size != SAME_SIZE_TRUNCATE_SIZE)
		errx(1, "%s has unexpected size %lld", path,
		    (long long)st.st_size);
	if (ftruncate(fd, st.st_size) == -1)
		err(1, "same-size ftruncate %s", path);
	if (fsync(fd) == -1)
		err(1, "fsync %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
usage(void)
{
	fprintf(stderr, "usage: msdosfs_test mutate image mutation\n"
	    "       msdosfs_test fh path\n"
	    "       msdosfs_test read-error path\n"
	    "       msdosfs_test bad-dir mountpoint\n"
	    "       msdosfs_test parent-cycle source target\n"
	    "       msdosfs_test free-cycle path probe expected-clusters\n"
	    "       msdosfs_test dates mountpoint\n"
	    "       msdosfs_test full-root path\n"
	    "       msdosfs_test same-size-truncate path\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	struct fat_image fs;

	if (argc == 4 && strcmp(argv[1], "mutate") == 0) {
		open_image(&fs, argv[2]);
		mutate(&fs, argv[3]);
		if (close(fs.fd) == -1)
			err(1, "close image");
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "fh") == 0)
		test_fh(argv[2]);
	else if (argc == 3 && strcmp(argv[1], "read-error") == 0)
		test_read_error(argv[2]);
	else if (argc == 3 && strcmp(argv[1], "bad-dir") == 0)
		test_bad_dir(argv[2]);
	else if (argc == 4 && strcmp(argv[1], "parent-cycle") == 0)
		test_parent_cycle(argv[2], argv[3]);
	else if (argc == 5 && strcmp(argv[1], "free-cycle") == 0)
		test_free_cycle(argv[2], argv[3], number(argv[4], "cluster count"));
	else if (argc == 3 && strcmp(argv[1], "dates") == 0)
		test_dates(argv[2]);
	else if (argc == 3 && strcmp(argv[1], "full-root") == 0)
		test_full_root(argv[2]);
	else if (argc == 3 &&
	    strcmp(argv[1], "same-size-truncate") == 0)
		test_same_size_truncate(argv[2]);
	else
		usage();
	return 0;
}
