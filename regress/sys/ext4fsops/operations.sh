#!/bin/sh
#
# Exercise ordinary ext4fs operations through the production kernel.
# Every stage is followed by an offline e2fsck pass so failures are
# attributed to the operation batch that caused them.

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
EXT4FSOPS_MODE=${EXT4FSOPS_MODE:-full}

for tool in "$MKE2FS" "$E2FSCK" "$DEBUGFS" "$DUMPE2FS" \
	    "$VNCONFIG" "$MOUNT_EXT4FS" \
	    "$UMOUNT" "$TIMEOUT" "$EXT4FSOPS" cmp dd diff hexdump id \
	    cp rmdir sha256; do
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

print_test_name()
{
	printf '%-52s' "kernel: $1"
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
	"$DUMPE2FS" "$image" 2>/dev/null |
	    awk -v group="$group_number:" '
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
		"$MOUNT_EXT4FS" -o "$options" "/dev/${vnd}c" \
		    "$mountpoint" ||
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
			"$PSTAT" -v \
			    >"$case_dir/pstat-unmount.log" 2>&1 || :
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

journal_sequence()
{
	journal_inode=$("$DUMPE2FS" -h "$image" 2>/dev/null | awk '
	    $1 == "Journal" && $2 == "inode:" { print $3; exit }
	')
	case "$journal_inode" in
	*[!0-9]*|'') fail "could not locate the journal inode" ;;
	esac
	journal_blocks=$("$DEBUGFS" -R "blocks <$journal_inode>" \
	    "$image" 2>/dev/null) ||
	    fail "could not map the journal inode"
	set -- $journal_blocks
	[ "$#" -gt 0 ] || fail "journal inode has no blocks"
	case "$1" in
	*[!0-9]*|'') fail "invalid journal superblock location" ;;
	esac
	journal_offset=$(($1 * block_size + 24))
	sequence=$(dd if="$image" bs=1 skip="$journal_offset" count=4 \
	    status=none | hexdump -ve '1/1 "%02x"')
	[ "${#sequence}" -eq 8 ] ||
	    fail "could not read the journal sequence"
	printf '%s\n' "$sequence"
}

check_last_orphan_clear()
{
	last_orphan=$(dd if="$image" bs=1 skip=$((1024 + 232)) \
	    count=4 status=none | hexdump -ve '1/1 "%02x"')
	[ "$last_orphan" = 00000000 ] ||
	    fail "classic orphan head was not cleared"
}

write_image_byte()
{
	byte_image=$1
	byte_offset=$2
	byte_value=$3
	case "$byte_offset:$byte_value" in
	*[!0-9:]*) fail "invalid image-byte write" ;;
	esac
	[ "$byte_value" -le 255 ] || fail "image byte exceeds 255"
	byte_octal=$(printf '%03o' "$byte_value")
	printf "\\$byte_octal" | dd of="$byte_image" bs=1 \
	    seek="$byte_offset" count=1 conv=notrunc status=none
}

inode_probe_number()
{
	inode_number=$("$DEBUGFS" -R \
	    'stat /inode-bitmap/inode-probe' "$image" 2>/dev/null |
	    awk '$1 == "Inode:" { print $2; exit }')
	case "$inode_number" in
	*[!0-9]*|'') fail "could not read the inode probe number" ;;
	esac
	[ "$inode_number" -gt 0 ] || fail "invalid inode probe number"
	printf '%s\n' "$inode_number"
}

group_descriptor_counts()
{
	"$DUMPE2FS" "$image" 2>/dev/null | awk '
	    $1 == "Group" {
		group = $2
		sub(/:$/, "", group)
	    }
	    /free blocks, .*free inodes, .*directories/ {
		print group, $1, $4, $7
	    }
	'
}

xattr_block()
{
	xattr_path=$1
	"$DEBUGFS" -R "stat $xattr_path" "$image" 2>/dev/null | awk '
	    $1 == "File" && $2 == "ACL:" { print $3; exit }
	'
}

