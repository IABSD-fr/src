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

/* Regression tests for makefs(8) MSDOS image creation. */

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMAGE_SIZE	(64LL * 1024 * 1024)
#define CONTAINER_SIZE	(128LL * 1024 * 1024)
#define IMAGE_SIZE_ARG	"64m"
#define IMAGE_OFFSET	4096
#define IMAGE_OFFSET_ARG "4096"

#define FAT32_MASK	0x0fffffffU
#define FAT32_RESERVED0	0x0ffffff0U
#define FAT32_RESERVED1	0x0fffffffU

struct bpb {
	uint16_t bps;
	uint16_t res;
	uint16_t sec;
	uint32_t bsec;
	uint32_t bspf;
	uint16_t infs;
	uint16_t bkbs;
};

static char workdir[PATH_MAX];
static char sourcedir[PATH_MAX];
static char default_makefs[] = "/usr/sbin/makefs";
static char *makefs_path;

static void
make_path(char *path, size_t pathlen, const char *name)
{
	int n;

	n = snprintf(path, pathlen, "%s/%s", workdir, name);
	if (n < 0 || (size_t)n >= pathlen)
		errx(1, "temporary path is too long");
}

static void
cleanup(void)
{
	static const char *const names[] = {
		"sector-size.img",
		"offset-size.img",
		"failure-status.img",
		"fsinfo-overlap.img",
		"bootstrap-offset.img",
		"timestamp-1.img",
		"timestamp-2.img"
	};
	char path[PATH_MAX];
	size_t i;

	if (workdir[0] == '\0')
		return;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		make_path(path, sizeof(path), names[i]);
		(void)unlink(path);
	}
	if (sourcedir[0] != '\0') {
		if (snprintf(path, sizeof(path), "%s/boot.bin", sourcedir) > 0)
			(void)unlink(path);
		(void)rmdir(sourcedir);
	}
	(void)rmdir(workdir);
}

static void
setup(void)
{
	const char *path;

	if (strlcpy(workdir, "/tmp/mkfs_msdos.XXXXXXXXXX",
	    sizeof(workdir)) >= sizeof(workdir))
		errx(1, "temporary directory template is too long");
	if (mkdtemp(workdir) == NULL)
		err(1, "mkdtemp");
	if (atexit(cleanup) != 0)
		errx(1, "atexit");
	make_path(sourcedir, sizeof(sourcedir), "source");
	if (mkdir(sourcedir, 0700) == -1)
		err(1, "mkdir %s", sourcedir);

	path = getenv("MAKEFS");
	makefs_path = path != NULL && path[0] != '\0' ?
	    (char *)path : default_makefs;
	if (access(makefs_path, X_OK) == -1)
		err(1, "%s", makefs_path);
}

static int
run_makefs(char *const argv[])
{
	pid_t pid, rv;
	int status;

	if ((pid = fork()) == -1)
		err(1, "fork");
	if (pid == 0) {
		execv(makefs_path, argv);
		warn("execv %s", makefs_path);
		_exit(127);
	}
	do {
		rv = waitpid(pid, &status, 0);
	} while (rv == -1 && errno == EINTR);
	if (rv == -1)
		err(1, "waitpid");
	if (WIFSIGNALED(status))
		errx(1, "%s terminated by signal %d", makefs_path,
		    WTERMSIG(status));
	if (!WIFEXITED(status))
		errx(1, "%s returned an unknown wait status", makefs_path);
	status = WEXITSTATUS(status);
	if (status == 127)
		errx(1, "could not execute %s", makefs_path);
	return status;
}

static void
expect_success(int status, const char *test)
{
	if (status != 0)
		errx(1, "%s: makefs exited with status %d", test, status);
}

