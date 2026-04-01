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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "fsutil.h"
#include "fsck.h"
#include "extern.h"

void print_bmap(u_char *, u_int32_t);

void
pass5(void)
{
	int c;
	struct m_ext4fs *fs = &sblock;
	u_int64_t dbase, dmax;
	u_int64_t d;
	long i, j;
	struct inodesc idesc[3];
	struct bufarea *ino_bitmap = NULL, *blk_bitmap = NULL;
	char *ibmap, *bbmap;
	u_int32_t cs_ndir, cs_nbfree, cs_nifree;
	char msg[255];

	cs_ndir = 0;
	cs_nbfree = 0;
	cs_nifree = 0;

	ibmap = malloc(fs->m_block_size);
	bbmap = malloc(fs->m_block_size);
	if (ibmap == NULL || bbmap == NULL)
		errexit("out of memory\n");

	for (c = 0; c < fs->m_block_group_count; c++) {
		u_int32_t nbfree = 0;
		u_int32_t nifree = fs->m_inodes_per_group;
		u_int32_t ndirs = 0;
		u_int64_t bb, ib;
		u_int16_t bgd_flags = letoh16(fs->m_gd[c].bgd_flags);
		int ino_uninit = bgd_flags & EXT4FS_BGD_FLAG_INODE_UNINIT;
		int blk_uninit = bgd_flags & EXT4FS_BGD_FLAG_BLOCK_UNINIT;

		bb = letoh32(fs->m_gd[c].bgd_block_bitmap_block_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			bb |= (u_int64_t)letoh32(fs->m_gd[c].bgd_block_bitmap_block_hi) << 32;
		ib = letoh32(fs->m_gd[c].bgd_inode_bitmap_block_lo);
		if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_64BIT)
			ib |= (u_int64_t)letoh32(fs->m_gd[c].bgd_inode_bitmap_block_hi) << 32;

		if (blk_bitmap == NULL)
			blk_bitmap = getdatablk(bb, fs->m_block_size);
		else
			getblk(blk_bitmap, bb, fs->m_block_size);
		if (ino_bitmap == NULL)
			ino_bitmap = getdatablk(ib, fs->m_block_size);
		else
			getblk(ino_bitmap, ib, fs->m_block_size);

		memset(bbmap, 0, fs->m_block_size);
		memset(ibmap, 0, fs->m_block_size);
		memset(&idesc[0], 0, sizeof idesc);
		for (i = 0; i < 3; i++)
			idesc[i].id_type = ADDR;

		j = fs->m_inodes_per_group * c + 1;

		for (i = 0; i < fs->m_inodes_per_group; j++, i++) {
			if ((j < EXT4FS_INODE_FIRST) &&
			    (j != EXT4FS_INODE_ROOT_DIR)) {
				setbit(ibmap, i);
				nifree--;
				continue;
			}
			if (j > fs->m_inodes_count) {
				setbit(ibmap, i);
				continue;
			}
			switch (statemap[j]) {

			case USTATE:
				break;

			case DSTATE:
			case DCLEAR:
			case DFOUND:
				ndirs++;
				/* fall through */

			case FSTATE:
			case FCLEAR:
			case SSTATE:
				nifree--;
				setbit(ibmap, i);
				break;

			default:
				errexit("BAD STATE %d FOR INODE I=%llu\n",
				    statemap[j], (unsigned long long)j);
			}
		}

		for (i = fs->m_inodes_per_group / NBBY;
		    i < fs->m_block_size; i++)
			ibmap[i] = 0xff;

		dbase = c * sblock.m_blocks_per_group +
		    sblock.m_first_data_block;
		dmax = (c + 1) * sblock.m_blocks_per_group +
		    sblock.m_first_data_block;

		for (i = 0, d = dbase; d < dmax; d++, i++) {
			if (testbmap(d) || d >= sblock.m_blocks_count) {
				setbit(bbmap, i);
				continue;
			} else {
				nbfree++;
			}
		}
		cs_nbfree += nbfree;
		cs_nifree += nifree;
		cs_ndir += ndirs;

		if (debug &&
		    (letoh16(fs->m_gd[c].bgd_free_blocks_count_lo) != nbfree ||
		    letoh16(fs->m_gd[c].bgd_free_inodes_count_lo) != nifree ||
		    letoh16(fs->m_gd[c].bgd_used_dirs_count_lo) != ndirs)) {
			printf("summary info for cg %d is %d, %d, %d,"
			    "should be %d, %d, %d\n", c,
			    letoh16(fs->m_gd[c].bgd_free_blocks_count_lo),
			    letoh16(fs->m_gd[c].bgd_free_inodes_count_lo),
			    letoh16(fs->m_gd[c].bgd_used_dirs_count_lo),
			    nbfree, nifree, ndirs);
		}
		(void)snprintf(msg, sizeof(msg),
		    "SUMMARY INFORMATIONS WRONG FOR CG #%d", c);
		if ((letoh16(fs->m_gd[c].bgd_free_blocks_count_lo) != nbfree ||
		    letoh16(fs->m_gd[c].bgd_free_inodes_count_lo) != nifree ||
		    letoh16(fs->m_gd[c].bgd_used_dirs_count_lo) != ndirs) &&
		    dofix(&idesc[0], msg)) {
			fs->m_gd[c].bgd_free_blocks_count_lo =
			    htole16(nbfree);
			fs->m_gd[c].bgd_free_inodes_count_lo =
			    htole16(nifree);
			fs->m_gd[c].bgd_used_dirs_count_lo =
			    htole16(ndirs);
			sbdirty();
		}

		if (debug &&
		    memcmp(blk_bitmap->b_un.b_buf, bbmap, fs->m_block_size)) {
			printf("blk_bitmap:\n");
			print_bmap(blk_bitmap->b_un.b_buf, fs->m_block_size);
			printf("bbmap:\n");
			print_bmap(bbmap, fs->m_block_size);
		}

		(void)snprintf(msg, sizeof(msg),
		    "BLK(S) MISSING IN BIT MAPS #%d", c);
		if (!blk_uninit &&
		    memcmp(blk_bitmap->b_un.b_buf, bbmap, fs->m_block_size) &&
		    dofix(&idesc[1], msg)) {
			memcpy(blk_bitmap->b_un.b_buf, bbmap,
			    fs->m_block_size);
			dirty(blk_bitmap);
		}
		if (debug &&
		    memcmp(ino_bitmap->b_un.b_buf, ibmap, fs->m_block_size)) {
			printf("ino_bitmap:\n");
			print_bmap(ino_bitmap->b_un.b_buf, fs->m_block_size);
			printf("ibmap:\n");
			print_bmap(ibmap, fs->m_block_size);
		}
		(void)snprintf(msg, sizeof(msg),
		    "INODE(S) MISSING IN BIT MAPS #%d", c);
		if (!ino_uninit &&
		    memcmp(ino_bitmap->b_un.b_buf, ibmap, fs->m_block_size) &&
		    dofix(&idesc[1], msg)) {
			memcpy(ino_bitmap->b_un.b_buf, ibmap,
			    fs->m_block_size);
			dirty(ino_bitmap);
		}
	}
	if (debug && (fs->m_free_blocks_count != cs_nbfree ||
	    fs->m_free_inodes_count != cs_nifree)) {
		printf("summary info bad in superblock: %llu, %u "
		    "should be %u, %u\n",
		    (unsigned long long)fs->m_free_blocks_count,
		    fs->m_free_inodes_count,
		    cs_nbfree, cs_nifree);
	}
	if ((fs->m_free_blocks_count != cs_nbfree ||
	    fs->m_free_inodes_count != cs_nifree) &&
	    dofix(&idesc[0], "SUPERBLK SUMMARY INFORMATION BAD")) {
		fs->m_free_blocks_count = cs_nbfree;
		fs->m_free_inodes_count = cs_nifree;
		sbdirty();
	}
	free(ibmap);
	free(bbmap);
}

void
print_bmap(u_char *map, u_int32_t size)
{
	int i, j;

	i = 0;
	while (i < size) {
		printf("%u: ", i);
		for (j = 0; j < 16; j++, i++)
			printf("%2x ", (u_int)map[i] & 0xff);
		printf("\n");
	}
}
