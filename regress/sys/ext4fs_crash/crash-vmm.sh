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

test_name=ext4fs-crash-vmm
fail()
{
	echo "FAILED: $test_name: $*" >&2
	exit 1
}

[ "$(id -u)" -eq 0 ] || fail "test must run as root"

tools="$MKE2FS $DEBUGFS $E2FSCK $DUMPE2FS $VMCTL $SSH $SCP \
$SYSCTL $TIMEOUT $SHA256 $SYNC $EXT4FS_CRASH awk cmp cp dd grep \
cat id kill mkdir mktemp mv rm sleep"
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
timed|flush) ;;
*)	fail "unsupported crash mode: $EXT4FS_CRASH_MODE" ;;
esac
if [ "$EXT4FS_CRASH_MODE" = flush ]; then
	[ -n "$EXT4FS_CRASH_FLUSH_COUNTS" ] ||
	    fail "flush-count list must not be empty"
	for flush_count in $EXT4FS_CRASH_FLUSH_COUNTS; do
		case "$flush_count" in
		''|*[!0-9]*|0)	fail "invalid flush count: $flush_count" ;;
		esac
		[ "$flush_count" -le 6 ] ||
		    fail "flush count exceeds rename transaction"
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
	log=$3
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
/tmp/ext4fs_crash verify /mnt/ext4/crash
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
	validate_guest "$image" '' "$case_dir/recovery.log"
	hash_before=$($SHA256 -q "$image")
	validate_guest "$image" ro "$case_dir/idempotence.log"
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

"$SYSCTL" -n kern.version >"$work/host-version.log"
old_data=$work/old.data
"$EXT4FS_CRASH" pattern-old "$old_data"

for block_size in $EXT4FS_CRASH_BLOCK_SIZES; do
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
			recover_case "$image" "$case_dir"
			echo ' ok'
		done
		continue
	fi

	for flush_count in $EXT4FS_CRASH_FLUSH_COUNTS; do
		test_name="rename flush $flush_count, "
		test_name="$test_name$block_size-byte blocks"
		printf '%-64s' "vmm: $test_name"
		case_dir=$work/flush-$flush_count-$block_size
		mkdir "$case_dir"
		image=$case_dir/ext4.img
		cp "$base" "$image"

		start_guest "$image"
		run_flush_crash "$image" rename "$flush_count" \
		    "$case_dir/workload.log" \
		    "$case_dir/flush-stop.log"
		preserve_durable "$image" "$case_dir"
		case "$flush_count" in
		1|2)
			expected_journal=clean
			expected_state='state=old next=full'
			;;
		3)
			expected_journal=recover
			expected_state='state=old next=full'
			;;
		4|5)
			expected_journal=recover
			expected_state='state=new next=absent'
			;;
		6)
			expected_journal=clean
			expected_state='state=new next=absent'
			;;
		esac
		[ "$durable_journal_state" = "$expected_journal" ] ||
		    fail "flush $flush_count journal state mismatch: \
$durable_journal_state != $expected_journal"
		recover_case "$image" "$case_dir"
		grep -qx "$expected_state" "$case_dir/recovery.log" || {
			cat "$case_dir/recovery.log" >&2
			fail "flush $flush_count recovered wrong state"
		}
		echo ' ok'
	done
done
