#	$OpenBSD: Makefile,v 1.13 2019/04/30 19:20:22 deraadt Exp $

SUBDIR+= miniroot

.if	make(obj) || make(cleandir) || make(clean)
SUBDIR+= iso
.endif

unconfig:
	cd miniroot; ${MAKE} unconfig

.include <bsd.subdir.mk>
