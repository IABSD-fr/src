#!/bin/sh
#
# Exercise ordinary ext4fs operations through the production kernel.  Every
# stage is followed by an offline e2fsck pass so failures are attributed to the
# operation batch that caused them.

set -eu

MKE2FS=${MKE2FS:-mke2fs}
E2FSCK=${E2FSCK:-e2fsck}
VNCONFIG=${VNCONFIG:-vnconfig}
MOUNT_EXT4FS=${MOUNT_EXT4FS:-mount_ext4fs}
UMOUNT=${UMOUNT:-umount}
TIMEOUT=${TIMEOUT:-timeout}
EXT4FSOPS=${EXT4FSOPS:-./ext4fsops}
EXT4FS_TIMEOUT=${EXT4FS_TIMEOUT:-60}
EXT4FS_IMAGE_MB=${EXT4FS_IMAGE_MB:-128}

for tool in "$MKE2FS" "$E2FSCK" "$VNCONFIG" "$MOUNT_EXT4FS" \
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
	if ! "$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" "$EXT4FSOPS" "$mode" \
	    "$mountpoint/tree" >"$case_dir/$mode.log" 2>&1; then
		cat "$case_dir/$mode.log" >&2
		fail "$mode workload failed"
	fi
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
	"$UMOUNT" "$mountpoint" || fail "unmount failed"
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
	case_dir=$work/block-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"

	test_name="${block_size}-byte blocks"
	printf '%-44s' "kernel: ordinary operations ($block_size byte blocks)"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O '^orphan_file' "$image" >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi

	attach_image
	mount_image ""
	run_step create
	unmount_image
	detach_image
	check_image create

	attach_image
	mount_image ""
	run_step verify-create
	run_step mutate
	unmount_image
	detach_image
	check_image mutate

	attach_image
	mount_image ""
	run_step verify-final
	unmount_image
	detach_image
	check_image final

	before=$(sha256 -q "$image")
	attach_image
	mount_image ro
	run_step verify-readonly
	unmount_image
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] || fail "read-only mount changed the image"
	check_image read-only

	echo " ok"
}

for block_size in 1024 2048 4096; do
	run_case "$block_size"
done
