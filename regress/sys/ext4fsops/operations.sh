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
holder_pid=
mountpoint=$work/mnt
mkdir "$mountpoint"

cleanup()
{
	rc=$?
	trap - EXIT HUP INT TERM
	if [ -n "$holder_pid" ]; then
		kill "$holder_pid" >/dev/null 2>&1 || :
		wait "$holder_pid" >/dev/null 2>&1 || :
		holder_pid=
	fi
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

group_free_inodes()
{
	group_number=$1
	"$DUMPE2FS" "$image" 2>/dev/null |
	    awk -v group="$group_number:" '
	    $1 == "Group" && $2 == group { selected = 1; next }
	    selected && /free blocks,/ { print $4; exit }
	'
}

super_free_blocks()
{
	"$DUMPE2FS" -h "$image" 2>/dev/null |
	    awk '$1 == "Free" && $2 == "blocks:" { print $3; exit }'
}

super_free_inodes()
{
	"$DUMPE2FS" -h "$image" 2>/dev/null |
	    awk '$1 == "Free" && $2 == "inodes:" { print $3; exit }'
}

filesystem_counts()
{
	"$DUMPE2FS" -h "$image" 2>/dev/null | awk '
	    $1 == "Free" && $2 == "blocks:" { blocks = $3 }
	    $1 == "Free" && $2 == "inodes:" { inodes = $3 }
	    END {
		if (blocks != "" && inodes != "")
			print blocks, inodes
	    }
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

remount_image()
{
	options=$1
	stage=$2
	if ! "$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" \
	    "$MOUNT_EXT4FS" -o "update,$options" \
	    "/dev/${vnd}c" "$mountpoint" \
	    >"$case_dir/remount-$stage.log" 2>&1; then
		cat "$case_dir/remount-$stage.log" >&2
		fail "$stage remount failed"
	fi
}

reject_readonly_remount()
{
	stage=$1
	if "$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" \
	    "$MOUNT_EXT4FS" -o update,ro \
	    "/dev/${vnd}c" "$mountpoint" \
	    >"$case_dir/remount-$stage.log" 2>&1; then
		fail "$stage remount unexpectedly succeeded"
	else
		status=$?
	fi
	case "$status" in
	1)
		;;
	124|137)
		cat "$case_dir/remount-$stage.log" >&2
		fail "$stage remount timed out"
		;;
	*)
		cat "$case_dir/remount-$stage.log" >&2
		fail "$stage remount exited with status $status"
		;;
	esac
}

start_held_remount_orphan()
{
	ready=$mountpoint/remount-orphan-ready
	holder_log=$case_dir/remount-hold-orphan.log
	"$EXT4FSOPS" remount-hold-orphan "$mountpoint" \
	    >"$holder_log" 2>&1 &
	holder_pid=$!
	wait_ticks=0
	max_ticks=$((EXT4FS_TIMEOUT * 10))
	while [ ! -f "$ready" ]; do
		if ! kill -0 "$holder_pid" 2>/dev/null; then
			if wait "$holder_pid"; then
				status=0
			else
				status=$?
			fi
			holder_pid=
			cat "$holder_log" >&2
			fail "orphan holder exited with status $status"
		fi
		[ "$wait_ticks" -lt "$max_ticks" ] ||
		    fail "timed out waiting for orphan holder"
		wait_ticks=$((wait_ticks + 1))
		sleep .1
	done
}

