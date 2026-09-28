#!/bin/sh
#
# Prove errors=panic in a disposable vmd guest.  This is an explicit
# root-only test and is never included in an ordinary regression run.

set -eu

MKE2FS=${MKE2FS:-mke2fs}
E2FSCK=${E2FSCK:-e2fsck}
DUMPE2FS=${DUMPE2FS:-dumpe2fs}
CMP=${CMP:-cmp}
SYSCTL=${SYSCTL:-sysctl}
VMCTL=${VMCTL:-vmctl}
SSH=${SSH:-ssh}
TIMEOUT=${TIMEOUT:-timeout}
EXT4FS_CORRUPT=${EXT4FS_CORRUPT:-./ext4fs_corrupt}
EXT4FS_VMM_BASE=${EXT4FS_VMM_BASE:-/home/vm/ext4fs-base.qcow2}
EXT4FS_VMM_KERNEL=${EXT4FS_VMM_KERNEL:-/bsd}
EXT4FS_VMM_DIR=${EXT4FS_VMM_DIR:-/home/vm}
EXT4FS_VMM_BOOT_TIMEOUT=${EXT4FS_VMM_BOOT_TIMEOUT:-120}
EXT4FS_VMM_PANIC_TIMEOUT=${EXT4FS_VMM_PANIC_TIMEOUT:-30}
EXT4FS_VMM_IMAGE_MB=${EXT4FS_VMM_IMAGE_MB:-128}
EXT4FS_VMM_SSH_KEY=${EXT4FS_VMM_SSH_KEY:-}

fail()
{
	echo "FAILED: errors=panic vmm: $*" >&2
	exit 1
}

[ "$(id -u)" -eq 0 ] || fail "test must run as root"

tools="$MKE2FS $E2FSCK $DUMPE2FS $CMP $SYSCTL $VMCTL $SSH \
$TIMEOUT $EXT4FS_CORRUPT awk dd grep kill mkfifo mktemp rm sleep tr"
for tool in $tools; do
	command -v "$tool" >/dev/null 2>&1 ||
	    fail "required tool not found: $tool"
done

[ -r "$EXT4FS_VMM_BASE" ] || fail "base image is not readable"
[ -r "$EXT4FS_VMM_KERNEL" ] || fail "kernel is not readable"
[ -d "$EXT4FS_VMM_DIR" ] || fail "vmm directory does not exist"

