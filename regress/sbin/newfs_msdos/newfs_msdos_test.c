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

/* Regression tests for newfs_msdos(8). */

#include <sys/types.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>

int test_getmntinfo(struct statfs **, int);
int test_ioctl(int, unsigned long, ...);

/* Exercise internal safety checks without using a real mounted device. */
#define getmntinfo test_getmntinfo
#define ioctl test_ioctl
#define main newfs_msdos_program_main
#include "../../../sbin/newfs_msdos/newfs_msdos.c"
#undef main
#undef ioctl
#undef getmntinfo

#define FAT32_MASK		0x0fffffffU
#define FAT32_RESERVED0		0x0ffffff0U
#define FAT32_RESERVED1		0x0fffffffU
#define FAT32_SECTORS		131072U
#define SECTOR_SIZE		512U
#define IMAGE_SIZE		((off_t)FAT32_SECTORS * SECTOR_SIZE)
#define BOOTSTRAP_SECTORS	20U

struct parsed_bpb {
	uint16_t bps;
	uint8_t spc;
	uint16_t res;
	uint8_t nft;
	uint16_t rde;
	uint16_t sec;
	uint16_t spf;
	uint32_t bsec;
	uint32_t bspf;
	uint16_t infs;
	uint16_t bkbs;
};

static struct statfs mock_mount;
static struct disklabel mock_label;
static char workdir[PATH_MAX];
static char alias_path[PATH_MAX];
static char default_newfs_msdos[] = "/sbin/newfs_msdos";
static char *newfs_msdos_path;

int
test_getmntinfo(struct statfs **mntbufp, int flags)
{
	(void)flags;
	*mntbufp = &mock_mount;
	return 1;
}

int
test_ioctl(int fd, unsigned long request, ...)
{
	struct disklabel *label;
	va_list ap;

	(void)fd;
	if (request != DIOCGDINFO) {
		errno = ENOTTY;
		return -1;
	}
	va_start(ap, request);
	label = va_arg(ap, struct disklabel *);
	va_end(ap);
	*label = mock_label;
	return 0;
}

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
		"alias", "fat-capacity.img", "fsinfo-overlap.img",
		"fat32-capacity.img",
		"bootstrap-reservation.img", "boot.bin",
		"sector-count-overflow.img", "cluster-size.img", "vd0a",
		"progress.img",
		"default-0.img", "default-1.img", "default-2.img",
		"default-3.img", "default-4.img"
	};
	char path[PATH_MAX];
	size_t i;

	if (workdir[0] == '\0')
		return;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		make_path(path, sizeof(path), names[i]);
		(void)unlink(path);
	}
	(void)rmdir(workdir);
}

static void
setup(void)
{
	const char *path;

	if (strlcpy(workdir, "/tmp/newfs_msdos.XXXXXXXXXX",
	    sizeof(workdir)) >= sizeof(workdir))
		errx(1, "temporary directory template is too long");
	if (mkdtemp(workdir) == NULL)
		err(1, "mkdtemp");
	if (atexit(cleanup) != 0)
		errx(1, "atexit");
	path = getenv("NEWFS_MSDOS");
	newfs_msdos_path = path != NULL && path[0] != '\0' ?
	    (char *)path : default_newfs_msdos;
	if (access(newfs_msdos_path, X_OK) == -1)
		err(1, "%s", newfs_msdos_path);
}

static int
run_newfs(char *const argv[])
{
	struct rlimit limit;
	pid_t pid, rv;
	int status;

	if ((pid = fork()) == -1)
		err(1, "fork");
	if (pid == 0) {
		limit.rlim_cur = limit.rlim_max = 0;
		(void)setrlimit(RLIMIT_CORE, &limit);
		execv(newfs_msdos_path, argv);
		warn("execv %s", newfs_msdos_path);
		_exit(127);
	}
	do {
		rv = waitpid(pid, &status, 0);
	} while (rv == -1 && errno == EINTR);
	if (rv == -1)
		err(1, "waitpid");
	return status;
}

