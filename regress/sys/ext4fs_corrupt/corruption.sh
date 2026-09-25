#!/bin/sh
#
# Generate malformed ext4 images and require the production kernel to reject
# them without changing a byte.  Fixture-only mode validates the generator
# without requiring root privileges.

set -eu

MKE2FS=${MKE2FS:-mke2fs}
DEBUGFS=${DEBUGFS:-debugfs}
E2FSCK=${E2FSCK:-e2fsck}
VNCONFIG=${VNCONFIG:-vnconfig}
MOUNT_EXT4FS=${MOUNT_EXT4FS:-mount_ext4fs}
MOUNT=${MOUNT:-mount}
UMOUNT=${UMOUNT:-umount}
TIMEOUT=${TIMEOUT:-timeout}
EXT4FS_CORRUPT=${EXT4FS_CORRUPT:-./ext4fs_corrupt}
EXT4FS_CORRUPT_MODE=${EXT4FS_CORRUPT_MODE:-kernel}
EXT4FS_CORRUPT_TIMEOUT=${EXT4FS_CORRUPT_TIMEOUT:-20}
EXT4FS_CORRUPT_IMAGE_MB=${EXT4FS_CORRUPT_IMAGE_MB:-64}

case "$EXT4FS_CORRUPT_MODE" in
fixtures|kernel) ;;
*)	echo "EXT4FS_CORRUPT_MODE must be 'fixtures' or 'kernel'" >&2
	exit 1
	;;
esac

tools="$MKE2FS $DEBUGFS $E2FSCK $TIMEOUT $EXT4FS_CORRUPT awk cat cmp cp \
dd id sha256 wc"
if [ "$EXT4FS_CORRUPT_MODE" = kernel ]; then
	tools="$tools $VNCONFIG $MOUNT_EXT4FS $MOUNT $UMOUNT $TIMEOUT grep"
fi
for tool in $tools; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "SKIPPED: ext4fs corruption regress requires $tool"
		exit 0
	fi
done

if [ "$EXT4FS_CORRUPT_MODE" = kernel ] && [ "$(id -u)" -ne 0 ]; then
	echo "SKIPPED: ext4fs corruption kernel regress must run as root"
	exit 0
fi

case "$EXT4FS_CORRUPT_TIMEOUT:$EXT4FS_CORRUPT_IMAGE_MB" in
*[!0-9:]*)
	echo "timeout and image size must be unsigned integers" >&2
	exit 1
	;;
esac
[ "$EXT4FS_CORRUPT_TIMEOUT" -gt 0 ] || {
	echo "EXT4FS_CORRUPT_TIMEOUT must be positive" >&2
	exit 1
}
[ "$EXT4FS_CORRUPT_TIMEOUT" -le 120 ] || {
	echo "EXT4FS_CORRUPT_TIMEOUT must not exceed 120" >&2
	exit 1
}
[ "$EXT4FS_CORRUPT_IMAGE_MB" -ge 64 ] || {
	echo "EXT4FS_CORRUPT_IMAGE_MB must be at least 64" >&2
	exit 1
}
[ "$EXT4FS_CORRUPT_IMAGE_MB" -le 4096 ] || {
	echo "EXT4FS_CORRUPT_IMAGE_MB must not exceed 4096" >&2
	exit 1
}

work=$(mktemp -d /tmp/ext4fs_corrupt.XXXXXXXX)
case "$work" in
/tmp/ext4fs_corrupt.*) ;;
*)	echo "unsafe temporary directory: $work" >&2
	exit 1
	;;
esac

vnd=
mounted=0
mountpoint=$work/mnt
mkdir "$mountpoint"
payload=$work/extent-payload
printf 'ext4fs extent payload\n' >"$payload"
payload_size=$(wc -c <"$payload")

is_mounted()
{
	"$MOUNT" | grep -F " on $mountpoint " >/dev/null 2>&1
}

