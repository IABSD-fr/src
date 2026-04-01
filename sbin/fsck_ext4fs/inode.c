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
#ifndef SMALL
#include <pwd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "fsck.h"
#include "fsutil.h"
#include "extern.h"

#define EXT4FS_DINODE_SIZE 256

#define fsck_ino_to_fsba(fs, x) \
	(letoh32((fs)->m_gd[((x)-1) / (fs)->m_inodes_per_group].bgd_inode_table_block_lo) + \
	(((x)-1) % (fs)->m_inodes_per_group) / (fs)->m_inodes_per_block)

static ino_t startinum;

static int walk_extents(struct ext4fs_dinode *, struct inodesc *);

u_int64_t
inosize(struct ext4fs_dinode *dp)
{
	u_int64_t size = letoh32(dp->i_size_lo);

	if ((letoh16(dp->i_mode) & IFMT) == IFREG)
		size |= (u_int64_t)letoh32(dp->i_size_hi) << 32;
	return size;
}

void
inossize(struct ext4fs_dinode *dp, u_int64_t size)
{
	if ((letoh16(dp->i_mode) & IFMT) == IFREG)
		dp->i_size_hi = htole32(size >> 32);
	else if (size >= 0x80000000U) {
		pfatal("TRYING TO SET FILESIZE TO %llu ON MODE %x FILE\n",
		    (unsigned long long)size, letoh16(dp->i_mode) & IFMT);
		return;
	}
	dp->i_size_lo = htole32(size);
}

static int walk_indirect(struct ext4fs_dinode *, struct inodesc *);
static int walk_indirect_block(struct inodesc *, u_int64_t, int,
    int (*)(struct inodesc *));

static int
walk_leaf_extents(struct ext4fs_extent *ext, u_int16_t entries,
    struct inodesc *idesc, int (*func)(struct inodesc *))
{
	int n, ret;
	u_int64_t pblk;
	u_int32_t lblk, len, prev_end = 0;
	u_int32_t i;

	for (n = 0; n < entries; n++, ext++) {
		u_int16_t raw_len = letoh16(ext->e_len);

		lblk = letoh32(ext->e_block);
		pblk = letoh32(ext->e_start_lo) |
		    ((u_int64_t)letoh16(ext->e_start_hi) << 32);
		len = raw_len > 32768 ? raw_len - 32768 : raw_len;

		if (len == 0) {
			pwarn("EXTENT WITH ZERO LENGTH I=%llu\n",
			    (unsigned long long)idesc->id_number);
			return (STOP);
		}
		if (n > 0 && lblk < prev_end) {
			pwarn("OVERLAPPING EXTENTS I=%llu "
			    "(lblk %u < prev_end %u)\n",
			    (unsigned long long)idesc->id_number,
			    lblk, prev_end);
			return (STOP);
		}
		if (pblk + len > maxfsblock) {
			pwarn("EXTENT BEYOND FILESYSTEM I=%llu "
			    "(pblk %llu + len %u > %llu)\n",
			    (unsigned long long)idesc->id_number,
			    (unsigned long long)pblk, len,
			    (unsigned long long)maxfsblock);
			return (STOP);
		}
		prev_end = lblk + len;

		for (i = 0; i < len; i++) {
			idesc->id_blkno = pblk + i;
			idesc->id_numfrags = 1;
			ret = (*func)(idesc);
			if (ret & STOP)
				return (ret);
		}
	}
	return (KEEPON);
}

