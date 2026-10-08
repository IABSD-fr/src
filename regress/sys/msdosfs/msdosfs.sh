#!/bin/sh
## Copyright (c) 2025,2026 kmx.io <contact@kmx.io>
##
## Permission to use, copy, modify, and distribute this software for any
## purpose with or without fee is hereby granted, provided that the
## above copyright notice and this permission notice appear in all
## copies.
##
## THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
## WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
## WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
## AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
## DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
## PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
## TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
## PERFORMANCE OF THIS SOFTWARE.

# Build deterministic FAT fixtures and, in kernel mode, exercise them through
# vnd(4) with a timeout around every potentially unsafe operation.

set -eu

MSDOSFS_TEST=${MSDOSFS_TEST:-./msdosfs_test}
MSDOSFS_MODE=${MSDOSFS_MODE:-kernel}
MSDOSFS_TIMEOUT=${MSDOSFS_TIMEOUT:-20}
NEWFS_MSDOS=${NEWFS_MSDOS:-/sbin/newfs_msdos}
VNCONFIG=${VNCONFIG:-vnconfig}
MOUNT_MSDOS=${MOUNT_MSDOS:-mount_msdos}
MOUNT=${MOUNT:-mount}
UMOUNT=${UMOUNT:-umount}
TIMEOUT=${TIMEOUT:-timeout}

case "$MSDOSFS_MODE" in
fixtures|kernel) ;;
*)	echo "MSDOSFS_MODE must be 'fixtures' or 'kernel'" >&2
	exit 1
	;;
esac

tools="$MSDOSFS_TEST $NEWFS_MSDOS $TIMEOUT cmp cp dd id mkdir sha256"
if [ "$MSDOSFS_MODE" = kernel ]; then
	tools="$tools $VNCONFIG $MOUNT_MSDOS $MOUNT $UMOUNT grep"
fi
for tool in $tools; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "SKIPPED: msdosfs regress requires $tool"
		exit 0
	fi
done

case "$MSDOSFS_TIMEOUT" in
''|*[!0-9]*)	echo "MSDOSFS_TIMEOUT must be an unsigned integer" >&2
	exit 1 ;;
esac
[ "$MSDOSFS_TIMEOUT" -gt 0 ] || {
	echo "MSDOSFS_TIMEOUT must be positive" >&2
	exit 1
}
[ "$MSDOSFS_TIMEOUT" -le 120 ] || {
	echo "MSDOSFS_TIMEOUT must not exceed 120" >&2
	exit 1
}

if [ "$MSDOSFS_MODE" = kernel ] && [ "$(id -u)" -ne 0 ]; then
	echo "SKIPPED: msdosfs kernel regress must run as root"
	exit 0
fi

work=$(mktemp -d /tmp/msdosfs.XXXXXXXX)
case "$work" in
/tmp/msdosfs.*) ;;
*)	echo "unsafe temporary directory: $work" >&2
	exit 1 ;;
esac

mountpoint=$work/mnt
mkdir "$mountpoint"
vnd=
mounted=0
test_name=
case_dir=

is_mounted()
{
	"$MOUNT" | grep -F " on $mountpoint " >/dev/null 2>&1
}

cleanup()
{
	rc=$?
	trap - EXIT HUP INT TERM
	if [ "$mounted" -eq 1 ] ||
	    { [ "$MSDOSFS_MODE" = kernel ] && is_mounted; }; then
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

fail()
{
	echo "FAILED: $test_name: $*" >&2
	exit 1
}

make_sparse()
{
	path=$1
	bytes=$2
	dd if=/dev/zero of="$path" bs=1 count=0 seek="$bytes" status=none
}

create_base()
{
	profile=$1
	case "$profile" in
	fat32)
		bps=512; sectors=131072; args='-F 32 -c 1 -a 1009'
		;;
	fat16-2048)
		bps=2048; sectors=10000; args='-F 16 -c 1 -e 64'
		;;
	fat16)
		bps=512; sectors=32768; args='-F 16 -c 1'
		;;
	fat32-16384)
		bps=16384; sectors=66000; args='-F 32 -c 1'
		;;
	fat32-32768)
		bps=32768; sectors=66000; args='-F 32 -c 1'
		;;
	*)	fail "unknown base profile $profile" ;;
	esac
	base=$work/base-$profile.img
	make_sparse "$base" "$((bps * sectors))"
	if ! "$NEWFS_MSDOS" -q $args -h 1 -u 1 -S "$bps" \
	    -s "$sectors" -o 0 -I 1 "$base" >"$work/newfs-$profile.log" 2>&1; then
		cat "$work/newfs-$profile.log" >&2
		fail "newfs_msdos failed for $profile"
	fi
}

