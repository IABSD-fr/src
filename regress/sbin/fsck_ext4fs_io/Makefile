PROG=	io_test
SRCS=	io_test.c io.c dkcksum.c
NOMAN=
CFLAGS+= -Wall -Wextra -Werror
CPPFLAGS+= -I${.CURDIR}/../../../sbin/fsck_ext4fs
CPPFLAGS+= -Dpread=test_pread -Dpwrite=test_pwrite
.PATH: ${.CURDIR}/../../../sbin/fsck_ext4fs
.PATH: ${.CURDIR}/../../../sbin/disklabel

REGRESS_TARGETS=	run-regress-io run-regress-images \
			run-regress-sector-size
REGRESS_ROOT_TARGETS=	run-regress-sector-size

FSCK_EXT4FS?=	${.CURDIR}/../../../sbin/fsck_ext4fs/obj/fsck_ext4fs

run-regress-io: ${PROG}
	./${PROG}

run-regress-images:
	env IO_TEST_MODE=images FSCK_EXT4FS='${FSCK_EXT4FS}' \
	    sh ${.CURDIR}/sector-size.sh

run-regress-sector-size: ${PROG}
	env IO_TEST='${.OBJDIR}/${PROG}' FSCK_EXT4FS='${FSCK_EXT4FS}' \
	    sh ${.CURDIR}/sector-size.sh

.include <bsd.regress.mk>