release_held_remount_orphan()
{
	ready=$mountpoint/remount-orphan-ready
	release=$mountpoint/remount-orphan-release
	holder_log=$case_dir/remount-hold-orphan.log
	if ! : >"$release"; then
		fail "mount was not writable after rejected remount"
	fi
	if wait "$holder_pid"; then
		status=0
	else
		status=$?
	fi
	holder_pid=
	if [ "$status" -ne 0 ]; then
		cat "$holder_log" >&2
		fail "orphan holder exited with status $status"
	fi
	[ ! -e "$ready" ] || fail "orphan ready marker survived"
	[ ! -e "$release" ] || fail "orphan release marker survived"
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

journal_word()
{
	field_offset=$1
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
	journal_offset=$(($1 * block_size + field_offset))
	word=$(dd if="$image" bs=1 skip="$journal_offset" count=4 \
	    status=none | hexdump -ve '1/1 "%02x"')
	[ "${#word}" -eq 8 ] || fail "could not read a journal word"
	printf '%s\n' "$word"
}

journal_sequence()
{
	journal_word 24
}

journal_start()
{
	journal_word 28
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

expect_counter_mount_failure()
{
	stage=$1
	before=$(sha256 -q "$image")
	attach_image
	if "$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" \
	    "$MOUNT_EXT4FS" "/dev/${vnd}c" "$mountpoint" \
	    >"$case_dir/mount-$stage.log" 2>&1; then
		mounted=1
		fail "kernel accepted invalid counters"
	fi
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "failed mount changed the counter fixture"
}

run_counter_case()
{
	block_size=$1
	case_dir=$work/counters-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="allocation/free counters ($block_size byte blocks)"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O 'metadata_csum,^orphan_file' "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi
	filesystem_counts >"$case_dir/super-before"
	group_descriptor_counts >"$case_dir/groups-before"
	[ -s "$case_dir/super-before" ] &&
	    [ -s "$case_dir/groups-before" ] ||
	    fail "could not read initial counters"
	sequence_before=$(journal_sequence)

	attach_image
	mount_image ""
	run_step bitmap-allocate "$mountpoint/counter-blocks"
	run_step inode-allocate "$mountpoint/counter-inodes"
	run_step inode-free "$mountpoint/counter-inodes"
	run_step bitmap-retire "$mountpoint/counter-blocks"
	rmdir "$mountpoint/counter-blocks" \
	    "$mountpoint/counter-inodes" ||
	    fail "could not retire counter directories"
	unmount_image counters
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "counter transactions did not advance the journal"
	check_image counters
	filesystem_counts >"$case_dir/super-after"
	group_descriptor_counts >"$case_dir/groups-after"
	cmp -s "$case_dir/super-before" "$case_dir/super-after" || {
		diff -u "$case_dir/super-before" \
		    "$case_dir/super-after" >&2 || :
		fail "superblock counters were not restored"
	}
	cmp -s "$case_dir/groups-before" \
	    "$case_dir/groups-after" || {
		diff -u "$case_dir/groups-before" \
		    "$case_dir/groups-after" >&2 || :
		fail "group counters were not restored"
	}
	echo " ok"
}

prepare_counter_corrupt_base()
{
	counter_base_dir=$work/counter-corrupt-base
	counter_base=$counter_base_dir/ext4.img
	mkdir "$counter_base_dir"
	dd if=/dev/zero of="$counter_base" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b 1024 \
	    -O 'metadata_csum,^orphan_file' "$counter_base" \
	    >"$counter_base_dir/mke2fs.log" 2>&1; then
		cat "$counter_base_dir/mke2fs.log" >&2
		fail "could not create counter-corruption base"
	fi
	: >"$counter_base_dir/empty"
	if ! "$DEBUGFS" -w -R \
	    "write $counter_base_dir/empty /counter-block-probe" \
	    "$counter_base" >"$counter_base_dir/debugfs.log" 2>&1; then
		cat "$counter_base_dir/debugfs.log" >&2
		fail "could not create the counter block probe"
	fi
	if ! "$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" "$E2FSCK" -fn \
	    "$counter_base" >"$counter_base_dir/e2fsck.log" 2>&1; then
		cat "$counter_base_dir/e2fsck.log" >&2
		fail "counter-corruption base is invalid"
	fi
}

run_counter_mount_reject_case()
{
	kind=$1
	case_dir=$work/counter-reject-$kind
	image=$case_dir/ext4.img
	block_size=1024
	mkdir "$case_dir"
	cp "$counter_base" "$image"

	case "$kind" in
	super-blocks)
		test_name="inconsistent superblock free-block count"
		value=$(super_free_blocks)
		field=free_blocks_count
		;;
	super-inodes)
		test_name="inconsistent superblock free-inode count"
		value=$(super_free_inodes)
		field=free_inodes_count
		;;
	group-blocks)
		test_name="inconsistent group free-block count"
		value=$(group_free_blocks 0)
		field=free_blocks_count
		;;
	group-inodes)
		test_name="inconsistent group free-inode count"
		value=$(group_free_inodes 0)
		field=free_inodes_count
		;;
	group-dirs)
		test_name="out-of-range group directory count"
		value=65534
		field=used_dirs_count
		;;
	*) fail "unknown counter rejection case: $kind" ;;
	esac
	print_test_name "$test_name"
	case "$value" in
	*[!0-9]*|'') fail "could not read the counter to corrupt" ;;
	esac
	value=$((value + 1))
	case "$kind" in
	super-*)
		if ! "$DEBUGFS" -w -R "ssv $field $value" "$image" \
		    >"$case_dir/debugfs.log" 2>&1; then
			cat "$case_dir/debugfs.log" >&2
			fail "could not corrupt the superblock counter"
		fi
		;;
	group-*)
		if ! "$DEBUGFS" -w -R "set_bg 0 $field $value" \
		    "$image" >"$case_dir/debugfs.log" 2>&1; then
			cat "$case_dir/debugfs.log" >&2
			fail "could not corrupt the group counter"
		fi
		if ! "$DEBUGFS" -w -R 'set_bg 0 checksum calc' \
		    "$image" >>"$case_dir/debugfs.log" 2>&1; then
			cat "$case_dir/debugfs.log" >&2
			fail "could not authenticate the group counter"
		fi
		;;
	esac
	expect_counter_mount_failure "$kind"
	echo " ok"
}

