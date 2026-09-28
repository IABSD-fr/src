# Regression tests for malformed ext4fs metadata.

PROG=	ext4fs_corrupt
NOMAN=

CFLAGS+=	-Wall -Werror -Wextra

REGRESS_TARGETS=	run-regress-ext4fs-corrupt-fixtures \
		run-regress-ext4fs-corrupt
REGRESS_ROOT_TARGETS=	run-regress-ext4fs-corrupt

EXT4FS_CORRUPT_TIMEOUT?=	20
EXT4FS_CORRUPT_IMAGE_MB?=	64
EXT4FS_VMM_BASE?=	/home/vm/ext4fs-base.qcow2
EXT4FS_VMM_KERNEL?=	/bsd
EXT4FS_VMM_DIR?=	/home/vm
EXT4FS_VMM_BOOT_TIMEOUT?=	120
EXT4FS_VMM_PANIC_TIMEOUT?=	30
EXT4FS_VMM_IMAGE_MB?=	128
EXT4FS_VMM_SSH_KEY?=

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

# Explicit only: this target deliberately panics a disposable vmd guest.
run-regress-ext4fs-corrupt-panic-vmm: ${PROG}
	env EXT4FS_CORRUPT='${.OBJDIR}/${PROG}' \
	    EXT4FS_VMM_BASE='${EXT4FS_VMM_BASE}' \
	    EXT4FS_VMM_KERNEL='${EXT4FS_VMM_KERNEL}' \
	    EXT4FS_VMM_DIR='${EXT4FS_VMM_DIR}' \
	    EXT4FS_VMM_BOOT_TIMEOUT='${EXT4FS_VMM_BOOT_TIMEOUT}' \
	    EXT4FS_VMM_PANIC_TIMEOUT='${EXT4FS_VMM_PANIC_TIMEOUT}' \
	    EXT4FS_VMM_IMAGE_MB='${EXT4FS_VMM_IMAGE_MB}' \
	    EXT4FS_VMM_SSH_KEY='${EXT4FS_VMM_SSH_KEY}' \
	    sh ${.CURDIR}/panic-vmm.sh

.include <bsd.regress.mk>