static int
run_newfs_capture(char *const argv[], int terminal, char *output,
    size_t outputlen)
{
	struct rlimit limit;
	char discard[1024];
	pid_t pid, rv;
	ssize_t n;
	size_t used;
	int devnull, fd, pipefd[2], status, truncated;

	if (outputlen == 0)
		errx(1, "capture buffer is empty");
	if (terminal) {
		if ((pid = forkpty(&fd, NULL, NULL, NULL)) == -1)
			err(1, "forkpty");
	} else {
		if (pipe(pipefd) == -1)
			err(1, "pipe");
		if ((pid = fork()) == -1)
			err(1, "fork");
		if (pid == 0) {
			(void)close(pipefd[0]);
			if (dup2(pipefd[1], STDERR_FILENO) == -1)
				err(1, "dup2 stderr");
			(void)close(pipefd[1]);
			if ((devnull = open(_PATH_DEVNULL, O_WRONLY)) == -1)
				err(1, "open %s", _PATH_DEVNULL);
			if (dup2(devnull, STDOUT_FILENO) == -1)
				err(1, "dup2 stdout");
			(void)close(devnull);
		} else {
			(void)close(pipefd[1]);
			fd = pipefd[0];
		}
	}
	if (pid == 0) {
		limit.rlim_cur = limit.rlim_max = 0;
		(void)setrlimit(RLIMIT_CORE, &limit);
		execv(newfs_msdos_path, argv);
		warn("execv %s", newfs_msdos_path);
		_exit(127);
	}
	used = 0;
	truncated = 0;
	for (;;) {
		if (used < outputlen - 1)
			n = read(fd, output + used, outputlen - used - 1);
		else
			n = read(fd, discard, sizeof(discard));
		if (n == -1 && errno == EINTR)
			continue;
		if (n == -1 && terminal && errno == EIO)
			break;
		if (n == -1)
			err(1, "read formatter output");
		if (n == 0)
			break;
		if (used < outputlen - 1)
			used += n;
		else
			truncated = 1;
	}
	output[used] = '\0';
	if (close(fd) == -1)
		err(1, "close formatter output");
	do {
		rv = waitpid(pid, &status, 0);
	} while (rv == -1 && errno == EINTR);
	if (rv == -1)
		err(1, "waitpid");
	if (truncated)
		errx(1, "formatter output exceeds capture buffer");
	return status;
}

static int
exit_status(int status, const char *test)
{
	if (WIFSIGNALED(status))
		errx(1, "%s: %s terminated by signal %d", test,
		    newfs_msdos_path, WTERMSIG(status));
	if (!WIFEXITED(status))
		errx(1, "%s: %s returned an unknown wait status", test,
		    newfs_msdos_path);
	if (WEXITSTATUS(status) == 127)
		errx(1, "%s: could not execute %s", test, newfs_msdos_path);
	return WEXITSTATUS(status);
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
write_zeros(const char *path, size_t size)
{
	uint8_t buf[SECTOR_SIZE];
	size_t chunk, left;
	ssize_t n;
	int fd;

	memset(buf, 0, sizeof(buf));
	if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600)) == -1)
		err(1, "open %s", path);
	for (left = size; left != 0; left -= (size_t)n) {
		chunk = MINIMUM(left, sizeof(buf));
		do {
			n = write(fd, buf, chunk);
		} while (n == -1 && errno == EINTR);
		if (n == -1)
			err(1, "write %s", path);
		if (n == 0)
			errx(1, "%s: short write", path);
	}
	if (close(fd) == -1)
		err(1, "close %s", path);
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
read_bpb(const char *path, struct parsed_bpb *bpb)
{
	uint8_t buf[64];

	read_exact(path, buf, sizeof(buf), 0);
	bpb->bps = get16(buf + 11);
	bpb->spc = buf[13];
	bpb->res = get16(buf + 14);
	bpb->nft = buf[16];
	bpb->rde = get16(buf + 17);
	bpb->sec = get16(buf + 19);
	bpb->spf = get16(buf + 22);
	bpb->bsec = get32(buf + 32);
	bpb->bspf = get32(buf + 36);
	bpb->infs = get16(buf + 48);
	bpb->bkbs = get16(buf + 50);
	if (bpb->bps == 0 || bpb->spc == 0)
		errx(1, "%s: invalid BPB geometry", path);
}

static uint32_t
total_sectors(const struct parsed_bpb *bpb)
{
	return bpb->sec != 0 ? bpb->sec : bpb->bsec;
}

