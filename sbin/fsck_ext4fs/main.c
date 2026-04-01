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

#include <sys/types.h>
#include <sys/signal.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <fstab.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <err.h>

#include "fsck.h"
#include "extern.h"
#include "fsutil.h"

volatile sig_atomic_t	returntosingle;

int	main(int, char *[]);

static int	argtoi(int, char *, char *, int);
static int	checkfilesys(char *, char *, long, int);
static  void usage(void);

struct bufarea bufhead;
struct bufarea sblk;
struct bufarea asblk;
struct bufarea *pdirbp;
struct bufarea *pbp;
struct bufarea *getdatablk(u_int64_t, long);
struct m_ext4fs sblock;

struct dups *duplist;
struct dups *muldup;

struct zlncnt *zlnhead;

struct inoinfo **inphead, **inpsort;
long numdirs, listmax, inplast;

long	secsize;
char	nflag;
char	yflag;
int	bflag;
int	debug;
int	preen;
char	havesb;
char	skipclean;
int	fsmodified;
int	fsreadfd;
int	fswritefd;
int	rerun;

u_int64_t	maxfsblock;
char	*blockmap;
ino_t	maxino;
ino_t	lastino;
char	*statemap;
u_char	*typemap;
int16_t	*lncntp;

ino_t	lfdir;

u_int64_t	n_blks;
u_int64_t	n_files;

struct	ext4fs_dinode zino;

int
main(int argc, char *argv[])
{
	int ch;
	int ret = 0;

	checkroot();

	sync();
	skipclean = 1;
	while ((ch = getopt(argc, argv, "b:dfm:npy")) != -1) {
		switch (ch) {
		case 'b':
			skipclean = 0;
			bflag = argtoi('b', "number", optarg, 10);
			printf("Alternate super block location: %d\n", bflag);
			break;

		case 'd':
			debug = 1;
			break;

		case 'f':
			skipclean = 0;
			break;

		case 'm':
			lfmode = argtoi('m', "mode", optarg, 8);
			if (lfmode &~ 07777)
				errexit("bad mode to -m: %o\n", lfmode);
			printf("** lost+found creation mode %o\n", lfmode);
			break;

		case 'n':
			nflag = 1;
			yflag = 0;
			break;

		case 'p':
			preen = 1;
			break;

		case 'y':
			yflag = 1;
			nflag = 0;
			break;

		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

	if (signal(SIGINT, SIG_IGN) != SIG_IGN)
		(void)signal(SIGINT, catch);
	if (preen)
		(void)signal(SIGQUIT, catchquit);

	(void)checkfilesys(blockcheck(*argv), 0, 0L, 0);

	if (returntosingle)
		ret = 2;

	exit(ret);
}

static int
argtoi(int flag, char *req, char *str, int base)
{
	char *cp;
	int ret;

	ret = (int)strtol(str, &cp, base);
	if (cp == str || *cp)
		errexit("-%c flag requires a %s\n", flag, req);
	return (ret);
}

static int
checkfilesys(char *filesys, char *mntpt, long auxdata, int child)
{
	u_int64_t n_bfree;
	struct dups *dp;
	struct zlncnt *zlnp;
	u_int64_t i;

	if (preen && child)
		(void)signal(SIGQUIT, voidquit);
	setcdevname(filesys, NULL, preen);
	if (debug && preen)
		pwarn("starting\n");

	switch (setup(filesys)) {
	case 0:
		if (preen)
			pfatal("CAN'T CHECK FILE SYSTEM.");
	case -1:
		return (0);
	}

	if (preen == 0) {
		if (sblock.m_revision_level > EXT4FS_REV_EXT2) {
			printf("** Last Mounted on %s\n",
			    sblock.m_sble.sb_last_mounted);
		}
		if (hotroot())
			printf("** Root file system\n");
		printf("** Phase 1 - Check Blocks and Sizes\n");
	}
	pass1();

	if (duplist) {
		if (preen)
			pfatal("INTERNAL ERROR: dups with -p");
		printf("** Phase 1b - Rescan For More DUPS\n");
		pass1b();
	}

	if (preen == 0)
		printf("** Phase 2 - Check Pathnames\n");
	pass2();

	if (preen == 0)
		printf("** Phase 3 - Check Connectivity\n");
	pass3();

	if (preen == 0)
		printf("** Phase 4 - Check Reference Counts\n");
	pass4();

	if (preen == 0)
		printf("** Phase 5 - Check Cyl groups\n");
	pass5();

	n_bfree = sblock.m_free_blocks_count;

	pwarn("%llu files, %llu used, %llu free\n",
	    (unsigned long long)n_files,
	    (unsigned long long)n_blks,
	    (unsigned long long)n_bfree);
	if (debug &&
	    (n_files -= maxino - EXT4FS_INODE_FIRST + 1 - sblock.m_free_inodes_count))
		printf("%llu files missing\n", (unsigned long long)n_files);
	if (debug) {
		for (i = 0; i < sblock.m_block_group_count; i++)
			n_blks += cgoverhead(i);
		n_blks += sblock.m_first_data_block;
		if (n_blks -= maxfsblock - n_bfree)
			printf("%llu blocks missing\n",
			    (unsigned long long)n_blks);
		if (duplist != NULL) {
			printf("The following duplicate blocks remain:");
			for (dp = duplist; dp; dp = dp->next)
				printf(" %llu,",
				    (unsigned long long)dp->dup);
			printf("\n");
		}
		if (zlnhead != NULL) {
			printf("The following zero link count inodes remain:");
			for (zlnp = zlnhead; zlnp; zlnp = zlnp->next)
				printf(" %llu,",
				    (unsigned long long)zlnp->zlncnt);
			printf("\n");
		}
	}
	zlnhead = NULL;
	duplist = NULL;
	muldup = NULL;
	inocleanup();
	if (fsmodified) {
		time_t t;
		(void)time(&t);
		sblock.m_sble.sb_write_time_lo = htole32((u_int32_t)t);
		sblock.m_sble.sb_check_time_lo = htole32((u_int32_t)t);
		sbdirty();
	}
	ckfini(1);
	free(blockmap);
	free(statemap);
	free((char *)lncntp);
	if (!fsmodified)
		return (0);
	if (!preen)
		printf("\n***** FILE SYSTEM WAS MODIFIED *****\n");
	if (rerun)
		printf("\n***** PLEASE RERUN FSCK *****\n");
	if (hotroot()) {
		struct statfs stfs_buf;

		if (statfs("/", &stfs_buf) == 0) {
			long flags = stfs_buf.f_flags;
			struct ufs_args args;
			int ret;

			if (flags & MNT_RDONLY) {
				args.fspec = 0;
				args.export_info.ex_flags = 0;
				args.export_info.ex_root = 0;
				flags |= MNT_UPDATE | MNT_RELOAD;
				ret = mount("ext4fs", "/", flags, &args);
				if (ret == 0)
					return(0);
			}
		}
		if (!preen)
			printf("\n***** REBOOT NOW *****\n");
		sync();
		return (4);
	}
	return (0);
}

static void
usage(void)
{
	extern char *__progname;

	(void) fprintf(stderr,
	    "usage: %s [-dfnpy] [-b block#] [-m mode] filesystem\n",
	    __progname);
	exit(1);
}
