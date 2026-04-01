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

char	*lfname = "lost+found";
int	lfmode = 01777;

struct ext4fs_dirtemplate {
	u_int32_t dot_ino;
	u_int16_t dot_reclen;
	u_int8_t  dot_namlen;
	u_int8_t  dot_type;
	char      dot_name[4];
	u_int32_t dotdot_ino;
	u_int16_t dotdot_reclen;
	u_int8_t  dotdot_namlen;
	u_int8_t  dotdot_type;
	char      dotdot_name[4];
};

static struct ext4fs_directory emptydir;
static struct ext4fs_dirtemplate dirhead;

static int expanddir(struct ext4fs_dinode *, char *);
static void freedir(ino_t, ino_t);
static struct ext4fs_directory *fsck_readdir(struct inodesc *);
static struct bufarea *getdirblk(u_int64_t, long);
static int lftempname(char *, ino_t);
static int mkentry(struct inodesc *);
static int chgino(struct inodesc *);

void
propagate(void)
{
	struct inoinfo **inpp, *inp, *pinp;
	struct inoinfo **inpend;

	inpend = &inpsort[inplast];
	for (inpp = inpsort; inpp < inpend; inpp++) {
		inp = *inpp;
		if (inp->i_parent == 0 ||
		    inp->i_number == EXT4FS_INODE_ROOT_DIR)
			continue;
		pinp = getinoinfo(inp->i_parent);
		inp->i_parentp = pinp;
		inp->i_sibling = pinp->i_child;
		pinp->i_child = inp;
	}
	inp = getinoinfo(EXT4FS_INODE_ROOT_DIR);
	while (inp) {
		statemap[inp->i_number] = DFOUND;
		if (inp->i_child &&
		    statemap[inp->i_child->i_number] == DSTATE)
			inp = inp->i_child;
		else if (inp->i_sibling)
			inp = inp->i_sibling;
		else
			inp = inp->i_parentp;
	}
}

int
dirscan(struct inodesc *idesc)
{
	struct ext4fs_directory *dp;
	struct bufarea *bp;
	int dsize, n;
	long blksiz;
	char *dbuf = NULL;

	if ((dbuf = malloc(sblock.m_block_size)) == NULL) {
		fprintf(stderr, "out of memory");
		exit(8);
	}

	if (idesc->id_type != DATA)
		errexit("wrong type to dirscan %d\n", idesc->id_type);
	if (idesc->id_entryno == 0 &&
	    (idesc->id_filesize & (sblock.m_block_size - 1)) != 0)
		idesc->id_filesize = roundup(idesc->id_filesize,
		    sblock.m_block_size);
	blksiz = idesc->id_numfrags * sblock.m_block_size;
	if (chkrange(idesc->id_blkno, idesc->id_numfrags)) {
		idesc->id_filesize -= blksiz;
		free(dbuf);
		return (SKIP);
	}
	idesc->id_loc = 0;
	for (dp = fsck_readdir(idesc); dp != NULL; dp = fsck_readdir(idesc)) {
		dsize = letoh16(dp->e4d_reclen);
		memcpy(dbuf, dp, (size_t)dsize);
		idesc->id_dirp = (struct ext4fs_directory *)dbuf;
		if ((n = (*idesc->id_func)(idesc)) & ALTERED) {
			bp = getdirblk(idesc->id_blkno, blksiz);
			memcpy(bp->b_un.b_buf + idesc->id_loc - dsize, dbuf,
			    (size_t)dsize);
			dirty(bp);
			sbdirty();
		}
		if (n & STOP) {
			free(dbuf);
			return (n);
		}
	}
	free(dbuf);
	return (idesc->id_filesize > 0 ? KEEPON : STOP);
}

