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

#	$OpenBSD: Makefile,v 1.28 2024/03/14 14:29:03 kettenis Exp $
#	$NetBSD: Makefile,v 1.4 1995/04/20 22:41:08 cgd Exp $

SUBDIR=	altivec_ast btcfi copy crypto dev ddb ext4fs ext4fs_corrupt \
	ext4fs_crash ext4fsops ffs \
	fifofs fileops kern mfs_noperm msdosfs \
	net netinet netinet6 nfs ptrace sys uvm
.if exists(arch/${MACHINE}/Makefile)
SUBDIR+= arch/${MACHINE}
.endif
.if exists(arch/${MACHINE_ARCH}/Makefile) && ${MACHINE} != ${MACHINE_ARCH}
SUBDIR+= arch/${MACHINE_ARCH}
.endif

install:

.include <bsd.subdir.mk>
