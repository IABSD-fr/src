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
#include <time.h>

#include "fsck.h"
#include "extern.h"
#include "fsutil.h"

static u_int64_t badblk;
static u_int64_t dupblk;
static void checkinode(ino_t, struct inodesc *);
int ext4fs_block_group_has_super_block(int);

static void
mark_indirect(u_int64_t blk, int level)
{
	char *buf;
	u_int32_t i, nptrs;

	if (blk == 0 || blk >= maxfsblock)
		return;
	setbmap(blk);
	if (level == 0)
		return;
	buf = malloc(sblock.m_block_size);
	if (buf == NULL)
		return;
	if (bread(fsreadfd, buf, EXT4FS_FSBTODB(&sblock, blk),
	    sblock.m_block_size) != 0) {
		free(buf);
		return;
	}
	nptrs = sblock.m_block_size / sizeof(u_int32_t);
	for (i = 0; i < nptrs; i++) {
		u_int32_t b = letoh32(((u_int32_t *)buf)[i]);
		if (b == 0)
			continue;
		if (level == 1)
			setbmap(b);
		else
			mark_indirect(b, level - 1);
	}
	free(buf);
}

static void
mark_reserved_inode_blocks(ino_t ino)
{
	u_int32_t group, index;
	u_int64_t itb, blk;
	u_int32_t off;
	char *ibuf;
	struct ext4fs_dinode *di;
	struct ext4fs_extent_header *eh;
	struct ext4fs_extent *ext;
	struct ext4fs_extent_idx *idx;
	u_int16_t entries, depth, n;

	ibuf = malloc(sblock.m_block_size);
	if (ibuf == NULL)
		return;

	group = (ino - 1) / sblock.m_inodes_per_group;
	index = (ino - 1) % sblock.m_inodes_per_group;
	itb = letoh32(sblock.m_gd[group].bgd_inode_table_block_lo);
	if (sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
		itb |= (u_int64_t)letoh32(sblock.m_gd[group].bgd_inode_table_block_hi) << 32;
	blk = itb + (index * sblock.m_inode_size) / sblock.m_block_size;
	off = (index * sblock.m_inode_size) % sblock.m_block_size;

	if (bread(fsreadfd, ibuf, EXT4FS_FSBTODB(&sblock, blk),
	    sblock.m_block_size) != 0) {
		free(ibuf);
		return;
	}

	di = (struct ext4fs_dinode *)(ibuf + off);

	if (letoh16(di->i_mode) == 0) {
		free(ibuf);
		return;
	}

	if (!(letoh32(di->i_flags) & EXTFS_INODE_FLAG_EXTENTS)) {
		u_int32_t i;
		for (i = 0; i < 12; i++) {
			u_int32_t b = letoh32(di->i_block[i]);
			if (b != 0)
				setbmap(b);
		}
		if (letoh32(di->i_block[12]) != 0)
			mark_indirect(letoh32(di->i_block[12]), 1);
		if (letoh32(di->i_block[13]) != 0)
			mark_indirect(letoh32(di->i_block[13]), 2);
		if (letoh32(di->i_block[14]) != 0)
			mark_indirect(letoh32(di->i_block[14]), 3);
		free(ibuf);
		return;
	}

	eh = &di->i_extent_header;
	if (letoh16(eh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC) {
		free(ibuf);
		return;
	}

	entries = letoh16(eh->eh_entries);
	depth = letoh16(eh->eh_depth);

	if (depth == 0) {
		ext = di->i_extent;
		for (n = 0; n < entries; n++) {
			u_int64_t pblk = (u_int64_t)letoh16(ext[n].e_start_hi) << 32 |
			    letoh32(ext[n].e_start_lo);
			u_int32_t len = letoh16(ext[n].e_len);
			u_int32_t j;
			if (len > 32768)
				len -= 32768;
			for (j = 0; j < len; j++)
				setbmap(pblk + j);
		}
	} else {
		idx = di->i_extent_idx;
		for (n = 0; n < entries; n++) {
			u_int64_t iblk = (u_int64_t)letoh16(idx[n].ei_leaf_hi) << 32 |
			    letoh32(idx[n].ei_leaf_lo);
			char *lbuf;
			struct ext4fs_extent_header *leh;
			struct ext4fs_extent *lext;
			u_int16_t lentries, j;

			setbmap(iblk);
			lbuf = malloc(sblock.m_block_size);
			if (lbuf == NULL)
				continue;
			if (bread(fsreadfd, lbuf,
			    EXT4FS_FSBTODB(&sblock, iblk),
			    sblock.m_block_size) != 0) {
				free(lbuf);
				continue;
			}
			leh = (struct ext4fs_extent_header *)lbuf;
			if (letoh16(leh->eh_magic) != EXT4FS_EXTENT_HEADER_MAGIC) {
				free(lbuf);
				continue;
			}
			lentries = letoh16(leh->eh_entries);
			lext = (struct ext4fs_extent *)(leh + 1);
			for (j = 0; j < lentries; j++) {
				u_int64_t pblk = (u_int64_t)letoh16(lext[j].e_start_hi) << 32 |
				    letoh32(lext[j].e_start_lo);
				u_int32_t len = letoh16(lext[j].e_len);
				u_int32_t k;
				if (len > 32768)
					len -= 32768;
				for (k = 0; k < len; k++)
					setbmap(pblk + k);
			}
			free(lbuf);
		}
	}
	free(ibuf);
}

static void
mark_reserved_blocks(void)
{
	ino_t ino;
	ino_t special[] = {
		sblock.m_journal_inode_number,
		sblock.m_user_quota_inode,
		sblock.m_group_quota_inode,
		sblock.m_project_quota_inode,
		sblock.m_orphan_file_inode,
		0
	};
	int i;

	for (ino = 1; ino < EXT4FS_INODE_FIRST; ino++) {
		if (ino == EXT4FS_INODE_ROOT_DIR)
			continue;
		mark_reserved_inode_blocks(ino);
	}
	for (i = 0; special[i] != 0; i++) {
		if (special[i] >= EXT4FS_INODE_FIRST)
			mark_reserved_inode_blocks(special[i]);
	}
}

void
pass1(void)
{
	ino_t inumber;
	int c;
	u_int32_t i;
	u_int64_t dbase;
	struct inodesc idesc;

	for (c = 0; c < sblock.m_block_group_count; c++) {
		u_int64_t itb, bb, ib;
		u_int32_t ngdb = sblock.m_block_group_descriptor_blocks_count;

		dbase = c * sblock.m_blocks_per_group +
		    sblock.m_first_data_block;

		itb = letoh32(sblock.m_gd[c].bgd_inode_table_block_lo);
		if (sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			itb |= (u_int64_t)letoh32(sblock.m_gd[c].bgd_inode_table_block_hi) << 32;
		for (i = 0; i < sblock.m_inode_table_blocks_per_group; i++)
			setbmap(itb + i);

		bb = letoh32(sblock.m_gd[c].bgd_block_bitmap_block_lo);
		if (sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			bb |= (u_int64_t)letoh32(sblock.m_gd[c].bgd_block_bitmap_block_hi) << 32;
		setbmap(bb);

		ib = letoh32(sblock.m_gd[c].bgd_inode_bitmap_block_lo);
		if (sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			ib |= (u_int64_t)letoh32(sblock.m_gd[c].bgd_inode_bitmap_block_hi) << 32;
		setbmap(ib);

		if ((sblock.m_feature_ro_compat &
		    EXT4FS_FEATURE_RO_COMPAT_SPARSE_SUPER) == 0 ||
		    ext4fs_block_group_has_super_block(c)) {
			setbmap(dbase);
			for (i = 1; i <= ngdb; i++)
				setbmap(dbase + i);
			for (i = 0; i < sblock.m_reserved_bgdt_blocks; i++)
				setbmap(dbase + ngdb + 1 + i);
		}

		if (c == 0) {
			for (i = 0; i < dbase; i++)
				setbmap(i);
		}
	}

	mark_reserved_blocks();

	memset(&idesc, 0, sizeof(struct inodesc));
	idesc.id_type = ADDR;
	idesc.id_func = pass1check;
	inumber = 1;
	n_files = n_blks = 0;
	resetinodebuf();
	for (c = 0; c < sblock.m_block_group_count; c++) {
		u_int16_t bgd_flags = letoh16(sblock.m_gd[c].bgd_flags);
		u_int32_t itable_unused =
		    letoh16(sblock.m_gd[c].bgd_inode_table_unused_lo);
		u_int64_t ibitmap_blk;
		char *ibitmap = NULL;

		if (sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			itable_unused |= (u_int32_t)
			    letoh16(sblock.m_gd[c].bgd_inode_table_unused_hi) << 16;

		if (!(bgd_flags & EXT4FS_BGD_FLAG_INODE_UNINIT)) {
			ibitmap_blk = letoh32(sblock.m_gd[c].bgd_inode_bitmap_block_lo);
			if (sblock.m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
				ibitmap_blk |= (u_int64_t)letoh32(
				    sblock.m_gd[c].bgd_inode_bitmap_block_hi) << 32;
			ibitmap = malloc(sblock.m_block_size);
			if (ibitmap != NULL)
				bread(fsreadfd, ibitmap,
				    EXT4FS_FSBTODB(&sblock, ibitmap_blk),
				    sblock.m_block_size);
		}

		for (i = 0;
		    i < sblock.m_inodes_per_group &&
		    inumber <= sblock.m_inodes_count;
		    i++, inumber++) {
			if (inumber < EXT4FS_INODE_ROOT_DIR)
				continue;
			if (bgd_flags & EXT4FS_BGD_FLAG_INODE_UNINIT) {
				getnextinode(inumber);
				statemap[inumber] = USTATE;
				continue;
			}
			if (i >= sblock.m_inodes_per_group - itable_unused) {
				getnextinode(inumber);
				statemap[inumber] = USTATE;
				continue;
			}
			if (ibitmap != NULL && !isset(ibitmap, i)) {
				getnextinode(inumber);
				statemap[inumber] = USTATE;
				continue;
			}
			checkinode(inumber, &idesc);
		}
		free(ibitmap);
	}
	freeinodebuf();
}

static void
checkinode(ino_t inumber, struct inodesc *idesc)
{
	struct ext4fs_dinode *dp;
	struct zlncnt *zlnp;
	mode_t mode;

	dp = getnextinode(inumber);
	if (inumber < EXT4FS_INODE_FIRST && inumber != EXT4FS_INODE_ROOT_DIR)
		return;
	if (inumber == sblock.m_journal_inode_number ||
	    (sblock.m_user_quota_inode && inumber == sblock.m_user_quota_inode) ||
	    (sblock.m_group_quota_inode && inumber == sblock.m_group_quota_inode) ||
	    (sblock.m_project_quota_inode && inumber == sblock.m_project_quota_inode) ||
	    (sblock.m_orphan_file_inode && inumber == sblock.m_orphan_file_inode)) {
		statemap[inumber] = SSTATE;
		n_files++;
		return;
	}

	if (sblock.m_feature_ro_compat &
	    EXT4FS_FEATURE_RO_COMPAT_METADATA_CSUM) {
		if (letoh16(dp->i_mode) != 0 ||
		    letoh16(dp->i_links_count) != 0 ||
		    letoh32(dp->i_dtime) != 0) {
			if (ext4fs_inode_csum_verify(&sblock,
			    (struct ext4fs_dinode_256 *)dp, inumber) != 0) {
				pfatal("INODE CHECKSUM INVALID I=%llu",
				    (unsigned long long)inumber);
				if (reply("CLEAR") == 1) {
					dp = ginode(inumber);
					clearinode(dp);
					inodirty();
					statemap[inumber] = USTATE;
					return;
				}
			}
		}
	}

	mode = letoh16(dp->i_mode) & IFMT;
	if (mode == 0 || (dp->i_dtime != 0 && dp->i_links_count == 0)) {
		if (mode == 0 && inosize(dp) != 0) {
			pfatal("PARTIALLY ALLOCATED INODE I=%llu",
			    (unsigned long long)inumber);
			if (reply("CLEAR") == 1) {
				dp = ginode(inumber);
				clearinode(dp);
				inodirty();
			}
		}
		statemap[inumber] = USTATE;
		return;
	}
	lastino = inumber;
	if (dp->i_dtime != 0) {
		time_t t = letoh32(dp->i_dtime);
		char *p = ctime(&t);
		if (p)
			pwarn("INODE I=%llu HAS DTIME=%12.12s %4.4s",
			    (unsigned long long)inumber, &p[4], &p[20]);
		else
			pwarn("INODE I=%llu HAS DTIME=%lld",
			    (unsigned long long)inumber, t);
		if (preen)
			printf(" (CORRECTED)\n");
		if (preen || reply("CORRECT")) {
			dp = ginode(inumber);
			dp->i_dtime = 0;
			inodirty();
		}
	}
	if (inosize(dp) + sblock.m_block_size - 1 < inosize(dp)) {
		if (debug)
			printf("bad size %llu:",
			    (unsigned long long)inosize(dp));
		goto unknown;
	}
	if (!preen && mode == IFMT && reply("HOLD BAD BLOCK") == 1) {
		dp = ginode(inumber);
		dp->i_mode = htole16(IFREG|0600);
		inossize(dp, sblock.m_block_size);
		inodirty();
	}
	if (ftypeok(dp) == 0)
		goto unknown;
	n_files++;
	lncntp[inumber] = letoh16(dp->i_links_count);
	if (dp->i_links_count == 0) {
		zlnp = malloc(sizeof *zlnp);
		if (zlnp == NULL) {
			pfatal("LINK COUNT TABLE OVERFLOW");
			if (reply("CONTINUE") == 0)
				errexit("%s\n", "");
		} else {
			zlnp->zlncnt = inumber;
			zlnp->next = zlnhead;
			zlnhead = zlnp;
		}
	}
	if (mode == IFDIR) {
		if (inosize(dp) == 0)
			statemap[inumber] = DCLEAR;
		else
			statemap[inumber] = DSTATE;
		cacheino(dp, inumber);
	} else {
		statemap[inumber] = FSTATE;
	}
	typemap[inumber] = ext4fs_mode_to_ft(mode);
	badblk = dupblk = 0;
	idesc->id_number = inumber;
	(void)ckinode(dp, idesc);
	idesc->id_entryno *= btodb(sblock.m_block_size);
	if (letoh32(dp->i_blocks_lo) != idesc->id_entryno) {
		pwarn("INCORRECT BLOCK COUNT I=%llu (%d should be %d)",
		    (unsigned long long)inumber,
		    letoh32(dp->i_blocks_lo), idesc->id_entryno);
		if (preen)
			printf(" (CORRECTED)\n");
		else if (reply("CORRECT") == 0)
			return;
		dp = ginode(inumber);
		dp->i_blocks_lo = htole32(idesc->id_entryno);
		inodirty();
	}
	return;
unknown:
	pfatal("UNKNOWN FILE TYPE I=%llu", (unsigned long long)inumber);
	statemap[inumber] = FCLEAR;
	if (reply("CLEAR") == 1) {
		statemap[inumber] = USTATE;
		dp = ginode(inumber);
		clearinode(dp);
		inodirty();
	}
}

int
pass1check(struct inodesc *idesc)
{
	int res = KEEPON;
	int anyout, nfrags;
	u_int64_t blkno = idesc->id_blkno;
	struct dups *dlp;
	struct dups *new;

	if ((anyout = chkrange(blkno, idesc->id_numfrags)) != 0) {
		blkerror(idesc->id_number, "BAD", blkno);
		if (badblk++ >= MAXBAD) {
			pwarn("EXCESSIVE BAD BLKS I=%llu",
			    (unsigned long long)idesc->id_number);
			if (preen)
				printf(" (SKIPPING)\n");
			else if (reply("CONTINUE") == 0)
				errexit("%s\n", "");
			return (STOP);
		}
	}
	for (nfrags = idesc->id_numfrags; nfrags > 0; blkno++, nfrags--) {
		if (anyout && chkrange(blkno, 1)) {
			res = SKIP;
		} else if (!testbmap(blkno)) {
			n_blks++;
			setbmap(blkno);
		} else {
			blkerror(idesc->id_number, "DUP", blkno);
			if (dupblk++ >= MAXDUP) {
				pwarn("EXCESSIVE DUP BLKS I=%llu",
				    (unsigned long long)idesc->id_number);
				if (preen)
					printf(" (SKIPPING)\n");
				else if (reply("CONTINUE") == 0)
					errexit("%s\n", "");
				return (STOP);
			}
			new = malloc(sizeof(struct dups));
			if (new == NULL) {
				pfatal("DUP TABLE OVERFLOW.");
				if (reply("CONTINUE") == 0)
					errexit("%s\n", "");
				return (STOP);
			}
			new->dup = blkno;
			if (muldup == 0) {
				duplist = muldup = new;
				new->next = 0;
			} else {
				new->next = muldup->next;
				muldup->next = new;
			}
			for (dlp = duplist; dlp != muldup; dlp = dlp->next)
				if (dlp->dup == blkno)
					break;
			if (dlp == muldup && dlp->dup != blkno)
				muldup = new;
		}
		idesc->id_entryno++;
	}
	return (res);
}

int
ext4fs_block_group_has_super_block(int group)
{
	int a3, a5, a7;

	if (group == 0 || group == 1)
		return 1;
	for (a3 = 3, a5 = 5, a7 = 7;
	    a3 <= group || a5 <= group || a7 <= group;
	    a3 *= 3, a5 *= 5, a7 *= 7)
		if (group == a3 || group == a5 || group == a7)
			return 1;
	return 0;
}