prepare_xattr_fixture()
{
	xattr_value=$case_dir/xattr.value
	xattr_commands=$case_dir/xattr.debugfs
	dd if=/dev/zero of="$xattr_value" bs=1 count=300 status=none
	{
		printf 'ea_set -f %s %s user.regress\n' \
		    "$xattr_value" /tree/xattr-unique
		printf 'ea_set -f %s %s user.regress\n' \
		    "$xattr_value" /tree/xattr-shared-a
		printf 'ea_set -f %s %s user.regress\n' \
		    "$xattr_value" /tree/xattr-shared-b
	} >"$xattr_commands"
	if ! "$DEBUGFS" -w -f "$xattr_commands" "$image" \
	    >"$case_dir/debugfs-xattr.log" 2>&1; then
		cat "$case_dir/debugfs-xattr.log" >&2
		fail "could not create external xattr blocks"
	fi
	unique_block=$(xattr_block /tree/xattr-unique)
	shared_a_block=$(xattr_block /tree/xattr-shared-a)
	shared_b_block=$(xattr_block /tree/xattr-shared-b)
	case "$unique_block:$shared_a_block:$shared_b_block" in
	*[!0-9:]*|:*|*::*|*:)
		fail "could not locate external xattr blocks"
		;;
	esac
	[ "$unique_block" -ne 0 ] && [ "$shared_a_block" -ne 0 ] &&
	    [ "$shared_b_block" -ne 0 ] ||
	    fail "xattr value was not stored in an external block"
	[ "$unique_block" -ne "$shared_a_block" ] &&
	    [ "$shared_a_block" -ne "$shared_b_block" ] ||
	    fail "fresh xattr blocks unexpectedly alias"
	share_command="set_inode_field /tree/xattr-shared-b"
	share_command="$share_command file_acl $shared_a_block"
	if ! "$DEBUGFS" -w -R "$share_command" \
	    "$image" >"$case_dir/debugfs-xattr-share.log" 2>&1; then
		cat "$case_dir/debugfs-xattr-share.log" >&2
		fail "could not construct shared xattr fixture"
	fi
	set +e
	"$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" "$E2FSCK" -fy "$image" \
	    >"$case_dir/e2fsck-xattr-prepare.log" 2>&1
	xattr_fsck_status=$?
	set -e
	if [ "$xattr_fsck_status" -gt 1 ]; then
		cat "$case_dir/e2fsck-xattr-prepare.log" >&2
		fail "e2fsck could not normalize shared xattr fixture"
	fi
	shared_b_block=$(xattr_block /tree/xattr-shared-b)
	[ "$shared_b_block" -eq "$shared_a_block" ] ||
	    fail "shared xattr fixture does not share its block"
	[ "$(xattr_block /tree/xattr-unique)" -eq "$unique_block" ] ||
	    fail "shared xattr preparation changed the unique block"
	check_image xattr-fixture
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
	print_test_name "$label"

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
	prepare_xattr_fixture
	attach_image
	mount_image ""
	run_step create-growdir
	unmount_image create-growdir
	detach_image
	check_image create-growdir

	attach_image
	mount_image ""
	run_step verify-create
	run_step mutate
	unmount_image mutate
	detach_image
	check_image mutate
	if [ "$orphan_format" = orphan-file ]; then
		if ! "$DUMPE2FS" "$image" \
		    >"$case_dir/dumpe2fs-after-mutate.log" 2>&1; then
			fail "dumpe2fs rejected mutated orphan image"
		fi
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
	[ "$before" = "$after" ] ||
	    fail "read-only mount changed the image"
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
	print_test_name "$test_name"
	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b 1024 \
	    -O '^orphan_file' "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
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
	[ "$reserve_count" -gt 0 ] ||
	    fail "fixture has no blocks to reserve"
	reserve_last=$((reserve_count - 1))
	{
		printf 'write %s /reservoir\n' "$empty"
		printf 'fallocate /reservoir 0 %s\n' "$reserve_last"
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
	probe_blocks=$(
	    "$DEBUGFS" -R 'blocks /allocation-probe' "$image" \
		2>/dev/null
	) || fail "could not locate allocation probe blocks"
	set -- $probe_blocks
	[ "$#" -eq 2 ] ||
	    fail "allocation probe does not use two blocks"
	for probe_block in "$@"; do
		case "$probe_block" in
		*[!0-9]*|'') fail "invalid allocation probe block" ;;
		esac
		[ "$probe_block" -ge 16385 ] &&
		    [ "$probe_block" -le 24576 ] ||
		    fail "allocation probe block is outside group 2"
	done

	attach_image
	mount_image ""
	run_step verify-allocation-probe "$mountpoint"
	unmount_image allocation-probe-remount
	detach_image
	check_image allocation-probe-remount
	echo " ok"
}

