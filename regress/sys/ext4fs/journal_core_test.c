/*
 * Copyright (c) 2026 kmx.io.
 *
 * Permission to use, copy, modify, and distribute this software for
 * any purpose with or without fee is hereby granted, provided that
 * the above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA
 * OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include <err.h>
#include <stdio.h>
#include <string.h>

#include <ufs/ext4fs/ext4fs_journal_state.h>

static void
check (int condition, const char *name)
{
	if (! condition)
		errx(1, "%s", name);
}

static void
test_admission (void)
{
	int aborted, error;

	check(ext4fs_journal_state_admission(0, 0, 0, 0) == 0,
	    "idle admission");
	check(ext4fs_journal_state_admission(1, 0, 0, 0) == EBUSY,
	    "active handle serialization");
	check(ext4fs_journal_state_admission(1, 1, ENOSPC, 0) == ENOSPC,
	    "abort precedes active handle");
	check(ext4fs_journal_state_admission(
	    1, 1, ENOSPC, 1) == ESHUTDOWN,
	    "shutdown precedes abort");
	check(ext4fs_journal_state_admission(0, 1, 0, 0) == EIO,
	    "zero abort error normalization");

	aborted = 0;
	error = 0;
	check(ext4fs_journal_state_abort(&aborted, &error, 0) == 1,
	    "initial abort transition");
	check(aborted == 1 && error == EIO, "initial sticky abort");
	check(ext4fs_journal_state_abort(&aborted, &error, ENOSPC) == 0,
	    "repeated abort transition");
	check(aborted == 1 && error == EIO, "first abort is sticky");
}

static void
test_reservation_case (u_int32_t maximum, u_int32_t used,
    u_int32_t reserved, u_int32_t request)
{
	u_int32_t original;
	int expected, result;

	original = reserved;
	if (request == 0)
		expected = EINVAL;
	else if (used > maximum || reserved > maximum - used ||
	    request > maximum - used - reserved)
		expected = ENOSPC;
	else
		expected = 0;
	result = ext4fs_journal_state_reserve(maximum, used,
	    &reserved, request);
	check(result == expected, "reservation result matrix");
	check(reserved ==
	    (expected == 0 ? original + request : original),
	    "reservation mutation matrix");
}

static void
test_reservations (void)
{
	u_int32_t handle, maximum, request, reserved, used;

	for (maximum = 0; maximum <= 32; maximum++) {
		for (used = 0; used <= 36; used++) {
			for (reserved = 0; reserved <= 36; reserved++) {
				for (request = 0; request <= 36;
				    request++)
					test_reservation_case(maximum,
					    used, reserved, request);
			}
		}
	}

	handle = 2;
	reserved = 2;
	used = 0;
	check(ext4fs_journal_state_consume(&handle, &reserved,
	    &used) == 0 &&
	    handle == 1 && reserved == 1 && used == 1,
	    "consume reserved credit");
	check(ext4fs_journal_state_consume(&handle, &reserved,
	    &used) == 0 &&
	    handle == 0 && reserved == 0 && used == 2,
	    "consume final credit");
	check(ext4fs_journal_state_consume(&handle, &reserved,
	    &used) == ENOSPC &&
	    handle == 0 && reserved == 0 && used == 2,
	    "credit exhaustion is non-mutating");

	handle = 1;
	reserved = 0;
	used = 7;
	check(ext4fs_journal_state_consume(&handle, &reserved,
	    &used) == EINVAL &&
	    handle == 1 && reserved == 0 && used == 7,
	    "inconsistent reservation is rejected");
	handle = 1;
	reserved = 1;
	used = 0xffffffffU;
	check(ext4fs_journal_state_consume(&handle, &reserved,
	    &used) == EINVAL &&
	    handle == 1 && reserved == 1 && used == 0xffffffffU,
	    "used-credit overflow is rejected");

	handle = 3;
	reserved = 5;
	check(ext4fs_journal_state_release(&reserved, &handle) == 0 &&
	    handle == 0 && reserved == 2, "release unused credits");
	handle = 3;
	reserved = 2;
	check(ext4fs_journal_state_release(&reserved,
	    &handle) == EINVAL &&
	    handle == 3 && reserved == 2,
	    "invalid credit release is non-mutating");
}

static void
test_diagnostics (void)
{
	static const char *const names[] = {
		"outside commit",
		"transaction validation",
		"ordered data",
		"ordered-data flush",
		"metadata log",
		"revoke log",
		"commit-slot clear",
		"pre-commit flush",
		"journal exposure",
		"journal-exposure flush",
		"commit record",
		"commit flush",
		"checkpoint",
		"checkpoint flush",
		"journal clear",
		"journal-clear flush"
	};
	int stage;

	check(sizeof(names) / sizeof(names[0]) ==
	    EXT4FS_JOURNAL_STAGE_COUNT,
	    "diagnostic stage name count");
	for (stage = EXT4FS_JOURNAL_STAGE_OUTSIDE;
	    stage < EXT4FS_JOURNAL_STAGE_COUNT; stage++)
		check(strcmp(ext4fs_journal_stage_name(stage),
		    names[stage]) == 0, "diagnostic stage name");
	check(strcmp(ext4fs_journal_stage_name(-1), "unknown") == 0,
	    "negative diagnostic stage");
	check(strcmp(ext4fs_journal_stage_name(
	    EXT4FS_JOURNAL_STAGE_COUNT), "unknown") == 0,
	    "past-end diagnostic stage");
}

static void
test_sequences (void)
{
	u_int32_t sequence;

	sequence = 0xffffffffU;
	check(ext4fs_journal_state_sequence(sequence) == 0xffffffffU,
	    "sequence is only observed at begin");
	check(sequence == 0xffffffffU,
	    "empty handle does not consume sequence");
	ext4fs_journal_state_sequence_advance(&sequence);
	check(sequence == 0, "transaction sequence wraparound");
}

static void
test_relations (void)
{
	int first, second;

	first = 0;
	second = 0;
	check(ext4fs_journal_state_metadata_relation(&first, 7,
	    &first, 7) == 0, "duplicate metadata access");
	check(ext4fs_journal_state_metadata_relation(&first, 7,
	    &first, 8) == EINVAL, "buffer block identity mismatch");
	check(ext4fs_journal_state_metadata_relation(&first, 7,
	    &second, 7) == EBUSY, "metadata block alias");
	check(ext4fs_journal_state_metadata_relation(&first, 7,
	    &second, 8) == ENOENT, "independent metadata access");
	check(ext4fs_journal_state_block_relation(9, 9) == 0,
	    "duplicate revoke");
	check(ext4fs_journal_state_block_relation(9, 10) == ENOENT,
	    "independent revoke");
}

int
main (void)
{
	test_admission();
	test_diagnostics();
	test_reservations();
	test_sequences();
	test_relations();
	printf("journal core state tests passed\n");
	return (0);
}