run_counter_local_reject_case()
{
	kind=$1
	case_dir=$work/counter-local-$kind
	image=$case_dir/ext4.img
	block_size=1024
	mkdir "$case_dir"
	cp "$counter_base" "$image"

	case "$kind" in
	blocks)
		test_name="local free-block mismatch is non-mutating"
		probe_inode=$("$DEBUGFS" -R \
		    'stat /counter-block-probe' "$image" 2>/dev/null |
		    awk '$1 == "Inode:" { print $2; exit }')
		inodes_per_group=$("$DUMPE2FS" -h "$image" \
		    2>/dev/null | awk '
		    $1 == "Inodes" && $2 == "per" && $3 == "group:" {
			print $4
			exit
		    }')
		case "$probe_inode:$inodes_per_group" in
		*[!0-9:]*|:*|*:0) fail "could not locate probe group" ;;
		esac
		group=$(((probe_inode - 1) / inodes_per_group))
		group_value=$(group_free_blocks "$group")
		super_value=$(super_free_blocks)
		field=free_blocks_count
		mode=counter-reject-block
		;;
	inodes)
		test_name="local free-inode mismatch is non-mutating"
		group=0
		group_value=$(group_free_inodes "$group")
		super_value=$(super_free_inodes)
		field=free_inodes_count
		mode=counter-reject-inode
		;;
	*) fail "unknown local counter case: $kind" ;;
	esac
	print_test_name "$test_name"
	case "$group:$group_value:$super_value" in
	*[!0-9:]*|:*|*::*|*:)
		fail "invalid local counter values"
		;;
	esac
	original_group=$group_value
	original_super=$super_value
	group_value=$((group_value + 1))
	super_value=$((super_value + 1))
	if ! "$DEBUGFS" -w -R \
	    "set_bg $group $field $group_value" "$image" \
	    >"$case_dir/debugfs.log" 2>&1; then
		cat "$case_dir/debugfs.log" >&2
		fail "could not forge the local group counter"
	fi
	if ! "$DEBUGFS" -w -R \
	    "set_bg $group checksum calc" "$image" \
	    >>"$case_dir/debugfs.log" 2>&1; then
		cat "$case_dir/debugfs.log" >&2
		fail "could not authenticate the local group counter"
	fi
	if ! "$DEBUGFS" -w -R "ssv $field $super_value" "$image" \
	    >>"$case_dir/debugfs.log" 2>&1; then
		cat "$case_dir/debugfs.log" >&2
		fail "could not forge the matching superblock counter"
	fi
	filesystem_counts >"$case_dir/super-before"
	group_descriptor_counts >"$case_dir/groups-before"
	sequence_before=$(journal_sequence)

	attach_image
	mount_image ""
	run_step "$mode" "$mountpoint"
	unmount_image "$kind-counter-reject"
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "rejected counter operation advanced the journal"
	filesystem_counts >"$case_dir/super-after"
	group_descriptor_counts >"$case_dir/groups-after"
	cmp -s "$case_dir/super-before" "$case_dir/super-after" ||
	    fail "rejected operation changed superblock counters"
	cmp -s "$case_dir/groups-before" \
	    "$case_dir/groups-after" ||
	    fail "rejected operation changed group counters"

	"$DEBUGFS" -w -R \
	    "set_bg $group $field $original_group" "$image" \
	    >"$case_dir/debugfs-restore.log" 2>&1 ||
	    fail "could not restore the local group counter"
	"$DEBUGFS" -w -R \
	    "set_bg $group checksum calc" "$image" \
	    >>"$case_dir/debugfs-restore.log" 2>&1 ||
	    fail "could not restore the group checksum"
	"$DEBUGFS" -w -R "ssv $field $original_super" "$image" \
	    >>"$case_dir/debugfs-restore.log" 2>&1 ||
	    fail "could not restore the superblock counter"
	check_image "$kind-counter-reject"
	echo " ok"
}

