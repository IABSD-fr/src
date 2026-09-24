# Regression tests for ordinary ext4fs filesystem operations.

PROG=	ext4fsops
NOMAN=

CFLAGS+=	-Wall -Werror -Wextra

REGRESS_TARGETS=	run-regress-ext4fsops
REGRESS_ROOT_TARGETS=	${REGRESS_TARGETS}

EXT4FS_TIMEOUT?=	60
EXT4FS_IMAGE_MB?=	128

run-regress-ext4fsops: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    sh ${.CURDIR}/operations.sh

.include <bsd.regress.mk>
