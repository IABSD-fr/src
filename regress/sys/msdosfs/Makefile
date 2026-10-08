## Copyright (c) 2025,2026 kmx.io <contact@kmx.io>
##
## Permission to use, copy, modify, and distribute this software for any
## purpose with or without fee is hereby granted, provided that the
## above copyright notice and this permission notice appear in all
## copies.
##
## THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
## WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
## WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
## AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
## DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
## PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
## TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
## PERFORMANCE OF THIS SOFTWARE.

# Regression tests for malformed MS-DOS filesystems.

PROG=	msdosfs_test
NOMAN=

CFLAGS+=	-Wall -Werror -Wextra

REGRESS_TARGETS=	run-regress-msdosfs-fixtures \
		run-regress-msdosfs
REGRESS_ROOT_TARGETS=	run-regress-msdosfs

MSDOSFS_TIMEOUT?=	20
NEWFS_MSDOS?=		/sbin/newfs_msdos

run-regress-msdosfs-fixtures: ${PROG}
	env MSDOSFS_TEST='${.OBJDIR}/${PROG}' \
	    MSDOSFS_MODE=fixtures \
	    MSDOSFS_TIMEOUT='${MSDOSFS_TIMEOUT}' \
	    NEWFS_MSDOS='${NEWFS_MSDOS}' \
	    sh ${.CURDIR}/msdosfs.sh

run-regress-msdosfs: ${PROG}
	env MSDOSFS_TEST='${.OBJDIR}/${PROG}' \
	    MSDOSFS_MODE=kernel \
	    MSDOSFS_TIMEOUT='${MSDOSFS_TIMEOUT}' \
	    NEWFS_MSDOS='${NEWFS_MSDOS}' \
	    sh ${.CURDIR}/msdosfs.sh

.include <bsd.regress.mk>