superblock_identity()
{
	"$DUMPE2FS" -h "$image" 2>/dev/null | awk '
	    /^Filesystem UUID:/ ||
	    /^Filesystem magic number:/ ||
	    /^Filesystem revision #:/ ||
	    /^Filesystem features:/ ||
	    /^Inode count:/ ||
	    /^Block count:/ ||
	    /^Reserved block count:/ ||
	    /^First block:/ ||
	    /^Block size:/ ||
	    /^Group descriptor size:/ ||
	    /^Blocks per group:/ ||
	    /^Inodes per group:/ ||
	    /^Inode size:/ ||
	    /^Journal inode:/ { print }
	'
}

check_superblock_clean()
{
	stage=$1
	output=$case_dir/dumpe2fs-$stage.log
	if ! "$DUMPE2FS" -h "$image" >"$output" 2>&1; then
		cat "$output" >&2
		fail "dumpe2fs rejected the superblock after $stage"
	fi
	grep -q '^Filesystem state: *clean$' "$output" ||
	    fail "filesystem is not clean after $stage"
	if grep -q '^Filesystem features:.*needs_recovery' "$output" ||
	    grep -q '^Filesystem features:.*orphan_present' "$output"; then
		fail "recovery feature remains after $stage"
	fi
	grep -q '^Journal start: *0$' "$output" ||
	    fail "journal is not empty after $stage"
}

run_superblock_case()
{
	block_size=$1
	case_dir=$work/superblock-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="superblock transactions ($block_size byte blocks)"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O 'metadata_csum,^orphan_file' "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi
	superblock_identity >"$case_dir/identity-before"
	filesystem_counts >"$case_dir/counts-before"
	[ -s "$case_dir/identity-before" ] &&
	    [ -s "$case_dir/counts-before" ] ||
	    fail "could not read the initial superblock"
	check_superblock_clean initial
	sequence_before=$(journal_sequence)

	attach_image
	mount_image ""
	run_step bitmap-allocate "$mountpoint/super-blocks"
	run_step inode-allocate "$mountpoint/super-inodes"
	run_step inode-free "$mountpoint/super-inodes"
	run_step bitmap-retire "$mountpoint/super-blocks"
	rmdir "$mountpoint/super-blocks" \
	    "$mountpoint/super-inodes" ||
	    fail "could not retire superblock probes"
	unmount_image superblock-transactions
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "superblock transactions did not advance the journal"
	check_superblock_clean transactions
	check_image superblock-transactions
	superblock_identity >"$case_dir/identity-after"
	filesystem_counts >"$case_dir/counts-after"
	cmp -s "$case_dir/identity-before" \
	    "$case_dir/identity-after" ||
	    fail "superblock identity fields changed"
	cmp -s "$case_dir/counts-before" "$case_dir/counts-after" ||
	    fail "balanced operations changed superblock counts"

	sequence_before=$sequence_after
	attach_image
	mount_image ""
	unmount_image empty-writable-cycle
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "empty lifecycle created a journal transaction"
	check_superblock_clean empty-writable-cycle
	check_image empty-writable-cycle

	before=$(sha256 -q "$image")
	attach_image
	mount_image ro
	unmount_image read-only-cycle
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "read-only lifecycle changed the superblock"
	check_superblock_clean read-only-cycle
	echo " ok"
}

