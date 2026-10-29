#	$OpenBSD: Makefile,v 1.15 2020/04/02 06:06:22 otto Exp $
#	$NetBSD: Makefile,v 1.1 2000/08/20 14:58:45 mrg Exp $

CURDIR=	${.CURDIR}
S=	${CURDIR}/../../../..

#
# Override normal settings
#

CLEANFILES=	machine ffs.fth.h ffs.fth.h.tmp \
		bootblk bootblk.tmp bootblk.input.tmp \
		bootblk.text bootblk.text.tmp -.d

NOMAN=
STRIPFLAG=
BINMODE=644

INCLUDES=	-I. -I$S/arch -I$S -nostdinc
CPPFLAGS=	${INCLUDES} ${IDENT} ${PARAM}

.if !make(clean) && !make(cleandir) && !make(includes) && !make(libdep) && \
    !make(sadep) && !make(salibdir) && !make(obj)
.BEGIN:
	@([ -h machine ] || ln -s ${.CURDIR}/../../include machine)
.endif

all: ${.OBJDIR}/bootblk

${.OBJDIR}/bootblk: ${.CURDIR}/Makefile ${.CURDIR}/bootblk.fth \
	${.CURDIR}/genassym.sh ${.CURDIR}/genfth.cf
	cd ${.OBJDIR} && \
		sh ${.CURDIR}/genassym.sh -f ${CC} ${CFLAGS} ${CPPFLAGS} ${PROF} \
		< ${.CURDIR}/genfth.cf >ffs.fth.h.tmp && \
		mv -f ffs.fth.h.tmp ffs.fth.h && \
		awk '/fload/ { print "#include \"" $$2 "\"" }; !/fload/' \
		${.CURDIR}/bootblk.fth >bootblk.input.tmp && \
		/usr/bin/cpp -P -I. <bootblk.input.tmp >bootblk.text.tmp && \
		mv -f bootblk.text.tmp bootblk.text && \
		rm -f bootblk.input.tmp && \
		fgen -o bootblk.tmp bootblk.text && \
		mv -f bootblk.tmp bootblk

beforeinstall: ${.OBJDIR}/bootblk
	${INSTALL} ${INSTALL_COPY} -o ${BINOWN} -g ${BINGRP} -m ${BINMODE} \
	    ${.OBJDIR}/bootblk ${DESTDIR}/usr/mdec

#
# The following are if you grab the fakeboot program from the Sun website
#

fake: ${.OBJDIR}/bootblk
	../fakeboot/fakeboot -elf32 <bootblk >/bootblk
	../fakeboot/fakeboot -elf32 <bootblk.text >/bootblk.text

.include <bsd.prog.mk>
