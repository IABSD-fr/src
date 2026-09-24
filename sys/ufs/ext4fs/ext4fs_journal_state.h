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

#ifndef _UFS_EXT4FS_EXT4FS_JOURNAL_STATE_H_
#define _UFS_EXT4FS_EXT4FS_JOURNAL_STATE_H_

int	ext4fs_journal_state_admission (int, int, int, int);
void	ext4fs_journal_state_abort (int *, int *, int);
int	ext4fs_journal_state_reserve (u_int32_t, u_int32_t,
    u_int32_t *, u_int32_t);
int	ext4fs_journal_state_consume (u_int32_t *, u_int32_t *,
    u_int32_t *);
int	ext4fs_journal_state_release (u_int32_t *, u_int32_t *);
u_int32_t	ext4fs_journal_state_sequence (u_int32_t);
void	ext4fs_journal_state_sequence_advance (u_int32_t *);
int	ext4fs_journal_state_metadata_relation (const void *, u_int64_t,
    const void *, u_int64_t);
int	ext4fs_journal_state_block_relation (u_int64_t, u_int64_t);

#endif /* _UFS_EXT4FS_EXT4FS_JOURNAL_STATE_H_ */
