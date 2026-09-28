# Regression tests for ordinary ext4fs filesystem operations.

PROG=	ext4fsops
NOMAN=

CFLAGS+=	-Wall -Werror -Wextra

REGRESS_TARGETS=	run-regress-ext4fsops \
			run-regress-ext4fsops-bitmap \
			run-regress-ext4fsops-inode-bitmap \
			run-regress-ext4fsops-bgd \
			run-regress-ext4fsops-counters \
			run-regress-ext4fsops-superblock \
			run-regress-ext4fsops-fsync \
			run-regress-ext4fsops-extents \
			run-regress-ext4fsops-directory \
			run-regress-ext4fsops-orphan \
			run-regress-ext4fsops-special
REGRESS_ROOT_TARGETS=	${REGRESS_TARGETS}

EXT4FS_TIMEOUT?=	60
EXT4FS_IMAGE_MB?=	128
EXT4FS_BLOCK_SIZES?=	1024 2048 4096

run-regress-ext4fsops-bitmap: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='bitmap' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-inode-bitmap: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='inode-bitmap' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-bgd: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='bgd' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-counters: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='counters' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-superblock: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='superblock' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-fsync: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='fsync' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-extents: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='extents' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-directory: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='directory' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-orphan: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='orphan' \
	    sh ${.CURDIR}/operations.sh

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