static struct ext4fs_directory *
fsck_readdir(struct inodesc *idesc)
{
	struct ext4fs_directory *dp, *ndp;
	struct bufarea *bp;
	long size, blksiz, fix, dploc;

	blksiz = idesc->id_numfrags * sblock.m_block_size;
	bp = getdirblk(idesc->id_blkno, blksiz);
	if (idesc->id_loc % sblock.m_block_size == 0 && idesc->id_filesize > 0 &&
	    idesc->id_loc < blksiz) {
		dp = (struct ext4fs_directory *)(bp->b_un.b_buf + idesc->id_loc);
		if (dircheck(idesc, dp))
			goto dpok;
		if (idesc->id_fix == IGNORE)
			return (0);
		fix = dofix(idesc, "DIRECTORY CORRUPTED");
		bp = getdirblk(idesc->id_blkno, blksiz);
		dp = (struct ext4fs_directory *)(bp->b_un.b_buf + idesc->id_loc);
		dp->e4d_reclen = htole16(sblock.m_block_size);
		dp->e4d_ino = 0;
		dp->e4d_namlen = 0;
		dp->e4d_type = 0;
		dp->e4d_name[0] = '\0';
		if (fix)
			dirty(bp);
		idesc->id_loc += sblock.m_block_size;
		idesc->id_filesize -= sblock.m_block_size;
		return (dp);
	}
dpok:
	if (idesc->id_filesize <= 0 || idesc->id_loc >= blksiz)
		return NULL;
	dploc = idesc->id_loc;
	dp = (struct ext4fs_directory *)(bp->b_un.b_buf + dploc);
	idesc->id_loc += letoh16(dp->e4d_reclen);
	idesc->id_filesize -= letoh16(dp->e4d_reclen);
	if ((idesc->id_loc % sblock.m_block_size) == 0)
		return (dp);
	ndp = (struct ext4fs_directory *)(bp->b_un.b_buf + idesc->id_loc);
	if (idesc->id_loc < blksiz && idesc->id_filesize > 0 &&
	    dircheck(idesc, ndp) == 0) {
		size = sblock.m_block_size - (idesc->id_loc % sblock.m_block_size);
		idesc->id_loc += size;
		idesc->id_filesize -= size;
		if (idesc->id_fix == IGNORE)
			return (0);
		fix = dofix(idesc, "DIRECTORY CORRUPTED");
		bp = getdirblk(idesc->id_blkno, blksiz);
		dp = (struct ext4fs_directory *)(bp->b_un.b_buf + dploc);
		dp->e4d_reclen = htole16(letoh16(dp->e4d_reclen) + size);
		if (fix)
			dirty(bp);
	}
	return (dp);
}

int
dircheck(struct inodesc *idesc, struct ext4fs_directory *dp)
{
	int size;
	char *cp;
	int spaceleft;
	u_int16_t reclen = letoh16(dp->e4d_reclen);

	spaceleft = sblock.m_block_size - (idesc->id_loc % sblock.m_block_size);
	if (letoh32(dp->e4d_ino) > maxino ||
	    reclen == 0 ||
	    reclen > spaceleft ||
	    (reclen & 0x3) != 0)
		return (0);
	if (dp->e4d_ino == 0)
		return (1);
	size = EXT4FS_DIRSIZ(dp->e4d_namlen);
	if (reclen < size ||
	    idesc->id_filesize < size ||
	    dp->e4d_namlen > EXT4FS_MAXNAMLEN)
		return (0);
	for (cp = dp->e4d_name, size = 0; size < dp->e4d_namlen; size++)
		if (*cp == '\0' || (*cp++ == '/'))
			return (0);
	return (1);
}

void
direrror(ino_t ino, char *errmesg)
{
	fileerror(ino, ino, errmesg);
}

void
fileerror(ino_t cwd, ino_t ino, char *errmesg)
{
	struct ext4fs_dinode *dp;
	char pathbuf[PATH_MAX + 1];

	pwarn("%s ", errmesg);
	pinode(ino);
	printf("\n");
	getpathname(pathbuf, sizeof pathbuf, cwd, ino);
	if ((ino < EXT4FS_INODE_FIRST && ino != EXT4FS_INODE_ROOT_DIR) ||
	    ino > maxino) {
		pfatal("NAME=%s\n", pathbuf);
		return;
	}
	dp = ginode(ino);
	if (ftypeok(dp))
		pfatal("%s=%s\n",
		    (letoh16(dp->i_mode) & IFMT) == IFDIR ? "DIR" : "FILE",
		    pathbuf);
	else
		pfatal("NAME=%s\n", pathbuf);
}