run_bitmap_case()
{
	block_size=$1
	case_dir=$work/bitmap-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="block-bitmap transactions ($block_size byte blocks)"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O 'metadata_csum,^orphan_file' "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi
	sequence_before=$(journal_sequence)

	attach_image
	mount_image ""
	run_step bitmap-allocate "$mountpoint/bitmap"
	unmount_image bitmap-allocate
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "allocation did not advance the journal sequence"
	check_image bitmap-allocate
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step bitmap-verify "$mountpoint/bitmap"
	run_step bitmap-free "$mountpoint/bitmap"
	unmount_image bitmap-free
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "free did not advance the journal sequence"
	check_image bitmap-free
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step bitmap-verify-free "$mountpoint/bitmap"
	run_step bitmap-reuse "$mountpoint/bitmap"
	unmount_image bitmap-reuse
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "reuse did not advance the journal sequence"
	check_image bitmap-reuse
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step bitmap-verify "$mountpoint/bitmap"
	run_step bitmap-retire "$mountpoint/bitmap"
	unmount_image bitmap-retire
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "retirement did not advance the journal sequence"
	check_image bitmap-retire

	attach_image
	mount_image ""
	run_step bitmap-verify-retired "$mountpoint/bitmap"
	unmount_image bitmap-final
	detach_image
	check_image bitmap-final
	echo " ok"
}

run_inode_bitmap_case()
{
	block_size=$1
	case_dir=$work/inode-bitmap-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="inode-bitmap transactions ($block_size byte blocks)"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O 'metadata_csum,^orphan_file' "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi
	if [ "$block_size" -eq 1024 ]; then
		"$DUMPE2FS" "$image" >"$case_dir/dumpe2fs-before.log" \
		    2>&1 || fail "dumpe2fs rejected the fresh image"
		grep -q '^Group 1:.*INODE_UNINIT' \
		    "$case_dir/dumpe2fs-before.log" ||
		    fail "fixture group 1 is not INODE_UNINIT"
	fi
	sequence_before=$(journal_sequence)

	attach_image
	mount_image ""
	run_step inode-allocate "$mountpoint/inode-bitmap"
	unmount_image inode-allocate
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "inode allocation did not advance the journal sequence"
	check_image inode-allocate
	if [ "$block_size" -eq 1024 ]; then
		if ! "$DUMPE2FS" "$image" \
		    >"$case_dir/dumpe2fs-allocate.log" 2>&1; then
			fail "dumpe2fs rejected the allocated image"
		fi
		if grep -q '^Group 1:.*INODE_UNINIT' \
		    "$case_dir/dumpe2fs-allocate.log"; then
			reason="group 1 inode bitmap is uninitialized"
			fail "$reason"
		fi
	fi
	first_inode=$(inode_probe_number)
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step inode-verify "$mountpoint/inode-bitmap"
	run_step inode-free "$mountpoint/inode-bitmap"
	unmount_image inode-free
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "inode free did not advance the journal sequence"
	check_image inode-free
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step inode-verify-retired "$mountpoint/inode-bitmap"
	run_step inode-reuse "$mountpoint/inode-bitmap"
	unmount_image inode-reuse
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "inode reuse did not advance the journal sequence"
	check_image inode-reuse
	reused_inode=$(inode_probe_number)
	[ "$reused_inode" -eq "$first_inode" ] ||
	    fail "freed inode was not immediately reused"
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step inode-verify "$mountpoint/inode-bitmap"
	run_step inode-free "$mountpoint/inode-bitmap"
	unmount_image inode-retire
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "inode retirement did not advance the journal sequence"
	check_image inode-retire

	attach_image
	mount_image ""
	run_step inode-verify-retired "$mountpoint/inode-bitmap"
	unmount_image inode-final
	detach_image
	check_image inode-final
	echo " ok"
}

