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
			run-regress-ext4fsops-sync \
			run-regress-ext4fsops-vfs-sync \
			run-regress-ext4fsops-remount \
			run-regress-ext4fsops-handoff \
			run-regress-ext4fsops-group \
			run-regress-ext4fsops-read-through \
			run-regress-ext4fsops-wxallowed \
			run-regress-ext4fsops-revoke-enable \
			run-regress-ext4fsops-rename-growth \
			run-regress-ext4fsops-directory-churn \
			run-regress-ext4fsops-allocation-run \
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

run-regress-ext4fsops-sync: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='sync' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-vfs-sync: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='vfs-sync' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-remount: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='remount' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-handoff: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='handoff' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-group: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='group' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-read-through: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='read-through' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-wxallowed: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FSOPS_MODE='wxallowed' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-revoke-enable: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='revoke-enable' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-rename-growth: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='rename-growth' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-directory-churn: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='directory-churn' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-extents: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='extents' \
	    sh ${.CURDIR}/operations.sh

run-regress-ext4fsops-allocation-run: ${PROG}
	env EXT4FSOPS='${.OBJDIR}/${PROG}' \
	    EXT4FS_TIMEOUT='${EXT4FS_TIMEOUT}' \
	    EXT4FS_IMAGE_MB='${EXT4FS_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    EXT4FSOPS_MODE='allocation-run' \
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
