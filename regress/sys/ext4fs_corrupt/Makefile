# Regression tests for malformed ext4fs metadata.

PROG=	ext4fs_corrupt
NOMAN=

CFLAGS+=	-Wall -Werror -Wextra

REGRESS_TARGETS=	run-regress-ext4fs-corrupt-fixtures \
		run-regress-ext4fs-corrupt
REGRESS_ROOT_TARGETS=	run-regress-ext4fs-corrupt

EXT4FS_CORRUPT_TIMEOUT?=	20
EXT4FS_CORRUPT_IMAGE_MB?=	64

run-regress-ext4fs-corrupt-fixtures: ${PROG}
	env EXT4FS_CORRUPT='${.OBJDIR}/${PROG}' \
	    EXT4FS_CORRUPT_MODE=fixtures \
	    EXT4FS_CORRUPT_TIMEOUT='${EXT4FS_CORRUPT_TIMEOUT}' \
	    EXT4FS_CORRUPT_IMAGE_MB='${EXT4FS_CORRUPT_IMAGE_MB}' \
	    sh ${.CURDIR}/corruption.sh

run-regress-ext4fs-corrupt: ${PROG}
	env EXT4FS_CORRUPT='${.OBJDIR}/${PROG}' \
	    EXT4FS_CORRUPT_MODE=kernel \
	    EXT4FS_CORRUPT_TIMEOUT='${EXT4FS_CORRUPT_TIMEOUT}' \
	    EXT4FS_CORRUPT_IMAGE_MB='${EXT4FS_CORRUPT_IMAGE_MB}' \
	    sh ${.CURDIR}/corruption.sh

.include <bsd.regress.mk>
