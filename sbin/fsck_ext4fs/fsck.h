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

#include <ufs/ext4fs/ext4fs_dinode.h>
#include <ufs/ext4fs/ext4fs.h>

#define	MAXDUP		10
#define	MAXBAD		10
#define	MAXBUFSPACE	80*1024
#define	INOBUFSIZE	128*1024

#define	USTATE	01
#define	FSTATE	02
#define	DSTATE	03
#define	DFOUND	04
#define	DCLEAR	05
#define	FCLEAR	06
#define	SSTATE	07

struct bufarea {
	struct bufarea	*b_next;
	struct bufarea	*b_prev;
	u_int64_t	b_bno;
	int	b_size;
	int	b_errs;
	int	b_flags;
	union {
		char	*b_buf;
		u_int32_t	*b_indir;
		struct	ext4fs *b_fs;
		struct	ext4fs_block_group_descriptor *b_cgd;
		struct	ext4fs_dinode *b_dinode;
	} b_un;
	char	b_dirty;
};

#define	B_INUSE 1

#define	MINBUFS		5
extern struct bufarea bufhead;
extern struct bufarea sblk;
extern struct bufarea asblk;
extern struct bufarea *pdirbp;
extern struct bufarea *pbp;
extern struct bufarea *getdatablk(u_int64_t, long);
extern struct m_ext4fs sblock;

#define	dirty(bp)	(bp)->b_dirty = 1
#define	initbarea(bp) \
	(bp)->b_dirty = 0; \
	(bp)->b_bno = (u_int64_t)-1; \
	(bp)->b_flags = 0;

#define	sbdirty()	sblk.b_dirty = 1

enum fixstate {DONTKNOW, NOFIX, FIX, IGNORE};

struct inodesc {
	enum fixstate id_fix;
	int (*id_func)(struct inodesc *);
	ino_t id_number;
	ino_t id_parent;
	u_int64_t id_blkno;
	int id_numfrags;
	quad_t id_filesize;
	int id_loc;
	int id_entryno;
	struct ext4fs_directory *id_dirp;
	char *id_name;
	char id_type;
};

#define	DATA	1
#define	ADDR	2

struct dups {
	struct dups *next;
	u_int64_t dup;
};
extern struct dups *duplist;
extern struct dups *muldup;

struct zlncnt {
	struct zlncnt *next;
	ino_t zlncnt;
};
extern struct zlncnt *zlnhead;

extern struct inoinfo {
	struct	inoinfo *i_nexthash;
	struct	inoinfo	*i_child, *i_sibling, *i_parentp;
	ino_t	i_number;
	ino_t	i_parent;
	ino_t	i_dotdot;
	u_int64_t	i_isize;
	u_int	i_numblks;
	u_int32_t	i_blks[1];
} **inphead, **inpsort;
extern long numdirs, listmax, inplast;

extern long	secsize;
extern char	nflag;
extern char	yflag;
extern int	bflag;
extern int	debug;
extern int	preen;
extern char	havesb;
extern char	skipclean;
extern int	fsmodified;
extern int	fsreadfd;
extern int	fswritefd;
extern int	rerun;

extern u_int64_t	maxfsblock;
extern char	*blockmap;
extern ino_t	maxino;
extern ino_t	lastino;
extern char	*statemap;
extern u_char	*typemap;
extern int16_t	*lncntp;

extern ino_t	lfdir;
extern char	*lfname;
extern int	lfmode;

extern u_int64_t	n_blks;
extern u_int64_t	n_files;

#define	clearinode(dp)	(*(dp) = zino)
extern struct	ext4fs_dinode zino;

#define	setbmap(blkno)	setbit(blockmap, blkno)
#define	testbmap(blkno)	isset(blockmap, blkno)
#define	clrbmap(blkno)	clrbit(blockmap, blkno)

#define	STOP	0x01
#define	SKIP	0x02
#define	KEEPON	0x04
#define	ALTERED	0x08
#define	FOUND	0x10

struct ext4fs_dinode *ginode(ino_t);
struct inoinfo *getinoinfo(ino_t);
void getblk(struct bufarea *, u_int64_t, long);
ino_t allocino(ino_t, int);
u_int64_t cgoverhead(int);