static int
walk_extents(struct ext4fs_dinode *dp, struct inodesc *idesc)
{
	struct ext4fs_extent_header *eh;
	struct ext4fs_extent_idx *idx;
	u_int16_t entries, depth;
	int ret, n;
	int (*func)(struct inodesc *);

	eh = &dp->i_extent_header;
	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC) {
		pwarn("BAD EXTENT MAGIC 0x%x I=%llu\n",
		    letoh16(eh->eh_magic),
		    (unsigned long long)idesc->id_number);
		return (STOP);
	}

	entries = letoh16(eh->eh_entries);
	depth = letoh16(eh->eh_depth);

	if (depth > EXT4FS_EXTENT_DEPTH_MAX) {
		pwarn("EXCESSIVE EXTENT DEPTH %u I=%llu\n",
		    depth, (unsigned long long)idesc->id_number);
		return (STOP);
	}
	if (entries > letoh16(eh->eh_max)) {
		pwarn("EXTENT ENTRIES %u > MAX %u I=%llu\n",
		    entries, letoh16(eh->eh_max),
		    (unsigned long long)idesc->id_number);
		return (STOP);
	}

	if (idesc->id_type == ADDR)
		func = idesc->id_func;
	else
		func = dirscan;

	if (depth == 0)
		return walk_leaf_extents(dp->i_extent, entries, idesc, func);

	idx = dp->i_extent_idx;
	for (n = 0; n < entries; n++, idx++) {
		u_int64_t iblk = letoh32(idx->ei_leaf_lo) |
		    ((u_int64_t)letoh16(idx->ei_leaf_hi) << 32);
		struct bufarea *bp;
		struct ext4fs_extent_header *ceh;
		u_int16_t centries, cdepth;

		if (iblk >= maxfsblock) {
			pwarn("EXTENT INDEX BEYOND FILESYSTEM I=%llu\n",
			    (unsigned long long)idesc->id_number);
			return (STOP);
		}

		if (idesc->id_type == ADDR) {
			idesc->id_blkno = iblk;
			idesc->id_numfrags = 1;
			ret = (*func)(idesc);
			if (ret & STOP)
				return (ret);
		}

		bp = getdatablk(iblk, sblock.m_block_size);
		ceh = (struct ext4fs_extent_header *)bp->b_un.b_buf;
		if (letoh16(ceh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC) {
			pwarn("BAD EXTENT MAGIC IN INDEX BLOCK I=%llu\n",
			    (unsigned long long)idesc->id_number);
			bp->b_flags &= ~B_INUSE;
			return (STOP);
		}
		centries = letoh16(ceh->eh_entries);
		cdepth = letoh16(ceh->eh_depth);
		if (cdepth != depth - 1) {
			pwarn("EXTENT DEPTH MISMATCH I=%llu "
			    "(expected %u got %u)\n",
			    (unsigned long long)idesc->id_number,
			    depth - 1, cdepth);
			bp->b_flags &= ~B_INUSE;
			return (STOP);
		}
		if (centries > letoh16(ceh->eh_max)) {
			pwarn("EXTENT ENTRIES %u > MAX %u IN INDEX I=%llu\n",
			    centries, letoh16(ceh->eh_max),
			    (unsigned long long)idesc->id_number);
			bp->b_flags &= ~B_INUSE;
			return (STOP);
		}
		if (cdepth == 0) {
			struct ext4fs_extent *cext =
			    (struct ext4fs_extent *)(ceh + 1);
			ret = walk_leaf_extents(cext, centries, idesc, func);
		} else {
			pwarn("EXTENT DEPTH > 1 NOT FULLY SUPPORTED I=%llu\n",
			    (unsigned long long)idesc->id_number);
			ret = KEEPON;
		}
		bp->b_flags &= ~B_INUSE;
		if (ret & STOP)
			return (ret);
	}
	return (KEEPON);
}

int
ckinode(struct ext4fs_dinode *dp, struct inodesc *idesc)
{
	mode_t mode;

	if (idesc->id_fix != IGNORE)
		idesc->id_fix = DONTKNOW;
	idesc->id_entryno = 0;
	idesc->id_filesize = inosize(dp);
	mode = letoh16(dp->i_mode) & IFMT;
	if (mode == IFBLK || mode == IFCHR || mode == IFIFO ||
	    (mode == IFLNK && (inosize(dp) < EXT4FS_SYMLINK_LEN_MAX)))
		return (KEEPON);

	if (letoh32(dp->i_flags) & EXTFS_INODE_FLAG_EXTENTS)
		return walk_extents(dp, idesc);

	return walk_indirect(dp, idesc);
}