case "$EXT4FS_VMM_DIR" in
/*) ;;
*)	fail "vmm directory must be an absolute path" ;;
esac
case "$EXT4FS_VMM_BOOT_TIMEOUT:$EXT4FS_VMM_PANIC_TIMEOUT:\
$EXT4FS_VMM_IMAGE_MB" in
*[!0-9:]*)
	fail "timeouts and image size must be unsigned integers"
	;;
esac
[ "$EXT4FS_VMM_BOOT_TIMEOUT" -gt 0 ] ||
    fail "boot timeout must be positive"
[ "$EXT4FS_VMM_BOOT_TIMEOUT" -le 600 ] ||
    fail "boot timeout must not exceed 600"
[ "$EXT4FS_VMM_PANIC_TIMEOUT" -gt 0 ] ||
    fail "panic timeout must be positive"
[ "$EXT4FS_VMM_PANIC_TIMEOUT" -le 120 ] ||
    fail "panic timeout must not exceed 120"
[ "$EXT4FS_VMM_IMAGE_MB" -ge 64 ] ||
    fail "image size must be at least 64 MiB"
[ "$EXT4FS_VMM_IMAGE_MB" -le 4096 ] ||
    fail "image size must not exceed 4096 MiB"
if [ -n "$EXT4FS_VMM_SSH_KEY" ]; then
	[ -r "$EXT4FS_VMM_SSH_KEY" ] ||
	    fail "SSH identity is not readable"
fi

work=$(mktemp -d "$EXT4FS_VMM_DIR/ext4fs_panic.XXXXXXXX")
case "$work" in
"$EXT4FS_VMM_DIR"/ext4fs_panic.*) ;;
*)	fail "unsafe temporary directory: $work" ;;
esac

overlay=$work/root.qcow2
image=$work/panic.img
console=$work/console.log
console_input=$work/console.fifo
stats=$work/dumpe2fs.log
vm_name=ext4fs-panic-$$
vm_started=0
console_pid=
console_fd_open=0

cleanup()
{
	rc=$?
	trap - EXIT HUP INT TERM
	if [ "$vm_started" -eq 1 ]; then
		"$TIMEOUT" -k 2 10 "$VMCTL" stop -fw "$vm_name" \
		    >/dev/null 2>&1 || :
	fi
	if [ "$console_fd_open" -eq 1 ]; then
		exec 8>&-
		console_fd_open=0
	fi
	if [ -n "$console_pid" ]; then
		kill "$console_pid" >/dev/null 2>&1 || :
		wait "$console_pid" >/dev/null 2>&1 || :
	fi
	if [ "${KEEP_TMP:-0}" = 1 ] || [ "$rc" -ne 0 ]; then
		echo "temporary files retained in $work"
	else
		case "$work" in
		"$EXT4FS_VMM_DIR"/ext4fs_panic.*)
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
	if [ -n "$EXT4FS_VMM_SSH_KEY" ]; then
		"$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
		    -i "$EXT4FS_VMM_SSH_KEY" "$@"
	else
		"$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
		    -o StrictHostKeyChecking=no \
		    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
		    "$@"
	fi
}

printf '%-64s' 'vmm: errors=panic journal abort policy'

"$SYSCTL" -n kern.version >"$work/host-version.log"
dd if=/dev/zero of="$image" bs=1m count=0 \
    seek="$EXT4FS_VMM_IMAGE_MB" status=none
"$MKE2FS" -q -F -t ext4 -I 256 -b 1024 -e panic \
    -O '^orphan_file,^uninit_bg' "$image"
"$E2FSCK" -fn "$image" >"$work/e2fsck.log" 2>&1 ||
    fail "e2fsck rejected the clean fixture"
"$EXT4FS_CORRUPT" "$image" runtime-block-bitmap-checksum

"$VMCTL" create -b "$EXT4FS_VMM_BASE" "$overlay" \
    >"$work/create.log" 2>&1 || fail "could not create guest overlay"
"$VMCTL" start -m 1G -b "$EXT4FS_VMM_KERNEL" \
    -d "qcow2:$overlay" -d "raw:$image" -L "$vm_name" \
    >"$work/start.log" 2>&1 || fail "could not start guest"
vm_started=1

vm_id=$("$VMCTL" status "$vm_name" |
    awk -v name="$vm_name" 'NR > 1 && $NF == name { print $1; exit }')
case "$vm_id" in
''|*[!0-9]*)	fail "could not determine guest id" ;;
esac
[ "$vm_id" -le 255 ] || fail "guest id is outside local IPv4 range"
[ "$vm_id" -gt 0 ] || fail "guest id is not positive"
guest=100.64.$vm_id.3

mkfifo "$console_input"
exec 8<>"$console_input"
console_fd_open=1
"$VMCTL" console "$vm_name" <&8 >"$console" 2>&1 &
console_pid=$!

ready=0
elapsed=0
while [ "$elapsed" -lt "$EXT4FS_VMM_BOOT_TIMEOUT" ]; do
	if ssh_run "root@$guest" true >/dev/null 2>&1; then
		ready=1
		break
	fi
	sleep 1
	elapsed=$((elapsed + 1))
done
[ "$ready" -eq 1 ] || fail "guest SSH did not become ready"
ssh_run "root@$guest" uname -a >"$work/uname.log" 2>&1 ||
    fail "could not query guest kernel"
ssh_run "root@$guest" sysctl -n kern.version \
    >"$work/guest-version.log" 2>&1 ||
    fail "could not query guest kernel version"
"$CMP" -s "$work/host-version.log" "$work/guest-version.log" ||
    fail "guest did not boot the running production kernel"

trigger='set -e
sysctl ddb.panic=1 >/dev/null
mkdir -p /mnt/ext4
mount_ext4fs /dev/sd1c /mnt/ext4
exec 9>/mnt/ext4/unlinked
rm /mnt/ext4/unlinked
mkdir /mnt/ext4/abort-directory'

set +e
if [ -n "$EXT4FS_VMM_SSH_KEY" ]; then
	"$TIMEOUT" -k 5 "$EXT4FS_VMM_PANIC_TIMEOUT" \
	    "$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
	    -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
	    -i "$EXT4FS_VMM_SSH_KEY" "root@$guest" "$trigger" \
	    >"$work/trigger.log" 2>&1
else
	"$TIMEOUT" -k 5 "$EXT4FS_VMM_PANIC_TIMEOUT" \
	    "$SSH" -o BatchMode=yes -o ConnectTimeout=3 \
	    -o StrictHostKeyChecking=no \
	    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
	    "root@$guest" "$trigger" >"$work/trigger.log" 2>&1
fi
set -e

elapsed=0
while [ "$elapsed" -lt "$EXT4FS_VMM_PANIC_TIMEOUT" ]; do
	grep -a -q 'panic: ext4fs: journal abort' "$console" && break
	sleep 1
	elapsed=$((elapsed + 1))
done
grep -a -q 'panic: ext4fs: journal abort' "$console" ||
    fail "guest did not emit the ext4fs panic"

"$TIMEOUT" -k 2 10 "$VMCTL" stop -fw "$vm_name" \
    >"$work/stop.log" 2>&1 || fail "could not stop panicked guest"
vm_started=0
exec 8>&-
console_fd_open=0
kill "$console_pid" >/dev/null 2>&1 || :
wait "$console_pid" >/dev/null 2>&1 || :
console_pid=

panic_count=$(grep -a -c 'panic: ext4fs: journal abort' \
    "$console" || :)
[ "$panic_count" -eq 1 ] ||
    fail "expected one panic diagnostic, found $panic_count"
panic_line=$(tr -d '\r' <"$console" | awk '
/panic: ext4fs: journal abort/ {
	line = $0
	while (line !~ /free [0-9]+$/ && getline > 0)
		line = line $0
	print line
	exit
}')
panic_re='^panic: ext4fs: journal abort on /mnt/ext4: '
panic_re=$panic_re'ext4fs_mkdir_journal at outside commit, error 22, '
panic_re=$panic_re'sequence [0-9]+, head [0-9]+, tail [0-9]+, '
panic_re=$panic_re'free [0-9]+$'
printf '%s\n' "$panic_line" | grep -Eq "$panic_re" ||
    fail "panic diagnostic has unexpected contents: $panic_line"

"$DUMPE2FS" -h "$image" >"$stats" 2>&1 ||
    fail "dumpe2fs rejected the aborted fixture"
grep -q '^Filesystem features:.*needs_recovery' "$stats" ||
    fail "panicked guest cleared RECOVER"
grep -q '^Filesystem state:.*not clean' "$stats" ||
    fail "panicked guest left a clean filesystem"
grep -q '^Errors behavior:.*Panic' "$stats" ||
    fail "fixture did not retain errors=panic"

echo ' ok'
