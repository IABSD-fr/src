#!/bin/ksh
#
# Build an IABSD snapshot
#
# Usage: snapshot.sh [-k] [-b] [-x] [-i] [-p] [-s]
#   -k  build kernel
#   -b  build base (userland + release)
#   -x  build xenocara + release
#   -i  build ISO and install image
#   -p  build ports with dpb
#   -s  sign packages
#   -f  grab firmware from upstream (OpenBSD)
#
# If none of -k -b -x -i -p -s -f are given, all steps are run.
#

set -e
umask 022

ARCH=$(machine)

# The kernel machine name and the package architecture name differ on arm64.
case ${ARCH} in
arm64)	PORTS_ARCH=aarch64;;
*)	PORTS_ARCH=${ARCH};;
esac

BASE_MP=/usr/xobj
BASE_DESTDIR=/usr/xobj/destdir
BASE_RELEASEDIR=/home/release

XENO_MP=/usr/obj
XENO_DESTDIR=/usr/obj/destdir
XENO_RELEASEDIR=/home/xenocara

CHROOT=/home/dpb
PACKAGES=${CHROOT}/usr/ports/packages/${PORTS_ARCH}/ftp

run() {
	echo "+ $@" >&2
	"$@"
}

err() {
	echo "snapshot: $1" >&2
	exit 1
}

usage() {
	echo "usage: ${0##*/} [-k] [-b] [-x] [-i] [-p] [-s] [-f]" >&2
	exit 1
}

DO_KERNEL=false
DO_BASE=false
DO_XENO=false
DO_ISO=false
DO_PORTS=false
DO_SIGN=false
DO_FIRMWARE=false

while getopts kbxipsf arg; do
	case ${arg} in
	k)	DO_KERNEL=true;;
	b)	DO_BASE=true;;
	x)	DO_XENO=true;;
	i)	DO_ISO=true;;
	p)	DO_PORTS=true;;
	s)	DO_SIGN=true;;
	f)	DO_FIRMWARE=true;;
	*)	usage;;
	esac
done

# If none specified, do all
if ! ${DO_KERNEL} && ! ${DO_BASE} && ! ${DO_XENO} && ! ${DO_ISO} && ! ${DO_PORTS} && ! ${DO_SIGN} && ! ${DO_FIRMWARE}; then
	DO_KERNEL=true
	DO_BASE=true
	DO_XENO=true
	DO_ISO=true
	DO_PORTS=true
	DO_SIGN=true
	DO_FIRMWARE=true
fi

[[ $(id -u) -eq 0 ]] || err "need root privileges"

NCPU=$(sysctl -n hw.ncpuonline)

echo "=== IABSD snapshot build $(date) ==="
echo "NCPU=${NCPU}"

if ${DO_KERNEL}; then
	echo "=== Building kernel ==="
	run cd /usr/src/sys/arch/${ARCH}/compile/GENERIC.MP
	run make obj
	run make config
	run make -j${NCPU}
	run make install
	echo "=== Kernel done ==="
fi

if ${DO_BASE}; then
	# Build userland
	echo "=== Building userland ==="
	run mount -u -o dev,exec,perm /usr/obj
	run chown -R build:wobj /usr/obj
	run chmod -R g+w /usr/obj
	run cd /usr/src
	run make obj
	run cd /usr/src/usr.bin/arch
	run make
	run make install
	run cd /usr/src
	run make -j${NCPU} build
	run sysmerge
	run cd /dev
	run ./MAKEDEV all
	echo "=== Userland done ==="

	# Make base release
	echo "=== Making base release ==="
	run install -d -o build -g wheel -m 700 ${BASE_MP}
	run install -d -o build -g wheel -m 700 ${BASE_DESTDIR}
	run install -d -o build -g wobj -m 755 ${BASE_RELEASEDIR}
	run mount -u -o noperm ${BASE_MP}
	run mkdir -p ${BASE_DESTDIR}
	run chown build:wobj ${BASE_MP} ${BASE_DESTDIR}
	run cd /usr/src/etc
	export DESTDIR=${BASE_DESTDIR} RELEASEDIR=${BASE_RELEASEDIR}
	run make release
	run mount -u -o perm ${BASE_MP}
	unset DESTDIR RELEASEDIR
	echo "=== Base release done ==="
fi