cleanup()
{
	rc=$?
	trap - EXIT HUP INT TERM
	if [ "$mounted" -eq 1 ] ||
	    { [ "$EXT4FS_CORRUPT_MODE" = kernel ] && is_mounted; }; then
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
case_dir=
fail()
{
	echo "FAILED: $test_name: $*" >&2
	exit 1
}

create_base()
{
	profile=$1
	block_size=$2
	base=$work/base-$profile-$block_size.img
	log=$work/mke2fs-$profile-$block_size.log
	commands=$work/debugfs-$profile-$block_size.cmd
	debuglog=$work/debugfs-$profile-$block_size.log
	checklog=$work/e2fsck-$profile-$block_size.log

	dd if=/dev/zero of="$base" bs=1m count=0 \
	    seek="$EXT4FS_CORRUPT_IMAGE_MB" status=none
	case "$profile" in
	checksum)	features='^orphan_file' ;;
	no-checksum)	features='^metadata_csum,^orphan_file' ;;
	*)		fail "unknown fixture profile: $profile" ;;
	esac
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O "$features" "$base" >"$log" 2>&1; then
		cat "$log" >&2
		fail "mke2fs failed for $profile profile"
	fi
	{
		printf 'write %s /extent-inline\n' "$payload"
		printf 'write %s /extent-depth1\n' "$payload"
		printf 'fallocate /extent-depth1 2 2\n'
		printf 'fallocate /extent-depth1 4 4\n'
		printf 'fallocate /extent-depth1 6 6\n'
		printf 'fallocate /extent-depth1 8 8\n'
		printf 'fallocate /extent-depth1 10 10\n'
		printf 'set_inode_field /extent-depth1 size %s\n' \
		    "$((11 * block_size))"
	} >"$commands"
	if ! "$DEBUGFS" -w -f "$commands" "$base" >"$debuglog" 2>&1; then
		cat "$debuglog" >&2
		fail "debugfs could not create extent controls"
	fi
	inline_inode=$("$DEBUGFS" -R 'stat /extent-inline' "$base" \
	    2>/dev/null | awk '/^Inode:/ { print $2; exit }')
	depth1_inode=$("$DEBUGFS" -R 'stat /extent-depth1' "$base" \
	    2>/dev/null | awk '/^Inode:/ { print $2; exit }')
	case "$inline_inode:$depth1_inode" in
	*[!0-9:]*)	fail "debugfs returned an invalid extent inode" ;;
	:*|*:)		fail "debugfs did not return extent inodes" ;;
	esac
	printf '%s\n' "$inline_inode" >"$base.inline-inode"
	printf '%s\n' "$depth1_inode" >"$base.depth1-inode"
	if ! "$TIMEOUT" -k 2 "$EXT4FS_CORRUPT_TIMEOUT" \
	    "$E2FSCK" -fn "$base" >"$checklog" 2>&1; then
		cat "$checklog" >&2
		fail "e2fsck rejected an extent control image"
	fi
}

fixture_profile()
{
	case "$1" in
bad-superblock-checksum|bad-group-descriptor-checksum|\
extent-block-bad-checksum)
		FIXTURE_PROFILE=checksum
		;;
	*)	FIXTURE_PROFILE=no-checksum ;;
	esac
}

make_fixture()
{
	mutation=$1
	block_size=$2
	fixture_profile "$mutation"
	base=$work/base-$FIXTURE_PROFILE-$block_size.img
	case_dir=$work/$block_size-$mutation
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	cp "$base" "$image"
	base_hash=$(sha256 -q "$image")
	target_path=
	case "$mutation" in
	extent-root-depth-too-large|extent-index-*|extent-leaf-*|\
	extent-block-bad-checksum)
		target_path=extent-depth1
		inode=$(cat "$base.depth1-inode")
		;;
	extent-root-*)
		target_path=extent-inline
		inode=$(cat "$base.inline-inode")
		;;
	*)	inode= ;;
	esac
	if [ -n "$inode" ]; then
		set -- "$image" "$mutation" "$inode"
	else
		set -- "$image" "$mutation"
	fi
	if ! "$EXT4FS_CORRUPT" "$@" \
	    >"$case_dir/mutate.log" 2>&1; then
		cat "$case_dir/mutate.log" >&2
		fail "fixture mutation failed"
	fi
	mutated_hash=$(sha256 -q "$image")
	[ "$base_hash" != "$mutated_hash" ] ||
	    fail "fixture mutation changed no bytes"
}

attach_image()
{
	if ! output=$("$VNCONFIG" "$image"); then
		fail "vnconfig failed"
	fi
	set -- $output
	vnd=${1:-}
	case "$vnd" in
	vnd[0-9]*) ;;
	*)	fail "unexpected vnconfig device: $output" ;;
	esac
}

detach_image()
{
	if [ "$mounted" -eq 1 ] || is_mounted; then
		"$UMOUNT" "$mountpoint" || fail "unmount failed"
		mounted=0
	fi
	"$VNCONFIG" -u "$vnd" || fail "could not detach $vnd"
	vnd=
}

mount_control()
{
	profile=$1
	block_size=$2
	test_name="valid $profile control, $block_size-byte blocks"
	case_dir=$work/control-$profile-$block_size
	image=$work/base-$profile-$block_size.img
	mkdir "$case_dir"
	printf '%-64s' "kernel: $test_name"
	before=$(sha256 -q "$image")
	attach_image
	if ! "$TIMEOUT" -k 2 "$EXT4FS_CORRUPT_TIMEOUT" \
	    "$MOUNT_EXT4FS" -o ro "/dev/${vnd}c" "$mountpoint" \
	    >"$case_dir/mount.log" 2>&1; then
		cat "$case_dir/mount.log" >&2
		detach_image
		fail "kernel rejected the valid control"
	fi
	mounted=1
	for target in extent-inline extent-depth1; do
		if ! "$TIMEOUT" -k 2 "$EXT4FS_CORRUPT_TIMEOUT" dd \
		    if="$mountpoint/$target" of="$case_dir/$target.data" \
		    bs=1 count="$payload_size" status=none \
		    >"$case_dir/$target.log" 2>&1; then
			cat "$case_dir/$target.log" >&2
			detach_image
			fail "could not read valid $target control"
		fi
		if ! cmp -s "$case_dir/$target.data" "$payload"; then
			detach_image
			fail "$target control data differs"
		fi
	done
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] || fail "read-only control mount changed image"
	echo " ok"
}