expect_superblock_mount_failure()
{
	stage=$1
	before=$(sha256 -q "$image")
	attach_image
	if "$TIMEOUT" -k 2 "$EXT4FS_TIMEOUT" \
	    "$MOUNT_EXT4FS" "/dev/${vnd}c" "$mountpoint" \
	    >"$case_dir/mount-$stage.log" 2>&1; then
		mounted=1
		fail "kernel accepted the malformed superblock"
	fi
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "failed mount changed the malformed superblock"
}

run_superblock_reject_case()
{
	kind=$1
	case_dir=$work/superblock-reject-$kind
	image=$case_dir/ext4.img
	block_size=1024
	mkdir "$case_dir"
	cp "$work/superblock-1024/ext4.img" "$image"

	case "$kind" in
	checksum)
		test_name="bad superblock checksum is non-mutating"
		offset=$((1024 + 1020))
		value=$(dd if="$image" bs=1 skip="$offset" count=1 \
		    status=none | hexdump -ve '1/1 "%u"')
		case "$value" in
		*[!0-9]*|'')
			fail "could not read superblock checksum"
			;;
		esac
		write_image_byte "$image" "$offset" $((value ^ 1))
		;;
	dirty)
		test_name="dirty superblock mount is non-mutating"
		if ! "$DEBUGFS" -w -R 'ssv state 0' "$image" \
		    >"$case_dir/debugfs.log" 2>&1; then
			cat "$case_dir/debugfs.log" >&2
			fail "could not create a dirty superblock"
		fi
		;;
	*) fail "unknown superblock rejection case: $kind" ;;
	esac
	print_test_name "$test_name"
	expect_superblock_mount_failure "$kind"
	echo " ok"
}

run_fsync_case()
{
	block_size=$1
	case_dir=$work/fsync-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="fsync ordering ($block_size byte blocks)"
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
	run_step fsync-create "$mountpoint"
	unmount_image fsync-create
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "initial fsync did not commit its inode"
	check_superblock_clean fsync-create
	check_image fsync-create
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step fsync-update "$mountpoint"
	unmount_image fsync-update
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "in-place fsync did not commit its inode"
	check_superblock_clean fsync-update
	check_image fsync-update
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step fsync-clean "$mountpoint"
	unmount_image fsync-clean
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "clean fsync created a journal transaction"
	check_superblock_clean fsync-clean
	check_image fsync-clean

	before=$(sha256 -q "$image")
	attach_image
	mount_image ro
	run_step fsync-verify "$mountpoint"
	unmount_image fsync-verify
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "read-only verification changed the image"
	echo " ok"
}

run_sync_case()
{
	block_size=$1
	case_dir=$work/sync-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="O_SYNC and sync mount ($block_size byte blocks)"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O 'metadata_csum,^orphan_file' "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi

	attach_image
	mount_image ""
	run_step sync-create "$mountpoint"
	unmount_image sync-create
	detach_image
	check_superblock_clean sync-create
	check_image sync-create
	sequence_before=$(journal_sequence)

	attach_image
	mount_image ""
	run_step osync-update "$mountpoint"
	sequence_live=$(journal_sequence)
	[ "$sequence_live" != "$sequence_before" ] ||
	    fail "O_SYNC returned before committing the inode"
	unmount_image osync-update
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_live" ] ||
	    fail "O_SYNC left a transaction for unmount"
	check_superblock_clean osync-update
	check_image osync-update

	before=$(sha256 -q "$image")
	attach_image
	mount_image ro
	run_step osync-verify "$mountpoint"
	unmount_image osync-verify
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "O_SYNC verification changed the image"
	sequence_before=$sequence_after

	attach_image
	mount_image sync
	run_step mount-sync-update "$mountpoint"
	sequence_live=$(journal_sequence)
	[ "$sequence_live" != "$sequence_before" ] ||
	    fail "synchronous mount returned before commit"
	unmount_image mount-sync-update
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_live" ] ||
	    fail "synchronous mount left a transaction for unmount"
	check_superblock_clean mount-sync-update
	check_image mount-sync-update
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step osync-clean "$mountpoint"
	sequence_live=$(journal_sequence)
	[ "$sequence_live" = "$sequence_before" ] ||
	    fail "empty O_SYNC write created a transaction"
	unmount_image osync-clean
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "clean synchronous cycle advanced the journal"
	check_superblock_clean osync-clean
	check_image osync-clean

	before=$(sha256 -q "$image")
	attach_image
	mount_image ro
	run_step sync-verify "$mountpoint"
	unmount_image sync-verify
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "synchronous-mount verification changed the image"
	echo " ok"
}

