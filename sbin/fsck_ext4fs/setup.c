/*
 * Copyright (c) 2025 kmx.io.
 * Copyright (c) 1997 Manuel Bouyer.
 * Copyright (c) 1980, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define DKTYPENAMES
#include <sys/param.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/dkio.h>
#include <sys/disklabel.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <util.h>
#include <string.h>
#include <ctype.h>
#include <err.h>

#include "fsck.h"
#include "extern.h"
#include "fsutil.h"

#define POWEROF2(num)	(((num) & ((num) - 1)) == 0)

void badsb(int, char *);
static struct disklabel *getdisklabel(char *, int);
static int readsb(int);
static void copyback_sb(void);
static char rdevname[PATH_MAX];

static int
cg_has_sb(int c)
{
	if (c == 0)
		return 1;
	if (!(sblock.m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER))
		return 1;
	if (c == 1)
		return 1;
	if (c % 3 == 0) {
		int p = 3;
		while (p < c)
			p *= 3;
		if (p == c)
			return 1;
	}
	if (c % 5 == 0) {
		int p = 5;
		while (p < c)
			p *= 5;
		if (p == c)
			return 1;
	}
	if (c % 7 == 0) {
		int p = 7;
		while (p < c)
			p *= 7;
		if (p == c)
			return 1;
	}
	return 0;
}

int
setup(char *dev)
{
	long cg, asked, i;
	long bmapsize;
	struct disklabel *lp;
	struct stat statb;
	char *realdev;
	int doskipclean;

	havesb = 0;
	fswritefd = -1;
	doskipclean = skipclean;
	if ((fsreadfd = opendev(dev, O_RDONLY, 0, &realdev)) == -1) {
		printf("Can't open %s: %s\n", dev, strerror(errno));
		return (0);
	}
	if (strncmp(dev, realdev, PATH_MAX) != 0) {
		blockcheck(unrawname(realdev));
		strlcpy(rdevname, realdev, sizeof(rdevname));
		setcdevname(rdevname, dev, preen);
	}
	if (fstat(fsreadfd, &statb) == -1) {
		printf("Can't stat %s: %s\n", realdev, strerror(errno));
		return (0);
	}
	if (!S_ISCHR(statb.st_mode)) {
		pfatal("%s is not a character device", realdev);
		if (reply("CONTINUE") == 0) {
			close(fsreadfd);
			return (0);
		}
	}
	if (preen == 0)
		printf("** %s", realdev);
	if (nflag || (fswritefd = opendev(dev, O_WRONLY, 0, NULL)) == -1) {
		fswritefd = -1;
		if (preen)
			pfatal("NO WRITE ACCESS");
		printf(" (NO WRITE)");
	}
	if (preen == 0)
		printf("\n");
	fsmodified = 0;
	lfdir = 0;
	initbarea(&sblk);
	initbarea(&asblk);
	sblk.b_un.b_buf = malloc(EXT4FS_SUPER_BLOCK_SIZE);
	asblk.b_un.b_buf = malloc(EXT4FS_SUPER_BLOCK_SIZE);
	if (sblk.b_un.b_buf == NULL || asblk.b_un.b_buf == NULL)
		errexit("cannot allocate space for superblock\n");
	if ((lp = getdisklabel(NULL, fsreadfd)) != NULL)
		secsize = lp->d_secsize;
	else
		secsize = DEV_BSIZE;

	if (!hotroot()) {
#ifndef SMALL
		if (pledge("stdio getpw", NULL) == -1)
			err(1, "pledge");
#else
		if (pledge("stdio", NULL) == -1)
			err(1, "pledge");
#endif
	}

	if (readsb(1) == 0) {
		if (bflag || preen)
			return(0);
		if (reply("LOOK FOR ALTERNATE SUPERBLOCKS") == 0)
			return (0);
		for (cg = 1; cg < (long)sblock.m_block_group_count; cg++) {
			bflag = EXT4FS_FSBTODB(&sblock,
			    cg * sblock.m_blocks_per_group +
			    sblock.m_first_data_block);
			if (readsb(0) != 0)
				break;
		}
		if (cg >= (long)sblock.m_block_group_count) {
			printf("%s %s\n%s %s\n%s %s\n",
			    "SEARCH FOR ALTERNATE SUPER-BLOCK",
			    "FAILED. YOU MUST USE THE",
			    "-b OPTION TO FSCK TO SPECIFY THE",
			    "LOCATION OF AN ALTERNATE",
			    "SUPER-BLOCK TO SUPPLY NEEDED",
			    "INFORMATION; SEE fsck_ext4fs(8).");
			return(0);
		}
		doskipclean = 0;
		pwarn("USING ALTERNATE SUPERBLOCK AT %d\n", bflag);
	}
	if (debug)
		printf("state = %d\n", sblock.m_state);
	if (sblock.m_state & EXT4FS_STATE_VALID) {
		if (doskipclean) {
			pwarn("%sile system is clean; not checking\n",
			    preen ? "f" : "** F");
			return (-1);
		}
		if (!preen)
			pwarn("** File system is already clean\n");
	}
	maxfsblock = sblock.m_blocks_count;
	maxino = sblock.m_block_group_count * sblock.m_inodes_per_group;

	sblock.m_gd = calloc(sblock.m_block_group_descriptor_blocks_count,
	    sblock.m_block_size);
	if (sblock.m_gd == NULL)
		errexit("out of memory\n");
	asked = 0;
	for (i = 0; i < (long)sblock.m_block_group_descriptor_blocks_count; i++) {
		u_int64_t gd_blk = ((sblock.m_block_size > 1024) ? 0 : 1) + i + 1;
		if (bread(fsreadfd, (char *)sblock.m_gd +
		    i * sblock.m_block_size,
		    EXT4FS_FSBTODB(&sblock, gd_blk),
		    sblock.m_block_size) != 0 && !asked) {
			pfatal("BAD SUMMARY INFORMATION");
			if (reply("CONTINUE") == 0)
				errexit("%s\n", "");
			asked++;
		}
	}

	if (sblock.m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM) {
		for (i = 0; i < (long)sblock.m_block_group_count; i++) {
			if (ext4fs_bgd_csum_verify(&sblock,
			    &sblock.m_gd[i], i) != 0) {
				pfatal("BAD GROUP DESCRIPTOR CHECKSUM CG #%ld",
				    i);
				if (reply("CONTINUE") == 0)
					errexit("%s\n", "");
			}
		}
	}

	if (fswritefd >= 0)
		fsck_journal_replay();

	bmapsize = roundup(howmany(maxfsblock, NBBY), sizeof(int16_t));
	blockmap = calloc((unsigned)bmapsize, sizeof (char));
	if (blockmap == NULL) {
		printf("cannot alloc %u bytes for blockmap\n",
			(unsigned)bmapsize);
		goto badsblabel;
	}
	statemap = calloc((unsigned)(maxino + 2), sizeof(char));
	if (statemap == NULL) {
		printf("cannot alloc %u bytes for statemap\n",
			(unsigned)(maxino + 1));
		goto badsblabel;
	}
	typemap = calloc((unsigned)(maxino + 1), sizeof(u_char));
	if (typemap == NULL) {
		printf("cannot alloc %u bytes for typemap\n",
		    (unsigned)(maxino + 1));
		goto badsblabel;
	}
	lncntp = calloc((unsigned)(maxino + 1), sizeof(int16_t));
	if (lncntp == NULL) {
		printf("cannot alloc %u bytes for lncntp\n",
			(unsigned)((maxino + 1) * sizeof(int16_t)));
		goto badsblabel;
	}
	for (numdirs = 0, cg = 0; cg < (long)sblock.m_block_group_count; cg++) {
		numdirs += letoh16(sblock.m_gd[cg].bgd_used_dirs_count_lo);
	}
	inplast = 0;
	listmax = numdirs + 10;
	inpsort = calloc((unsigned)listmax, sizeof(struct inoinfo *));
	inphead = calloc((unsigned)numdirs, sizeof(struct inoinfo *));
	if (inpsort == NULL || inphead == NULL) {
		printf("cannot alloc %u bytes for inphead\n",
			(unsigned)(numdirs * sizeof(struct inoinfo *)));
		goto badsblabel;
	}
	bufinit();
	return (1);

badsblabel:
	ckfini(0);
	return (0);
}

static int
readsb(int listerr)
{
	u_int64_t super = bflag ? bflag :
	    EXT4FS_SUPER_BLOCK_OFFSET / DEV_BSIZE;
	struct ext4fs *fs;

	if (bread(fsreadfd, (char *)sblk.b_un.b_buf, super,
	    (long)EXT4FS_SUPER_BLOCK_SIZE) != 0)
		return (0);
	sblk.b_bno = super;
	sblk.b_size = EXT4FS_SUPER_BLOCK_SIZE;

	fs = sblk.b_un.b_fs;

	if (letoh16(fs->sb_magic) != EXT4FS_MAGIC) {
		badsb(listerr, "MAGIC NUMBER WRONG");
		return (0);
	}
	if (letoh32(fs->sb_log_block_size) > 6) {
		badsb(listerr, "BAD LOG_BSIZE");
		return (0);
	}
	if (letoh32(fs->sb_blocks_per_group) == 0) {
		badsb(listerr, "BAD BLOCKS PER GROUP");
		return (0);
	}

	sblock.m_inodes_count = letoh32(fs->sb_inodes_count);
	sblock.m_blocks_count = letoh32(fs->sb_blocks_count_lo) |
	    ((u_int64_t)letoh32(fs->sb_blocks_count_hi) << 32);
	sblock.m_reserved_blocks_count = letoh32(fs->sb_reserved_blocks_count_lo) |
	    ((u_int64_t)letoh32(fs->sb_reserved_blocks_count_hi) << 32);
	sblock.m_free_blocks_count = letoh32(fs->sb_free_blocks_count_lo) |
	    ((u_int64_t)letoh32(fs->sb_free_blocks_count_hi) << 32);
	sblock.m_free_inodes_count = letoh32(fs->sb_free_inodes_count);
	sblock.m_first_data_block = letoh32(fs->sb_first_data_block);
	sblock.m_log_block_size = letoh32(fs->sb_log_block_size);
	sblock.m_blocks_per_group = letoh32(fs->sb_blocks_per_group);
	sblock.m_inodes_per_group = letoh32(fs->sb_inodes_per_group);
	sblock.m_state = letoh16(fs->sb_state);
	sblock.m_revision_level = letoh32(fs->sb_revision_level);
	sblock.m_inode_size = letoh16(fs->sb_inode_size);
	sblock.m_first_non_reserved_inode =
	    letoh32(fs->sb_first_non_reserved_inode);
	sblock.m_feature_compat = letoh32(fs->sb_feature_compat);
	sblock.m_feature_incompat = letoh32(fs->sb_feature_incompat);
	sblock.m_feature_ro_compat = letoh32(fs->sb_feature_ro_compat);
	sblock.m_block_group_descriptor_size =
	    letoh16(fs->sb_block_group_descriptor_size);
	sblock.m_reserved_bgdt_blocks =
	    letoh16(fs->sb_reserved_bgdt_blocks);
	sblock.m_checksum_seed = letoh32(fs->sb_checksum_seed);
	sblock.m_last_orphan = letoh32(fs->sb_last_orphan);
	sblock.m_orphan_file_inode = letoh32(fs->sb_orphan_file_inode);
	sblock.m_journal_inode_number = letoh32(fs->sb_journal_inode_number);
	sblock.m_user_quota_inode = letoh32(fs->sb_user_quota_inode);
	sblock.m_group_quota_inode = letoh32(fs->sb_group_quota_inode);
	sblock.m_lost_and_found_inode = letoh32(fs->sb_lost_and_found_inode);
	sblock.m_project_quota_inode = letoh32(fs->sb_project_quota_inode);

	if (sblock.m_revision_level < EXT4FS_REV_DYNAMIC) {
		sblock.m_inode_size = 128;
		sblock.m_block_group_descriptor_size = 32;
	}
	if (sblock.m_block_group_descriptor_size == 0)
		sblock.m_block_group_descriptor_size = 32;

	memcpy(&sblock.m_sble, fs, sizeof(struct ext4fs));

	sblock.m_block_group_count =
	    howmany(sblock.m_blocks_count - sblock.m_first_data_block,
	    sblock.m_blocks_per_group);
	sblock.m_block_size = 1024 << sblock.m_log_block_size;
	sblock.m_block_size_shift = 10 + sblock.m_log_block_size;
	sblock.m_fs_block_to_disk_block = sblock.m_log_block_size + 1;
	sblock.m_inodes_per_block = sblock.m_block_size / sblock.m_inode_size;
	sblock.m_inode_table_blocks_per_group =
	    sblock.m_inodes_per_group / sblock.m_inodes_per_block;
	sblock.m_block_group_descriptor_blocks_count =
	    howmany(sblock.m_block_group_count *
	    sblock.m_block_group_descriptor_size, sblock.m_block_size);

	sblk.b_bno = super / DEV_BSIZE;

	if (sblock.m_block_group_count == 1) {
		havesb = 1;
		return 1;
	}

	getblk(&asblk,
	    1 * sblock.m_blocks_per_group + sblock.m_first_data_block,
	    (long)EXT4FS_SUPER_BLOCK_SIZE);
	if (asblk.b_errs)
		return (0);
	if (bflag) {
		havesb = 1;
		return (1);
	}

	if (sblock.m_feature_incompat & ~EXT4FS_FEATURE_INCOMPAT_SUPPORTED) {
		if (debug) {
			printf("compat 0x%08x, incompat 0x%08x, compat_ro "
			    "0x%08x\n",
			    sblock.m_feature_compat,
			    sblock.m_feature_incompat,
			    sblock.m_feature_ro_compat);
		}
		badsb(listerr, "INCOMPATIBLE FEATURE BITS IN SUPER BLOCK");
		return 0;
	}

	havesb = 1;
	return (1);
}

static void
copyback_sb(void)
{
	memcpy(sblk.b_un.b_buf, &sblock.m_sble, sizeof(struct ext4fs));
}

void
badsb(int listerr, char *s)
{
	if (!listerr)
		return;
	if (preen)
		printf("%s: ", cdevname());
	pfatal("BAD SUPER BLOCK: %s\n", s);
}

static struct disklabel *
getdisklabel(char *s, int fd)
{
	static struct disklabel lab;

	if (ioctl(fd, DIOCGDINFO, (char *)&lab) == -1) {
		if (s == NULL)
			return (NULL);
		pwarn("ioctl (GCINFO): %s\n", strerror(errno));
		errexit("%s: can't read disk label\n", s);
	}
	return (&lab);
}

u_int64_t
cgoverhead(int c)
{
	u_int64_t overh;
	overh =	1 + 1 + sblock.m_inode_table_blocks_per_group;
	if (sblock.m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER) {
		if (cg_has_sb(c) == 0)
			return overh;
	}
	overh += 1 + sblock.m_block_group_descriptor_blocks_count;
	return overh;
}