reject_extent_fixture()
{
	mutation=$1
	block_size=$2
	test_name="$mutation, $block_size-byte blocks"
	printf '%-64s' "kernel: $test_name"
	make_fixture "$mutation" "$block_size"
	before=$(sha256 -q "$image")
	attach_image
	if ! "$TIMEOUT" -k 2 "$EXT4FS_CORRUPT_TIMEOUT" \
	    "$MOUNT_EXT4FS" -o ro "/dev/${vnd}c" "$mountpoint" \
	    >"$case_dir/mount.log" 2>&1; then
		cat "$case_dir/mount.log" >&2
		detach_image
		fail "kernel rejected filesystem before extent access"
	fi
	mounted=1
	status=0
	if "$TIMEOUT" -k 2 "$EXT4FS_CORRUPT_TIMEOUT" dd \
	    if="$mountpoint/$target_path" of=/dev/null bs=1 count=1 \
	    status=none >"$case_dir/read.log" 2>&1; then
		status=0
	else
		status=$?
	fi
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "failed read-only extent access modified the image"
	case "$status" in
	124|137|143)	fail "extent access exceeded timeout" ;;
	0)		fail "kernel accepted malformed extent metadata" ;;
	esac
	if [ "$status" -ge 128 ]; then
		fail "extent access terminated abnormally (status $status)"
	fi
	echo " ok"
}

reject_fixture()
{
	mutation=$1
	block_size=$2
	test_name="$mutation, $block_size-byte blocks"
	printf '%-64s' "kernel: $test_name"
	make_fixture "$mutation" "$block_size"
	before=$(sha256 -q "$image")
	attach_image
	status=0
	if "$TIMEOUT" -k 2 "$EXT4FS_CORRUPT_TIMEOUT" \
	    "$MOUNT_EXT4FS" -o ro "/dev/${vnd}c" "$mountpoint" \
	    >"$case_dir/mount.log" 2>&1; then
		mounted=1
		status=0
	else
		status=$?
		if is_mounted; then
			mounted=1
		fi
	fi
	if [ "$mounted" -eq 1 ]; then
		detach_image
		fail "kernel mounted a malformed fixture"
	fi
	case "$status" in
	124|137|143)
		detach_image
		fail "mount exceeded timeout"
		;;
	0)
		detach_image
		fail "kernel accepted malformed metadata"
		;;
	esac
	if [ "$status" -ge 128 ]; then
		detach_image
		fail "mount terminated abnormally (status $status)"
	fi
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "failed read-only mount modified the image"
	echo " ok"
}

checksum_mutations='bad-superblock-checksum bad-group-descriptor-checksum'
geometry_mutations='block-size-too-large zero-blocks-per-group
zero-inodes-per-group invalid-inode-size invalid-first-inode
invalid-descriptor-size unsupported-incompat-feature recover-without-journal'
extent_mutations='extent-root-bad-magic
extent-root-physical-out-of-range extent-leaf-bad-magic
extent-index-out-of-range extent-root-depth-too-large
extent-root-entries-over-max extent-root-max-too-large
extent-root-unordered extent-root-overlap extent-root-zero-length
extent-leaf-entries-over-max extent-leaf-max-too-large
extent-leaf-unordered extent-leaf-overlap extent-block-bad-checksum'

for block_size in 1024 2048 4096; do
	test_name="create base images, $block_size-byte blocks"
	create_base checksum "$block_size"
	create_base no-checksum "$block_size"

	if [ "$EXT4FS_CORRUPT_MODE" = kernel ]; then
		mount_control checksum "$block_size"
		mount_control no-checksum "$block_size"
	fi

	for mutation in $checksum_mutations $geometry_mutations; do
		if [ "$EXT4FS_CORRUPT_MODE" = fixtures ]; then
			test_name="$mutation, $block_size-byte blocks"
			make_fixture "$mutation" "$block_size"
		else
			reject_fixture "$mutation" "$block_size"
		fi
	done
	for mutation in $extent_mutations; do
		if [ "$EXT4FS_CORRUPT_MODE" = fixtures ]; then
			test_name="$mutation, $block_size-byte blocks"
			make_fixture "$mutation" "$block_size"
		else
			reject_extent_fixture "$mutation" "$block_size"
		fi
	done
done

if [ "$EXT4FS_CORRUPT_MODE" = fixtures ]; then
	echo "ext4fs malformed-metadata fixtures passed"
fi