fixture_base()
{
	case "$1" in
	misaligned-fixed-root|full-fixed-root)	FIXTURE_PROFILE=fat16-2048 ;;
	*)					FIXTURE_PROFILE=fat32 ;;
	esac
}

make_fixture()
{
	mutation=$1
	fixture_base "$mutation"
	case_dir=$work/$mutation
	image=$case_dir/msdos.img
	mkdir "$case_dir"
	cp "$work/base-$FIXTURE_PROFILE.img" "$image"
	before=$(sha256 -q "$image")
	if ! "$MSDOSFS_TEST" mutate "$image" "$mutation" \
	    >"$case_dir/mutate.log" 2>&1; then
		cat "$case_dir/mutate.log" >&2
		fail "fixture mutation failed"
	fi
	after=$(sha256 -q "$image")
	[ "$before" != "$after" ] || fail "fixture mutation changed no bytes"
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

mount_image()
{
	mode=$1
	if "$TIMEOUT" -k 2 "$MSDOSFS_TIMEOUT" "$MOUNT_MSDOS" \
	    -o "$mode" "/dev/${vnd}c" "$mountpoint" \
	    >"$case_dir/mount.log" 2>&1; then
		mounted=1
		return 0
	else
		mount_status=$?
		cat "$case_dir/mount.log" >&2
		return "$mount_status"
	fi
}

reject_mount()
{
	mutation=$1
	test_name=$mutation
	printf '%-62s' "kernel: $test_name"
	make_fixture "$mutation"
	attach_image
	status=0
	if mount_image ro; then
		status=0
	else
		status=$?
		is_mounted && mounted=1
	fi
	case "$status" in
	124|137|143)	detach_image; fail "mount exceeded timeout" ;;
	0)		detach_image; fail "kernel mounted malformed image" ;;
	esac
	detach_image
	echo " ok"
}

mount_control()
{
	profile=$1
	test_name="valid $profile control"
	printf '%-62s' "kernel: $test_name"
	case_dir=$work/control-$profile
	image=$work/base-$profile.img
	mkdir "$case_dir"
	attach_image
	mount_image ro || { detach_image; fail "valid control was rejected"; }
	"$TIMEOUT" -k 2 "$MSDOSFS_TIMEOUT" ls "$mountpoint" \
	    >"$case_dir/ls.log" 2>&1 || {
		detach_image
		fail "could not read valid root directory"
	}
	detach_image
	echo " ok"
}

run_read_error()
{
	mutation=$1
	path=$2
	test_name=$mutation
	printf '%-62s' "kernel: $test_name"
	make_fixture "$mutation"
	attach_image
	mount_image ro || { detach_image; fail "fixture mount failed"; }
	if ! "$TIMEOUT" -k 2 "$MSDOSFS_TIMEOUT" "$MSDOSFS_TEST" \
	    read-error "$mountpoint/$path" >"$case_dir/read.log" 2>&1; then
		cat "$case_dir/read.log" >&2
		detach_image
		fail "corrupt chain did not return EIO"
	fi
	detach_image
	echo " ok"
}

run_bad_dir()
{
	test_name=bad-directory
	printf '%-62s' "kernel: $test_name"
	make_fixture bad-directory
	attach_image
	mount_image ro || { detach_image; fail "fixture mount failed"; }
	if ! "$TIMEOUT" -k 2 "$MSDOSFS_TIMEOUT" "$MSDOSFS_TEST" \
	    bad-dir "$mountpoint" >"$case_dir/lookup.log" 2>&1; then
		cat "$case_dir/lookup.log" >&2
		detach_image
		fail "failed deget unwind regression"
	fi
	detach_image
	echo " ok"
}

run_parent_cycle()
{
	mutation=$1
	test_name=$mutation
	printf '%-62s' "kernel: $test_name"
	make_fixture "$mutation"
	attach_image
	mount_image rw || { detach_image; fail "fixture mount failed"; }
	if ! "$TIMEOUT" -k 2 "$MSDOSFS_TIMEOUT" "$MSDOSFS_TEST" \
	    parent-cycle "$mountpoint/SOURCE" "$mountpoint/TARGET" \
	    >"$case_dir/rename.log" 2>&1; then
		cat "$case_dir/rename.log" >&2
		detach_image
		fail "cyclic parent rename regression failed"
	fi
	detach_image
	echo " ok"
}