void
adjust(struct inodesc *idesc, short lcnt)
{
	struct ext4fs_dinode *dp;

	dp = ginode(idesc->id_number);
	if (letoh16(dp->i_links_count) == lcnt) {
		if (linkup(idesc->id_number, (ino_t)0) == 0)
			clri(idesc, "UNREF", 0);
	} else {
		pwarn("LINK COUNT %s", (lfdir == idesc->id_number) ? lfname :
		    ((letoh16(dp->i_mode) & IFMT) == IFDIR ? "DIR" : "FILE"));
		pinode(idesc->id_number);
		printf(" COUNT %d SHOULD BE %d",
		    letoh16(dp->i_links_count),
		    letoh16(dp->i_links_count) - lcnt);
		if (preen) {
			if (lcnt < 0) {
				printf("\n");
				pfatal("LINK COUNT INCREASING");
			}
			printf(" (ADJUSTED)\n");
		}
		if (preen || reply("ADJUST") == 1) {
			dp->i_links_count =
			    htole16(letoh16(dp->i_links_count) - lcnt);
			inodirty();
		}
	}
}

static int
mkentry(struct inodesc *idesc)
{
	struct ext4fs_directory *dirp = idesc->id_dirp;
	struct ext4fs_directory newent;
	int newlen, oldlen;

	newent.e4d_type = EXT4FS_FT_UNKNOWN;
	newent.e4d_namlen = strlen(idesc->id_name);
	newent.e4d_type = ext4fs_mode_to_ft(typemap[idesc->id_parent] << 12);
	newlen = EXT4FS_DIRSIZ(newent.e4d_namlen);
	if (dirp->e4d_ino != 0)
		oldlen = EXT4FS_DIRSIZ(dirp->e4d_namlen);
	else
		oldlen = 0;
	if (letoh16(dirp->e4d_reclen) - oldlen < newlen)
		return (KEEPON);
	newent.e4d_reclen = htole16(letoh16(dirp->e4d_reclen) - oldlen);
	dirp->e4d_reclen = htole16(oldlen);
	dirp = (struct ext4fs_directory *)(((char *)dirp) + oldlen);
	dirp->e4d_ino = htole32(idesc->id_parent);
	dirp->e4d_reclen = newent.e4d_reclen;
	dirp->e4d_namlen = newent.e4d_namlen;
	dirp->e4d_type = newent.e4d_type;
	memcpy(dirp->e4d_name, idesc->id_name, (size_t)(dirp->e4d_namlen));
	return (ALTERED|STOP);
}

static int
chgino(struct inodesc *idesc)
{
	struct ext4fs_directory *dirp = idesc->id_dirp;
	u_int8_t namlen = dirp->e4d_namlen;

	if (strlen(idesc->id_name) != namlen ||
	    strncmp(dirp->e4d_name, idesc->id_name, (int)namlen))
		return (KEEPON);
	dirp->e4d_ino = htole32(idesc->id_parent);
	dirp->e4d_type = ext4fs_mode_to_ft(typemap[idesc->id_parent] << 12);
	return (ALTERED|STOP);
}

