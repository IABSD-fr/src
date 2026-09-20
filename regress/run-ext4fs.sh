#!/bin/sh
#
# Run all ext4fs regressions serially, including vmd panic/crash tests
# when vmctl is available.
# Only regression helpers are built; fsck_ext4fs must exist in obj, or
# be selected explicitly with FSCK_EXT4FS.

set -eu

MAKE=${MAKE:-make}
directories='sys/ext4fs sbin/fsck_ext4fs_io sbin/fsck_ext4fs
    sys/ext4fsops sys/ext4fs_corrupt sys/ext4fs_crash'

[ "$#" -eq 0 ] || {
	echo "usage: ${0##*/}" >&2
	exit 1
}
[ "$(id -u)" -eq 0 ] || {
	echo "${0##*/}: the complete ext4fs suite requires root" >&2
	exit 1
}

# Disable pathname expansion when splitting target lists.
set -f

regress_root=$(CDPATH= cd "$(dirname "$0")" && pwd)
srcroot=$(CDPATH= cd "$regress_root/.." && pwd)
FSCK_EXT4FS=${FSCK_EXT4FS:-$srcroot/sbin/fsck_ext4fs/obj/fsck_ext4fs}
case "$FSCK_EXT4FS" in
/*) ;;
*)	FSCK_EXT4FS=$(pwd)/$FSCK_EXT4FS ;;
esac

# Do not rebuild or install the checker as a regression side effect.
if [ ! -f "$FSCK_EXT4FS" ] || [ ! -x "$FSCK_EXT4FS" ]; then
	echo "${0##*/}: fsck_ext4fs missing or not executable:" \
	    "$FSCK_EXT4FS" >&2
	exit 1
fi

# Check native prerequisites here; VMM tests check their own tools.
for tool in "$MAKE" "${MKE2FS:-mke2fs}" "${DEBUGFS:-debugfs}" \
    "${E2FSCK:-e2fsck}" "${DUMPE2FS:-dumpe2fs}" \
    "${TIMEOUT:-timeout}" "${VNCONFIG:-vnconfig}" \
    "${MOUNT_EXT4FS:-mount_ext4fs}" "${MOUNT:-mount}" \
    "${UMOUNT:-umount}"; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "${0##*/}: required tool not found: $tool" >&2
		exit 1
	}
done
object_root=$(mktemp -d /tmp/ext4fs_regress.XXXXXXXX)
case "$object_root" in
/tmp/ext4fs_regress.*) ;;
*)	echo "${0##*/}: unsafe object directory: $object_root" >&2
	exit 1
	;;
esac

cleanup()
{
	status=$?
	trap - EXIT HUP INT TERM
	case "$object_root" in
	/tmp/ext4fs_regress.*)
		rm -rf "$object_root"
		;;
	*)	echo "${0##*/}: refusing to remove $object_root" >&2
		status=1
		;;
	esac
	exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

EXT4FS_BLOCK_SIZES='1024 2048 4096'
JOURNAL_BLOCK_SIZES=$EXT4FS_BLOCK_SIZES
EXT4FS_CRASH_BLOCK_SIZES=$EXT4FS_BLOCK_SIZES
EXT4FS_VMM_BLOCK_SIZES=$EXT4FS_BLOCK_SIZES
export EXT4FS_BLOCK_SIZES JOURNAL_BLOCK_SIZES
export EXT4FS_CRASH_BLOCK_SIZES EXT4FS_VMM_BLOCK_SIZES
export FSCK_EXT4FS

printf 'Filesystem block sizes: %s\n' "$EXT4FS_BLOCK_SIZES"
printf 'fsck_ext4fs: %s\n' "$FSCK_EXT4FS"

run_target()
{
	directory=$1
	target=$2
	object_directory=$object_root/$directory
	mkdir -p "$object_directory"
	printf '\n==== ext4fs regress: %s: %s ====\n' \
	    "$directory" "$target"
	MAKEOBJDIR="$object_directory" \
	    "$MAKE" -C "$regress_root/$directory" \
	    FSCK_EXT4FS="$FSCK_EXT4FS" SUDO= \
	    REGRESS_SKIP_TARGETS= REGRESS_SKIP_SLOW=no \
	    REGRESS_FAIL_EARLY=yes "$target"
}

for directory in $directories; do
	run_target "$directory" regress
done

if command -v "${VMCTL:-vmctl}" >/dev/null 2>&1; then
	for directory in $directories; do
		targets=$("$MAKE" -s -C "$regress_root/$directory" \
		    -V REGRESS_VMM_TARGETS SUDO=)
		for target in $targets; do
			run_target "$directory" "$target"
		done
	done
else
	printf '\nSkipping VMM tests: %s is not available.\n' \
	    "${VMCTL:-vmctl}"
fi

printf '\nAll available ext4fs regression tests passed.\n'