if ${DO_XENO}; then
	# Build xenocara
	echo "=== Building xenocara ==="
	run mount -u -o dev,exec,perm /usr/xobj
	run chown -R build:wobj /usr/xobj
	run chmod -R g+w /usr/xobj
	run cd /usr/xenocara
	run make bootstrap
	run make obj
	run make -j${NCPU} build
	echo "=== Xenocara done ==="

	# Make xenocara release
	echo "=== Making xenocara release ==="
	run install -d -o root -g wheel -m 700 ${XENO_MP}
	run install -d -o root -g wheel -m 700 ${XENO_DESTDIR}
	run install -d -o build -g wobj -m 755 ${XENO_RELEASEDIR}
	run mount -u -o noperm ${XENO_MP}
	run mkdir -p ${XENO_DESTDIR}
	run chown build:wobj ${XENO_MP} ${XENO_DESTDIR}
	run cd /usr/xenocara
	run make DESTDIR=${XENO_DESTDIR} RELEASEDIR=${XENO_RELEASEDIR} release
	run mount -u -o perm ${XENO_MP}
	echo "=== Xenocara release done ==="
fi

if ${DO_ISO}; then
	echo "=== Preparing release ==="
	# Copy xenocara sets into base release dir
	if ls ${XENO_RELEASEDIR}/*.tgz >/dev/null 2>&1; then
		run cp -p ${XENO_RELEASEDIR}/*.tgz ${BASE_RELEASEDIR}/
	fi

	# Sign sets so ISO/img contain SHA256.sig
	echo "=== Signing sets ==="
	run cd ${BASE_RELEASEDIR}
	run cksum -a sha256 BUILDINFO INSTALL.${ARCH} \
	    bsd bsd.mp bsd.rd *.tgz > SHA256
	run sort -o SHA256 SHA256
	while ! run signify -S -s /root/signify/iabsd-01-base.sec \
	    -m SHA256 -e -x SHA256.sig; do echo "try again" >&2; done

	# Build ISO and install image
	echo "=== Building ISO ==="
	run cd /usr/src/distrib/${ARCH}/iso
	run make obj
	run make RELDIR=${BASE_RELEASEDIR} RELXDIR=${XENO_RELEASEDIR} \
	    DESTDIR=${BASE_DESTDIR}
	run make RELDIR=${BASE_RELEASEDIR} install

	# Re-sign with install images included
	echo "=== Signing release ==="
	run cd ${BASE_RELEASEDIR}
	_ckfiles="BUILDINFO INSTALL.${ARCH} bsd bsd.mp bsd.rd *.tgz *.iso"
	if ls *.img >/dev/null 2>&1; then
		_ckfiles="${_ckfiles} *.img"
	fi
	run cksum -a sha256 ${_ckfiles} > SHA256
	run sort -o SHA256 SHA256
	while ! run signify -S -s /root/signify/iabsd-01-base.sec \
	    -m SHA256 -e -x SHA256.sig; do echo "try again" >&2; done
	echo "=== ISO done ==="
fi

if ${DO_PORTS}; then
	echo "=== Setting up ports chroot ==="
	# Keep proot's source locate database synchronized with the current
	# architecture lists.  This avoids copying files retired from the tree.
	run cd /usr/src/distrib/sets
	run make makedb DESTDIR=/
	run mkdir -p ${CHROOT}
	run /usr/ports/infrastructure/bin/proot -B ${CHROOT} actions=unpopulate_light
	run /usr/ports/infrastructure/bin/proot -B ${CHROOT} \
	    actions=locate actions=resolv actions=copy_ports \
	    actions=ldconfig actions=devs actions=ports_subdirs
	echo "=== Chroot ready ==="

	echo "=== Building ports ==="
	run /usr/ports/infrastructure/bin/dpb -B ${CHROOT} -j ${NCPU}
	echo "=== Ports done ==="

	echo "Packages in ${PACKAGES}/"
	ls ${PACKAGES}/ | wc -l

fi

if ${DO_SIGN}; then
	echo "=== Signing packages ==="
	run pkg_sign -C -s signify2 -s /root/signify/iabsd-01-pkg.sec \
	    -o ${PACKAGES}/signed/ -S ${PACKAGES}/
	echo "=== Packages signed ==="
fi

if ${DO_FIRMWARE}; then
	echo "=== Updating firmware files ==="
	run rm -rf /var/www/firmware.iabsd.fr/.tmp
	run mkdir /var/www/firmware.iabsd.fr/.tmp
	run cd /var/www/firmware.iabsd.fr/.tmp
	run lftp -c mget http://firmware.openbsd.org/firmware/snapshots/*.tgz
	run cksum -a sha256 -b -h SHA256 *.tgz
	run signify -S -s /root/signify/iabsd-01-fw.sec -m SHA256 -e
	run ls -ln > index.txt
	run cd /var/www/firmware.iabsd.fr
	run rsync -aP --delete .tmp/. ../firmware/snapshots/
	run rm -rf .tmp
fi

echo ""
echo "=== Snapshot complete ==="
ls -lh ${BASE_RELEASEDIR}/
