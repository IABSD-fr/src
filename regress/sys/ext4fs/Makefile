# Regression tests for the ext4fs journal core.

PROG=	journal_core_test
SRCS=	journal_core_test.c ext4fs_journal_state.c
CPPFLAGS+=	-I${.CURDIR}/../../../sys
NOMAN=

.PATH:	${.CURDIR}/../../../sys/ufs/ext4fs

REGRESS_TARGETS=	run-regress-journal-core

run-regress-journal-core: ${PROG}
	./${PROG}

.include <bsd.regress.mk>
