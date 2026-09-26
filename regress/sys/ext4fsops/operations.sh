#!/bin/sh
#
# Exercise ordinary ext4fs operations through the production kernel.  Every
# stage is followed by an offline e2fsck pass so failures are attributed to the
# operation batch that caused them.

set -eu

MKE2FS=${MKE2FS:-mke2fs}
E2FSCK=${E2FSCK:-e2fsck}
DEBUGFS=${DEBUGFS:-debugfs}
DUMPE2FS=${DUMPE2FS:-dumpe2fs}
VNCONFIG=${VNCONFIG:-vnconfig}
MOUNT_EXT4FS=${MOUNT_EXT4FS:-mount_ext4fs}
UMOUNT=${UMOUNT:-umount}
PSTAT=${PSTAT:-pstat}
TIMEOUT=${TIMEOUT:-timeout}
EXT4FSOPS=${EXT4FSOPS:-./ext4fsops}
EXT4FS_TIMEOUT=${EXT4FS_TIMEOUT:-60}
EXT4FS_IMAGE_MB=${EXT4FS_IMAGE_MB:-128}
EXT4FS_BLOCK_SIZES=${EXT4FS_BLOCK_SIZES:-"1024 2048 4096"}

for tool in "$MKE2FS" "$E2FSCK" "$DEBUGFS" "$DUMPE2FS" \
    "$VNCONFIG" "$MOUNT_EXT4FS" \
    "$UMOUNT" "$TIMEOUT" "$EXT4FSOPS" dd id sha256; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "SKIPPED: ext4fs operations regress requires $tool"
		exit 0
	fi
done

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIPPED: ext4fs operations regress must run as root"
	exit 0
fi

case "$EXT4FS_TIMEOUT:$EXT4FS_IMAGE_MB" in
*[!0-9:]*)
	echo "timeout and image size must be unsigned integers" >&2
	exit 1
	;;
esac
[ "$EXT4FS_TIMEOUT" -gt 0 ] || {
	echo "EXT4FS_TIMEOUT must be positive" >&2
	exit 1
}
[ "$EXT4FS_TIMEOUT" -le 600 ] || {
	echo "EXT4FS_TIMEOUT must not exceed 600" >&2
	exit 1
}
[ "$EXT4FS_IMAGE_MB" -ge 64 ] || {
	echo "EXT4FS_IMAGE_MB must be at least 64" >&2
	exit 1
}
[ "$EXT4FS_IMAGE_MB" -le 4096 ] || {
	echo "EXT4FS_IMAGE_MB must not exceed 4096" >&2
	exit 1
}

work=$(mktemp -d /tmp/ext4fs_operations.XXXXXXXX)
case "$work" in
/tmp/ext4fs_operations.*) ;;
*)
	echo "unsafe temporary directory: $work" >&2
	exit 1
	;;
esac

vnd=
mounted=0
mountpoint=$work/mnt
mkdir "$mountpoint"

cleanup()
{
	rc=$?
	trap - EXIT HUP INT TERM
	if [ "$mounted" -eq 1 ]; then
		"$UMOUNT" "$mountpoint" >/dev/null 2>&1 || :
		mounted=0
	fi
	if [ -n "$vnd" ]; then
		"$VNCONFIG" -u "$vnd" >/dev/null 2>&1 || :
		vnd=
	fi
	if [ "${KEEP_TMP:-0}" = 1 ] || [ "$rc" -ne 0 ]; then
		echo "temporary files retained in $work"
	else
		rm -rf "$work"
	fi
	exit "$rc"
}
trap cleanup EXIT HUP INT TERM

test_name=
fail()
{
	echo "FAILED: $test_name: $*" >&2
	exit 1
}

run_step()
{
	mode=$1
	step_root=${2:-$mountpoint/tree}
	if ! "$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" "$EXT4FSOPS" "$mode" \
	    "$step_root" >"$case_dir/$mode.log" 2>&1; then
		cat "$case_dir/$mode.log" >&2
		fail "$mode workload failed"
	fi
}