static int
walk_indirect(struct ext4fs_dinode *dp, struct inodesc *idesc)
{
	int n, ret;
	u_int32_t blk;
	int (*func)(struct inodesc *);

	if (idesc->id_type == ADDR)
		func = idesc->id_func;
	else
		func = dirscan;

	for (n = 0; n < 12; n++) {
		blk = letoh32(dp->i_block[n]);
		if (blk == 0)
			continue;
		idesc->id_blkno = blk;
		idesc->id_numfrags = 1;
		ret = (*func)(idesc);
		if (ret & STOP)
			return (ret);
	}
	for (n = 0; n < 3; n++) {
		blk = letoh32(dp->i_block[12 + n]);
		if (blk == 0)
			continue;
		idesc->id_blkno = blk;
		idesc->id_numfrags = 1;
		if (idesc->id_type == ADDR) {
			ret = (*func)(idesc);
			if (ret & STOP)
				return (ret);
		}
		ret = walk_indirect_block(idesc, blk, n + 1, func);
		if (ret & STOP)
			return (ret);
	}
	return (KEEPON);
}

static int
walk_indirect_block(struct inodesc *idesc, u_int64_t blk, int level,
    int (*func)(struct inodesc *))
{
	struct bufarea *bp;
	u_int32_t *ptrs;
	u_int32_t nptrs, i;
	int ret;

	if (chkrange(blk, 1))
		return (SKIP);
	bp = getdatablk(blk, sblock.m_block_size);
	ptrs = bp->b_un.b_indir;
	nptrs = sblock.m_block_size / sizeof(u_int32_t);

	for (i = 0; i < nptrs; i++) {
		u_int32_t b = letoh32(ptrs[i]);
		if (b == 0)
			continue;
		if (level == 1) {
			idesc->id_blkno = b;
			idesc->id_numfrags = 1;
			ret = (*func)(idesc);
			if (ret & STOP) {
				bp->b_flags &= ~B_INUSE;
				return (ret);
			}
		} else {
			idesc->id_blkno = b;
			idesc->id_numfrags = 1;
			if (idesc->id_type == ADDR) {
				ret = (*func)(idesc);
				if (ret & STOP) {
					bp->b_flags &= ~B_INUSE;
					return (ret);
				}
			}
			ret = walk_indirect_block(idesc, b, level - 1, func);
			if (ret & STOP) {
				bp->b_flags &= ~B_INUSE;
				return (ret);
			}
		}
	}
	bp->b_flags &= ~B_INUSE;
	return (KEEPON);
}

int
chkrange(u_int64_t blk, int cnt)
{
	u_int64_t c, overh;

	if (blk + cnt > maxfsblock)
		return (1);
	c = (blk - sblock.m_first_data_block) / sblock.m_blocks_per_group;
	overh = cgoverhead(c);
	if (blk < sblock.m_blocks_per_group * c + overh +
	    sblock.m_first_data_block) {
		if ((blk + cnt) > sblock.m_blocks_per_group * c + overh +
		    sblock.m_first_data_block) {
			if (debug) {
				printf("blk %llu < cgdmin %llu;",
				    (unsigned long long)blk,
				    (unsigned long long)(sblock.m_blocks_per_group * c +
				    overh + sblock.m_first_data_block));
				printf(" blk + cnt %llu > cgsbase %llu\n",
				    (unsigned long long)(blk + cnt),
				    (unsigned long long)(sblock.m_blocks_per_group * c +
				    overh + sblock.m_first_data_block));
			}
			return (1);
		}
	} else {
		if ((blk + cnt) > sblock.m_blocks_per_group * (c + 1) + overh +
		    sblock.m_first_data_block) {
			if (debug) {
				printf("blk %llu >= cgdmin %llu;",
				    (unsigned long long)blk,
				    (unsigned long long)(sblock.m_blocks_per_group * c +
				    overh + sblock.m_first_data_block));
				printf(" blk + cnt %llu > cgdmax %llu\n",
				    (unsigned long long)(blk + cnt),
				    (unsigned long long)(sblock.m_blocks_per_group * (c + 1) +
				    overh + sblock.m_first_data_block));
			}
			return (1);
		}
	}
	return (0);
}

