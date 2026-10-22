# Regression tests for fsck_ext4fs journal recovery.

REGRESS_TARGETS=	run-regress-journal run-regress-journal-mount
REGRESS_ROOT_TARGETS=	run-regress-journal-mount

JOURNAL_SEED?=	260921
JOURNAL_BLOCK_SIZES?=	1024 2048 4096
RANDOM_CASES?=	6
MUTATION_CASES?=	18
FSCK_TIMEOUT?=	20
FSCK_EXT4FS?=	${.CURDIR}/../../../sbin/fsck_ext4fs/obj/fsck_ext4fs

run-regress-journal:
	@set -e; block_sizes='${JOURNAL_BLOCK_SIZES}'; \
	[ -n "$$block_sizes" ] || { \
		echo 'JOURNAL_BLOCK_SIZES must not be empty' >&2; \
		exit 1; \
	}; \
	for block_size in $$block_sizes; do \
		env FSCK_EXT4FS='${FSCK_EXT4FS}' \
		    JOURNAL_SEED='${JOURNAL_SEED}' \
		    JOURNAL_BLOCK_SIZE="$$block_size" \
		    RANDOM_CASES='${RANDOM_CASES}' \
		    MUTATION_CASES='${MUTATION_CASES}' \
		    FSCK_TIMEOUT='${FSCK_TIMEOUT}' \
		    sh ${.CURDIR}/journal.sh; \
	done

run-regress-journal-mount:
	@set -e; block_sizes='${JOURNAL_BLOCK_SIZES}'; \
	[ -n "$$block_sizes" ] || { \
		echo 'JOURNAL_BLOCK_SIZES must not be empty' >&2; \
		exit 1; \
	}; \
	for block_size in $$block_sizes; do \
		env JOURNAL_TEST_MODE=kernel \
		    JOURNAL_SEED='${JOURNAL_SEED}' \
		    JOURNAL_BLOCK_SIZE="$$block_size" \
		    sh ${.CURDIR}/journal.sh; \
	done

.include <bsd.regress.mk>
