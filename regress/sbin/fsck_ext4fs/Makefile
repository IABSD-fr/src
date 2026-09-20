# Regression tests for fsck_ext4fs journal recovery.

REGRESS_TARGETS=	run-regress-journal run-regress-journal-mount
REGRESS_ROOT_TARGETS=	run-regress-journal-mount

JOURNAL_SEED?=	260921
RANDOM_CASES?=	6
MUTATION_CASES?=	18
FSCK_TIMEOUT?=	20

run-regress-journal:
	env MAKE='${MAKE}' JOURNAL_SEED='${JOURNAL_SEED}' \
	    RANDOM_CASES='${RANDOM_CASES}' MUTATION_CASES='${MUTATION_CASES}' \
	    FSCK_TIMEOUT='${FSCK_TIMEOUT}' sh ${.CURDIR}/journal.sh

run-regress-journal-mount:
	env JOURNAL_TEST_MODE=kernel JOURNAL_SEED='${JOURNAL_SEED}' \
	    sh ${.CURDIR}/journal.sh

.include <bsd.regress.mk>
