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

#include <sys/param.h>
#include <sys/time.h>

#include <ufs/ufs/dinode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "fsck.h"
#include "fsutil.h"
#include "extern.h"

#define MINDIRSIZE	(sizeof(struct ext4fs_directory))

static int pass2check(struct inodesc *);
static int blksort(const void *, const void *);

void
pass2(void)
{
	struct ext4fs_dinode *dp;
	struct inoinfo **inpp, *inp;
	struct inoinfo **inpend;
	struct inodesc curino;
	struct ext4fs_dinode dino;
	char pathbuf[PATH_MAX + 1];

	switch (statemap[EXT4FS_INODE_ROOT_DIR]) {

	case USTATE:
		pfatal("ROOT INODE UNALLOCATED");
		if (reply("ALLOCATE") == 0)
			errexit("%s\n", "");
		if (allocdir(EXT4FS_INODE_ROOT_DIR, EXT4FS_INODE_ROOT_DIR, 0755) !=
		    EXT4FS_INODE_ROOT_DIR)
			errexit("CANNOT ALLOCATE ROOT INODE\n");
		break;

	case DCLEAR:
		pfatal("DUPS/BAD IN ROOT INODE");
		if (reply("REALLOCATE")) {
			freeino(EXT4FS_INODE_ROOT_DIR);
			if (allocdir(EXT4FS_INODE_ROOT_DIR, EXT4FS_INODE_ROOT_DIR,
			    0755) != EXT4FS_INODE_ROOT_DIR)
				errexit("CANNOT ALLOCATE ROOT INODE\n");
			break;
		}
		if (reply("CONTINUE") == 0)
			errexit("%s\n", "");
		break;

	case FSTATE:
	case FCLEAR:
		pfatal("ROOT INODE NOT DIRECTORY");
		if (reply("REALLOCATE")) {
			freeino(EXT4FS_INODE_ROOT_DIR);
			if (allocdir(EXT4FS_INODE_ROOT_DIR, EXT4FS_INODE_ROOT_DIR,
			    0755) != EXT4FS_INODE_ROOT_DIR)
				errexit("CANNOT ALLOCATE ROOT INODE\n");
			break;
		}
		if (reply("FIX") == 0)
			errexit("%s\n", "");
		dp = ginode(EXT4FS_INODE_ROOT_DIR);
		dp->i_mode = htole16((letoh16(dp->i_mode) & ~IFMT) | IFDIR);
		inodirty();
		break;

	case DSTATE:
		break;

	default:
		errexit("BAD STATE %d FOR ROOT INODE\n",
		    statemap[EXT4FS_INODE_ROOT_DIR]);
	}

	qsort((char *)inpsort, (size_t)inplast, sizeof *inpsort, blksort);

	memset(&curino, 0, sizeof(struct inodesc));
	curino.id_type = DATA;
	curino.id_func = pass2check;
	inpend = &inpsort[inplast];
	for (inpp = inpsort; inpp < inpend; inpp++) {
		inp = *inpp;
		if (inp->i_isize == 0)
			continue;
		if (inp->i_isize < MINDIRSIZE) {
			direrror(inp->i_number, "DIRECTORY TOO SHORT");
			inp->i_isize = roundup(MINDIRSIZE, sblock.m_block_size);
			if (reply("FIX") == 1) {
				dp = ginode(inp->i_number);
				inossize(dp, inp->i_isize);
				inodirty();
			}
		} else if ((inp->i_isize & (sblock.m_block_size - 1)) != 0) {
			getpathname(pathbuf, sizeof pathbuf, inp->i_number,
			    inp->i_number);
			pwarn("DIRECTORY %s: LENGTH %llu NOT MULTIPLE OF %llu",
			    pathbuf, (unsigned long long)inp->i_isize,
			    (unsigned long long)sblock.m_block_size);
			if (preen)
				printf(" (ADJUSTED)\n");
			inp->i_isize = roundup(inp->i_isize, sblock.m_block_size);
			if (preen || reply("ADJUST") == 1) {
				dp = ginode(inp->i_number);
				inossize(dp, inp->i_isize);
				inodirty();
			}
		}
		dp = ginode(inp->i_number);
		memset(&dino, 0, sizeof(dino));
		dino.i_mode = htole16(IFDIR);
		dino.i_flags = dp->i_flags;
		inossize(&dino, inp->i_isize);
		memcpy(&dino.i_block[0], &inp->i_blks[0],
		    (size_t)inp->i_numblks);
		curino.id_number = inp->i_number;
		curino.id_parent = inp->i_parent;
		(void)ckinode(&dino, &curino);
	}

	for (inpp = inpsort; inpp < inpend; inpp++) {
		inp = *inpp;
		if (inp->i_parent == 0 || inp->i_isize == 0)
			continue;
		if (inp->i_dotdot == inp->i_parent ||
		    inp->i_dotdot == (ino_t)-1)
			continue;
		if (inp->i_dotdot == 0) {
			inp->i_dotdot = inp->i_parent;
			fileerror(inp->i_parent, inp->i_number, "MISSING '..'");
			if (reply("FIX") == 0)
				continue;
			(void)makeentry(inp->i_number, inp->i_parent, "..");
			lncntp[inp->i_parent]--;
			continue;
		}
		fileerror(inp->i_parent, inp->i_number,
		    "BAD INODE NUMBER FOR '..'");
		if (reply("FIX") == 0)
			continue;
		lncntp[inp->i_dotdot]++;
		lncntp[inp->i_parent]--;
		inp->i_dotdot = inp->i_parent;
		(void)changeino(inp->i_number, "..", inp->i_parent);
	}

	propagate();
}