run_bgd_case()
{
	block_size=$1
	descriptor_size=$2
	case_dir=$work/bgd-$descriptor_size-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="group descriptors $descriptor_size-byte "
	test_name="$test_name($block_size byte blocks)"
	print_test_name "$test_name"

	case "$descriptor_size" in
	32) features='metadata_csum,^64bit,^orphan_file' ;;
	64) features='metadata_csum,^orphan_file' ;;
	*) fail "unsupported descriptor size: $descriptor_size" ;;
	esac
	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O "$features" "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi
	"$DUMPE2FS" -h "$image" >"$case_dir/dumpe2fs.log" \
	    2>&1 || fail "dumpe2fs rejected the fresh image"
	if [ "$descriptor_size" -eq 64 ]; then
		grep -q '^Filesystem features:.*64bit' \
		    "$case_dir/dumpe2fs.log" ||
		    fail "fixture does not use 64-byte descriptors"
		grep -q '^Group descriptor size: *64$' \
		    "$case_dir/dumpe2fs.log" ||
		    fail "fixture descriptor size is not 64"
	elif grep -q '^Filesystem features:.*64bit' \
	    "$case_dir/dumpe2fs.log"; then
		fail "fixture does not use 32-byte descriptors"
	fi
	group_descriptor_counts >"$case_dir/counts-before"
	[ -s "$case_dir/counts-before" ] ||
	    fail "could not read group descriptor counters"
	sequence_before=$(journal_sequence)

	attach_image
	mount_image ""
	run_step bitmap-allocate "$mountpoint/bgd"
	unmount_image bgd-allocate
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "allocation did not advance the journal sequence"
	check_image bgd-allocate
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step bitmap-verify "$mountpoint/bgd"
	run_step bitmap-retire "$mountpoint/bgd"
	if ! "$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" \
	    rmdir "$mountpoint/bgd" \
	    >"$case_dir/rmdir.log" 2>&1; then
		cat "$case_dir/rmdir.log" >&2
		fail "descriptor probe directory removal failed"
	fi
	unmount_image bgd-retire
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "retirement did not advance the journal sequence"
	check_image bgd-retire
	group_descriptor_counts >"$case_dir/counts-after"
	cmp -s "$case_dir/counts-before" "$case_dir/counts-after" || {
		diff -u "$case_dir/counts-before" \
		    "$case_dir/counts-after" >&2 || :
		fail "group descriptor counters were not restored"
	}

	attach_image
	mount_image ""
	[ ! -e "$mountpoint/bgd" ] ||
	    fail "retired descriptor probe survived remount"
	unmount_image bgd-final
	detach_image
	check_image bgd-final
	echo " ok"
}

extent_leaf_count()
{
	stage=$1
	output=$case_dir/extents-$stage.log
	if ! "$DEBUGFS" -R 'stat /extent-probe' "$image" \
	    >"$output" 2>&1; then
		cat "$output" >&2
		fail "could not inspect the extent fixture after $stage"
	fi
	awk '
	    {
		line = $0
		while (match(line, /\(ETB0\):/)) {
			count++
			line = substr(line, RSTART + RLENGTH)
		}
	    }
	    END { print count + 0 }
	' "$output"
}