static void
check_fat32_start(const char *test, const char *path,
    const struct parsed_bpb *bpb)
{
	uint8_t fat[12];

	read_exact(path, fat, sizeof(fat), (off_t)bpb->res * bpb->bps);
	if ((get32(fat) & FAT32_MASK) != FAT32_RESERVED0 ||
	    (get32(fat + 4) & FAT32_MASK) != FAT32_RESERVED1 ||
	    (get32(fat + 8) & FAT32_MASK) != FAT32_RESERVED1)
		errx(1, "%s: first FAT has invalid reserved entries", test);
}

static void
mount_alias_child(void)
{
	struct stat sb;

	if (stat(alias_path, &sb) == -1)
		err(1, "stat %s", alias_path);
	check_mounted(alias_path, &sb);
}

static void
disklabel_range_child(void)
{
	struct bpb bpb;
	int fd;

	memset(&mock_label, 0, sizeof(mock_label));
	mock_label.d_secsize = SECTOR_SIZE;
	mock_label.d_npartitions = 1;
	DL_SETPSIZE(&mock_label.d_partitions[0], 4096);
	DL_SETPOFFSET(&mock_label.d_partitions[0], UINT64_C(0x100000000));
	memset(&bpb, 0, sizeof(bpb));
	bpb.bps = SECTOR_SIZE;
	bpb.spt = bpb.hds = 1;
	if ((fd = open(_PATH_DEVNULL, O_RDONLY)) == -1)
		err(1, "open %s", _PATH_DEVNULL);
	getdiskinfo(fd, "sd0a", NULL, 0, &bpb);
}

static void
expect_internal_rejection(void (*function)(void), const char *test)
{
	pid_t pid, rv;
	int devnull, status;

	if ((pid = fork()) == -1)
		err(1, "fork");
	if (pid == 0) {
		if ((devnull = open(_PATH_DEVNULL, O_WRONLY)) != -1) {
			(void)dup2(devnull, STDERR_FILENO);
			(void)close(devnull);
		}
		function();
		_exit(0);
	}
	do {
		rv = waitpid(pid, &status, 0);
	} while (rv == -1 && errno == EINTR);
	if (rv == -1)
		err(1, "waitpid");
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
		return;
	if (WIFSIGNALED(status))
		errx(1, "%s: safety check terminated by signal %d", test,
		    WTERMSIG(status));
	if (!WIFEXITED(status))
		errx(1, "%s: safety check returned an unknown wait status", test);
	errx(1, "%s: unsafe input was accepted", test);
}

static void
test_mount_alias(void)
{
	make_path(alias_path, sizeof(alias_path), "alias");
	if (symlink(_PATH_DEVNULL, alias_path) == -1)
		err(1, "symlink %s", alias_path);
	memset(&mock_mount, 0, sizeof(mock_mount));
	if (strlcpy(mock_mount.f_mntfromname, _PATH_DEVNULL,
	    sizeof(mock_mount.f_mntfromname)) >=
	    sizeof(mock_mount.f_mntfromname))
		errx(1, "mock mount path is too long");
	expect_internal_rejection(mount_alias_child, "mount-alias");
}

static void
test_disklabel_units(void)
{
	struct bpb bpb;
	int fd;

	memset(&mock_label, 0, sizeof(mock_label));
	mock_label.d_secsize = SECTOR_SIZE;
	mock_label.d_npartitions = 1;
	DL_SETPSIZE(&mock_label.d_partitions[0], 4096);
	DL_SETPOFFSET(&mock_label.d_partitions[0], 2048);
	memset(&bpb, 0, sizeof(bpb));
	bpb.bps = 1024;
	bpb.spt = bpb.hds = 1;
	if ((fd = open(_PATH_DEVNULL, O_RDONLY)) == -1)
		err(1, "open %s", _PATH_DEVNULL);
	getdiskinfo(fd, "sd0a", NULL, 0, &bpb);
	if (close(fd) == -1)
		err(1, "close %s", _PATH_DEVNULL);
	if (bpb.bsec != 2048 || bpb.hid != 1024)
		errx(1, "disklabel-units: got size %u and offset %u sectors, "
		    "expected 2048 and 1024", bpb.bsec, bpb.hid);
}