static void
read_exact(const char *path, void *buf, size_t len, off_t offset)
{
	uint8_t *p = buf;
	ssize_t n;
	int fd;

	if ((fd = open(path, O_RDONLY)) == -1)
		err(1, "open %s", path);
	while (len != 0) {
		n = pread(fd, p, len, offset);
		if (n == -1 && errno == EINTR)
			continue;
		if (n == -1)
			err(1, "pread %s", path);
		if (n == 0)
			errx(1, "%s: short read at %lld", path,
			    (long long)offset);
		p += n;
		len -= n;
		offset += n;
	}
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t
get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
read_bpb(const char *path, off_t offset, struct bpb *bpb)
{
	uint8_t buf[64];

	read_exact(path, buf, sizeof(buf), offset);
	bpb->bps = get16(buf + 11);
	bpb->res = get16(buf + 14);
	bpb->sec = get16(buf + 19);
	bpb->bsec = get32(buf + 32);
	bpb->bspf = get32(buf + 36);
	bpb->infs = get16(buf + 48);
	bpb->bkbs = get16(buf + 50);
	if (bpb->bps == 0)
		errx(1, "%s: zero bytes per sector", path);
}

static uint32_t
total_sectors(const struct bpb *bpb)
{
	return bpb->sec != 0 ? bpb->sec : bpb->bsec;
}

static void
create_sparse(const char *path, off_t size)
{
	int fd;

	if ((fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600)) == -1)
		err(1, "open %s", path);
	if (ftruncate(fd, size) == -1)
		err(1, "ftruncate %s", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
write_bootstrap(const char *path)
{
	uint8_t buf[512];
	ssize_t n;
	int fd;

	memset(buf, 0, sizeof(buf));
	buf[0] = 0xeb;
	buf[1] = 0x3c;
	buf[2] = 0x90;
	buf[510] = 0x55;
	buf[511] = 0xaa;
	if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600)) == -1)
		err(1, "open %s", path);
	do {
		n = write(fd, buf, sizeof(buf));
	} while (n == -1 && errno == EINTR);
	if (n == -1)
		err(1, "write %s", path);
	if ((size_t)n != sizeof(buf))
		errx(1, "%s: short write", path);
	if (close(fd) == -1)
		err(1, "close %s", path);
}

static void
compare_images(const char *path1, const char *path2)
{
	uint8_t buf1[65536], buf2[65536];
	struct stat st1, st2;
	off_t offset;
	size_t len;

	if (stat(path1, &st1) == -1)
		err(1, "stat %s", path1);
	if (stat(path2, &st2) == -1)
		err(1, "stat %s", path2);
	if (st1.st_size != st2.st_size)
		errx(1, "timestamp: image sizes differ (%lld != %lld)",
		    (long long)st1.st_size, (long long)st2.st_size);
	for (offset = 0; offset < st1.st_size; offset += len) {
		len = sizeof(buf1);
		if ((off_t)len > st1.st_size - offset)
			len = st1.st_size - offset;
		read_exact(path1, buf1, len, offset);
		read_exact(path2, buf2, len, offset);
		if (memcmp(buf1, buf2, len) != 0)
			errx(1, "timestamp: images differ at offset %lld",
			    (long long)offset);
	}
}

static void
test_sector_size(void)
{
	char image[PATH_MAX];
	struct bpb bpb;
	char *argv[] = { makefs_path, "-t", "msdos", "-s", IMAGE_SIZE_ARG,
	    "-S", "4096", image, sourcedir, NULL };

	make_path(image, sizeof(image), "sector-size.img");
	expect_success(run_makefs(argv), "sector-size");
	read_bpb(image, 0, &bpb);
	if (bpb.bps != 4096)
		errx(1, "sector-size: BPB reports %u bytes per sector", bpb.bps);
	if (total_sectors(&bpb) != IMAGE_SIZE / bpb.bps)
		errx(1, "sector-size: BPB reports %u sectors, expected %lld",
		    total_sectors(&bpb), (long long)(IMAGE_SIZE / bpb.bps));
}

static void
test_offset_size(void)
{
	char image[PATH_MAX];
	struct bpb bpb;
	char *argv[] = { makefs_path, "-t", "msdos", "-s", IMAGE_SIZE_ARG,
	    "-O", IMAGE_OFFSET_ARG, image, sourcedir, NULL };

	make_path(image, sizeof(image), "offset-size.img");
	create_sparse(image, CONTAINER_SIZE);
	expect_success(run_makefs(argv), "offset-size");
	read_bpb(image, IMAGE_OFFSET, &bpb);
	if (total_sectors(&bpb) != IMAGE_SIZE / bpb.bps)
		errx(1, "offset-size: BPB reports %u sectors, expected %lld",
		    total_sectors(&bpb), (long long)(IMAGE_SIZE / bpb.bps));
}

static void
test_failure_status(void)
{
	char image[PATH_MAX];
	int status;
	char *argv[] = { makefs_path, "-t", "msdos", "-s", IMAGE_SIZE_ARG,
	    "-o", "block_size=4096,sectors_per_cluster=1", image,
	    sourcedir, NULL };

	make_path(image, sizeof(image), "failure-status.img");
	status = run_makefs(argv);
	if (status == 0)
		errx(1, "failure-status: invalid options returned success");
}

