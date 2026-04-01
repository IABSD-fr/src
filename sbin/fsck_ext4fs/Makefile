PROG=	fsck_ext4fs
MAN=	fsck_ext4fs.8
SRCS=	dir.c ext4fs_crc32c.c inode.c journal.c main.c pass1.c pass1b.c \
	pass2.c pass3.c pass4.c pass5.c fsutil.c setup.c utilities.c
.PATH:	${.CURDIR}/../fsck
CFLAGS+= -I${.CURDIR}/../fsck
DPADD+=	${LIBUTIL}
LDADD+=	-lutil

.include <bsd.prog.mk>