struct ext4fs_dinode *
ginode(ino_t inumber)
{
	u_int64_t iblk;

	if ((inumber < EXT4FS_INODE_FIRST && inumber != EXT4FS_INODE_ROOT_DIR)
		|| inumber > maxino)
		errexit("bad inode number %llu to ginode\n",
		    (unsigned long long)inumber);
	if (startinum == 0 ||
	    inumber < startinum || inumber >= startinum + sblock.m_inodes_per_block) {
		iblk = fsck_ino_to_fsba(&sblock, inumber);
		if (pbp != 0)
			pbp->b_flags &= ~B_INUSE;
		pbp = getdatablk(iblk, sblock.m_block_size);
		startinum = ((inumber - 1) / sblock.m_inodes_per_block) *
		    sblock.m_inodes_per_block + 1;
	}
	return (struct ext4fs_dinode *)((char *)pbp->b_un.b_buf +
	    ((inumber - 1) % sblock.m_inodes_per_block) * sblock.m_inode_size);
}

ino_t nextino, lastinum;
long readcnt, readpercg, fullcnt, inobufsize, partialcnt, partialsize;
struct ext4fs_dinode *inodebuf;

struct ext4fs_dinode *
getnextinode(ino_t inumber)
{
	long size;
	u_int64_t dblk;
	struct ext4fs_dinode *dp;
	static char *bp;

	if (inumber != nextino++ || inumber > maxino)
		errexit("bad inode number %llu to nextinode\n",
		    (unsigned long long)inumber);
	if (inumber >= lastinum) {
		readcnt++;
		dblk = EXT4FS_FSBTODB(&sblock,
		    fsck_ino_to_fsba(&sblock, lastinum));
		if (readcnt % readpercg == 0) {
			size = partialsize;
			lastinum += partialcnt;
		} else {
			size = inobufsize;
			lastinum += fullcnt;
		}
		(void)bread(fsreadfd, (char *)inodebuf, dblk, size);
		bp = (char *)inodebuf;
	}

	dp = (struct ext4fs_dinode *)bp;
	bp += sblock.m_inode_size;

	return (dp);
}

void
resetinodebuf(void)
{
	long bsize;

	startinum = 0;
	nextino = 1;
	lastinum = 1;
	readcnt = 0;
	bsize = sblock.m_block_size;
	inobufsize = roundup(INOBUFSIZE, bsize);
	fullcnt = inobufsize / sblock.m_inode_size;
	readpercg = sblock.m_inodes_per_group / fullcnt;
	partialcnt = sblock.m_inodes_per_group % fullcnt;
	partialsize = partialcnt * sblock.m_inode_size;
	if (partialcnt != 0) {
		readpercg++;
	} else {
		partialcnt = fullcnt;
		partialsize = inobufsize;
	}
	if (inodebuf == NULL &&
	    (inodebuf = malloc((unsigned)inobufsize)) == NULL)
		errexit("Cannot allocate space for inode buffer\n");
	while (nextino < EXT4FS_INODE_ROOT_DIR)
		(void)getnextinode(nextino);
}

void
freeinodebuf(void)
{
	if (inodebuf != NULL)
		free((char *)inodebuf);
	inodebuf = NULL;
}

