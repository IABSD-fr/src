# Regression tests for fsck_ext4fs journal recovery.

REGRESS_TARGETS=	run-regress-journal

run-regress-journal:
	MAKE='${MAKE}' sh ${.CURDIR}/journal.sh

.include <bsd.regress.mk>