static void
test_disklabel_range(void)
{
	expect_internal_rejection(disklabel_range_child, "disklabel-range");
}

static void
test_fat_capacity(void)
{
	char image[PATH_MAX];
	struct parsed_bpb bpb;
	uint64_t capacity, clusters, data, metadata, root;
	int status;
	char *argv[] = { newfs_msdos_path, "-F", "16", "-a", "16",
	    "-c", "1", "-h", "1", "-u", "1", "-S", "512",
	    "-s", "10000", "-o", "0", "-I", "1", image, NULL };
	char *fat32_argv[] = { newfs_msdos_path, "-F", "32", "-c", "1",
	    "-h", "1", "-u", "1", "-S", "512", "-s", "131072",
	    "-o", "0", "-I", "1", image, NULL };

	make_path(image, sizeof(image), "fat-capacity.img");
	create_sparse(image, (off_t)10000 * SECTOR_SIZE);
	status = exit_status(run_newfs(argv), "fat-capacity");
	if (status == 0)
		errx(1, "fat-capacity: accepted an undersized FAT16");

	make_path(image, sizeof(image), "fat32-capacity.img");
	create_sparse(image, IMAGE_SIZE);
	status = exit_status(run_newfs(fat32_argv), "fat32-capacity");
	if (status != 0)
		errx(1, "fat32-capacity: formatter rejected valid geometry");
	read_bpb(image, &bpb);
	root = ((uint64_t)bpb.rde * 32 + bpb.bps - 1) / bpb.bps;
	metadata = bpb.res + (uint64_t)bpb.nft * bpb.bspf + root;
	if (metadata >= total_sectors(&bpb))
		errx(1, "fat32-capacity: metadata exceeds file system size");
	data = total_sectors(&bpb) - metadata;
	clusters = data / bpb.spc;
	capacity = (uint64_t)bpb.bspf * bpb.bps / 4 - 2;
	if (clusters > capacity)
		errx(1, "fat32-capacity: BPB describes %llu clusters but the FAT "
		    "holds only %llu", (unsigned long long)clusters,
		    (unsigned long long)capacity);
}

static void
test_fsinfo_overlap(void)
{
	char image[PATH_MAX];
	struct parsed_bpb bpb;
	int status;
	char *argv[] = { newfs_msdos_path, "-F", "32", "-c", "1",
	    "-h", "1", "-u", "1", "-S", "512", "-s", "131072",
	    "-o", "0", "-r", "10", "-i", "1", "-k", "9",
	    "-I", "1", image, NULL };

	make_path(image, sizeof(image), "fsinfo-overlap.img");
	create_sparse(image, IMAGE_SIZE);
	status = exit_status(run_newfs(argv), "fsinfo-overlap");
	if (status != 0)
		return;
	read_bpb(image, &bpb);
	if (bpb.bkbs != UINT16_MAX && bpb.infs != UINT16_MAX &&
	    (uint32_t)bpb.bkbs + bpb.infs >= bpb.res)
		errx(1, "fsinfo-overlap: backup FSInfo sector %u is outside "
		    "the reserved area", bpb.bkbs + bpb.infs);
	check_fat32_start("fsinfo-overlap", image, &bpb);
}

static void
test_bootstrap_reservation(void)
{
	char boot[PATH_MAX], image[PATH_MAX];
	struct parsed_bpb bpb;
	int status;
	char *argv[] = { newfs_msdos_path, "-F", "32", "-c", "1",
	    "-h", "1", "-u", "1", "-S", "512", "-s", "131072",
	    "-o", "0", "-B", boot, "-I", "1", image, NULL };

	make_path(boot, sizeof(boot), "boot.bin");
	make_path(image, sizeof(image), "bootstrap-reservation.img");
	write_zeros(boot, BOOTSTRAP_SECTORS * SECTOR_SIZE);
	create_sparse(image, IMAGE_SIZE);
	status = exit_status(run_newfs(argv), "bootstrap-reservation");
	if (status != 0)
		return;
	read_bpb(image, &bpb);
	if (BOOTSTRAP_SECTORS > bpb.res ||
	    (bpb.infs != UINT16_MAX && bpb.infs >= bpb.res) ||
	    (bpb.bkbs != UINT16_MAX &&
	    (uint32_t)bpb.bkbs + BOOTSTRAP_SECTORS > bpb.res) ||
	    (bpb.bkbs != UINT16_MAX && bpb.infs != UINT16_MAX &&
	    (uint32_t)bpb.bkbs + bpb.infs >= bpb.res))
		errx(1, "bootstrap-reservation: FAT32 metadata extends beyond "
		    "%u reserved sectors", bpb.res);
	check_fat32_start("bootstrap-reservation", image, &bpb);
}