check_extent_leaf_count()
{
	expected=$1
	stage=$2
	actual=$(extent_leaf_count "$stage")
	case "$actual" in
	*[!0-9]*|'')
		fail "could not count extent leaves after $stage"
		;;
	esac
	if [ "$actual" -ne "$expected" ]; then
		reason="expected $expected leaves after $stage"
		fail "$reason, got $actual"
	fi
}

prepare_extent_fixture()
{
	fixture_entries=$1
	source_root=$case_dir/source
	source_file=$source_root/extent-probe
	fixture_size=$(((2 * fixture_entries - 1) * block_size))
	mkdir "$source_root"
	dd if=/dev/zero of="$source_file" bs=1 count=0 \
	    seek="$fixture_size" status=none
	fixture_entry=0
	while [ "$fixture_entry" -lt "$fixture_entries" ]; do
		fixture_offset=$((2 * fixture_entry * block_size))
		printf x | dd of="$source_file" bs=1 \
		    seek="$fixture_offset" conv=notrunc status=none
		fixture_entry=$((fixture_entry + 1))
	done
	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O 'metadata_csum,^orphan_file' -d "$source_root" \
	    "$image" >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs could not create the extent fixture"
	fi
	check_image fixture
}

run_extent_step()
{
	action=$1
	expected_leaves=$2
	attach_image
	mount_image ""
	run_step "$action" "$mountpoint"
	unmount_image "$action"
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "$action did not advance the journal sequence"
	check_image "$action"
	check_extent_leaf_count "$expected_leaves" "$action"
	sequence_before=$sequence_after
}

run_extent_case()
{
	block_size=$1
	case_dir=$work/extents-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="extent-tree transactions ($block_size byte blocks)"
	print_test_name "$test_name"

	extent_capacity=$(((block_size - 12) / 12))
	prepare_extent_fixture "$extent_capacity"
	check_extent_leaf_count 1 fixture
	sequence_before=$(journal_sequence)
	run_extent_step extent-split 2
	run_extent_step extent-append 2
	run_extent_step extent-shrink 2
	run_extent_step extent-prune 1
	run_extent_step extent-zero 0

	attach_image
	mount_image ""
	run_step extent-verify-zero "$mountpoint"
	unmount_image extent-final
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "zero extent verification started a transaction"
	check_image extent-final
	echo " ok"
}

run_extent_reject_case()
{
	block_size=1024
	case_dir=$work/extents-reject-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="depth-2 extent growth rejection"
	print_test_name "$test_name"

	extent_capacity=$(((block_size - 12) / 12))
	prepare_extent_fixture "$((4 * extent_capacity - 3))"
	check_extent_leaf_count 4 fixture
	group_descriptor_counts >"$case_dir/counts-before"
	sequence_before=$(journal_sequence)

	attach_image
	mount_image noatime
	run_step extent-reject "$mountpoint"
	unmount_image extent-reject
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "rejected deep growth started a transaction"
	check_image extent-reject
	check_extent_leaf_count 4 extent-reject
	group_descriptor_counts >"$case_dir/counts-after"
	cmp -s "$case_dir/counts-before" "$case_dir/counts-after" || {
		diff -u "$case_dir/counts-before" \
		    "$case_dir/counts-after" >&2 || :
		fail "rejected deep growth changed descriptor counters"
	}
	echo " ok"
}