run_vfs_sync_case()
{
	block_size=$1
	case_dir=$work/vfs-sync-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="VFS sync ($block_size byte blocks)"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O 'metadata_csum,^orphan_file' "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi

	attach_image
	mount_image ""
	run_step sync-create "$mountpoint"
	unmount_image vfs-sync-create
	detach_image
	check_superblock_clean vfs-sync-create
	check_image vfs-sync-create
	sequence_before=$(journal_sequence)
	[ "$(journal_start)" = 00000000 ] ||
	    fail "initial journal was not empty"

	attach_image
	mount_image ""
	run_step vfs-sync-update "$mountpoint"
	sequence_live=$(journal_sequence)
	[ "$sequence_live" != "$sequence_before" ] ||
	    fail "sync returned before committing the transaction"
	[ "$(journal_start)" = 00000000 ] ||
	    fail "sync returned before checkpointing the transaction"
	unmount_image vfs-sync-update
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_live" ] ||
	    fail "sync left a transaction for unmount"
	[ "$(journal_start)" = 00000000 ] ||
	    fail "journal was not empty after unmount"
	check_superblock_clean vfs-sync-update
	check_image vfs-sync-update
	sequence_before=$sequence_after

	attach_image
	mount_image ""
	run_step vfs-sync-clean "$mountpoint"
	sequence_live=$(journal_sequence)
	[ "$sequence_live" = "$sequence_before" ] ||
	    fail "clean sync created a journal transaction"
	[ "$(journal_start)" = 00000000 ] ||
	    fail "clean sync left a live transaction"
	unmount_image vfs-sync-clean
	detach_image
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "clean sync left a transaction for unmount"
	check_superblock_clean vfs-sync-clean
	check_image vfs-sync-clean

	before=$(sha256 -q "$image")
	attach_image
	mount_image ro
	run_step vfs-sync-verify "$mountpoint"
	unmount_image vfs-sync-verify
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] ||
	    fail "read-only verification changed the image"
	echo " ok"
}

superblock_recover_set()
{
	feature=$(dd if="$image" bs=1 skip=$((1024 + 96)) count=1 \
	    status=none | hexdump -ve '1/1 "%u"')
	case "$feature" in
	*[!0-9]*|'') fail "could not read incompat features" ;;
	esac
	[ $((feature & 4)) -ne 0 ]
}

superblock_valid_set()
{
	state=$(dd if="$image" bs=1 skip=$((1024 + 58)) count=1 \
	    status=none | hexdump -ve '1/1 "%u"')
	case "$state" in
	*[!0-9]*|'') fail "could not read filesystem state" ;;
	esac
	[ $((state & 1)) -ne 0 ]
}

