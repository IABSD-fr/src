/*
 * Copyright (c) 2026 kmx.io.
 *
 * Permission to use, copy, modify, and distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/errno.h>

#include <ufs/ext4fs/ext4fs_journal_state.h>

const char *
ext4fs_journal_transaction_state_name (
    enum ext4fs_journal_transaction_state state)
{
	static const char *const
	    names[EXT4FS_JOURNAL_TRANSACTION_STATE_COUNT] = {
		[EXT4FS_JOURNAL_TRANSACTION_NEW] = "new",
		[EXT4FS_JOURNAL_TRANSACTION_RUNNING] = "running",
		[EXT4FS_JOURNAL_TRANSACTION_COMMITTING] = "committing",
		[EXT4FS_JOURNAL_TRANSACTION_FROZEN] = "frozen",
		[EXT4FS_JOURNAL_TRANSACTION_COMMITTED] = "committed",
		[EXT4FS_JOURNAL_TRANSACTION_CHECKPOINTING] =
		    "checkpointing",
		[EXT4FS_JOURNAL_TRANSACTION_DONE] = "done"
	};

	if (state < 0 ||
	    state >= EXT4FS_JOURNAL_TRANSACTION_STATE_COUNT ||
	    ! names[state])
		return ("unknown");
	return (names[state]);
}

int
ext4fs_journal_transaction_transition (
    enum ext4fs_journal_transaction_state *state,
    enum ext4fs_journal_transaction_state next)
{
	if (state == NULL)
		return (EINVAL);
	switch (*state) {
	case EXT4FS_JOURNAL_TRANSACTION_NEW:
		if (next != EXT4FS_JOURNAL_TRANSACTION_RUNNING)
			return (EINVAL);
		break;
	case EXT4FS_JOURNAL_TRANSACTION_RUNNING:
		if (next != EXT4FS_JOURNAL_TRANSACTION_COMMITTING)
			return (EINVAL);
		break;
	case EXT4FS_JOURNAL_TRANSACTION_COMMITTING:
		if (next != EXT4FS_JOURNAL_TRANSACTION_FROZEN)
			return (EINVAL);
		break;
	case EXT4FS_JOURNAL_TRANSACTION_FROZEN:
		if (next != EXT4FS_JOURNAL_TRANSACTION_COMMITTED)
			return (EINVAL);
		break;
	case EXT4FS_JOURNAL_TRANSACTION_COMMITTED:
		if (next != EXT4FS_JOURNAL_TRANSACTION_CHECKPOINTING)
			return (EINVAL);
		break;
	case EXT4FS_JOURNAL_TRANSACTION_CHECKPOINTING:
		if (next != EXT4FS_JOURNAL_TRANSACTION_DONE)
			return (EINVAL);
		break;
	default:
		return (EINVAL);
	}
	*state = next;
	return (0);
}

int
ext4fs_journal_transaction_can_overlap (
    enum ext4fs_journal_transaction_state state)
{
	return (state == EXT4FS_JOURNAL_TRANSACTION_FROZEN ||
	    state == EXT4FS_JOURNAL_TRANSACTION_CHECKPOINTING);
}

int
ext4fs_journal_state_admission (int active, int aborted, int error,
    int shutting_down)
{
	if (shutting_down)
		return (ESHUTDOWN);
	if (aborted)
		return (error != 0 ? error : EIO);
	if (active)
		return (EBUSY);
	return (0);
}

int
ext4fs_journal_state_abort (int *aborted, int *stored_error, int error)
{
	if (error == 0)
		error = EIO;
	if (! *aborted) {
		*aborted = 1;
		*stored_error = error;
		return (1);
	}
	return (0);
}

const char *
ext4fs_journal_stage_name (enum ext4fs_journal_stage stage)
{
	static const char *const
	    names[EXT4FS_JOURNAL_STAGE_COUNT] = {
		[EXT4FS_JOURNAL_STAGE_OUTSIDE] = "outside commit",
		[EXT4FS_JOURNAL_STAGE_VALIDATE] =
		    "transaction validation",
		[EXT4FS_JOURNAL_STAGE_ORDERED_DATA] = "ordered data",
		[EXT4FS_JOURNAL_STAGE_METADATA] = "metadata log",
		[EXT4FS_JOURNAL_STAGE_REVOKES] = "revoke log",
		[EXT4FS_JOURNAL_STAGE_COMMIT_CLEAR] =
		    "commit-slot clear",
		[EXT4FS_JOURNAL_STAGE_PRECOMMIT_FLUSH] =
		    "pre-commit flush",
		[EXT4FS_JOURNAL_STAGE_EXPOSE] = "journal exposure",
		[EXT4FS_JOURNAL_STAGE_COMMIT] = "commit record",
		[EXT4FS_JOURNAL_STAGE_COMMIT_FLUSH] = "commit flush",
		[EXT4FS_JOURNAL_STAGE_CHECKPOINT] = "checkpoint",
		[EXT4FS_JOURNAL_STAGE_CHECKPOINT_FLUSH] =
		    "checkpoint flush",
		[EXT4FS_JOURNAL_STAGE_CLEAR] = "journal clear",
		[EXT4FS_JOURNAL_STAGE_CLEAR_FLUSH] =
		    "journal-clear flush"
	};

	if (stage < 0 || stage >= EXT4FS_JOURNAL_STAGE_COUNT ||
	    ! names[stage])
		return ("unknown");
	return (names[stage]);
}

const char *
ext4fs_journal_commit_reason_name (
    enum ext4fs_journal_commit_reason reason)
{
	static const char *const
	    names[EXT4FS_JOURNAL_COMMIT_REASON_COUNT] = {
		[EXT4FS_JOURNAL_COMMIT_ORDINARY] = "ordinary",
		[EXT4FS_JOURNAL_COMMIT_FSYNC] = "fsync",
		[EXT4FS_JOURNAL_COMMIT_O_SYNC] = "O_SYNC",
		[EXT4FS_JOURNAL_COMMIT_SYNC_MOUNT] = "sync mount",
		[EXT4FS_JOURNAL_COMMIT_VFS_SYNC] = "VFS sync",
		[EXT4FS_JOURNAL_COMMIT_REMOUNT] = "remount",
		[EXT4FS_JOURNAL_COMMIT_UNMOUNT] = "unmount",
		[EXT4FS_JOURNAL_COMMIT_CREDIT_EXHAUSTION] =
		    "credit exhaustion",
		[EXT4FS_JOURNAL_COMMIT_JOURNAL_PRESSURE] =
		    "journal pressure",
		[EXT4FS_JOURNAL_COMMIT_MEMORY_PRESSURE] =
		    "memory pressure",
		[EXT4FS_JOURNAL_COMMIT_TRANSACTION_AGE] =
		    "transaction age"
	};

	if (! ext4fs_journal_commit_reason_valid(reason))
		return ("unknown");
	return (names[reason]);
}

int
ext4fs_journal_commit_reason_valid (
    enum ext4fs_journal_commit_reason reason)
{
	return (reason >= EXT4FS_JOURNAL_COMMIT_ORDINARY &&
	    reason < EXT4FS_JOURNAL_COMMIT_REASON_COUNT);
}

int
ext4fs_journal_commit_reason_requires_checkpoint (
    enum ext4fs_journal_commit_reason reason)
{
	switch (reason) {
	case EXT4FS_JOURNAL_COMMIT_FSYNC:
	case EXT4FS_JOURNAL_COMMIT_SYNC_MOUNT:
	case EXT4FS_JOURNAL_COMMIT_VFS_SYNC:
	case EXT4FS_JOURNAL_COMMIT_REMOUNT:
	case EXT4FS_JOURNAL_COMMIT_UNMOUNT:
	case EXT4FS_JOURNAL_COMMIT_JOURNAL_PRESSURE:
	case EXT4FS_JOURNAL_COMMIT_MEMORY_PRESSURE:
		return (1);
	default:
		return (0);
	}
}

int
ext4fs_journal_state_reserve (u_int32_t maximum, u_int32_t used,
    u_int32_t *reserved, u_int32_t request)
{
	u_int32_t available;

	if (request == 0)
		return (EINVAL);
	if (used > maximum || *reserved > maximum - used)
		return (ENOSPC);
	available = maximum - used - *reserved;
	if (request > available)
		return (ENOSPC);
	*reserved += request;
	return (0);
}

int
ext4fs_journal_state_consume (u_int32_t *handle_credits,
    u_int32_t *reserved, u_int32_t *used)
{
	if (*handle_credits == 0)
		return (ENOSPC);
	if (*reserved == 0 || *used == 0xffffffffU)
		return (EINVAL);
	(*handle_credits)--;
	(*reserved)--;
	(*used)++;
	return (0);
}

int
ext4fs_journal_state_release (u_int32_t *reserved,
    u_int32_t *handle_credits)
{
	if (*reserved < *handle_credits)
		return (EINVAL);
	*reserved -= *handle_credits;
	*handle_credits = 0;
	return (0);
}

u_int32_t
ext4fs_journal_state_credit_limit (u_int32_t usable,
    u_int32_t max_transaction, u_int32_t block_size,
    u_int32_t dirty_bytes)
{
	u_int32_t limit, memory_limit, transaction_limit;

	limit = usable > 1 ? (usable - 1) / 2 : 0;
	if (max_transaction != 0) {
		transaction_limit = max_transaction > 1 ?
		    (max_transaction - 1) / 2 : 0;
		if (transaction_limit < limit)
			limit = transaction_limit;
	}
	if (block_size == 0)
		return (0);
	memory_limit = dirty_bytes / block_size;
	if (memory_limit < limit)
		limit = memory_limit;
	return (limit);
}

u_int32_t
ext4fs_journal_state_sequence (u_int32_t next_sequence)
{
	return (next_sequence);
}

void
ext4fs_journal_state_sequence_advance (u_int32_t *next_sequence)
{
	(*next_sequence)++;
}

int
ext4fs_journal_state_sequence_before (u_int32_t sequence,
    u_int32_t fence)
{
	u_int32_t distance;

	distance = fence - sequence;
	return (distance != 0 && distance < 0x80000000U);
}

int
ext4fs_journal_state_checkpoint_release (u_int32_t usable,
    u_int32_t *free_blocks, u_int32_t *transaction_count,
    u_int32_t *committed_blocks, u_int32_t released_count,
    u_int32_t released_blocks)
{
	if (free_blocks == NULL || transaction_count == NULL ||
	    committed_blocks == NULL || *free_blocks > usable ||
	    released_count > *transaction_count ||
	    released_blocks > *committed_blocks ||
	    released_blocks > usable - *free_blocks)
		return (EINVAL);
	*free_blocks += released_blocks;
	*transaction_count -= released_count;
	*committed_blocks -= released_blocks;
	return (0);
}

int
ext4fs_journal_state_metadata_relation (const void *existing_object,
    u_int64_t existing_block, const void *candidate_object,
    u_int64_t candidate_block)
{
	if (existing_object == candidate_object)
		return (existing_block == candidate_block ? 0 : EINVAL);
	if (existing_block == candidate_block)
		return (EBUSY);
	return (ENOENT);
}

int
ext4fs_journal_state_block_relation (u_int64_t existing_block,
    u_int64_t candidate_block)
{
	return (existing_block == candidate_block ? 0 : ENOENT);
}
