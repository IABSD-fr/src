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
test_credit_limit (void)
{
	check(ext4fs_journal_state_credit_limit(101, 0, 4096,
	    4U * 1024U * 1024U) == 50,
	    "journal geometry limits transaction credits");
	check(ext4fs_journal_state_credit_limit(101, 41, 4096,
	    4U * 1024U * 1024U) == 20,
	    "journal maximum limits transaction credits");
	check(ext4fs_journal_state_credit_limit(10001, 0, 4096,
	    4U * 1024U * 1024U) == 1024,
	    "dirty metadata limits transaction credits");
	check(ext4fs_journal_state_credit_limit(10001, 0, 1024,
	    4U * 1024U * 1024U) == 4096,
	    "dirty metadata limit follows block size");
	check(ext4fs_journal_state_credit_limit(101, 1, 4096,
	    4U * 1024U * 1024U) == 0,
	    "empty journal maximum rejects credits");
	check(ext4fs_journal_state_credit_limit(101, 0, 0,
	    4U * 1024U * 1024U) == 0,
	    "zero block size rejects credits");
}

static void
test_diagnostics (void)
{
	static const char *const names[] = {
		"outside commit",
		"transaction validation",
		"ordered data",
		"metadata log",
		"revoke log",
		"commit-slot clear",
		"pre-commit flush",
		"journal exposure",
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
test_transaction_states (void)
{
	static const char *const names[] = {
		"new",
		"running",
		"committing",
		"frozen",
		"committed",
		"checkpointing",
		"done"
	};
	enum ext4fs_journal_transaction_state state;
	int value;

	check(sizeof(names) / sizeof(names[0]) ==
	    EXT4FS_JOURNAL_TRANSACTION_STATE_COUNT,
	    "transaction state name count");
	for (value = EXT4FS_JOURNAL_TRANSACTION_NEW;
	    value < EXT4FS_JOURNAL_TRANSACTION_STATE_COUNT; value++)
		check(strcmp(ext4fs_journal_transaction_state_name(value),
		    names[value]) == 0, "transaction state name");
	check(strcmp(ext4fs_journal_transaction_state_name(-1),
	    "unknown") == 0, "negative transaction state");
	check(strcmp(ext4fs_journal_transaction_state_name(
	    EXT4FS_JOURNAL_TRANSACTION_STATE_COUNT), "unknown") == 0,
	    "past-end transaction state");

	state = EXT4FS_JOURNAL_TRANSACTION_NEW;
	check(ext4fs_journal_transaction_transition(&state,
	    EXT4FS_JOURNAL_TRANSACTION_COMMITTED) == EINVAL &&
	    state == EXT4FS_JOURNAL_TRANSACTION_NEW,
	    "invalid transaction transition is non-mutating");
	check(ext4fs_journal_transaction_transition(NULL,
	    EXT4FS_JOURNAL_TRANSACTION_RUNNING) == EINVAL,
	    "null transaction state is rejected");
	check(ext4fs_journal_transaction_transition(&state,
	    EXT4FS_JOURNAL_TRANSACTION_RUNNING) == 0,
	    "new transaction starts running");
	check(ext4fs_journal_transaction_transition(&state,
	    EXT4FS_JOURNAL_TRANSACTION_COMMITTING) == 0,
	    "running transaction starts committing");
	check(ext4fs_journal_transaction_transition(&state,
	    EXT4FS_JOURNAL_TRANSACTION_COMMITTED) == EINVAL &&
	    state == EXT4FS_JOURNAL_TRANSACTION_COMMITTING,
	    "commit cannot become durable before freezing");
	check(ext4fs_journal_transaction_transition(&state,
	    EXT4FS_JOURNAL_TRANSACTION_FROZEN) == 0,
	    "ordered transaction becomes frozen");
	check(ext4fs_journal_transaction_can_overlap(state),
	    "frozen transaction permits a successor");
	check(ext4fs_journal_transaction_transition(&state,
	    EXT4FS_JOURNAL_TRANSACTION_COMMITTED) == 0,
	    "frozen transaction becomes durable");
	check(ext4fs_journal_transaction_transition(&state,
	    EXT4FS_JOURNAL_TRANSACTION_CHECKPOINTING) == 0,
	    "committed transaction starts checkpointing");
	check(ext4fs_journal_transaction_can_overlap(state),
	    "checkpoint permits a running transaction");
	check(ext4fs_journal_transaction_transition(&state,
	    EXT4FS_JOURNAL_TRANSACTION_DONE) == 0,
	    "checkpointed transaction completes");
	check(! ext4fs_journal_transaction_can_overlap(state),
	    "completed transaction is not an overlap window");
	check(ext4fs_journal_transaction_transition(&state,
	    EXT4FS_JOURNAL_TRANSACTION_RUNNING) == EINVAL &&
	    state == EXT4FS_JOURNAL_TRANSACTION_DONE,
	    "completed transaction cannot restart");
	check(! ext4fs_journal_transaction_can_overlap(
	    EXT4FS_JOURNAL_TRANSACTION_NEW),
	    "new transaction prevents overlap");
	check(! ext4fs_journal_transaction_can_overlap(
	    EXT4FS_JOURNAL_TRANSACTION_RUNNING),
	    "running transaction prevents overlap");
	check(! ext4fs_journal_transaction_can_overlap(
	    EXT4FS_JOURNAL_TRANSACTION_COMMITTING),
	    "ordered-data commit prevents overlap");
	check(! ext4fs_journal_transaction_can_overlap(
	    EXT4FS_JOURNAL_TRANSACTION_COMMITTED),
	    "committed transition prevents overlap");
}

static void
test_commit_reasons (void)
{
	static const char *const names[] = {
		"ordinary",
		"fsync",
		"O_SYNC",
		"sync mount",
		"VFS sync",
		"remount",
		"unmount",
		"credit exhaustion",
		"journal pressure",
		"memory pressure",
		"transaction age"
	};
	int reason;

	check(sizeof(names) / sizeof(names[0]) ==
	    EXT4FS_JOURNAL_COMMIT_REASON_COUNT,
	    "commit reason name count");
	for (reason = EXT4FS_JOURNAL_COMMIT_ORDINARY;
	    reason < EXT4FS_JOURNAL_COMMIT_REASON_COUNT; reason++) {
		check(ext4fs_journal_commit_reason_valid(reason),
		    "valid commit reason");
		check(strcmp(ext4fs_journal_commit_reason_name(reason),
		    names[reason]) == 0, "commit reason name");
	}
	check(! ext4fs_journal_commit_reason_valid(-1),
	    "negative commit reason is invalid");
	check(! ext4fs_journal_commit_reason_valid(
	    EXT4FS_JOURNAL_COMMIT_REASON_COUNT),
	    "past-end commit reason is invalid");
	check(strcmp(ext4fs_journal_commit_reason_name(-1),
	    "unknown") == 0, "negative commit reason name");
	check(! ext4fs_journal_commit_reason_requires_checkpoint(
	    EXT4FS_JOURNAL_COMMIT_ORDINARY),
	    "ordinary commit retains checkpoint batch");
	check(! ext4fs_journal_commit_reason_requires_checkpoint(
	    EXT4FS_JOURNAL_COMMIT_O_SYNC),
	    "O_SYNC accepts durable journal commit");
	check(! ext4fs_journal_commit_reason_requires_checkpoint(
	    EXT4FS_JOURNAL_COMMIT_CREDIT_EXHAUSTION),
	    "credit exhaustion accepts durable journal commit");
	check(! ext4fs_journal_commit_reason_requires_checkpoint(
	    EXT4FS_JOURNAL_COMMIT_TRANSACTION_AGE),
	    "aged transaction accepts durable journal commit");
	for (reason = EXT4FS_JOURNAL_COMMIT_FSYNC;
	    reason <= EXT4FS_JOURNAL_COMMIT_MEMORY_PRESSURE; reason++) {
		if (reason == EXT4FS_JOURNAL_COMMIT_O_SYNC ||
		    reason == EXT4FS_JOURNAL_COMMIT_CREDIT_EXHAUSTION)
			continue;
		check(ext4fs_journal_commit_reason_requires_checkpoint(
		    reason), "checkpointing commit reason");
	}
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
	check(ext4fs_journal_state_sequence_before(6, 7),
	    "previous sequence is before fence");
	check(! ext4fs_journal_state_sequence_before(7, 7),
	    "fence sequence is not before itself");
	check(! ext4fs_journal_state_sequence_before(8, 7),
	    "successor sequence is not before fence");
	check(ext4fs_journal_state_sequence_before(0xffffffffU, 0),
	    "wrapped predecessor is before fence");
	check(! ext4fs_journal_state_sequence_before(0, 0xffffffffU),
	    "wrapped successor is not before fence");
}

static void
test_checkpoint_release (void)
{
	u_int32_t blocks, count, free_blocks;

	free_blocks = 40;
	count = 3;
	blocks = 24;
	check(ext4fs_journal_state_checkpoint_release(64,
	    &free_blocks, &count, &blocks, 1, 8) == 0,
	    "partial checkpoint accounting");
	check(free_blocks == 48 && count == 2 && blocks == 16,
	    "partial checkpoint preserves successor accounting");
	check(ext4fs_journal_state_checkpoint_release(64,
	    &free_blocks, &count, &blocks, 2, 16) == 0,
	    "final checkpoint accounting");
	check(free_blocks == 64 && count == 0 && blocks == 0,
	    "final checkpoint restores journal space");
	check(ext4fs_journal_state_checkpoint_release(64,
	    &free_blocks, &count, &blocks, 1, 1) == EINVAL,
	    "checkpoint accounting rejects an empty queue");
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
	test_credit_limit();
	test_transaction_states();
	test_commit_reasons();
	test_sequences();
	test_checkpoint_release();
	test_relations();
	printf("journal core state tests passed\n");
	return (0);
}