run_directory_case()
{
	block_size=$1
	checksum=$2
	case_dir=$work/directory-$checksum-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	case "$checksum" in
	enabled)
		features='metadata_csum,^orphan_file'
		;;
	disabled)
		features='^metadata_csum,^orphan_file'
		;;
	*)
		fail "unknown directory checksum mode: $checksum"
		;;
	esac
	test_name="directory tails $checksum ($block_size byte blocks)"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O "$features" "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi
	"$DUMPE2FS" -h "$image" >"$case_dir/dumpe2fs.log" \
	    2>&1 || fail "dumpe2fs rejected the directory image"
	if [ "$checksum" = enabled ]; then
		grep -q '^Filesystem features:.*metadata_csum' \
		    "$case_dir/dumpe2fs.log" ||
		    fail "directory image lacks metadata_csum"
	elif grep -q '^Filesystem features:.*metadata_csum' \
	    "$case_dir/dumpe2fs.log"; then
		fail "directory image unexpectedly has metadata_csum"
	fi
	sequence_before=$(journal_sequence)

	attach_image
	mount_image ""
	run_step directory-create "$mountpoint/directory"
	unmount_image directory-create
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "directory creation did not advance the journal"
	check_image directory-create
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step directory-verify "$mountpoint/directory"
	run_step directory-mutate "$mountpoint/directory"
	unmount_image directory-mutate
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "directory mutation did not advance the journal"
	check_image directory-mutate

	before=$(sha256 -q "$image")
	attach_image
	mount_image ro
	run_step directory-verify-final "$mountpoint/directory"
	unmount_image directory-final
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "read-only directory verification changed the image"
	check_image directory-final
	echo " ok"
}

run_directory_corrupt_case()
{
	block_size=1024
	case_dir=$work/directory-corrupt-tail
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="corrupt directory tail is non-mutating"
	print_test_name "$test_name"
	cp "$work/directory-enabled-1024/ext4.img" "$image"

	directory_blocks=$(
		"$DEBUGFS" -R 'blocks /directory/remove' "$image" \
		    2>/dev/null
	) || fail "could not locate the directory block"
	set -- $directory_blocks
	[ "$#" -eq 1 ] || fail "directory fixture is not one block"
	case "$1" in
	*[!0-9]*|'') fail "invalid directory block number" ;;
	esac
	directory_block=$1
	tail_offset=$(((directory_block + 1) * block_size - 1))
	tail_byte=$(dd if="$image" bs=1 skip="$tail_offset" count=1 \
	    status=none | hexdump -ve '1/1 "%u"')
	case "$tail_byte" in
	*[!0-9]*|'') fail "could not read directory checksum byte" ;;
	esac
	write_image_byte "$image" "$tail_offset" $((tail_byte ^ 1))
	dd if="$image" of="$case_dir/block-before" bs="$block_size" \
	    skip="$directory_block" count=1 status=none
	set +e
	"$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" "$E2FSCK" -fn \
	    "$image" >"$case_dir/e2fsck-corrupt.log" 2>&1
	fsck_status=$?
	set -e
	if [ "$fsck_status" -ne 4 ]; then
		cat "$case_dir/e2fsck-corrupt.log" >&2
		fail "unexpected corrupt-directory e2fsck status"
	fi
	sequence_before=$(journal_sequence)

	attach_image
	mount_image noatime
	run_step directory-reject "$mountpoint/directory/remove"
	unmount_image directory-reject
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "corrupt directory lookup started a transaction"
	dd if="$image" of="$case_dir/block-after" bs="$block_size" \
	    skip="$directory_block" count=1 status=none
	cmp -s "$case_dir/block-before" "$case_dir/block-after" ||
	    fail "corrupt directory block changed"
	echo " ok"
}

