#!/bin/sh
#
# Check full fsck and both journal recovery paths through raw vnd I/O.
# The in-core label changes only the disposable device's sector size.
# Image mode checks the same fixtures without needing root.

set -eu

MKE2FS=${MKE2FS:-mke2fs}
DEBUGFS=${DEBUGFS:-debugfs}
E2FSCK=${E2FSCK:-e2fsck}
TIMEOUT=${TIMEOUT:-timeout}
VNCONFIG=${VNCONFIG:-vnconfig}
IO_TEST_MODE=${IO_TEST_MODE:-device}

case "$IO_TEST_MODE" in
images)	sectors=512 ;;
device)	sectors='512 1024 2048 4096'
	[ "$(id -u)" -eq 0 ] || {
		echo 'SKIPPED: sector-size device tests require root'
		exit 0
	}
	: "${IO_TEST:?set IO_TEST to the built io_test program}"
	;;
*)	echo "invalid IO_TEST_MODE: $IO_TEST_MODE" >&2
	exit 1
	;;
esac

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
srcroot=$(CDPATH= cd "$script_dir/../../.." && pwd)
FSCK_EXT4FS=${FSCK_EXT4FS:-$srcroot/sbin/fsck_ext4fs/obj/fsck_ext4fs}
if [ ! -f "$FSCK_EXT4FS" ] || [ ! -x "$FSCK_EXT4FS" ]; then
	echo "fsck_ext4fs missing or not executable: $FSCK_EXT4FS" >&2
	exit 1
fi

for tool in "$MKE2FS" "$DEBUGFS" "$E2FSCK" "$TIMEOUT" \
    sha256 dd tr cmp awk sed od grep; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "SKIPPED: sector-size regress requires $tool"
		exit 0
	}
done

work=$(mktemp -d /tmp/fsck_ext4fs_io.XXXXXXXX)
vnd=
test_name=setup

fail()
{
	echo "FAILED: $test_name: $*" >&2
	exit 1
}

cleanup()
{
	status=$?
	trap - EXIT HUP INT TERM
	if [ -n "$vnd" ]; then
		"$VNCONFIG" -u "$vnd" || status=1
	fi
	if [ "$status" -ne 0 ] || [ "${KEEP_TMP:-0}" = 1 ]; then
		echo "temporary files retained in $work"
	else
		case "$work" in
		/tmp/fsck_ext4fs_io.*) rm -rf "$work" ;;
		*) echo "refusing to remove $work" >&2; status=1 ;;
		esac
	fi
	exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

read_flags()
{
	dd if="$image" bs=1 skip=1120 count=4 status=none |
	    od -An -tu1 | awk '{
		print $1 + $2 * 256 + $3 * 65536 + $4 * 16777216
	    }'
}

run_fsck()
{
	log=$case_dir/fsck-$1.log
	status=0
	"$TIMEOUT" -k 2 30 "$FSCK_EXT4FS" "$1" "$device" \
	    >"$log" 2>&1 || status=$?
	if [ "$status" -ne "$2" ]; then
		cat "$log" >&2
		fail "fsck $1 returned $status, expected $2"
	fi
}