run_free_cycle()
{
	mutation=$1
	path=$2
	count=$3
	test_name=$mutation
	printf '%-62s' "kernel: $test_name"
	make_fixture "$mutation"
	attach_image
	mount_image rw || { detach_image; fail "fixture mount failed"; }
	if ! "$TIMEOUT" -k 2 "$MSDOSFS_TIMEOUT" "$MSDOSFS_TEST" \
	    free-cycle "$mountpoint/$path" "$mountpoint/PROBE.TXT" "$count" \
	    >"$case_dir/free.log" 2>&1; then
		cat "$case_dir/free.log" >&2
		detach_image
		fail "free-chain accounting regression failed"
	fi
	detach_image
	echo " ok"
}

run_dates()
{
	test_name=invalid-dates
	printf '%-62s' "kernel: $test_name"
	make_fixture dates
	attach_image
	mount_image ro || { detach_image; fail "fixture mount failed"; }
	if ! "$TIMEOUT" -k 2 "$MSDOSFS_TIMEOUT" "$MSDOSFS_TEST" \
	    dates "$mountpoint" >"$case_dir/dates.log" 2>&1; then
		cat "$case_dir/dates.log" >&2
		detach_image
		fail "date validation regression failed"
	fi
	detach_image
	echo " ok"
}

run_fh()
{
	test_name=file-handles
	printf '%-62s' "kernel: $test_name"
	make_fixture fh
	attach_image
	mount_image rw || { detach_image; fail "fixture mount failed"; }
	if ! "$TIMEOUT" -k 2 "$MSDOSFS_TIMEOUT" "$MSDOSFS_TEST" \
	    fh "$mountpoint/HANDLE.TXT" >"$case_dir/fh.log" 2>&1; then
		cat "$case_dir/fh.log" >&2
		detach_image
		fail "file-handle regression failed"
	fi
	detach_image
	echo " ok"
}

run_full_root()
{
	test_name=full-fixed-root-2048
	printf '%-62s' "kernel: $test_name"
	make_fixture full-fixed-root
	before=$(sha256 -q "$image")
	attach_image
	mount_image rw || { detach_image; fail "fixture mount failed"; }
	if ! "$TIMEOUT" -k 2 "$MSDOSFS_TIMEOUT" "$MSDOSFS_TEST" \
	    full-root "$mountpoint/NEW.TXT" >"$case_dir/full.log" 2>&1; then
		cat "$case_dir/full.log" >&2
		detach_image
		fail "fixed-root capacity regression failed"
	fi
	detach_image
	after=$(sha256 -q "$image")
	[ "$before" = "$after" ] || fail "failed create modified the image"
	echo " ok"
}

test_name=fixture-setup
create_base fat32
create_base fat16
create_base fat16-2048

mutations='bad-boot-signature zero-fats active-fat-out-of-range
root-cluster-out-of-range fsinfo-outside-reserved first-data-at-volume-end
fat-past-volume fat-too-small declared-volume-too-large
misaligned-fixed-root invalid-fsinfo-signature root-self-cycle
file-self-cycle file-two-cycle file-out-of-range bad-directory
parent-self-cycle parent-two-cycle free-self-cycle free-two-cycle dates fh
full-fixed-root'

if [ "$MSDOSFS_MODE" = fixtures ]; then
	for mutation in $mutations; do
		test_name=$mutation
		printf '%-62s' "fixture: $test_name"
		make_fixture "$mutation"
		echo " ok"
	done
	exit 0
fi

create_base fat32-16384
create_base fat32-32768

mount_control fat16
mount_control fat32
mount_control fat32-16384
mount_control fat32-32768

for mutation in bad-boot-signature zero-fats active-fat-out-of-range \
    root-cluster-out-of-range fsinfo-outside-reserved \
    first-data-at-volume-end fat-past-volume fat-too-small \
    declared-volume-too-large misaligned-fixed-root root-self-cycle; do
	reject_mount "$mutation"
done

test_name=invalid-fsinfo-signature
printf '%-62s' "kernel: $test_name"
make_fixture invalid-fsinfo-signature
attach_image
mount_image ro || { detach_image; fail "invalid FSInfo did not fall back"; }
detach_image
echo " ok"

run_read_error file-self-cycle SELFCYCL.BIN
run_read_error file-two-cycle TWOCYCLE.BIN
run_read_error file-out-of-range BADRANGE.BIN
run_bad_dir
run_parent_cycle parent-self-cycle
run_parent_cycle parent-two-cycle
run_free_cycle free-self-cycle FREESELF.BIN 1
run_free_cycle free-two-cycle FREETWO.BIN 2
run_dates
run_fh
run_full_root
