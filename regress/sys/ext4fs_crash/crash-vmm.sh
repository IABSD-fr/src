#!/bin/sh
#
# Exercise ext4fs crash recovery through abruptly stopped vmd guests.
# This target is explicit and must never run during an ordinary regress.

set -eu

MKE2FS=${MKE2FS:-mke2fs}
DEBUGFS=${DEBUGFS:-debugfs}
E2FSCK=${E2FSCK:-e2fsck}
DUMPE2FS=${DUMPE2FS:-dumpe2fs}
VMCTL=${VMCTL:-vmctl}
SSH=${SSH:-ssh}
SCP=${SCP:-scp}
SYSCTL=${SYSCTL:-sysctl}
TIMEOUT=${TIMEOUT:-timeout}
SHA256=${SHA256:-sha256}
SYNC=${SYNC:-sync}
EXT4FS_CRASH=${EXT4FS_CRASH:-./ext4fs_crash}
EXT4FS_CRASH_VMM_BASE=${EXT4FS_CRASH_VMM_BASE:-\
/home/vm/ext4fs-base.qcow2}
EXT4FS_CRASH_VMM_KERNEL=${EXT4FS_CRASH_VMM_KERNEL:-/bsd}
EXT4FS_CRASH_VMM_DIR=${EXT4FS_CRASH_VMM_DIR:-/home/vm}
EXT4FS_CRASH_BOOT_TIMEOUT=${EXT4FS_CRASH_BOOT_TIMEOUT:-120}
EXT4FS_CRASH_STEP_TIMEOUT=${EXT4FS_CRASH_STEP_TIMEOUT:-120}
EXT4FS_CRASH_IMAGE_MB=${EXT4FS_CRASH_IMAGE_MB:-128}
EXT4FS_CRASH_BLOCK_SIZES=${EXT4FS_CRASH_BLOCK_SIZES:-1024}
EXT4FS_CRASH_STAGES=${EXT4FS_CRASH_STAGES:-"write fsync rename"}
EXT4FS_CRASH_CUT_DELAY=${EXT4FS_CRASH_CUT_DELAY:-2.01}
EXT4FS_CRASH_MODE=${EXT4FS_CRASH_MODE:-timed}
EXT4FS_CRASH_FLUSH_COUNTS=${EXT4FS_CRASH_FLUSH_COUNTS:-"1 2 3 4 5 6"}
EXT4FS_CRASH_SSH_KEY=${EXT4FS_CRASH_SSH_KEY:-}

JBD2_OFF_MAXLEN=16
JBD2_OFF_FIRST=20
JBD2_OFF_START=28
JBD2_OFF_FEATURE_COMPAT=36
JBD2_OFF_FEATURE_INCOMPAT=40
JBD2_OFF_FEATURE_RO_COMPAT=44
JBD2_OFF_MAX_TRANSACTION=72
JBD2_OFF_HEAD=88

test_name=ext4fs-crash-vmm
fail()
{
	echo "FAILED: $test_name: $*" >&2
	exit 1
}

[ "$(id -u)" -eq 0 ] || fail "test must run as root"

tools="$MKE2FS $DEBUGFS $E2FSCK $DUMPE2FS $VMCTL $SSH $SCP \
$SYSCTL $TIMEOUT $SHA256 $SYNC $EXT4FS_CRASH awk cmp cp dd grep \
cat id kill mkdir mktemp mv od rm sleep"
for tool in $tools; do
	command -v "$tool" >/dev/null 2>&1 ||
	    fail "required tool not found: $tool"
done

[ -r "$EXT4FS_CRASH_VMM_BASE" ] ||
    fail "base image is not readable"
[ -r "$EXT4FS_CRASH_VMM_KERNEL" ] ||
    fail "kernel is not readable"
[ -d "$EXT4FS_CRASH_VMM_DIR" ] ||
    fail "vmm directory does not exist"
