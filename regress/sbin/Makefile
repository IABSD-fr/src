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

#	$OpenBSD: Makefile,v 1.14 2025/12/27 22:14:40 kn Exp $

SUBDIR+= disklabel fsck_ext4fs ifconfig iked isakmpd newfs newfs_msdos route
SUBDIR+= slaacd sysctl

.if defined(REGRESS_FULL) || make(clean) || make(cleandir) || make(obj)
SUBDIR+= ipsecctl pfctl
.endif

install:

.include <bsd.subdir.mk>
