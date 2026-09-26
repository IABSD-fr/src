# Regression tests for ordinary ext4fs filesystem operations.

PROG=	ext4fsops
NOMAN=

CFLAGS+=	-Wall -Werror -Wextra

REGRESS_TARGETS=	run-regress-ext4fsops
REGRESS_ROOT_TARGETS=	${REGRESS_TARGETS} run-regress-ext4fsops-special

EXT4FS_TIMEOUT?=	60
EXT4FS_IMAGE_MB?=	128
EXT4FS_BLOCK_SIZES?=	1024 2048 4096

run-regress-ext4fsops-special: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FSOPS_MODE='special' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    sh ${.CURDIR}/operations.sh

.include <bsd.regress.mk>