group_free_blocks()
{
	group_number=$1
	"$DUMPE2FS" "$image" 2>/dev/null | awk -v group="$group_number:" '
	    $1 == "Group" && $2 == group { selected = 1; next }
	    selected && /free blocks,/ { print $1; exit }
	'
}

attach_image()
{
	vnd_output=$($VNCONFIG "$image") || fail "vnconfig failed"
	set -- $vnd_output
	[ "$#" -gt 0 ] || fail "vnconfig returned no device"
	vnd=$1
	case "$vnd" in
	vnd[0-9]*) ;;
	*) fail "unexpected vnconfig device: $vnd" ;;
	esac
}

detach_image()
{
	"$VNCONFIG" -u "$vnd" || fail "could not detach $vnd"
	vnd=
}

mount_image()
{
	options=$1
	if [ -n "$options" ]; then
		"$MOUNT_EXT4FS" -o "$options" "/dev/${vnd}c" "$mountpoint" ||
		    fail "mount_ext4fs failed"
	else
		"$MOUNT_EXT4FS" "/dev/${vnd}c" "$mountpoint" ||
		    fail "mount_ext4fs failed"
	fi
	mounted=1
}

unmount_image()
{
	stage=$1
	if ! "$UMOUNT" "$mountpoint"; then
		if command -v "$PSTAT" >/dev/null 2>&1; then
			"$PSTAT" -v >"$case_dir/pstat-unmount.log" 2>&1 || :
			awk -v mountpoint="$mountpoint" '
			    /^\*\*\* MOUNT / {
				if (printing)
					exit
				if (index($0, mountpoint) != 0)
					printing = 1
			    }
			    printing { print }
			' "$case_dir/pstat-unmount.log" >&2
		fi
		fail "unmount failed after $stage"
	fi
	mounted=0
}

check_image()
{
	stage=$1
	if ! "$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" "$E2FSCK" -fn "$image" \
	    >"$case_dir/e2fsck-$stage.log" 2>&1; then
		cat "$case_dir/e2fsck-$stage.log" >&2
		fail "e2fsck rejected the image after $stage"
	fi
}

run_case()
{
	block_size=$1
	orphan_format=${2:-classic}
	case "$orphan_format" in
	classic)
		case_dir=$work/block-$block_size
		features='metadata_csum,^orphan_file'
		label="ordinary operations ($block_size byte blocks)"
		;;
	orphan-file)
		case_dir=$work/orphan-file-$block_size
		features='metadata_csum,orphan_file'
		label="orphan-file operations ($block_size byte blocks)"
		;;
	*)
		fail "unknown orphan format: $orphan_format"
		;;
	esac
	image=$case_dir/ext4.img
	mkdir "$case_dir"

	test_name="$label"
	printf '%-52s' "kernel: $label"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O "$features" "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi
	if [ "$orphan_format" = orphan-file ]; then
		"$DUMPE2FS" "$image" >"$case_dir/dumpe2fs.log" 2>&1 ||
		    fail "dumpe2fs rejected the orphan-file image"
		grep -q '^Filesystem features:.*orphan_file' \
		    "$case_dir/dumpe2fs.log" ||
		    fail "fixture does not enable orphan_file"
	fi

	attach_image
	mount_image ""
	run_step create
	unmount_image create
	detach_image
	check_image create

	attach_image
	mount_image ""
	run_step verify-create
	run_step mutate
	unmount_image mutate
	detach_image
	check_image mutate
	if [ "$orphan_format" = orphan-file ]; then
		"$DUMPE2FS" "$image" >"$case_dir/dumpe2fs-after-mutate.log" \
		    2>&1 || fail "dumpe2fs rejected the mutated orphan-file image"
		if grep -q '^Filesystem features:.*orphan_present' \
		    "$case_dir/dumpe2fs-after-mutate.log"; then
			fail "clean close retained ORPHAN_PRESENT"
		fi
	fi

	attach_image
	mount_image ""
	run_step verify-final
	unmount_image final
	detach_image
	check_image final

	before=$(sha256 -q "$image")
	attach_image
	mount_image ro
	run_step verify-readonly
	unmount_image read-only
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] || fail "read-only mount changed the image"
	check_image read-only

	echo " ok"
}

