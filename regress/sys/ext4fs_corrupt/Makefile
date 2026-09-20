# Regression tests for malformed ext4fs metadata.

PROG=	ext4fs_corrupt
NOMAN=

CFLAGS+=	-Wall -Werror -Wextra

REGRESS_TARGETS=	run-regress-ext4fs-corrupt-fixtures \
		run-regress-ext4fs-corrupt
REGRESS_ROOT_TARGETS=	run-regress-ext4fs-corrupt

# Included by the global runner, not by this directory's regress target.
REGRESS_VMM_TARGETS=	run-regress-ext4fs-corrupt-panic-vmm

EXT4FS_CORRUPT_TIMEOUT?=	20
EXT4FS_CORRUPT_IMAGE_MB?=	64
EXT4FS_BLOCK_SIZES?=	1024 2048 4096
EXT4FS_VMM_BASE?=	/home/vm/ext4fs-base.qcow2
EXT4FS_VMM_KERNEL?=	/bsd
EXT4FS_VMM_DIR?=	/home/vm
EXT4FS_VMM_BOOT_TIMEOUT?=	120
EXT4FS_VMM_PANIC_TIMEOUT?=	30
EXT4FS_VMM_IMAGE_MB?=	128
EXT4FS_VMM_SSH_KEY?=
EXT4FS_VMM_BLOCK_SIZES?=	1024 2048 4096

run-regress-ext4fs-corrupt-fixtures: ${PROG}
	env EXT4FS_CORRUPT='${.OBJDIR}/${PROG}' \
	    EXT4FS_CORRUPT_MODE=fixtures \
	    EXT4FS_CORRUPT_TIMEOUT='${EXT4FS_CORRUPT_TIMEOUT}' \
	    EXT4FS_CORRUPT_IMAGE_MB='${EXT4FS_CORRUPT_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    sh ${.CURDIR}/corruption.sh

run-regress-ext4fs-corrupt: ${PROG}
	env EXT4FS_CORRUPT='${.OBJDIR}/${PROG}' \
	    EXT4FS_CORRUPT_MODE=kernel \
	    EXT4FS_CORRUPT_TIMEOUT='${EXT4FS_CORRUPT_TIMEOUT}' \
	    EXT4FS_CORRUPT_IMAGE_MB='${EXT4FS_CORRUPT_IMAGE_MB}' \
	    EXT4FS_BLOCK_SIZES='${EXT4FS_BLOCK_SIZES}' \
	    sh ${.CURDIR}/corruption.sh

# Explicit only: this target deliberately panics a disposable vmd guest.
run-regress-ext4fs-corrupt-panic-vmm: ${PROG}
	@set -e; block_sizes='${EXT4FS_VMM_BLOCK_SIZES}'; \
	boot_timeout='${EXT4FS_VMM_BOOT_TIMEOUT}'; \
	panic_timeout='${EXT4FS_VMM_PANIC_TIMEOUT}'; \
	[ -n "$$block_sizes" ] || { \
		echo 'EXT4FS_VMM_BLOCK_SIZES must not be empty' >&2; \
		exit 1; \
	}; \
	for block_size in $$block_sizes; do \
		env EXT4FS_CORRUPT='${.OBJDIR}/${PROG}' \
		    EXT4FS_VMM_BASE='${EXT4FS_VMM_BASE}' \
		    EXT4FS_VMM_KERNEL='${EXT4FS_VMM_KERNEL}' \
		    EXT4FS_VMM_DIR='${EXT4FS_VMM_DIR}' \
		    EXT4FS_VMM_BOOT_TIMEOUT="$$boot_timeout" \
		    EXT4FS_VMM_PANIC_TIMEOUT="$$panic_timeout" \
		    EXT4FS_VMM_IMAGE_MB='${EXT4FS_VMM_IMAGE_MB}' \
		    EXT4FS_VMM_SSH_KEY='${EXT4FS_VMM_SSH_KEY}' \
		    EXT4FS_VMM_BLOCK_SIZE="$$block_size" \
		    sh ${.CURDIR}/panic-vmm.sh; \
	done

.include <bsd.regress.mk>