static void
test_sector_count_overflow(void)
{
	char image[PATH_MAX];
	struct parsed_bpb bpb;
	int status;
	char *argv[] = { newfs_msdos_path, "-F", "32", "-c", "1",
	    "-h", "1", "-u", "1", "-S", "512", "-s", "131072",
	    "-o", "4294836224", "-I", "1", image, NULL };

	make_path(image, sizeof(image), "sector-count-overflow.img");
	create_sparse(image, IMAGE_SIZE);
	status = exit_status(run_newfs(argv), "sector-count-overflow");
	if (status != 0)
		return;
	read_bpb(image, &bpb);
	if (bpb.sec != 0 || bpb.bsec != FAT32_SECTORS)
		errx(1, "sector-count-overflow: BPB has sec=%u and bsec=%u",
		    bpb.sec, bpb.bsec);
}

static void
test_cluster_size(void)
{
	char image[PATH_MAX];
	int status;
	char *argv[] = { newfs_msdos_path, "-N", "-F", "16", "-c", "4",
	    "-h", "1", "-u", "1", "-S", "32768", "-s", "20000",
	    "-o", "0", image, NULL };

	make_path(image, sizeof(image), "cluster-size.img");
	create_sparse(image, 1);
	status = exit_status(run_newfs(argv), "cluster-size");
	if (status == 0)
		errx(1, "cluster-size: accepted a 131072-byte cluster that "
		    "msdosfs cannot mount");
}

static u_int
fat_type(const struct parsed_bpb *bpb)
{
	uint64_t clusters, metadata, root;
	uint32_t fat_sectors;

	if (bpb->rde == 0)
		return 32;
	fat_sectors = bpb->spf != 0 ? bpb->spf : bpb->bspf;
	root = ((uint64_t)bpb->rde * 32 + bpb->bps - 1) / bpb->bps;
	metadata = bpb->res + (uint64_t)bpb->nft * fat_sectors + root;
	if (metadata >= total_sectors(bpb))
		errx(1, "ms-defaults: metadata exceeds file system size");
	clusters = (total_sectors(bpb) - metadata) / bpb->spc;
	return clusters < 4085 ? 12 : 16;
}

static void
check_ms_default(u_int index, uint32_t sectors, u_int expected_fat,
    u_int expected_cluster)
{
	char image[PATH_MAX], name[32], sector_arg[32];
	struct parsed_bpb bpb;
	int n, status;
	char *argv[] = { newfs_msdos_path, "-h", "1", "-u", "1",
	    "-S", "512", "-s", sector_arg, "-o", "0", "-I", "1",
	    image, NULL };

	n = snprintf(name, sizeof(name), "default-%u.img", index);
	if (n < 0 || (size_t)n >= sizeof(name))
		errx(1, "ms-defaults: image name is too long");
	n = snprintf(sector_arg, sizeof(sector_arg), "%u", sectors);
	if (n < 0 || (size_t)n >= sizeof(sector_arg))
		errx(1, "ms-defaults: sector count is too long");
	make_path(image, sizeof(image), name);
	create_sparse(image, (off_t)sectors * SECTOR_SIZE);
	status = exit_status(run_newfs(argv), "ms-defaults");
	if (status != 0)
		errx(1, "ms-defaults: formatter rejected %u sectors", sectors);
	read_bpb(image, &bpb);
	if (fat_type(&bpb) != expected_fat)
		errx(1, "ms-defaults: %u sectors produced FAT%u, expected FAT%u",
		    sectors, fat_type(&bpb), expected_fat);
	if ((u_int)bpb.bps * bpb.spc != expected_cluster)
		errx(1, "ms-defaults: %u sectors produced %u-byte clusters, "
		    "expected %u", sectors, bpb.bps * bpb.spc,
		    expected_cluster);
	if (expected_fat == 32 &&
	    (bpb.res != 32 || bpb.infs != 1 || bpb.bkbs != 6))
		errx(1, "ms-defaults: FAT32 metadata is res=%u infs=%u bkbs=%u",
		    bpb.res, bpb.infs, bpb.bkbs);
}