void
cacheino(struct ext4fs_dinode *dp, ino_t inumber)
{
	struct inoinfo *inp;
	struct inoinfo **inpp;
	unsigned int blks;

	blks = 15;
	inp = malloc(sizeof(*inp) + (blks - 1) * sizeof(u_int32_t));
	if (inp == NULL)
		return;
	inpp = &inphead[inumber % numdirs];
	inp->i_nexthash = *inpp;
	*inpp = inp;
	inp->i_child = inp->i_sibling = inp->i_parentp = 0;
	if (inumber == EXT4FS_INODE_ROOT_DIR)
		inp->i_parent = EXT4FS_INODE_ROOT_DIR;
	else
		inp->i_parent = (ino_t)0;
	inp->i_dotdot = (ino_t)0;
	inp->i_number = inumber;
	inp->i_isize = inosize(dp);
	inp->i_numblks = 15 * sizeof(u_int32_t);
	memcpy(&inp->i_blks[0], &dp->i_block[0], 15 * sizeof(u_int32_t));
	if (inplast == listmax) {
		listmax += 100;
		inpsort = reallocarray(inpsort, listmax,
		    sizeof(struct inoinfo *));
		if (inpsort == NULL)
			errexit("cannot increase directory list\n");
	}
	inpsort[inplast++] = inp;
}

struct inoinfo *
getinoinfo(ino_t inumber)
{
	struct inoinfo *inp;

	for (inp = inphead[inumber % numdirs]; inp; inp = inp->i_nexthash) {
		if (inp->i_number != inumber)
			continue;
		return (inp);
	}
	errexit("cannot find inode %llu\n", (unsigned long long)inumber);
	return (NULL);
}

void
inocleanup(void)
{
	struct inoinfo **inpp;

	if (inphead == NULL)
		return;
	for (inpp = &inpsort[inplast - 1]; inpp >= inpsort; inpp--)
		free((char *)(*inpp));
	free((char *)inphead);
	free((char *)inpsort);
	inphead = inpsort = NULL;
}

void
inodirty(void)
{
	dirty(pbp);
}

void
clri(struct inodesc *idesc, char *type, int flag)
{
	struct ext4fs_dinode *dp;

	dp = ginode(idesc->id_number);
	if (flag == 1) {
		pwarn("%s %s", type,
		    (letoh16(dp->i_mode) & IFMT) == IFDIR ? "DIR" : "FILE");
		pinode(idesc->id_number);
	}
	if (preen || reply("CLEAR") == 1) {
		if (preen)
			printf(" (CLEARED)\n");
		n_files--;
		(void)ckinode(dp, idesc);
		clearinode(dp);
		statemap[idesc->id_number] = USTATE;
		inodirty();
	}
}

int
findname(struct inodesc *idesc)
{
	struct ext4fs_directory *dirp = idesc->id_dirp;
	u_int8_t namlen = dirp->e4d_namlen;

	if (letoh32(dirp->e4d_ino) != idesc->id_parent)
		return (KEEPON);
	memcpy(idesc->id_name, dirp->e4d_name, (size_t)namlen);
	idesc->id_name[namlen] = '\0';
	return (STOP|FOUND);
}

int
findino(struct inodesc *idesc)
{
	struct ext4fs_directory *dirp = idesc->id_dirp;
	u_int32_t ino = letoh32(dirp->e4d_ino);

	if (ino == 0)
		return (KEEPON);
	if (strcmp(dirp->e4d_name, idesc->id_name) == 0 &&
	    (ino == EXT4FS_INODE_ROOT_DIR || ino >= EXT4FS_INODE_FIRST)
		&& ino <= maxino) {
		idesc->id_parent = ino;
		return (STOP|FOUND);
	}
	return (KEEPON);
}