run_remount_case()
{
	block_size=$1
	orphan_format=$2
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
	case_dir=$work/remount-$orphan_format-$block_size
	image=$case_dir/ext4.img
	mkdir "$case_dir"
	test_name="remount $orphan_format ($block_size byte blocks)"
	print_test_name "$test_name"

	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O "$features" "$image" \
	    >"$case_dir/mke2fs.log" 2>&1; then
		cat "$case_dir/mke2fs.log" >&2
		fail "mke2fs failed"
	fi

	attach_image
	mount_image ""
	run_step remount-create "$mountpoint"
	start_held_remount_orphan
	reject_readonly_remount active-unlinked-inode
	superblock_recover_set ||
	    fail "rejected remount cleared RECOVER"
	if superblock_valid_set; then
		fail "rejected remount marked the filesystem valid"
	fi
	release_held_remount_orphan
	sequence_before=$(journal_sequence)
	run_step remount-dirty "$mountpoint"
	remount_image ro dirty-to-read-only
	sequence_after=$(journal_sequence)
	[ "$sequence_after" != "$sequence_before" ] ||
	    fail "read-only remount did not commit the dirty inode"
	[ "$(journal_start)" = 00000000 ] ||
	    fail "read-only remount did not empty the journal"
	if superblock_recover_set; then
		fail "read-only remount retained RECOVER"
	fi
	superblock_valid_set ||
	    fail "read-only remount did not mark the filesystem valid"
	run_step remount-ro-dirty "$mountpoint"

	remount_image rw read-write
	superblock_recover_set ||
	    fail "read-write remount did not set RECOVER"
	if superblock_valid_set; then
		fail "read-write remount left the filesystem valid"
	fi
	run_step remount-sync "$mountpoint"
	sequence_before=$(journal_sequence)
	remount_image ro final-read-only
	sequence_after=$(journal_sequence)
	[ "$sequence_after" = "$sequence_before" ] ||
	    fail "clean read-only remount created a transaction"
	[ "$(journal_start)" = 00000000 ] ||
	    fail "final read-only remount left a live journal"
	if superblock_recover_set; then
		fail "final read-only remount retained RECOVER"
	fi
	superblock_valid_set ||
	    fail "final read-only remount did not mark a valid filesystem"
	run_step remount-ro-final "$mountpoint"
	unmount_image remount-transitions
	detach_image
	check_superblock_clean remount-transitions
	check_image remount-transitions
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
counters)
	for block_size in $EXT4FS_BLOCK_SIZES; do
		case "$block_size" in
		1024|2048|4096) ;;
		*)
			echo "bad block size: $block_size" >&2
			exit 1
			;;
		esac
		run_counter_case "$block_size"
	done
	case " $EXT4FS_BLOCK_SIZES " in
	*' 1024 '*)
		prepare_counter_corrupt_base
		for kind in super-blocks super-inodes group-blocks \
		    group-inodes group-dirs; do
			run_counter_mount_reject_case "$kind"
		done
		for kind in blocks inodes; do
			run_counter_local_reject_case "$kind"
		done
		;;
	esac
	exit 0
	;;
superblock)
	for block_size in $EXT4FS_BLOCK_SIZES; do
		case "$block_size" in
		1024|2048|4096) ;;
		*)
			echo "bad block size: $block_size" >&2
			exit 1
			;;
		esac
		run_superblock_case "$block_size"
	done
	case " $EXT4FS_BLOCK_SIZES " in
	*' 1024 '*)
		for kind in checksum dirty; do
			run_superblock_reject_case "$kind"
		done
		;;
	esac
	exit 0
	;;
fsync)
	for block_size in $EXT4FS_BLOCK_SIZES; do
		case "$block_size" in
		1024|2048|4096) ;;
		*)
			echo "bad block size: $block_size" >&2
			exit 1
			;;
		esac
		run_fsync_case "$block_size"
	done
	exit 0
	;;
sync)
	for block_size in $EXT4FS_BLOCK_SIZES; do
		case "$block_size" in
		1024|2048|4096) ;;
		*)
			echo "bad block size: $block_size" >&2
			exit 1
			;;
		esac
		run_sync_case "$block_size"
	done
	exit 0
	;;
vfs-sync)
	for block_size in $EXT4FS_BLOCK_SIZES; do
		case "$block_size" in
		1024|2048|4096) ;;
		*)
			echo "bad block size: $block_size" >&2
			exit 1
			;;
		esac
		run_vfs_sync_case "$block_size"
	done
	exit 0
	;;
remount)
	for block_size in $EXT4FS_BLOCK_SIZES; do
		case "$block_size" in
		1024|2048|4096) ;;
		*)
			echo "bad block size: $block_size" >&2
			exit 1
			;;
		esac
		run_remount_case "$block_size" classic
	done
	run_remount_case 1024 orphan-file
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