run_orphan_case()
{
	block_size=$1
	orphan_format=$2
	case_dir=$work/orphan-$orphan_format-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	case "$orphan_format" in
	classic)
		features='metadata_csum,^orphan_file'
		;;
	orphan-file)
		features='metadata_csum,orphan_file'
		;;
	*)
		fail "unknown orphan format: $orphan_format"
		;;
	esac
	test_name="$orphan_format metadata ($block_size byte blocks)"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O "$features" "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi
	"$DUMPE2FS" -h "$image" >"$case_dir/dumpe2fs.log" \
	    2>&1 || fail "dumpe2fs rejected the orphan image"
	if [ "$orphan_format" = orphan-file ]; then
		grep -q '^Filesystem features:.*orphan_file' \
		    "$case_dir/dumpe2fs.log" ||
		    fail "orphan-file feature is missing"
	else
		if grep -q '^Filesystem features:.*orphan_file' \
		    "$case_dir/dumpe2fs.log"; then
			fail "classic fixture has an orphan file"
		fi
	fi

	attach_image
	mount_image ""
	run_step orphan-create "$mountpoint/orphan"
	unmount_image orphan-create
	detach_image
	check_image orphan-create
	if [ "$orphan_format:$block_size" = orphan-file:1024 ]; then
		cp "$image" "$case_dir/base.img"
	fi
	sequence_before=$(journal_sequence)

	attach_image
	mount_image ""
	run_step orphan-cycle "$mountpoint/orphan"
	unmount_image orphan-cycle
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "orphan lifecycle did not advance the journal"
	check_last_orphan_clear
	"$DUMPE2FS" -h "$image" >"$case_dir/dumpe2fs-after.log" \
	    2>&1 || fail "dumpe2fs rejected the retired orphan image"
	if grep -q '^Filesystem features:.*orphan_present' \
	    "$case_dir/dumpe2fs-after.log"; then
		fail "orphan retirement retained ORPHAN_PRESENT"
	fi
	check_image orphan-cycle

	before=$(sha256 -q "$image")
	attach_image
	mount_image ro
	run_step orphan-verify "$mountpoint/orphan"
	unmount_image orphan-verify
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "read-only orphan verification changed the image"
	check_image orphan-verify
	echo " ok"
}

run_orphan_corrupt_case()
{
	block_size=1024
	case_dir=$work/orphan-corrupt-file
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="corrupt orphan file is non-mutating"
	print_test_name "$test_name"
	cp "$work/orphan-orphan-file-1024/base.img" "$image"

	orphan_inode=$("$DUMPE2FS" -h "$image" 2>/dev/null | awk '
	    $1 == "Orphan" && $2 == "file" && $3 == "inode:" {
		print $4
		exit
	    }
	')
	case "$orphan_inode" in
	*[!0-9]*|'') fail "could not locate the orphan-file inode" ;;
	esac
	orphan_blocks=$(
		"$DEBUGFS" -R "blocks <$orphan_inode>" "$image" \
		    2>/dev/null
	) || fail "could not map the orphan file"
	set -- $orphan_blocks
	[ "$#" -gt 0 ] || fail "orphan file has no blocks"
	case "$1" in
	*[!0-9]*|'') fail "invalid orphan-file block" ;;
	esac
	orphan_block=$1
	tail_offset=$(((orphan_block + 1) * block_size - 1))
	tail_byte=$(dd if="$image" bs=1 skip="$tail_offset" count=1 \
	    status=none | hexdump -ve '1/1 "%u"')
	case "$tail_byte" in
	*[!0-9]*|'') fail "could not read orphan checksum byte" ;;
	esac
	write_image_byte "$image" "$tail_offset" $((tail_byte ^ 1))
	dd if="$image" of="$case_dir/orphan-before" bs="$block_size" \
	    skip="$orphan_block" count=1 status=none

	directory_blocks=$(
		"$DEBUGFS" -R 'blocks /orphan' "$image" 2>/dev/null
	) || fail "could not locate the orphan fixture directory"
	set -- $directory_blocks
	[ "$#" -eq 1 ] ||
	    fail "orphan fixture directory is not one block"
	case "$1" in
	*[!0-9]*|'') fail "invalid orphan directory block" ;;
	esac
	directory_block=$1
	dd if="$image" of="$case_dir/directory-before" \
	    bs="$block_size" skip="$directory_block" count=1 status=none
	set +e
	"$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" "$E2FSCK" -fn \
	    "$image" >"$case_dir/e2fsck-corrupt.log" 2>&1
	fsck_status=$?
	set -e
	if [ "$fsck_status" -ne 4 ]; then
		cat "$case_dir/e2fsck-corrupt.log" >&2
		fail "unexpected corrupt orphan-file e2fsck status"
	fi
	sequence_before=$(journal_sequence)

	attach_image
	mount_image noatime
	run_step orphan-reject "$mountpoint/orphan"
	unmount_image orphan-reject
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "corrupt orphan-file unlink started a transaction"
	dd if="$image" of="$case_dir/orphan-after" bs="$block_size" \
	    skip="$orphan_block" count=1 status=none
	cmp -s "$case_dir/orphan-before" "$case_dir/orphan-after" ||
	    fail "corrupt orphan-file block changed"
	dd if="$image" of="$case_dir/directory-after" \
	    bs="$block_size" skip="$directory_block" count=1 status=none
	cmp -s "$case_dir/directory-before" \
	    "$case_dir/directory-after" ||
	    fail "rejected orphan-file unlink changed the namespace"
	echo " ok"
}

