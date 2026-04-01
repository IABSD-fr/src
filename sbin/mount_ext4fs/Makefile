#	$OpenBSD: Makefile,v 1.1 1996/06/27 07:20:28 downsj Exp $

PROG=	mount_ext4fs
SRCS=	mount_ext4fs.c getmntopts.c
MAN=	mount_ext4fs.8

MOUNT=	${.CURDIR}/../mount
CFLAGS+= -I${MOUNT}
.PATH:	${MOUNT}

.include <bsd.prog.mk>