void
pinode(ino_t ino)
{
	struct ext4fs_dinode *dp;
	const char *p;
	time_t t;
	u_int32_t uid;

	printf(" I=%llu ", (unsigned long long)ino);
	if ((ino < EXT4FS_INODE_FIRST && ino != EXT4FS_INODE_ROOT_DIR) ||
	    ino > maxino)
		return;
	dp = ginode(ino);
	printf(" OWNER=");
	uid = letoh16(dp->i_uid_lo) | (letoh16(dp->i_uid_hi) << 16);
#ifndef SMALL
	if ((p = user_from_uid(uid, 1)) != NULL)
		printf("%s ", p);
	else
#endif
		printf("%u ", uid);
	printf("MODE=%o\n", letoh16(dp->i_mode));
	if (preen)
		printf("%s: ", cdevname());
	printf("SIZE=%llu ", (long long)inosize(dp));
	t = (time_t) letoh32(dp->i_mtime);
	p = ctime(&t);
	if (p)
		printf("MTIME=%12.12s %4.4s ", &p[4], &p[20]);
	else
		printf("MTIME=%lld ", t);
}

void
blkerror(ino_t ino, char *type, u_int64_t blk)
{
	pfatal("%llu %s I=%llu", (unsigned long long)blk, type,
	    (unsigned long long)ino);
	printf("\n");
	switch (statemap[ino]) {

	case FSTATE:
		statemap[ino] = FCLEAR;
		return;

	case DSTATE:
		statemap[ino] = DCLEAR;
		return;

	case FCLEAR:
	case DCLEAR:
		return;

	default:
		errexit("BAD STATE %d TO BLKERR\n", statemap[ino]);
	}
}

ino_t
allocino(ino_t request, int type)
{
	ino_t ino;
	struct ext4fs_dinode *dp;
	time_t t;

	if (request == 0)
		request = EXT4FS_INODE_ROOT_DIR;
	else if (statemap[request] != USTATE)
		return (0);
	for (ino = request; ino < maxino; ino++) {
		if ((ino > EXT4FS_INODE_ROOT_DIR) && (ino < EXT4FS_INODE_FIRST))
			continue;
		if (statemap[ino] == USTATE)
			break;
	}
	if (ino == maxino)
		return (0);
	switch (type & IFMT) {
	case IFDIR:
		statemap[ino] = DSTATE;
		break;
	case IFREG:
	case IFLNK:
		statemap[ino] = FSTATE;
		break;
	default:
		return (0);
	}
	dp = ginode(ino);
	u_int64_t blk = allocblk();
	if (blk == 0) {
		statemap[ino] = USTATE;
		return (0);
	}
	memset(dp, 0, sblock.m_inode_size);
	dp->i_mode = htole16(type);
	dp->i_flags = htole32(EXTFS_INODE_FLAG_EXTENTS);
	dp->i_extent_header.eh_magic = htole16(EXT4FS_EXTENT_HEADER_MAGIC);
	dp->i_extent_header.eh_entries = htole16(1);
	dp->i_extent_header.eh_max = htole16(4);
	dp->i_extent_header.eh_depth = 0;
	dp->i_extent[0].e_block = 0;
	dp->i_extent[0].e_len = htole16(1);
	dp->i_extent[0].e_start_lo = htole32((u_int32_t)blk);
	dp->i_extent[0].e_start_hi = htole16((u_int16_t)(blk >> 32));
	(void)time(&t);
	dp->i_atime = htole32((u_int32_t)t);
	dp->i_mtime = dp->i_ctime = dp->i_atime;
	dp->i_dtime = 0;
	inossize(dp, sblock.m_block_size);
	dp->i_blocks_lo = htole32(sblock.m_block_size / 512);
	dp->i_links_count = htole16(1);
	n_files++;
	inodirty();
	typemap[ino] = ext4fs_mode_to_ft(type);
	return (ino);
}

void
freeino(ino_t ino)
{
	struct inodesc idesc;
	struct ext4fs_dinode *dp;

	memset(&idesc, 0, sizeof(struct inodesc));
	idesc.id_type = ADDR;
	idesc.id_func = pass4check;
	idesc.id_number = ino;
	dp = ginode(ino);
	(void)ckinode(dp, &idesc);
	clearinode(dp);
	inodirty();
	statemap[ino] = USTATE;
	n_files--;
}
