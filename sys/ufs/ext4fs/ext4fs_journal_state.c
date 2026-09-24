/*
 * Copyright (c) 2026 kmx.io.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/errno.h>

#include <ufs/ext4fs/ext4fs_journal_state.h>

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

void
ext4fs_journal_state_abort (int *aborted, int *stored_error, int error)
{
	if (error == 0)
		error = EIO;
	if (!*aborted) {
		*aborted = 1;
		*stored_error = error;
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