int
linkup(ino_t orphan, ino_t parentdir)
{
	struct ext4fs_dinode *dp;
	int lostdir;
	ino_t oldlfdir;
	struct inodesc idesc;
	char tempname[BUFSIZ];

	memset(&idesc, 0, sizeof(struct inodesc));
	dp = ginode(orphan);
	lostdir = (letoh16(dp->i_mode) & IFMT) == IFDIR;
	pwarn("UNREF %s ", lostdir ? "DIR" : "FILE");
	pinode(orphan);
	if (preen && inosize(dp) == 0)
		return (0);
	if (preen)
		printf(" (RECONNECTED)\n");
	else
		if (reply("RECONNECT") == 0)
			return (0);
	if (lfdir == 0) {
		dp = ginode(EXT4FS_INODE_ROOT_DIR);
		idesc.id_name = lfname;
		idesc.id_type = DATA;
		idesc.id_func = findino;
		idesc.id_number = EXT4FS_INODE_ROOT_DIR;
		if ((ckinode(dp, &idesc) & FOUND) != 0) {
			lfdir = idesc.id_parent;
		} else {
			pwarn("NO lost+found DIRECTORY");
			if (preen || reply("CREATE")) {
				lfdir = allocdir(EXT4FS_INODE_ROOT_DIR,
				    (ino_t)0, lfmode);
				if (lfdir != 0) {
					if (makeentry(EXT4FS_INODE_ROOT_DIR,
					    lfdir, lfname) != 0) {
						if (preen)
							printf(" (CREATED)\n");
					} else {
						freedir(lfdir,
						    EXT4FS_INODE_ROOT_DIR);
						lfdir = 0;
						if (preen)
							printf("\n");
					}
				}
			}
		}
		if (lfdir == 0) {
			pfatal("SORRY. CANNOT CREATE lost+found DIRECTORY");
			printf("\n\n");
			return (0);
		}
	}
	dp = ginode(lfdir);
	if ((letoh16(dp->i_mode) & IFMT) != IFDIR) {
		pfatal("lost+found IS NOT A DIRECTORY");
		if (reply("REALLOCATE") == 0)
			return (0);
		oldlfdir = lfdir;
		if ((lfdir = allocdir(EXT4FS_INODE_ROOT_DIR, (ino_t)0,
		    lfmode)) == 0) {
			pfatal("SORRY. CANNOT CREATE lost+found DIRECTORY\n\n");
			return (0);
		}
		if ((changeino(EXT4FS_INODE_ROOT_DIR, lfname, lfdir) &
		    ALTERED) == 0) {
			pfatal("SORRY. CANNOT CREATE lost+found DIRECTORY\n\n");
			return (0);
		}
		inodirty();
		idesc.id_type = ADDR;
		idesc.id_func = pass4check;
		idesc.id_number = oldlfdir;
		adjust(&idesc, lncntp[oldlfdir] + 1);
		lncntp[oldlfdir] = 0;
		dp = ginode(lfdir);
	}
	if (statemap[lfdir] != DFOUND) {
		pfatal("SORRY. NO lost+found DIRECTORY\n\n");
		return (0);
	}
	(void)lftempname(tempname, orphan);
	if (makeentry(lfdir, orphan, tempname) == 0) {
		pfatal("SORRY. NO SPACE IN lost+found DIRECTORY");
		printf("\n\n");
		return (0);
	}
	lncntp[orphan]--;
	if (lostdir) {
		if ((changeino(orphan, "..", lfdir) & ALTERED) == 0 &&
		    parentdir != (ino_t)-1)
			(void)makeentry(orphan, lfdir, "..");
		dp = ginode(lfdir);
		dp->i_links_count =
		    htole16(letoh16(dp->i_links_count) + 1);
		inodirty();
		lncntp[lfdir]++;
		pwarn("DIR I=%llu CONNECTED. ",
		    (unsigned long long)orphan);
		if (parentdir != (ino_t)-1)
			printf("PARENT WAS I=%llu\n",
			    (unsigned long long)parentdir);
		if (preen == 0)
			printf("\n");
	}
	return (1);
}

int
changeino(ino_t dir, char *name, ino_t newnum)
{
	struct inodesc idesc;

	memset(&idesc, 0, sizeof(struct inodesc));
	idesc.id_type = DATA;
	idesc.id_func = chgino;
	idesc.id_number = dir;
	idesc.id_fix = DONTKNOW;
	idesc.id_name = name;
	idesc.id_parent = newnum;
	return (ckinode(ginode(dir), &idesc));
}

int
makeentry(ino_t parent, ino_t ino, char *name)
{
	struct ext4fs_dinode *dp;
	struct inodesc idesc;
	char pathbuf[PATH_MAX + 1];

	if ((parent < EXT4FS_INODE_FIRST && parent != EXT4FS_INODE_ROOT_DIR)
	    || parent >= maxino ||
	    (ino < EXT4FS_INODE_FIRST && ino < EXT4FS_INODE_ROOT_DIR) ||
	    ino >= maxino)
		return (0);
	memset(&idesc, 0, sizeof(struct inodesc));
	idesc.id_type = DATA;
	idesc.id_func = mkentry;
	idesc.id_number = parent;
	idesc.id_parent = ino;
	idesc.id_fix = DONTKNOW;
	idesc.id_name = name;
	dp = ginode(parent);
	if (inosize(dp) % sblock.m_block_size) {
		inossize(dp, roundup(inosize(dp), sblock.m_block_size));
		inodirty();
	}
	if ((ckinode(dp, &idesc) & ALTERED) != 0)
		return (1);
	getpathname(pathbuf, sizeof pathbuf, parent, parent);
	dp = ginode(parent);
	if (expanddir(dp, pathbuf) == 0)
		return (0);
	return (ckinode(dp, &idesc) & ALTERED);
}

