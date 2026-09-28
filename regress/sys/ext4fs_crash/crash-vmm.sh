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
cat id kill mkdir mktemp rm sleep"
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
	"$TIMEOUT" -k 2 10 "$VMCTL" stop -fw "$vm_name" \
	    >"$work/stop-$serial.log" 2>&1 ||
	    fail "could not force-stop guest"
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
		validate_guest "$image" '' \
		    "$case_dir/recovery.log"
		hash_before=$("$SHA256" -q "$image")
		validate_guest "$image" ro \
		    "$case_dir/idempotence.log"
		hash_after=$("$SHA256" -q "$image")
		[ "$hash_before" = "$hash_after" ] ||
		    fail "second read-only mount modified the image"
		cmp -s "$case_dir/recovery.log" \
		    "$case_dir/idempotence.log" ||
		    fail "second mount changed the recovered state"
		"$E2FSCK" -fn "$image" \
		    >"$case_dir/e2fsck.log" 2>&1 ||
		    fail "e2fsck rejected the recovered image"
		"$DUMPE2FS" -h "$image" \
		    >"$case_dir/dumpe2fs.log" 2>&1 ||
		    fail "dumpe2fs rejected the recovered image"
		if grep -q '^Filesystem features:.*needs_recovery' \
		    "$case_dir/dumpe2fs.log"; then
			fail "recovered image retained RECOVER"
		fi
		grep -q '^Filesystem state:.*clean' \
		    "$case_dir/dumpe2fs.log" ||
		    fail "recovered image is not clean"
		echo ' ok'
	done
done