case "$EXT4FS_CRASH_VMM_DIR" in
/*) ;;
*)	fail "vmm directory must be an absolute path" ;;
esac
case "$EXT4FS_CRASH_BOOT_TIMEOUT:$EXT4FS_CRASH_STEP_TIMEOUT:\
$EXT4FS_CRASH_IMAGE_MB" in
*[!0-9:]*)
	fail "timeouts and image size must be unsigned integers"
	;;
esac
[ "$EXT4FS_CRASH_BOOT_TIMEOUT" -gt 0 ] ||
    fail "boot timeout must be positive"
[ "$EXT4FS_CRASH_BOOT_TIMEOUT" -le 600 ] ||
    fail "boot timeout must not exceed 600"
[ "$EXT4FS_CRASH_STEP_TIMEOUT" -gt 0 ] ||
    fail "step timeout must be positive"
[ "$EXT4FS_CRASH_STEP_TIMEOUT" -le 600 ] ||
    fail "step timeout must not exceed 600"
[ "$EXT4FS_CRASH_IMAGE_MB" -ge 128 ] ||
    fail "image size must be at least 128 MiB"
[ "$EXT4FS_CRASH_IMAGE_MB" -le 4096 ] ||
    fail "image size must not exceed 4096 MiB"
case "$EXT4FS_CRASH_CUT_DELAY" in
''|*[!0-9.]*|*.*.*|.*|*.)
	fail "cut delay must be a non-negative decimal"
	;;
esac
if [ -n "$EXT4FS_CRASH_SSH_KEY" ]; then
	[ -r "$EXT4FS_CRASH_SSH_KEY" ] ||
	    fail "SSH identity is not readable"
fi
[ -n "$EXT4FS_CRASH_BLOCK_SIZES" ] ||
    fail "block-size list must not be empty"
for block_size in $EXT4FS_CRASH_BLOCK_SIZES; do
	case "$block_size" in
	1024|2048|4096) ;;
	*)	fail "unsupported block size: $block_size" ;;
	esac
done
[ -n "$EXT4FS_CRASH_STAGES" ] ||
    fail "stage list must not be empty"
for stage in $EXT4FS_CRASH_STAGES; do
	case "$stage" in
	write|fsync|rename) ;;
	*)	fail "unsupported crash stage: $stage" ;;
	esac
done
case "$EXT4FS_CRASH_MODE" in
timed|flush|orphan|directory|extent|truncate|reuse|journal) ;;
*)	fail "unsupported crash mode: $EXT4FS_CRASH_MODE" ;;
esac
if [ "$EXT4FS_CRASH_MODE" != timed ]; then
	[ -n "$EXT4FS_CRASH_FLUSH_COUNTS" ] ||
	    fail "flush-count list must not be empty"
	for flush_count in $EXT4FS_CRASH_FLUSH_COUNTS; do
		case "$flush_count" in
		''|*[!0-9]*|0)	fail "invalid flush count: $flush_count" ;;
		esac
		[ "$flush_count" -le 6 ] ||
		    fail "flush count exceeds transaction"
	done
fi

work=$(mktemp -d "$EXT4FS_CRASH_VMM_DIR/\
ext4fs_crash.XXXXXXXX")
case "$work" in
"$EXT4FS_CRASH_VMM_DIR"/ext4fs_crash.*) ;;
*)	fail "unsafe temporary directory: $work" ;;
esac

vm_started=0
vm_name=
guest=
ssh_pid=
control_pid=
serial=0

cleanup()
{
	rc=$?
	trap - EXIT HUP INT TERM
	if [ -n "$ssh_pid" ]; then
		kill "$ssh_pid" >/dev/null 2>&1 || :
		wait "$ssh_pid" >/dev/null 2>&1 || :
		ssh_pid=
	fi
	if [ -n "$control_pid" ]; then
		kill "$control_pid" >/dev/null 2>&1 || :
		wait "$control_pid" >/dev/null 2>&1 || :
		control_pid=
	fi
	if [ "$vm_started" -eq 1 ]; then
		"$TIMEOUT" -k 2 10 "$VMCTL" stop -fw "$vm_name" \
		    >/dev/null 2>&1 || :
		vm_started=0
	fi
	if [ "${KEEP_TMP:-0}" = 1 ] || [ "$rc" -ne 0 ]; then
		echo "temporary files retained in $work"
	else
		case "$work" in
		"$EXT4FS_CRASH_VMM_DIR"/ext4fs_crash.*)
			rm -rf "$work"
			;;
		*)	fail "refusing to remove unsafe path: $work" ;;
		esac
	fi
	exit "$rc"
}
trap cleanup EXIT HUP INT TERM

ssh_run()
{
	if [ -n "$EXT4FS_CRASH_SSH_KEY" ]; then
		"$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null \
		    -o LogLevel=ERROR \
		    -i "$EXT4FS_CRASH_SSH_KEY" "$@"
	else
		"$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null \
		    -o LogLevel=ERROR "$@"
	fi
}

ssh_step()
{
	if [ -n "$EXT4FS_CRASH_SSH_KEY" ]; then
		"$TIMEOUT" -k 5 "$EXT4FS_CRASH_STEP_TIMEOUT" \
		    "$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null \
		    -o LogLevel=ERROR \
		    -i "$EXT4FS_CRASH_SSH_KEY" "$@"
	else
		"$TIMEOUT" -k 5 "$EXT4FS_CRASH_STEP_TIMEOUT" \
		    "$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null \
		    -o LogLevel=ERROR "$@"
	fi
}

ssh_workload()
{
	if [ -n "$EXT4FS_CRASH_SSH_KEY" ]; then
		exec "$TIMEOUT" -k 5 "$EXT4FS_CRASH_STEP_TIMEOUT" \
		    "$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null \
		    -o LogLevel=ERROR \
		    -i "$EXT4FS_CRASH_SSH_KEY" "$@"
	else
		exec "$TIMEOUT" -k 5 "$EXT4FS_CRASH_STEP_TIMEOUT" \
		    "$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null \
		    -o LogLevel=ERROR "$@"
	fi
}

flush_stop_control()
{
	exec "$TIMEOUT" -k 5 "$EXT4FS_CRASH_STEP_TIMEOUT" \
	    "$VMCTL" flush-stop "$@"
}

copy_helper()
{
	if [ -n "$EXT4FS_CRASH_SSH_KEY" ]; then
		"$SCP" -q -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null \
		    -o LogLevel=ERROR \
		    -i "$EXT4FS_CRASH_SSH_KEY" \
		    "$EXT4FS_CRASH" "root@$guest:/tmp/ext4fs_crash"
	else
		"$SCP" -q -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null \
		    -o LogLevel=ERROR "$EXT4FS_CRASH" \
		    "root@$guest:/tmp/ext4fs_crash"
	fi
}

start_guest()
{
	image=$1
	serial=$((serial + 1))
	overlay=$work/root-$serial.qcow2
	vm_name=ext4fs-crash-$$-$serial
	"$VMCTL" create -b "$EXT4FS_CRASH_VMM_BASE" "$overlay" \
	    >"$work/create-$serial.log" 2>&1 ||
	    fail "could not create guest overlay"
	"$VMCTL" start -m 1G -b "$EXT4FS_CRASH_VMM_KERNEL" \
	    -d "qcow2:$overlay" -d "raw:$image" -L "$vm_name" \
	    >"$work/start-$serial.log" 2>&1 ||
	    fail "could not start guest"
	vm_started=1
	vm_id=$("$VMCTL" status "$vm_name" |
	    awk -v name="$vm_name" \
	    'NR > 1 && $NF == name { print $1; exit }')
	case "$vm_id" in
	''|*[!0-9]*)	fail "could not determine guest id" ;;
	esac
	[ "$vm_id" -gt 0 ] || fail "guest id is not positive"
	[ "$vm_id" -le 255 ] ||
	    fail "guest id is outside local IPv4 range"
	guest=100.64.$vm_id.3
	ready=0
	elapsed=0
	while [ "$elapsed" -lt "$EXT4FS_CRASH_BOOT_TIMEOUT" ]; do
		if ssh_run "root@$guest" true >/dev/null 2>&1; then
			ready=1
			break
		fi
		sleep 1
		elapsed=$((elapsed + 1))
	done
	[ "$ready" -eq 1 ] || fail "guest SSH did not become ready"
	ssh_run "root@$guest" sysctl -n kern.version \
	    >"$work/guest-version-$serial.log" 2>&1 ||
	    fail "could not query guest kernel version"
	cmp -s "$work/host-version.log" \
	    "$work/guest-version-$serial.log" ||
	    fail "guest did not boot the running production kernel"
	copy_helper || fail "could not copy workload helper"
}

stop_guest()
{
	stop_log=$work/stop-$serial.log
	if ! "$TIMEOUT" -k 2 10 "$VMCTL" stop -fw "$vm_name" \
	    >"$stop_log" 2>&1; then
		cat "$stop_log" >&2
		fail "could not force-stop guest"
	fi
	vm_started=0
}

run_crash()
{
	image=$1
	stage=$2
	log=$3
	trigger="set -e
mkdir -p /mnt/ext4
mount_ext4fs /dev/sd1c /mnt/ext4
exec /tmp/ext4fs_crash workload $stage /mnt/ext4/crash"
	ssh_workload "root@$guest" "$trigger" >"$log" 2>&1 &
	ssh_pid=$!
	wait_ticks=0
	max_ticks=$((EXT4FS_CRASH_STEP_TIMEOUT * 10))
	while ! grep -q "^READY $stage\$" "$log" 2>/dev/null; do
		if ! kill -0 "$ssh_pid" 2>/dev/null; then
			wait "$ssh_pid" || status=$?
			ssh_pid=
			cat "$log" >&2
			fail "workload exited before its crash marker"
		fi
		[ "$wait_ticks" -lt "$max_ticks" ] ||
		    fail "workload marker timed out"
		sleep 0.1
		wait_ticks=$((wait_ticks + 1))
	done
	sleep "$EXT4FS_CRASH_CUT_DELAY"
	stop_guest
	kill "$ssh_pid" >/dev/null 2>&1 || :
	wait "$ssh_pid" >/dev/null 2>&1 || :
	ssh_pid=
	"$SYNC"
}

run_flush_crash()
{
	image=$1
	stage=$2
	flush_count=$3
	log=$4
	control_log=$5
	trigger="set -e
mkdir -p /mnt/ext4
mount_ext4fs /dev/sd1c /mnt/ext4
exec /tmp/ext4fs_crash boundary $stage /mnt/ext4/crash"
	ssh_workload "root@$guest" "$trigger" >"$log" 2>&1 &
	ssh_pid=$!
	wait_ticks=0
	max_ticks=$((EXT4FS_CRASH_STEP_TIMEOUT * 10))
	while ! grep -q "^READY $stage\$" "$log" 2>/dev/null; do
		if ! kill -0 "$ssh_pid" 2>/dev/null; then
			wait "$ssh_pid" || status=$?
			ssh_pid=
			cat "$log" >&2
			fail "workload exited before its boundary marker"
		fi
		[ "$wait_ticks" -lt "$max_ticks" ] ||
		    fail "workload boundary marker timed out"
		sleep 0.1
		wait_ticks=$((wait_ticks + 1))
	done
	flush_stop_control "$vm_name" 1 "$flush_count" \
	    >"$control_log" 2>&1 &
	control_pid=$!
	wait_ticks=0
	while ! grep -q 'armed vm .* disk 1 at flush' \
	    "$control_log" 2>/dev/null; do
		if ! kill -0 "$control_pid" 2>/dev/null; then
			wait "$control_pid" || status=$?
			control_pid=
			cat "$control_log" >&2
			fail "flush-stop control exited before arming"
		fi
		[ "$wait_ticks" -lt "$max_ticks" ] ||
		    fail "flush-stop arm timed out"
		sleep 0.1
		wait_ticks=$((wait_ticks + 1))
	done
	ssh_step "root@$guest" touch /tmp/ext4fs_crash.go ||
	    fail "could not release boundary workload"
	if ! wait "$control_pid"; then
		control_pid=
		cat "$control_log" >&2
		fail "flush-stop control failed"
	fi
	control_pid=
	grep -q 'paused vm .* at disk 1 flush' "$control_log" || {
		cat "$control_log" >&2
		fail "flush-stop did not report a paused VM"
	}
	stop_guest
	kill "$ssh_pid" >/dev/null 2>&1 || :
	wait "$ssh_pid" >/dev/null 2>&1 || :
	ssh_pid=
	"$SYNC"
}

validate_guest()
{
	image=$1
	options=$2
	verifier=$3
	log=$4
	start_guest "$image"
	if [ -n "$options" ]; then
		mount_command="mount_ext4fs -o $options \
/dev/sd1c /mnt/ext4"
	else
		mount_command="mount_ext4fs /dev/sd1c /mnt/ext4"
	fi
	verify="set -e
mkdir -p /mnt/ext4
$mount_command
/tmp/ext4fs_crash $verifier /mnt/ext4/crash
umount /mnt/ext4"
	if ! ssh_step "root@$guest" "$verify" >"$log" 2>&1; then
		cat "$log" >&2
		fail "recovery verification failed"
	fi
	grep -q '^state=' "$log" ||
	    fail "verification did not report recovered state"
	stop_guest
	"$SYNC"
}

preserve_durable()
{
	image=$1
	case_dir=$2
	durable=$case_dir/durable.img
	mv "$image" "$durable"
	durable_hash=$($SHA256 -q "$durable")
	"$DUMPE2FS" -h "$durable" \
	    >"$case_dir/durable-super.log" 2>&1 ||
	    fail "dumpe2fs rejected durable image"
	grep -q '^Filesystem features:.*needs_recovery' \
	    "$case_dir/durable-super.log" ||
	    fail "durable image does not require recovery"
	journal=$case_dir/journal.bin
	"$DEBUGFS" -R "dump <8> $journal" "$durable" \
	    >"$case_dir/durable-journal-dump.log" 2>&1 ||
	    fail "could not extract durable journal"
	"$EXT4FS_CRASH" journal-state "$journal" \
	    >"$case_dir/durable-journal.log" 2>&1 || {
		cat "$case_dir/durable-journal.log" >&2
		fail "could not classify durable journal"
	}
	durable_journal_state=$(awk -F'[ =]' \
	    '/^journal=/ { print $2; exit }' \
	    "$case_dir/durable-journal.log")
	case "$durable_journal_state" in
	clean|recover) ;;
	*)	fail "durable journal state was not reported" ;;
	esac
	[ "$durable_hash" = "$($SHA256 -q "$durable")" ] ||
	    fail "durable-state inspection modified image"
	cp "$durable" "$image"
}

recover_case()
{
	image=$1
	case_dir=$2
	verifier=$3
	validate_guest "$image" '' "$verifier" \
	    "$case_dir/recovery.log"
	hash_before=$($SHA256 -q "$image")
	validate_guest "$image" ro "$verifier" \
	    "$case_dir/idempotence.log"
	hash_after=$($SHA256 -q "$image")
	[ "$hash_before" = "$hash_after" ] ||
	    fail "second read-only mount modified the image"
	cmp -s "$case_dir/recovery.log" \
	    "$case_dir/idempotence.log" ||
	    fail "second mount changed the recovered state"
	"$E2FSCK" -fn "$image" >"$case_dir/e2fsck.log" 2>&1 ||
	    fail "e2fsck rejected the recovered image"
	"$DUMPE2FS" -h "$image" >"$case_dir/dumpe2fs.log" 2>&1 ||
	    fail "dumpe2fs rejected the recovered image"
	if grep -q '^Filesystem features:.*needs_recovery' \
	    "$case_dir/dumpe2fs.log"; then
		fail "recovered image retained RECOVER"
	fi
	grep -q '^Filesystem state:.*clean' \
	    "$case_dir/dumpe2fs.log" ||
	    fail "recovered image is not clean"
}

extent_leaf_count()
{
	image=$1
	extent_path=$2
	output=$3
	if ! "$DEBUGFS" -R "stat $extent_path" "$image" \
	    >"$output" 2>&1; then
		cat "$output" >&2
		fail "could not inspect recovered extent tree"
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
	image=$1
	extent_path=$2
	expected=$3
	output=$4
	actual=$(extent_leaf_count "$image" "$extent_path" "$output")
	case "$actual" in
	''|*[!0-9]*)
		fail "could not count recovered extent leaves"
		;;
	esac
	[ "$actual" -eq "$expected" ] ||
	    fail "expected $expected extent leaves, got $actual"
}

journal_revoke_records()
{
	journal=$1
	output=$2
	if ! "$EXT4FS_CRASH" journal-revokes "$journal" \
	    >"$output" 2>&1; then
		cat "$output" >&2
		fail "could not inspect journal revokes"
	fi
	records=$(awk -F'[ =]' \
	    '/^revokes=/ { print $2; exit }' "$output")
	case "$records" in
	''|*[!0-9]*) fail "journal revoke count was not reported" ;;
	esac
	echo "$records"
}

filesystem_free_blocks()
{
	output=$1
	blocks=$(awk -F: '
	    $1 == "Free blocks" {
		gsub(/[[:space:]]/, "", $2)
		print $2
		exit
	    }
	' "$output")
	case "$blocks" in
	''|*[!0-9]*) fail "filesystem free-block count was not reported" ;;
	esac
	echo "$blocks"
}

filesystem_free_inodes()
{
	output=$1
	inodes=$(awk -F: '
	    $1 == "Free inodes" {
		gsub(/[[:space:]]/, "", $2)
		print $2
		exit
	    }
	' "$output")
	case "$inodes" in
	''|*[!0-9]*) fail "filesystem free-inode count was not reported" ;;
	esac
	echo "$inodes"
}

filesystem_block_map()
{
	image=$1
	block_path=$2
	logical=$3
	output=$4
	if ! mapped_block=$("$DEBUGFS" \
	    -R "bmap $block_path $logical" "$image" 2>"$output"); then
		cat "$output" >&2
		fail "could not map $block_path logical block $logical"
	fi
	mapped_block=$(echo "$mapped_block" |
	    awk 'NF { value = $NF } END { print value }')
	case "$mapped_block" in
	''|*[!0-9]*)
		cat "$output" >&2
		fail "invalid physical block for $block_path"
		;;
	esac
	echo "$mapped_block"
}

journal_value()
{
	journal_log=$1
	journal_name=$2
	value=$(awk -v name="$journal_name" '
	    {
		for (i = 1; i <= NF; i++) {
			split($i, field, "=")
			if (field[1] == name) {
				print field[2]
				exit
			}
		}
	    }
	' "$journal_log")
	case "$value" in
	''|*[!0-9]*)
		fail "journal $journal_name was not reported"
		;;
	esac
	echo "$value"
}

journal_superblock_offset()
{
	journal_image=$1
	journal_block_size=$2
	journal_output=$3
	journal_block=$("$DEBUGFS" -R 'bmap <8> 0' \
	    "$journal_image" 2>"$journal_output" |
	    awk 'NF { value = $NF } END { print value }')
	case "$journal_block" in
	''|*[!0-9]*)
		cat "$journal_output" >&2
		fail "could not map journal superblock"
		;;
	esac
	echo $((journal_block * journal_block_size))
}

read_be32()
{
	be_image=$1
	be_offset=$2
	be_value=$(dd if="$be_image" bs=1 skip="$be_offset" count=4 \
	    status=none | od -An -tu1 | awk '
	    NF == 4 {
		value = $1 * 16777216 + $2 * 65536 + $3 * 256 + $4
		printf "%.0f\n", value
		exit
	    }
	')
	case "$be_value" in
	''|*[!0-9]*) fail "could not read journal field" ;;
	esac
	echo "$be_value"
}

write_be32()
{
	be_image=$1
	be_offset=$2
	be_value=$3
	case "$be_value" in
	''|*[!0-9]*) fail "invalid journal field value" ;;
	esac
	be_byte0=$(((be_value >> 24) & 255))
	be_byte1=$(((be_value >> 16) & 255))
	be_byte2=$(((be_value >> 8) & 255))
	be_byte3=$((be_value & 255))
	{
		printf "\\$(printf '%03o' "$be_byte0")"
		printf "\\$(printf '%03o' "$be_byte1")"
		printf "\\$(printf '%03o' "$be_byte2")"
		printf "\\$(printf '%03o' "$be_byte3")"
	} | dd of="$be_image" bs=1 seek="$be_offset" count=4 \
	    conv=notrunc status=none
}

tune_journal_fixture()
{
	tune_image=$1
	tune_block_size=$2
	tune_mode=$3
	tune_output=$4
	tune_offset=$(journal_superblock_offset "$tune_image" \
	    "$tune_block_size" "$tune_output")
	tune_start=$(read_be32 "$tune_image" \
	    $((tune_offset + JBD2_OFF_START)))
	tune_compat=$(read_be32 "$tune_image" \
	    $((tune_offset + JBD2_OFF_FEATURE_COMPAT)))
	tune_incompat=$(read_be32 "$tune_image" \
	    $((tune_offset + JBD2_OFF_FEATURE_INCOMPAT)))
	tune_ro_compat=$(read_be32 "$tune_image" \
	    $((tune_offset + JBD2_OFF_FEATURE_RO_COMPAT)))
	[ "$tune_start" -eq 0 ] ||
	    fail "fixture journal is not clean"
	[ "$tune_compat" -eq 0 ] &&
	    [ "$tune_incompat" -eq 0 ] &&
	    [ "$tune_ro_compat" -eq 0 ] ||
	    fail "fixture journal has checksum features"
	case "$tune_mode" in
	exhaustion)
		# Raw limit 47 admits 23 credits; mkdir reserves 24.
		write_be32 "$tune_image" \
		    $((tune_offset + JBD2_OFF_MAX_TRANSACTION)) 47
		;;
	wrap)
		tune_maxlen=$(read_be32 "$tune_image" \
		    $((tune_offset + JBD2_OFF_MAXLEN)))
		tune_first=$(read_be32 "$tune_image" \
		    $((tune_offset + JBD2_OFF_FIRST)))
		[ "$tune_maxlen" -gt $((tune_first + 2)) ] ||
		    fail "fixture journal is too short to wrap"
		tune_head=$((tune_maxlen - 2))
		write_be32 "$tune_image" \
		    $((tune_offset + JBD2_OFF_HEAD)) \
		    "$tune_head"
		;;
	*)	fail "unknown journal fixture mode: $tune_mode" ;;
	esac
	if ! "$E2FSCK" -fn "$tune_image" >"$tune_output" 2>&1; then
		cat "$tune_output" >&2
		fail "e2fsck rejected $tune_mode journal fixture"
	fi
	tune_journal=$tune_output.journal
	tune_state=$tune_output.state
	"$DEBUGFS" -R "dump <8> $tune_journal" "$tune_image" \
	    >"$tune_output.dump" 2>&1 ||
	    fail "could not extract $tune_mode journal fixture"
	"$EXT4FS_CRASH" journal-state "$tune_journal" \
	    >"$tune_state" 2>&1 ||
	    fail "could not inspect $tune_mode journal fixture"
	grep -q '^journal=clean ' "$tune_state" ||
	    fail "$tune_mode journal fixture is not clean"
	if [ "$tune_mode" = exhaustion ]; then
		[ "$(journal_value "$tune_state" maxtrans)" -eq 47 ] ||
		    fail "journal transaction limit was not installed"
	else
		tune_head=$(journal_value "$tune_state" head)
		tune_maxlen=$(journal_value "$tune_state" maxlen)
		tune_maxtrans=$(journal_value "$tune_state" maxtrans)
		[ "$tune_head" -eq $((tune_maxlen - 2)) ] ||
		    fail "journal wrap head was not installed"
		[ "$tune_maxtrans" -eq 0 ] ||
		    fail "journal wrap fixture has a transaction limit"
	fi
}

run_flush_matrix()
{
	base=$1
	block_size=$2
	stage=$3
	label=$4
	verifier=$5
	before_state=$6
	after_state=$7
	before_leaves=${8:-}
	after_leaves=${9:-}
	matrix_extent_path=
	matrix_block_path=
	case "$stage" in
	extent-*) matrix_extent_path=/crash/extent-probe ;;
	truncate-*) matrix_extent_path=/crash/truncate-probe ;;
	block-reuse) matrix_block_path=/crash/reuse-probe ;;
	esac
	base_free_blocks=
	base_reuse_block=
	truncate_freed_blocks=
	if [ "$stage" = truncate-prune ]; then
		base_super=$work/base-super-$block_size.log
		"$DUMPE2FS" -h "$base" >"$base_super" 2>&1 ||
		    fail "could not inspect truncate baseline"
		base_free_blocks=$(filesystem_free_blocks "$base_super")
		truncate_capacity=$(((block_size - 12) / 12))
		truncate_freed_blocks=$((truncate_capacity / 2 + 2))
		base_journal=$work/base-journal-$block_size.bin
		"$DEBUGFS" -R "dump <8> $base_journal" "$base" \
		    >"$work/base-journal-$block_size.log" 2>&1 ||
		    fail "could not extract truncate baseline journal"
		base_revokes=$(journal_revoke_records "$base_journal" \
		    "$work/base-revokes-$block_size.log")
		[ "$base_revokes" -eq 0 ] ||
		    fail "truncate baseline contains stale revokes"
		grep -q ' incompat=1$' \
		    "$work/base-revokes-$block_size.log" ||
		    fail "truncate baseline lacks REVOKE feature"
	elif [ "$stage" = block-reuse ]; then
		base_super=$work/base-reuse-super-$block_size.log
		"$DUMPE2FS" -h "$base" >"$base_super" 2>&1 ||
		    fail "could not inspect block-reuse baseline"
		base_free_blocks=$(filesystem_free_blocks "$base_super")
		base_reuse_block=$(filesystem_block_map "$base" \
		    "$matrix_block_path" 0 \
		    "$work/base-reuse-map-$block_size.log")
		[ "$base_reuse_block" -gt 0 ] ||
		    fail "block-reuse source has no physical block"
	fi

	for flush_count in $EXT4FS_CRASH_FLUSH_COUNTS; do
		test_name="$label flush $flush_count, "
		test_name="$test_name$block_size-byte blocks"
		printf '%-64s' "vmm: $test_name"
		case_dir=$work/$stage-flush-$flush_count-$block_size
		mkdir "$case_dir"
		image=$case_dir/ext4.img
		cp "$base" "$image"

		start_guest "$image"
		run_flush_crash "$image" "$stage" "$flush_count" \
		    "$case_dir/workload.log" \
		    "$case_dir/flush-stop.log"
		preserve_durable "$image" "$case_dir"
		if [ "$stage" = rename-wrap ]; then
			wrap_start=$(journal_value \
			    "$case_dir/durable-journal.log" start)
			wrap_head=$(journal_value \
			    "$case_dir/durable-journal.log" head)
			wrap_first=$(journal_value \
			    "$case_dir/durable-journal.log" first)
			wrap_maxlen=$(journal_value \
			    "$case_dir/durable-journal.log" maxlen)
			wrap_boundary=$((wrap_maxlen - 2))
			case "$flush_count" in
			1|2|6) expected_start=0 ;;
			3|4|5) expected_start=$wrap_boundary ;;
			esac
			[ "$wrap_start" -eq "$expected_start" ] ||
			    fail "journal wrap start mismatch"
			if [ "$flush_count" -lt 6 ]; then
				[ "$wrap_head" -eq "$wrap_boundary" ] ||
				    fail "journal moved before clean commit"
			else
				[ "$wrap_head" -ge "$wrap_first" ] &&
				    [ "$wrap_head" -lt "$wrap_boundary" ] ||
				    fail "journal head did not wrap"
			fi
		fi
		if [ "$stage" = truncate-prune ]; then
			durable_revokes=$(journal_revoke_records \
			    "$case_dir/journal.bin" \
			    "$case_dir/durable-revokes.log")
			grep -q ' incompat=1$' \
			    "$case_dir/durable-revokes.log" ||
			    fail "durable journal lost REVOKE feature"
			if [ "$flush_count" -eq 1 ]; then
				expected_revokes=0
			else
				expected_revokes=1
			fi
			if [ "$durable_revokes" -ne "$expected_revokes" ]; then
				reason="truncate flush $flush_count has "
				reason="$reason$durable_revokes revokes, "
				fail "${reason}expected $expected_revokes"
			fi
		fi
		case "$flush_count" in
		1|2)
			expected_journal=clean
			expected_state=$before_state
			expected_leaves=$before_leaves
			;;
		3)
			expected_journal=recover
			expected_state=$before_state
			expected_leaves=$before_leaves
			;;
		4|5)
			expected_journal=recover
			expected_state=$after_state
			expected_leaves=$after_leaves
			;;
		6)
			expected_journal=clean
			expected_state=$after_state
			expected_leaves=$after_leaves
			;;
		esac
		[ "$durable_journal_state" = "$expected_journal" ] ||
		    fail "$label flush $flush_count journal mismatch: \
$durable_journal_state != $expected_journal"
		recover_case "$image" "$case_dir" "$verifier"
		grep -qx "$expected_state" "$case_dir/recovery.log" || {
			cat "$case_dir/recovery.log" >&2
			fail "$label flush $flush_count recovered wrong state"
		}
		if [ -n "$expected_leaves" ]; then
			check_extent_leaf_count "$image" \
			    "$matrix_extent_path" \
			    "$expected_leaves" \
			    "$case_dir/recovered-extents.log"
		fi
		if [ "$stage" = truncate-prune ]; then
			recovered_free=$(filesystem_free_blocks \
			    "$case_dir/dumpe2fs.log")
			expected_free=$base_free_blocks
			if [ "$expected_state" = state=truncate-pruned ]; then
				expected_free=$((base_free_blocks + \
				    truncate_freed_blocks))
			fi
			[ "$recovered_free" -eq "$expected_free" ] ||
			    fail "truncate free-block count mismatch"
		elif [ "$stage" = block-reuse ]; then
			recovered_free=$(filesystem_free_blocks \
			    "$case_dir/dumpe2fs.log")
			recovered_block=$(filesystem_block_map "$image" \
			    "$matrix_block_path" 0 \
			    "$case_dir/recovered-block-map.log")
			if [ "$expected_state" = state=reuse-free ]; then
				expected_free=$((base_free_blocks + 1))
				expected_block=0
			else
				expected_free=$base_free_blocks
				expected_block=$base_reuse_block
			fi
			[ "$recovered_free" -eq "$expected_free" ] ||
			    fail "block-reuse free-block count mismatch"
			[ "$recovered_block" -eq "$expected_block" ] ||
			    fail "block-reuse physical block mismatch"
		fi
		echo ' ok'
	done
}

run_journal_exhaustion_case()
{
	exhaustion_source=$1
	exhaustion_block_size=$2
	test_name="journal credit exhaustion, "
	test_name="$test_name$exhaustion_block_size-byte blocks"
	printf '%-64s' "vmm: $test_name"
	case_dir=$work/journal-exhaustion-$exhaustion_block_size
	mkdir "$case_dir"
	image=$case_dir/ext4.img
	cp "$exhaustion_source" "$image"
	"$DUMPE2FS" -h "$exhaustion_source" \
	    >"$case_dir/base-super.log" 2>&1 ||
	    fail "could not inspect journal-exhaustion baseline"
	base_free_blocks=$(filesystem_free_blocks \
	    "$case_dir/base-super.log")
	base_free_inodes=$(filesystem_free_inodes \
	    "$case_dir/base-super.log")

	start_guest "$image"
	trigger="set -e
mkdir -p /mnt/ext4
mount_ext4fs /dev/sd1c /mnt/ext4
/tmp/ext4fs_crash journal-exhaustion /mnt/ext4/crash
umount /mnt/ext4"
	if ! ssh_step "root@$guest" "$trigger" \
	    >"$case_dir/workload.log" 2>&1; then
		cat "$case_dir/workload.log" >&2
		fail "journal-exhaustion workload failed"
	fi
	grep -qx 'state=journal-exhausted' \
	    "$case_dir/workload.log" ||
	    fail "journal exhaustion did not report exact ENOSPC"
	stop_guest
	"$SYNC"

	hash_before=$($SHA256 -q "$image")
	validate_guest "$image" ro verify-journal-exhaustion \
	    "$case_dir/verify.log"
	hash_after=$($SHA256 -q "$image")
	[ "$hash_before" = "$hash_after" ] ||
	    fail "read-only exhaustion verification changed image"
	"$E2FSCK" -fn "$image" >"$case_dir/e2fsck.log" 2>&1 ||
	    fail "e2fsck rejected journal-exhaustion image"
	"$DUMPE2FS" -h "$image" >"$case_dir/dumpe2fs.log" 2>&1 ||
	    fail "dumpe2fs rejected journal-exhaustion image"
	if grep -q '^Filesystem features:.*needs_recovery' \
	    "$case_dir/dumpe2fs.log"; then
		fail "journal exhaustion retained RECOVER"
	fi
	grep -q '^Filesystem state:.*clean' \
	    "$case_dir/dumpe2fs.log" ||
	    fail "journal-exhaustion image is not clean"
	[ "$(filesystem_free_blocks "$case_dir/dumpe2fs.log")" \
	    -eq "$base_free_blocks" ] ||
	    fail "journal exhaustion changed free-block accounting"
	[ "$(filesystem_free_inodes "$case_dir/dumpe2fs.log")" \
	    -eq "$base_free_inodes" ] ||
	    fail "journal exhaustion changed free-inode accounting"
	journal=$case_dir/journal.bin
	"$DEBUGFS" -R "dump <8> $journal" "$image" \
	    >"$case_dir/journal-dump.log" 2>&1 ||
	    fail "could not extract exhaustion journal"
	"$EXT4FS_CRASH" journal-state "$journal" \
	    >"$case_dir/journal.log" 2>&1 ||
	    fail "could not inspect exhaustion journal"
	grep -q '^journal=clean ' "$case_dir/journal.log" ||
	    fail "journal exhaustion left a live transaction"
	[ "$(journal_value "$case_dir/journal.log" maxtrans)" \
	    -eq 47 ] || fail "journal transaction limit changed"
	echo ' ok'
}

prepare_block_reuse_base()
{
	image=$1
	block_size=$2
	source_root=$work/source-reuse-$block_size
	source_file=$source_root/crash/reuse-probe

	mkdir -p "$source_root/crash"
	dd if=/dev/zero of="$source_file" bs=1 count=0 \
	    seek="$block_size" status=none
	printf x | dd of="$source_file" bs=1 conv=notrunc status=none
	dd if=/dev/zero of="$image" bs=1m count=0 \
	    seek="$EXT4FS_CRASH_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O 'metadata_csum,^orphan_file' -d "$source_root" \
	    "$image" >"$work/mke2fs-reuse-$block_size.log" 2>&1; then
		cat "$work/mke2fs-reuse-$block_size.log" >&2
		fail "could not create block-reuse fixture"
	fi
	"$E2FSCK" -fn "$image" \
	    >"$work/e2fsck-reuse-$block_size.log" 2>&1 ||
	    fail "e2fsck rejected block-reuse fixture"
}

prepare_sparse_extent_base()
{
	image=$1
	block_size=$2
	fixture_entries=$3
	expected_leaves=$4
	fixture_name=$5
	fixture_file=$6
	source_root=$work/source-$fixture_name-$block_size
	source_file=$source_root/crash/$fixture_file
	fixture_size=$(((2 * fixture_entries - 1) * block_size))

	mkdir -p "$source_root/crash"
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
	    seek="$EXT4FS_CRASH_IMAGE_MB" status=none
	if ! "$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O 'metadata_csum,^orphan_file' -d "$source_root" \
	    "$image" >"$work/mke2fs-$fixture_name-$block_size.log" \
	    2>&1; then
		cat "$work/mke2fs-$fixture_name-$block_size.log" >&2
		fail "could not create $fixture_name extent fixture"
	fi
	"$E2FSCK" -fn "$image" \
	    >"$work/e2fsck-$fixture_name-$block_size.log" 2>&1 ||
	    fail "e2fsck rejected $fixture_name extent fixture"
	check_extent_leaf_count "$image" "/crash/$fixture_file" \
	    "$expected_leaves" \
	    "$work/extents-$fixture_name-$block_size.log"
}

enable_journal_revoke()
{
	image=$1
	block_size=$2
	output=$3
	journal_block=$("$DEBUGFS" -R 'bmap <8> 0' "$image" \
	    2>"$output" | awk 'NF { value = $NF } END { print value }')
	case "$journal_block" in
	''|*[!0-9]*)
		cat "$output" >&2
		fail "could not map journal superblock"
		;;
	esac
	feature_offset=$((journal_block * block_size + \
	    JBD2_OFF_FEATURE_INCOMPAT))
	set -- $(dd if="$image" bs=1 skip="$feature_offset" count=4 \
	    status=none | od -An -tu1)
	[ "$#" -eq 4 ] || fail "could not read journal features"
	[ "$1" -eq 0 ] && [ "$2" -eq 0 ] &&
	    [ "$3" -eq 0 ] && [ "$4" -eq 0 ] ||
	    fail "truncate journal has unexpected incompat features"
	# This clean journal has no checksum feature, so enabling the
	# published REVOKE bit does not require a checksum update.
	printf '\000\000\000\001' | dd of="$image" bs=1 \
	    seek="$feature_offset" count=4 conv=notrunc status=none
	"$E2FSCK" -fn "$image" >"$output" 2>&1 || {
		cat "$output" >&2
		fail "e2fsck rejected revoke-enabled journal"
	}
}

"$SYSCTL" -n kern.version >"$work/host-version.log"
old_data=$work/old.data
"$EXT4FS_CRASH" pattern-old "$old_data"
if [ "$EXT4FS_CRASH_MODE" = journal ]; then
	new_data=$work/new.data
	"$EXT4FS_CRASH" pattern-new "$new_data"
fi

for block_size in $EXT4FS_CRASH_BLOCK_SIZES; do
	if [ "$EXT4FS_CRASH_MODE" = reuse ]; then
		reuse_base=$work/base-reuse-$block_size.img
		prepare_block_reuse_base "$reuse_base" "$block_size"
		run_flush_matrix "$reuse_base" "$block_size" \
		    block-reuse 'block reuse' verify-block-reuse \
		    'state=reuse-free' 'state=reuse-allocated'
		continue
	fi

	if [ "$EXT4FS_CRASH_MODE" = truncate ]; then
		extent_capacity=$(((block_size - 12) / 12))
		truncate_base=$work/base-truncate-$block_size.img
		prepare_sparse_extent_base "$truncate_base" \
		    "$block_size" "$((extent_capacity + 1))" 2 \
		    truncate truncate-probe
		enable_journal_revoke "$truncate_base" "$block_size" \
		    "$work/revoke-feature-$block_size.log"
		run_flush_matrix "$truncate_base" "$block_size" \
		    truncate-prune 'extent truncate' \
		    verify-truncate-prune 'state=truncate-full' \
		    'state=truncate-pruned' 2 1
		continue
	fi

	if [ "$EXT4FS_CRASH_MODE" = extent ]; then
		extent_capacity=$(((block_size - 12) / 12))
		promote_base=$work/base-extent-promote-$block_size.img
		prepare_sparse_extent_base "$promote_base" \
		    "$block_size" 4 0 promote extent-probe
		run_flush_matrix "$promote_base" "$block_size" \
		    extent-promote 'extent root promotion' \
		    verify-extent-promote 'state=extent-inline' \
		    'state=extent-promoted' 0 1

		split_base=$work/base-extent-split-$block_size.img
		prepare_sparse_extent_base "$split_base" \
		    "$block_size" "$extent_capacity" 1 split \
		    extent-probe
		run_flush_matrix "$split_base" "$block_size" \
		    extent-split 'extent leaf split' \
		    verify-extent-split 'state=extent-one-leaf' \
		    'state=extent-two-leaves' 1 2
		continue
	fi

	base=$work/base-$block_size.img
	dd if=/dev/zero of="$base" bs=1m count=0 \
	    seek="$EXT4FS_CRASH_IMAGE_MB" status=none
	"$MKE2FS" -q -F -t ext4 -I 256 -b "$block_size" \
	    -O '^orphan_file,^uninit_bg' "$base"
	"$DEBUGFS" -w -R 'mkdir /crash' "$base" \
	    >"$work/debugfs-dir-$block_size.log" 2>&1 ||
	    fail "could not create baseline directory"
	"$DEBUGFS" -w -R "write $old_data /crash/current" "$base" \
	    >"$work/debugfs-file-$block_size.log" 2>&1 ||
	    fail "could not create baseline file"
	"$E2FSCK" -fn "$base" \
	    >"$work/e2fsck-base-$block_size.log" 2>&1 ||
	    fail "e2fsck rejected baseline image"

	if [ "$EXT4FS_CRASH_MODE" = timed ]; then
		for stage in $EXT4FS_CRASH_STAGES; do
			test_name="$stage crash, $block_size-byte blocks"
			printf '%-64s' "vmm: $test_name"
			case_dir=$work/$stage-$block_size
			mkdir "$case_dir"
			image=$case_dir/ext4.img
			cp "$base" "$image"

			start_guest "$image"
			run_crash "$image" "$stage" \
			    "$case_dir/workload.log"
			preserve_durable "$image" "$case_dir"
			recover_case "$image" "$case_dir" verify
			echo ' ok'
		done
		continue
	fi

	case "$EXT4FS_CRASH_MODE" in
	flush)
		run_flush_matrix "$base" "$block_size" rename rename \
		    verify 'state=old next=full' \
		    'state=new next=absent'
		;;
	orphan)
		run_flush_matrix "$base" "$block_size" unlink-open \
		    'open unlink' verify-unlink 'state=linked' \
		    'state=absent'
		;;
	directory)
		run_flush_matrix "$base" "$block_size" dir-grow \
		    'directory growth' verify-dir-growth \
		    'state=directory-old' 'state=directory-grown'
		run_flush_matrix "$base" "$block_size" rmdir-open \
		    'directory removal' verify-rmdir \
		    'state=directory-linked' \
		    'state=directory-absent'
		;;
	journal)
		journal_base=$base
		exhaustion_base=$work/base-exhaustion-$block_size.img
		cp "$journal_base" "$exhaustion_base"
		tune_journal_fixture "$exhaustion_base" \
		    "$block_size" exhaustion \
		    "$work/exhaustion-fixture-$block_size.log"
		run_journal_exhaustion_case "$exhaustion_base" \
		    "$block_size"

		wrap_base=$work/base-wrap-$block_size.img
		cp "$journal_base" "$wrap_base"
		"$DEBUGFS" -w -R \
		    "write $new_data /crash/next" "$wrap_base" \
		    >"$work/debugfs-wrap-$block_size.log" 2>&1 ||
		    fail "could not create journal-wrap source"
		tune_journal_fixture "$wrap_base" "$block_size" wrap \
		    "$work/wrap-fixture-$block_size.log"
		run_flush_matrix "$wrap_base" "$block_size" \
		    rename-wrap 'journal wraparound' verify \
		    'state=old next=full' \
		    'state=new next=absent'
		;;
	esac
done