static int
expanddir(struct ext4fs_dinode *dp, char *name)
{
	pwarn("NO SPACE LEFT IN %s", name);
	if (preen)
		printf(" (CANNOT EXPAND WITH EXTENTS YET)\n");
	else
		printf("\nCANNOT EXPAND DIRECTORY WITH EXTENTS YET\n");
	return (0);
}

int
allocdir(ino_t parent, ino_t request, int mode)
{
	ino_t ino;
	struct ext4fs_dinode *dp;
	struct bufarea *bp;

	ino = allocino(request, IFDIR|mode);
	if (ino == 0)
		return (0);
	memset(&dirhead, 0, sizeof(dirhead));
	dirhead.dot_ino = htole32(ino);
	dirhead.dot_reclen = htole16(12);
	dirhead.dot_namlen = 1;
	dirhead.dot_type = EXT4FS_FT_DIR;
	dirhead.dot_name[0] = '.';
	dirhead.dotdot_ino = htole32(parent);
	dirhead.dotdot_reclen = htole16(sblock.m_block_size - 12);
	dirhead.dotdot_namlen = 2;
	dirhead.dotdot_type = EXT4FS_FT_DIR;
	dirhead.dotdot_name[0] = '.';
	dirhead.dotdot_name[1] = '.';
	dp = ginode(ino);
	u_int64_t blk = letoh32(dp->i_extent[0].e_start_lo) |
	    ((u_int64_t)letoh16(dp->i_extent[0].e_start_hi) << 32);
	bp = getdirblk(blk, sblock.m_block_size);
	if (bp->b_errs) {
		freeino(ino);
		return (0);
	}
	memcpy(bp->b_un.b_buf, &dirhead, sizeof(struct ext4fs_dirtemplate));
	dirty(bp);
	dp->i_links_count = htole16(2);
	inodirty();
	if (ino == EXT4FS_INODE_ROOT_DIR) {
		lncntp[ino] = letoh16(dp->i_links_count);
		cacheino(dp, ino);
		return(ino);
	}
	if (statemap[parent] != DSTATE && statemap[parent] != DFOUND) {
		freeino(ino);
		return (0);
	}
	cacheino(dp, ino);
	statemap[ino] = statemap[parent];
	if (statemap[ino] == DSTATE) {
		lncntp[ino] = letoh16(dp->i_links_count);
		lncntp[parent]++;
	}
	dp = ginode(parent);
	dp->i_links_count = htole16(letoh16(dp->i_links_count) + 1);
	inodirty();
	return (ino);
}

static void
freedir(ino_t ino, ino_t parent)
{
	struct ext4fs_dinode *dp;

	if (ino != parent) {
		dp = ginode(parent);
		dp->i_links_count =
		    htole16(letoh16(dp->i_links_count) - 1);
		inodirty();
	}
	freeino(ino);
}

static int
lftempname(char *bufp, ino_t ino)
{
	ino_t in;
	char *cp;
	int namlen;

	cp = bufp + 2;
	for (in = maxino; in > 0; in /= 10)
		cp++;
	*--cp = 0;
	namlen = cp - bufp;
	in = ino;
	while (cp > bufp) {
		*--cp = (in % 10) + '0';
		in /= 10;
	}
	*cp = '#';
	return (namlen);
}

static struct bufarea *
getdirblk(u_int64_t blkno, long size)
{
	if (pdirbp != 0)
		pdirbp->b_flags &= ~B_INUSE;
	pdirbp = getdatablk(blkno, size);
	return (pdirbp);
}