static void
test_fsinfo_overlap(void)
{
	char image[PATH_MAX];
	struct bpb bpb;
	uint8_t fat[12];
	int status;
	char *argv[] = { makefs_path, "-t", "msdos", "-s", IMAGE_SIZE_ARG,
	    "-o", "fat_type=32,reserved_sectors=10,backup_sector=9,"
	    "info_sector=1,sectors_per_cluster=1", image, sourcedir, NULL };

	make_path(image, sizeof(image), "fsinfo-overlap.img");
	status = run_makefs(argv);
	if (status != 0)
		return;
	read_bpb(image, 0, &bpb);
	if (bpb.bkbs != UINT16_MAX && bpb.infs != UINT16_MAX &&
	    (uint32_t)bpb.bkbs + bpb.infs >= bpb.res)
		errx(1, "fsinfo-overlap: backup FSInfo sector %u is not reserved",
		    bpb.bkbs + bpb.infs);
	read_exact(image, fat, sizeof(fat), (off_t)bpb.res * bpb.bps);
	if ((get32(fat) & FAT32_MASK) != FAT32_RESERVED0 ||
	    (get32(fat + 4) & FAT32_MASK) != FAT32_RESERVED1 ||
	    (get32(fat + 8) & FAT32_MASK) != FAT32_RESERVED1)
		errx(1, "fsinfo-overlap: first FAT has invalid reserved entries");
}

static void
test_bootstrap_offset(void)
{
	char boot[PATH_MAX], image[PATH_MAX], option[PATH_MAX + 32];
	struct bpb bpb;
	uint8_t *primary, *backup;
	int n;
	char *argv[] = { makefs_path, "-t", "msdos", "-s", IMAGE_SIZE_ARG,
	    "-O", IMAGE_OFFSET_ARG, "-o", option, image, sourcedir, NULL };

	if ((n = snprintf(boot, sizeof(boot), "%s/boot.bin", sourcedir)) < 0 ||
	    (size_t)n >= sizeof(boot))
		errx(1, "bootstrap path is too long");
	write_bootstrap(boot);
	if ((n = snprintf(option, sizeof(option),
	    "fat_type=32,sectors_per_cluster=1,bootstrap=%s", boot)) < 0 ||
	    (size_t)n >= sizeof(option))
		errx(1, "bootstrap option is too long");
	make_path(image, sizeof(image), "bootstrap-offset.img");
	expect_success(run_makefs(argv), "bootstrap-offset");
	read_bpb(image, IMAGE_OFFSET, &bpb);
	if (bpb.bkbs == UINT16_MAX)
		errx(1, "bootstrap-offset: no backup boot sector");
	primary = malloc(bpb.bps);
	backup = malloc(bpb.bps);
	if (primary == NULL || backup == NULL)
		err(1, "malloc");
	read_exact(image, primary, bpb.bps, IMAGE_OFFSET);
	read_exact(image, backup, bpb.bps,
	    IMAGE_OFFSET + (off_t)bpb.bkbs * bpb.bps);
	if (memcmp(primary, backup, bpb.bps) != 0)
		errx(1, "bootstrap-offset: backup boot sector differs from primary");
	free(primary);
	free(backup);
}

static void
test_timestamp(void)
{
	char image1[PATH_MAX], image2[PATH_MAX];
	char *argv1[] = { makefs_path, "-t", "msdos", "-s", IMAGE_SIZE_ARG,
	    "-T", "0", image1, sourcedir, NULL };
	char *argv2[] = { makefs_path, "-t", "msdos", "-s", IMAGE_SIZE_ARG,
	    "-T", "0", image2, sourcedir, NULL };

	make_path(image1, sizeof(image1), "timestamp-1.img");
	make_path(image2, sizeof(image2), "timestamp-2.img");
	expect_success(run_makefs(argv1), "timestamp");
	expect_success(run_makefs(argv2), "timestamp");
	compare_images(image1, image2);
}

int
main(int argc, char *argv[])
{
	if (argc != 2)
		errx(1, "usage: %s test", argv[0]);
	setup();
	if (strcmp(argv[1], "sector-size") == 0)
		test_sector_size();
	else if (strcmp(argv[1], "offset-size") == 0)
		test_offset_size();
	else if (strcmp(argv[1], "failure-status") == 0)
		test_failure_status();
	else if (strcmp(argv[1], "fsinfo-overlap") == 0)
		test_fsinfo_overlap();
	else if (strcmp(argv[1], "bootstrap-offset") == 0)
		test_bootstrap_offset();
	else if (strcmp(argv[1], "timestamp") == 0)
		test_timestamp();
	else
		errx(1, "unknown test: %s", argv[1]);
	return 0;
}