for sector in $sectors; do
	for operation in full replay recover; do
		test_name="$operation, $sector-byte sectors"
		printf 'fsck %s: %-40s' "$IO_TEST_MODE" "$test_name"
		case_dir=$work/$operation-$sector
		mkdir "$case_dir"
		image=$case_dir/ext4.img
		dd if=/dev/zero of="$image" bs=1m count=0 seek=64 \
		    status=none
		"$MKE2FS" -q -F -t ext4 -b 4096 -O '^orphan_file' \
		    "$image" >"$case_dir/mke2fs.log" 2>&1 ||
		    fail 'mke2fs failed'

		# Nonzero guards around the primary superblock catch an
		# incorrect read-modify-write of the surrounding sector.
		dd if=/dev/zero bs=1024 count=1 status=none |
		    tr '\000' P >"$case_dir/prefix"
		dd if=/dev/zero bs=2048 count=1 status=none |
		    tr '\000' S >"$case_dir/suffix"
		dd if="$case_dir/prefix" of="$image" conv=notrunc \
		    status=none
		dd if="$case_dir/suffix" of="$image" bs=2048 seek=1 \
		    conv=notrunc status=none

		if [ "$operation" = full ]; then
			{
				printf 'ssv state 0\n'
				printf 'ssv free_blocks_count 0\n'
				printf 'ssv free_inodes_count 0\n'
			} >"$case_dir/debugfs.cmd"
		else
			block=$("$DEBUGFS" -R 'find_free_block 1' \
			    "$image" 2>/dev/null |
			    sed -n 's/^Free blocks found: *//p')
			# Strip whitespace; validate the block number.
			block=$(echo "$block" | awk '{print $1}')
			case "$block" in
			''|*[!0-9]*) fail 'no free block for replay' ;;
			esac
			dd if=/dev/zero bs=4096 count=1 status=none |
			    tr '\000' Z >"$case_dir/payload"
			{
				printf 'journal_open\n'
				printf 'journal_write -b %s %s\n' \
				    "$block" "$case_dir/payload"
				printf 'journal_close\n'
			} >"$case_dir/debugfs.cmd"
		fi
		"$DEBUGFS" -w -f "$case_dir/debugfs.cmd" "$image" \
		    >"$case_dir/debugfs.log" 2>&1 ||
		    fail 'debugfs failed'
		if [ "$operation" != full ]; then
			flags=$(read_flags)
			[ $((flags & 4)) -ne 0 ] ||
			    fail 'fixture does not require recovery'
		fi
		if [ "$operation" = recover ]; then
			# Crash after replay and journal clearing, but
			# before clearing the filesystem's RECOVER bit.
			dd if="$case_dir/payload" of="$image" bs=4096 \
			    seek="$block" conv=notrunc status=none
			jblock=$("$DEBUGFS" -R 'bmap <8> 0' "$image" \
			    2>/dev/null)
			case "$jblock" in
			''|*[!0-9]*) fail 'no journal superblock' ;;
			esac
			printf '\000\000\000\002\000\000\000\000' |
			    dd of="$image" bs=1 \
			    seek=$((jblock * 4096 + 24)) \
			    conv=notrunc status=none
		fi

		device=$image
		if [ "$IO_TEST_MODE" = device ]; then
			vnd=$("$VNCONFIG" "$image") ||
			    fail 'could not attach disposable image'
			case "$vnd" in
			vnd[0-9]*) ;;
			*) fail "unexpected virtual device: $vnd" ;;
			esac
			device=/dev/r${vnd}c
			"$IO_TEST" sector-size "$device" "$sector"
		fi
		before=$(sha256 -q "$image")
		if [ "$operation" = full ]; then
			run_fsck -fn 0
		else
			run_fsck -n 8
		fi
		[ "$before" = "$(sha256 -q "$image")" ] ||
		    fail 'read-only fsck modified the image'
		if [ "$operation" = full ]; then
			run_fsck -fy 0
		elif [ "$IO_TEST_MODE" = device ]; then
			run_fsck -p 0
		else
			run_fsck -y 0
		fi
		if [ "$operation" != full ]; then
			grep -q 'journal replay complete' "$log" ||
			    fail 'journal recovery did not complete'
		fi
		if [ -n "$vnd" ]; then
			"$VNCONFIG" -u "$vnd" || fail 'detach failed'
			vnd=
		fi

		dd if="$image" bs=1024 count=1 status=none |
		    cmp -s - "$case_dir/prefix" ||
		    fail 'superblock prefix changed'
		dd if="$image" bs=2048 skip=1 count=1 status=none |
		    cmp -s - "$case_dir/suffix" ||
		    fail 'superblock suffix changed'
		flags=$(read_flags)
		[ $((flags & 4)) -eq 0 ] || fail 'RECOVER remains set'
		if [ "$operation" != full ]; then
			dd if="$image" bs=4096 skip="$block" count=1 \
			    status=none >"$case_dir/replayed"
			cmp -s "$case_dir/replayed" \
			    "$case_dir/payload" ||
			    fail 'replayed data differs'
		fi
		state=$(dd if="$image" bs=1 skip=1082 count=2 \
		    status=none | od -An -tu1 |
		    awk '{print $1 + $2 * 256}')
		[ $((state & 1)) -ne 0 ] ||
		    fail 'filesystem was not marked clean'
		if ! "$E2FSCK" -fn "$image" \
		    >"$case_dir/e2fsck.log" 2>&1; then
			cat "$case_dir/e2fsck.log" >&2
			fail 'e2fsck rejected the checked image'
		fi
		echo ' ok'
	done
done