run_flex_uninit_case()
{
	case_dir=$work/flex-bg-block-uninit
	image=$case_dir/ext4.img
	empty=$case_dir/empty
	mkdir "$case_dir"
	: >"$empty"

	test_name="FLEX_BG BLOCK_UNINIT allocation"
	printf '%-44s' "kernel: FLEX_BG BLOCK_UNINIT allocation"
	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b 1024 \
	    -O '^orphan_file' "$image" >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi
	"$DUMPE2FS" "$image" >"$case_dir/dumpe2fs-before.log" 2>&1 ||
	    fail "dumpe2fs rejected the fresh image"
	grep -q '^Filesystem features:.*flex_bg' \
	    "$case_dir/dumpe2fs-before.log" ||
	    fail "fixture does not enable FLEX_BG"
	grep -q '^Group 2:.*BLOCK_UNINIT' \
	    "$case_dir/dumpe2fs-before.log" ||
	    fail "fixture group 2 is not BLOCK_UNINIT"

	free0=$(group_free_blocks 0)
	free1=$(group_free_blocks 1)
	case "$free0:$free1" in
	*[!0-9:]*) fail "could not read group free-block counts" ;;
	esac
	reserve_count=$((free0 + free1))
	[ "$reserve_count" -gt 0 ] || fail "fixture has no blocks to reserve"
	{
		printf 'write %s /reservoir\n' "$empty"
		printf 'fallocate /reservoir 0 %s\n' $((reserve_count - 1))
	} >"$case_dir/debugfs.cmd"
	if ! "$DEBUGFS" -w -f "$case_dir/debugfs.cmd" "$image" \
	    >"$case_dir/debugfs.log" 2>&1; then
		cat "$case_dir/debugfs.log" >&2
		fail "could not exhaust block groups 0 and 1"
	fi
	[ "$(group_free_blocks 0)" -eq 0 ] ||
	    fail "block group 0 was not exhausted"
	[ "$(group_free_blocks 1)" -eq 0 ] ||
	    fail "block group 1 was not exhausted"
	"$DUMPE2FS" "$image" >"$case_dir/dumpe2fs-filled.log" 2>&1 ||
	    fail "dumpe2fs rejected the filled image"
	grep -q '^Group 2:.*BLOCK_UNINIT' \
	    "$case_dir/dumpe2fs-filled.log" ||
	    fail "offline preparation initialized block group 2"

	attach_image
	mount_image ""
	run_step create-allocation-probe "$mountpoint"
	unmount_image allocation-probe
	detach_image
	check_image allocation-probe

	"$DUMPE2FS" "$image" >"$case_dir/dumpe2fs-after.log" 2>&1 ||
	    fail "dumpe2fs rejected the allocated image"
	if grep -q '^Group 2:.*BLOCK_UNINIT' \
	    "$case_dir/dumpe2fs-after.log"; then
		fail "kernel did not initialize block group 2"
	fi
	probe_blocks=$($DEBUGFS -R 'blocks /allocation-probe' "$image" \
	    2>/dev/null) || fail "could not locate allocation probe blocks"
	set -- $probe_blocks
	[ "$#" -eq 2 ] || fail "allocation probe does not use two blocks"
	for probe_block in "$@"; do
		case "$probe_block" in
		*[!0-9]*|'') fail "invalid allocation probe block" ;;
		esac
		[ "$probe_block" -ge 16385 ] && [ "$probe_block" -le 24576 ] ||
		    fail "allocation probe block is outside block group 2"
	done

	attach_image
	mount_image ""
	run_step verify-allocation-probe "$mountpoint"
	unmount_image allocation-probe-remount
	detach_image
	check_image allocation-probe-remount
	echo " ok"
}

for block_size in $EXT4FS_BLOCK_SIZES; do
	case "$block_size" in
	1024|2048|4096) ;;
	*)
		echo "unsupported filesystem block size: $block_size" >&2
		exit 1
		;;
	esac
	run_case "$block_size"
done

case " $EXT4FS_BLOCK_SIZES " in
*' 1024 '*) run_case 1024 orphan-file ;;
esac

case " $EXT4FS_BLOCK_SIZES " in
*' 1024 '*) run_flex_uninit_case ;;
esac