run_special_case()
{
	case_dir=$work/special-inodes
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="special-inode lookup and removal"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b 1024 \
	    -O 'metadata_csum,^orphan_file' "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi

	attach_image
	mount_image ""
	run_step create-special "$mountpoint/special"
	unmount_image special-create
	detach_image
	check_image special-create

	attach_image
	mount_image ""
	run_step verify-special "$mountpoint/special"
	run_step remove-special "$mountpoint/special"
	unmount_image special-remove
	detach_image
	check_image special-remove
	echo " ok"
}

case "$EXT4FSOPS_MODE" in
special)
	run_special_case
	exit 0
	;;
bitmap)
	for block_size in $EXT4FS_BLOCK_SIZES; do
		case "$block_size" in
		1024|2048|4096) ;;
		*)
			echo "unsupported filesystem block size: " \
			    "$block_size" >&2
			exit 1
			;;
		esac
		run_bitmap_case "$block_size"
	done
	exit 0
	;;
inode-bitmap)
	for block_size in $EXT4FS_BLOCK_SIZES; do
		case "$block_size" in
		1024|2048|4096) ;;
		*)
			echo "unsupported filesystem block size: " \
			    "$block_size" >&2
			exit 1
			;;
		esac
		run_inode_bitmap_case "$block_size"
	done
	exit 0
	;;
bgd)
	for descriptor_size in 32 64; do
		for block_size in $EXT4FS_BLOCK_SIZES; do
			case "$block_size" in
			1024|2048|4096) ;;
			*)
				echo "bad block size: $block_size" >&2
				exit 1
				;;
			esac
			run_bgd_case "$block_size" "$descriptor_size"
		done
	done
	exit 0
	;;
extents)
	for block_size in $EXT4FS_BLOCK_SIZES; do
		case "$block_size" in
		1024|2048|4096) ;;
		*)
			echo "bad block size: $block_size" >&2
			exit 1
			;;
		esac
		run_extent_case "$block_size"
	done
	case " $EXT4FS_BLOCK_SIZES " in
	*' 1024 '*) run_extent_reject_case ;;
	esac
	exit 0
	;;
directory)
	for checksum in enabled disabled; do
		for block_size in $EXT4FS_BLOCK_SIZES; do
			case "$block_size" in
			1024|2048|4096) ;;
			*)
				echo "bad block size: $block_size" >&2
				exit 1
				;;
			esac
			run_directory_case "$block_size" "$checksum"
		done
	done
	case " $EXT4FS_BLOCK_SIZES " in
	*' 1024 '*) run_directory_corrupt_case ;;
	esac
	exit 0
	;;
orphan)
	for orphan_format in classic orphan-file; do
		for block_size in $EXT4FS_BLOCK_SIZES; do
			case "$block_size" in
			1024|2048|4096) ;;
			*)
				echo "bad block size: $block_size" >&2
				exit 1
				;;
			esac
			run_orphan_case "$block_size" "$orphan_format"
		done
	done
	case " $EXT4FS_BLOCK_SIZES " in
	*' 1024 '*) run_orphan_corrupt_case ;;
	esac
	exit 0
	;;
full) ;;
*)
	echo "unsupported ext4fsops mode: $EXT4FSOPS_MODE" >&2
	exit 1
	;;
esac

for block_size in $EXT4FS_BLOCK_SIZES; do
	case "$block_size" in
	1024|2048|4096) ;;
	*)
		echo "unsupported filesystem block size: " \
		    "$block_size" >&2
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