static int
pass2check(struct inodesc *idesc)
{
	struct ext4fs_directory *dirp = idesc->id_dirp;
	struct inoinfo *inp;
	int n, entrysize, ret = 0;
	struct ext4fs_dinode *dp;
	char *errmsg;
	struct ext4fs_directory proto;
	char namebuf[PATH_MAX + 1];
	char pathbuf[PATH_MAX + 1];

	if (idesc->id_entryno != 0)
		goto chk1;
	if (letoh32(dirp->e4d_ino) != 0 && dirp->e4d_namlen == 1 &&
	    dirp->e4d_name[0] == '.') {
		if (letoh32(dirp->e4d_ino) != idesc->id_number) {
			direrror(idesc->id_number, "BAD INODE NUMBER FOR '.'");
			dirp->e4d_ino = htole32(idesc->id_number);
			if (reply("FIX") == 1)
				ret |= ALTERED;
		}
		if (dirp->e4d_type != EXT4FS_FT_DIR) {
			direrror(idesc->id_number, "BAD TYPE VALUE FOR '.'");
			dirp->e4d_type = EXT4FS_FT_DIR;
			if (reply("FIX") == 1)
				ret |= ALTERED;
		}
		goto chk1;
	}
	direrror(idesc->id_number, "MISSING '.'");
	proto.e4d_ino = htole32(idesc->id_number);
	proto.e4d_namlen = 1;
	proto.e4d_type = EXT4FS_FT_DIR;
	(void)strlcpy(proto.e4d_name, ".", sizeof proto.e4d_name);
	entrysize = EXT4FS_DIRSIZ(proto.e4d_namlen);
	if (letoh32(dirp->e4d_ino) != 0 &&
	    strcmp(dirp->e4d_name, "..") != 0) {
		pfatal("CANNOT FIX, FIRST ENTRY IN DIRECTORY CONTAINS %s\n",
		    dirp->e4d_name);
	} else if (letoh16(dirp->e4d_reclen) < entrysize) {
		pfatal("CANNOT FIX, INSUFFICIENT SPACE TO ADD '.'\n");
	} else if (letoh16(dirp->e4d_reclen) < 2 * entrysize) {
		proto.e4d_reclen = dirp->e4d_reclen;
		memcpy(dirp, &proto, (size_t)entrysize);
		if (reply("FIX") == 1)
			ret |= ALTERED;
	} else {
		n = letoh16(dirp->e4d_reclen) - entrysize;
		proto.e4d_reclen = htole16(entrysize);
		memcpy(dirp, &proto, (size_t)entrysize);
		idesc->id_entryno++;
		lncntp[letoh32(dirp->e4d_ino)]--;
		dirp = (struct ext4fs_directory *)((char *)(dirp) + entrysize);
		memset(dirp, 0, (size_t)n);
		dirp->e4d_reclen = htole16(n);
		if (reply("FIX") == 1)
			ret |= ALTERED;
	}
chk1:
	if (idesc->id_entryno > 1)
		goto chk2;
	inp = getinoinfo(idesc->id_number);
	proto.e4d_ino = htole32(inp->i_parent);
	proto.e4d_namlen = 2;
	proto.e4d_type = EXT4FS_FT_DIR;
	(void)strlcpy(proto.e4d_name, "..", sizeof proto.e4d_name);
	entrysize = EXT4FS_DIRSIZ(2);
	if (idesc->id_entryno == 0) {
		n = EXT4FS_DIRSIZ(dirp->e4d_namlen);
		if (letoh16(dirp->e4d_reclen) < n + entrysize)
			goto chk2;
		proto.e4d_reclen = htole16(letoh16(dirp->e4d_reclen) - n);
		dirp->e4d_reclen = htole16(n);
		idesc->id_entryno++;
		lncntp[letoh32(dirp->e4d_ino)]--;
		dirp = (struct ext4fs_directory *)((char *)(dirp) + n);
		memset(dirp, 0, (size_t)letoh16(proto.e4d_reclen));
		dirp->e4d_reclen = proto.e4d_reclen;
	}
	if (letoh32(dirp->e4d_ino) != 0 &&
	    dirp->e4d_namlen == 2 &&
	    strncmp(dirp->e4d_name, "..", 2) == 0) {
		inp->i_dotdot = letoh32(dirp->e4d_ino);
		if (dirp->e4d_type != EXT4FS_FT_DIR) {
			direrror(idesc->id_number, "BAD TYPE VALUE FOR '..'");
			dirp->e4d_type = EXT4FS_FT_DIR;
			if (reply("FIX") == 1)
				ret |= ALTERED;
		}
		goto chk2;
	}
	if (letoh32(dirp->e4d_ino) != 0 &&
	    dirp->e4d_namlen == 1 &&
	    strncmp(dirp->e4d_name, ".", 1) != 0) {
		fileerror(inp->i_parent, idesc->id_number, "MISSING '..'");
		pfatal("CANNOT FIX, SECOND ENTRY IN DIRECTORY CONTAINS %s\n",
		    dirp->e4d_name);
		inp->i_dotdot = (ino_t)-1;
	} else if (letoh16(dirp->e4d_reclen) < entrysize) {
		fileerror(inp->i_parent, idesc->id_number, "MISSING '..'");
		pfatal("CANNOT FIX, INSUFFICIENT SPACE TO ADD '..'\n");
		inp->i_dotdot = (ino_t)-1;
	} else if (inp->i_parent != 0) {
		inp->i_dotdot = inp->i_parent;
		fileerror(inp->i_parent, idesc->id_number, "MISSING '..'");
		proto.e4d_reclen = dirp->e4d_reclen;
		memcpy(dirp, &proto, (size_t)entrysize);
		if (reply("FIX") == 1)
			ret |= ALTERED;
	}
	idesc->id_entryno++;
	if (letoh32(dirp->e4d_ino) != 0)
		lncntp[letoh32(dirp->e4d_ino)]--;
	return (ret|KEEPON);
chk2:
	if (letoh32(dirp->e4d_ino) == 0)
		return (ret|KEEPON);
	if (dirp->e4d_namlen <= 2 &&
	    dirp->e4d_name[0] == '.' &&
	    idesc->id_entryno >= 2) {
		if (dirp->e4d_namlen == 1) {
			direrror(idesc->id_number, "EXTRA '.' ENTRY");
			dirp->e4d_ino = 0;
			if (reply("FIX") == 1)
				ret |= ALTERED;
			return (KEEPON | ret);
		}
		if (dirp->e4d_name[1] == '.') {
			direrror(idesc->id_number, "EXTRA '..' ENTRY");
			dirp->e4d_ino = 0;
			if (reply("FIX") == 1)
				ret |= ALTERED;
			return (KEEPON | ret);
		}
	}
	idesc->id_entryno++;
	n = 0;
	if (letoh32(dirp->e4d_ino) > maxino ||
	    (letoh32(dirp->e4d_ino) < EXT4FS_INODE_FIRST &&
	     letoh32(dirp->e4d_ino) != EXT4FS_INODE_ROOT_DIR)) {
		fileerror(idesc->id_number, letoh32(dirp->e4d_ino),
		    "I OUT OF RANGE");
		n = reply("REMOVE");
	} else {
again:
		switch (statemap[letoh32(dirp->e4d_ino)]) {
		case USTATE:
			if (idesc->id_entryno <= 2)
				break;
			fileerror(idesc->id_number, letoh32(dirp->e4d_ino),
			    "UNALLOCATED");
			n = reply("REMOVE");
			break;

		case DCLEAR:
		case FCLEAR:
			if (idesc->id_entryno <= 2)
				break;
			if (statemap[letoh32(dirp->e4d_ino)] == FCLEAR)
				errmsg = "DUP/BAD";
			else if (!preen)
				errmsg = "ZERO LENGTH DIRECTORY";
			else {
				n = 1;
				break;
			}
			fileerror(idesc->id_number, letoh32(dirp->e4d_ino),
			    errmsg);
			if ((n = reply("REMOVE")) == 1)
				break;
			dp = ginode(letoh32(dirp->e4d_ino));
			statemap[letoh32(dirp->e4d_ino)] =
			    (letoh16(dp->i_mode) & IFMT) == IFDIR ?
			    DSTATE : FSTATE;
			lncntp[letoh32(dirp->e4d_ino)] =
			    letoh16(dp->i_links_count);
			goto again;

		case DSTATE:
		case DFOUND:
			inp = getinoinfo(letoh32(dirp->e4d_ino));
			if (inp->i_parent != 0 && idesc->id_entryno > 2) {
				getpathname(pathbuf, sizeof pathbuf,
				    idesc->id_number, idesc->id_number);
				getpathname(namebuf, sizeof namebuf,
				    letoh32(dirp->e4d_ino),
				    letoh32(dirp->e4d_ino));
				pwarn("%s %s %s\n", pathbuf,
				    "IS AN EXTRANEOUS HARD LINK TO DIRECTORY",
				    namebuf);
				if (preen)
					printf(" (IGNORED)\n");
				else if ((n = reply("REMOVE")) == 1)
					break;
			}
			if (idesc->id_entryno > 2)
				inp->i_parent = idesc->id_number;
			/* fall through */

		case FSTATE:
			if (dirp->e4d_type !=
			    typemap[letoh32(dirp->e4d_ino)]) {
				dirp->e4d_type =
				    typemap[letoh32(dirp->e4d_ino)];
				fileerror(idesc->id_number,
				    letoh32(dirp->e4d_ino),
				    "BAD TYPE VALUE");
				if (reply("FIX") == 1)
					ret |= ALTERED;
			}
			lncntp[letoh32(dirp->e4d_ino)]--;
			break;

		default:
			errexit("BAD STATE %d FOR INODE I=%llu\n",
			    statemap[letoh32(dirp->e4d_ino)],
			    (unsigned long long)letoh32(dirp->e4d_ino));
		}
	}
	if (n == 0)
		return (ret|KEEPON);
	dirp->e4d_ino = 0;
	return (ret|KEEPON|ALTERED);
}

static int
blksort(const void *inpp1, const void *inpp2)
{
	return ((* (struct inoinfo **) inpp1)->i_blks[0] -
		(* (struct inoinfo **) inpp2)->i_blks[0]);
}
