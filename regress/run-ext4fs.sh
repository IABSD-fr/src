#!/bin/sh
#
# Run the complete ext4fs regression collection serially.  The ordinary
# collection includes unit, fsck, kernel operation, corruption, and
# crash model tests.  Use -v to add the explicit, slow vmd crash tests.

set -eu

MAKE=${MAKE:-make}
EXT4FS_ALL_BLOCK_SIZES=${EXT4FS_ALL_BLOCK_SIZES:-"1024 2048 4096"}
run_vmm=0

usage()
{
	echo "usage: ${0##*/} [-v]" >&2
	exit 1
}

while getopts v option; do
	case "$option" in
	v)	run_vmm=1 ;;
	*)	usage ;;
	esac
done
shift $((OPTIND - 1))
[ "$#" -eq 0 ] || usage

[ "$(id -u)" -eq 0 ] || {
	echo "${0##*/}: the complete ext4fs suite requires root" >&2
	exit 1
}

[ -n "$EXT4FS_ALL_BLOCK_SIZES" ] || {
	echo "${0##*/}: block-size list must not be empty" >&2
	exit 1
}
for block_size in $EXT4FS_ALL_BLOCK_SIZES; do
	case "$block_size" in
	1024|2048|4096) ;;
	*)	echo "${0##*/}: bad block size: $block_size" >&2
		exit 1
		;;
	esac
done

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
regress_root=$script_dir
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
trap cleanup EXIT HUP INT TERM

EXT4FS_BLOCK_SIZES=$EXT4FS_ALL_BLOCK_SIZES
JOURNAL_BLOCK_SIZES=$EXT4FS_ALL_BLOCK_SIZES
EXT4FS_CRASH_BLOCK_SIZES=$EXT4FS_ALL_BLOCK_SIZES
EXT4FS_VMM_BLOCK_SIZES=$EXT4FS_ALL_BLOCK_SIZES
export EXT4FS_BLOCK_SIZES JOURNAL_BLOCK_SIZES
export EXT4FS_CRASH_BLOCK_SIZES EXT4FS_VMM_BLOCK_SIZES

run_directory()
{
	directory=$1
	object_directory=$object_root/$directory
	mkdir -p "$object_directory"
	printf '\n==== ext4fs regress: %s ====\n' "$directory"
	MAKEOBJDIR="$object_directory" \
	    "$MAKE" -C "$regress_root/$directory" regress
}

run_target()
{
	directory=$1
	target=$2
	object_directory=$object_root/$directory
	mkdir -p "$object_directory"
	MAKEOBJDIR="$object_directory" \
	    "$MAKE" -C "$regress_root/$directory" "$target"
}

for directory in sys/ext4fs sbin/fsck_ext4fs_io sbin/fsck_ext4fs \
    sys/ext4fsops sys/ext4fs_corrupt sys/ext4fs_crash; do
	run_directory "$directory"
done

if [ "$run_vmm" -eq 1 ]; then
	printf '\n==== ext4fs regress: explicit vmd tests ====\n'
	run_target sys/ext4fs_corrupt \
	    run-regress-ext4fs-corrupt-panic-vmm
	for target in \
	    run-regress-ext4fs-crash-vmm \
	    run-regress-ext4fs-crash-flush-vmm \
	    run-regress-ext4fs-crash-orphan-vmm \
	    run-regress-ext4fs-crash-directory-vmm \
	    run-regress-ext4fs-crash-extent-vmm \
	    run-regress-ext4fs-crash-truncate-vmm \
	    run-regress-ext4fs-crash-reuse-vmm \
	    run-regress-ext4fs-crash-journal-vmm \
	    run-regress-ext4fs-crash-queue-vmm \
	    run-regress-ext4fs-crash-errors-vmm \
	    run-regress-ext4fs-crash-allocation-error-vmm \
	    run-regress-ext4fs-crash-allocation-error-all-vmm; do
		run_target sys/ext4fs_crash "$target"
	done
fi

printf '\nAll selected ext4fs regression tests passed.\n'