static void
test_ms_defaults(void)
{
	check_ms_default(0, 8000, 12, 1024);
	check_ms_default(1, 204800, 16, 2048);
	check_ms_default(2, 1048576, 32, 4096);
	check_ms_default(3, 16777217, 32, 8192);
	check_ms_default(4, 67108865, 32, 32768);
}

static void
test_progress(void)
{
	char image[PATH_MAX], output[32768];
	int status;
	char *argv[] = { newfs_msdos_path, "-F", "32", "-c", "1",
	    "-h", "1", "-u", "1", "-S", "512", "-s", "131072",
	    "-o", "0", "-I", "1", image, NULL };
	char *quiet_argv[] = { newfs_msdos_path, "-q", "-F", "32",
	    "-c", "1", "-h", "1", "-u", "1", "-S", "512", "-s",
	    "131072", "-o", "0", "-I", "1", image, NULL };

	make_path(image, sizeof(image), "progress.img");
	create_sparse(image, IMAGE_SIZE);
	status = exit_status(run_newfs_capture(argv, 1, output, sizeof(output)),
	    "progress-tty");
	if (status != 0)
		errx(1, "progress-tty: formatter failed");
	if (strstr(output, "Formatting: [") == NULL ||
	    strstr(output, "] 100%") == NULL)
		errx(1, "progress-tty: missing completed progress bar");

	status = exit_status(run_newfs_capture(argv, 0, output,
	    sizeof(output)), "progress-pipe");
	if (status != 0)
		errx(1, "progress-pipe: formatter failed");
	if (strstr(output, "Formatting:") != NULL)
		errx(1, "progress-pipe: progress was written to a pipe");

	status = exit_status(run_newfs_capture(quiet_argv, 1, output,
	    sizeof(output)), "progress-quiet");
	if (status != 0)
		errx(1, "progress-quiet: formatter failed");
	if (strstr(output, "Formatting:") != NULL)
		errx(1, "progress-quiet: -q did not suppress progress");
}

static void
test_disktab_fallback(void)
{
	char image[PATH_MAX];
	int status;
	char *argv[] = { newfs_msdos_path, "-N", image, "rdroot", NULL };

	make_path(image, sizeof(image), "vd0a");
	create_sparse(image, 1);
	status = run_newfs(argv);
	if (WIFSIGNALED(status))
		errx(1, "disktab-fallback: %s terminated by signal %d",
		    newfs_msdos_path, WTERMSIG(status));
	if (!WIFEXITED(status) || WEXITSTATUS(status) == 127)
		errx(1, "disktab-fallback: could not run %s", newfs_msdos_path);
}

int
main(int argc, char *argv[])
{
	if (argc != 2)
		errx(1, "usage: %s test", argv[0]);
	setup();
	if (strcmp(argv[1], "mount-alias") == 0)
		test_mount_alias();
	else if (strcmp(argv[1], "disklabel-units") == 0)
		test_disklabel_units();
	else if (strcmp(argv[1], "disklabel-range") == 0)
		test_disklabel_range();
	else if (strcmp(argv[1], "fat-capacity") == 0)
		test_fat_capacity();
	else if (strcmp(argv[1], "fsinfo-overlap") == 0)
		test_fsinfo_overlap();
	else if (strcmp(argv[1], "bootstrap-reservation") == 0)
		test_bootstrap_reservation();
	else if (strcmp(argv[1], "sector-count-overflow") == 0)
		test_sector_count_overflow();
	else if (strcmp(argv[1], "cluster-size") == 0)
		test_cluster_size();
	else if (strcmp(argv[1], "ms-defaults") == 0)
		test_ms_defaults();
	else if (strcmp(argv[1], "progress") == 0)
		test_progress();
	else if (strcmp(argv[1], "disktab-fallback") == 0)
		test_disktab_fallback();
	else
		errx(1, "unknown test: %s", argv[1]);
	return 0;
}
