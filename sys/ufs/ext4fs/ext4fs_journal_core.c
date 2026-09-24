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

/*
 * Serialized runtime journal state for ext4fs.
 *
 * Phase 2 deliberately keeps one active handle and one running transaction
 * per mount.  Phase 3 will add transaction serialization to JBD2 and the
 * committing/checkpointing state transitions.  Until then, force_commit()
 * fails closed if a transaction contains work.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/vnode.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>

#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_journal.h>
#include <ufs/ext4fs/ext4fs_journal_state.h>

struct ext4fs_journal_metadata {
	TAILQ_ENTRY(ext4fs_journal_metadata) jm_entry;
	struct buf	*jm_buf;	/* kept B_BUSY while journal-owned */
	struct ext4fs_journal_handle *jm_owner; /* handle until dirtied */
	u_int64_t	 jm_fsblock;
	int		 jm_dirty;
};

struct ext4fs_journal_revoke {
	TAILQ_ENTRY(ext4fs_journal_revoke) jr_entry;
	u_int64_t	 jr_fsblock;
};

struct ext4fs_journal_ordered {
	TAILQ_ENTRY(ext4fs_journal_ordered) jo_entry;
	struct vnode	*jo_vnode;
};

TAILQ_HEAD(ext4fs_journal_metadata_head, ext4fs_journal_metadata);
TAILQ_HEAD(ext4fs_journal_revoke_head, ext4fs_journal_revoke);
TAILQ_HEAD(ext4fs_journal_ordered_head, ext4fs_journal_ordered);

struct ext4fs_journal_transaction {
	struct ext4fs_journal_metadata_head jt_metadata;
	struct ext4fs_journal_revoke_head jt_revokes;
	struct ext4fs_journal_ordered_head jt_ordered;
	u_int32_t	 jt_sequence;
	u_int32_t	 jt_credits_reserved;
	u_int32_t	 jt_credits_used;
};

struct ext4fs_journal {
	struct mutex	 j_lock;
	struct mount	*j_mp;
	struct ext4fs_journal_handle *j_active;
	struct ext4fs_journal_transaction *j_running;
	struct ext4fs_journal_transaction *j_committing;

	struct jbd2_blockmap_entry *j_blockmap;
	u_int64_t	*j_blockset;
	u_int64_t	 j_superblock;
	u_int32_t	 j_blockmap_count;
	u_int32_t	 j_blockset_mask;
	u_int32_t	 j_blocksize;
	u_int32_t	 j_maxlen;
	u_int32_t	 j_first;
	u_int32_t	 j_head;
	u_int32_t	 j_tail;
	u_int32_t	 j_free;
	u_int32_t	 j_next_sequence;
	u_int32_t	 j_max_transaction_credits;
	u_int32_t	 j_features_compat;
	u_int32_t	 j_features_incompat;
	u_int32_t	 j_features_ro_compat;
	u_int32_t	 j_checksum_seed;
	u_int8_t	 j_uuid[16];
	int		 j_aborted;
	int		 j_error;
	int		 j_shutting_down;
};

struct ext4fs_journal_handle {
	struct ext4fs_journal *jh_journal;
	struct ext4fs_journal_transaction *jh_transaction;
	u_int32_t	 jh_credits;
	int		 jh_ended;
};

static void	ext4fs_journal_transaction_free (
		    struct ext4fs_journal_transaction *);
static int	ext4fs_journal_block_member (struct ext4fs_journal *,
		    u_int64_t);
static int	ext4fs_journal_handle_error (struct ext4fs_journal_handle *);

static void
ext4fs_journal_transaction_free (struct ext4fs_journal_transaction *tx)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal_ordered *ordered;
	struct ext4fs_journal_revoke *revoke;

	if (tx == NULL)
		return;
	while ((metadata = TAILQ_FIRST(&tx->jt_metadata)) != NULL) {
		TAILQ_REMOVE(&tx->jt_metadata, metadata, jm_entry);
		/*
		 * Only dirtied, transaction-owned buffers survive a handle.
		 * Teardown without a checkpoint must discard their contents.
		 */
		KASSERT(metadata->jm_dirty);
		KASSERT(metadata->jm_owner == NULL);
		KASSERT(ISSET(metadata->jm_buf->b_flags, B_BUSY));
		SET(metadata->jm_buf->b_flags, B_INVAL);
		brelse(metadata->jm_buf);
		free(metadata, M_UFSMNT, sizeof(*metadata));
	}
	while ((revoke = TAILQ_FIRST(&tx->jt_revokes)) != NULL) {
		TAILQ_REMOVE(&tx->jt_revokes, revoke, jr_entry);
		free(revoke, M_UFSMNT, sizeof(*revoke));
	}
	while ((ordered = TAILQ_FIRST(&tx->jt_ordered)) != NULL) {
		TAILQ_REMOVE(&tx->jt_ordered, ordered, jo_entry);
		vrele(ordered->jo_vnode);
		free(ordered, M_UFSMNT, sizeof(*ordered));
	}
	free(tx, M_UFSMNT, sizeof(*tx));
}

static int
ext4fs_journal_block_member (struct ext4fs_journal *journal,
    u_int64_t fsblock)
{
	u_int32_t hash;

	if (journal->j_blockset == NULL)
		return (0);
	hash = ((u_int32_t)fsblock ^ (u_int32_t)(fsblock >> 32)) *
	    2654435761U;
	hash &= journal->j_blockset_mask;
	while (journal->j_blockset[hash] != 0) {
		if (journal->j_blockset[hash] == fsblock)
			return (1);
		hash = (hash + 1) & journal->j_blockset_mask;
	}
	return (0);
}

static int
ext4fs_journal_handle_error (struct ext4fs_journal_handle *handle)
{
	struct ext4fs_journal *journal;

	if (handle == NULL || handle->jh_journal == NULL || handle->jh_ended)
		return (EINVAL);
	journal = handle->jh_journal;
	if (journal->j_active != handle ||
	    journal->j_running != handle->jh_transaction)
		return (EINVAL);
	return (ext4fs_journal_state_admission(0, journal->j_aborted,
	    journal->j_error, journal->j_shutting_down));
}

int
ext4fs_journal_init (struct mount *mp)
{
	struct jbd2_replay_ctx ctx;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	struct ufsmount *ump;
	u_int64_t superblock;
	u_int32_t usable;
	int error;

	ump = VFSTOUFS(mp);
	fs = ump->um_e4fs;
	if (fs->m_journal != NULL)
		return (EBUSY);
	if (!(fs->m_feature_compat & EXT4FS_FEATURE_COMPAT_HAS_JOURNAL))
		return (0);

	error = jbd2_journal_open(ump->um_devvp, fs, &ctx, &superblock);
	if (error)
		return (error);
	if (ctx.rc_start != 0) {
		printf("ext4fs: refusing runtime journal with non-empty log\n");
		jbd2_journal_close(&ctx);
		return (EINVAL);
	}

	journal = malloc(sizeof(*journal), M_UFSMNT, M_WAITOK | M_ZERO);
	mtx_init(&journal->j_lock, IPL_NONE);
	journal->j_mp = mp;
	journal->j_superblock = superblock;
	journal->j_blocksize = ctx.rc_blocksize;
	journal->j_maxlen = ctx.rc_maxlen;
	journal->j_first = ctx.rc_first;
	journal->j_head = ctx.rc_first;
	journal->j_tail = ctx.rc_first;
	journal->j_free = ctx.rc_maxlen - ctx.rc_first;
	journal->j_next_sequence = ctx.rc_sequence;
	journal->j_features_compat = ctx.rc_features_compat;
	journal->j_features_incompat = ctx.rc_features_incompat;
	journal->j_features_ro_compat = ctx.rc_features_ro_compat;
	journal->j_checksum_seed = ctx.rc_checksum_seed;
	memcpy(journal->j_uuid, ctx.rc_uuid, sizeof(journal->j_uuid));

	usable = ctx.rc_maxlen - ctx.rc_first;
	/*
	 * In the worst case each metadata credit needs one descriptor block
	 * and one payload block.  Keep one further block for the commit record.
	 */
	journal->j_max_transaction_credits = usable > 1 ?
	    (usable - 1) / 2 : 0;
	if (ctx.rc_max_transaction != 0 &&
	    ctx.rc_max_transaction < journal->j_max_transaction_credits)
		journal->j_max_transaction_credits = ctx.rc_max_transaction;

	journal->j_blockmap = ctx.rc_blockmap;
	journal->j_blockmap_count = ctx.rc_blockmap_count;
	journal->j_blockset = ctx.rc_blockset;
	journal->j_blockset_mask = ctx.rc_blockset_mask;
	ctx.rc_blockmap = NULL;
	ctx.rc_blockset = NULL;
	jbd2_journal_close(&ctx);

	fs->m_journal = journal;
	return (0);
}

void
ext4fs_journal_destroy (struct mount *mp)
{
	struct ext4fs_journal *journal;
	struct ext4fs_journal_transaction *committing, *running;
	struct m_ext4fs *fs;

	fs = VFSTOUFS(mp)->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL)
		return;

	mtx_enter(&journal->j_lock);
	journal->j_shutting_down = 1;
	wakeup(&journal->j_active);
	while (journal->j_active != NULL)
		msleep_nsec(&journal->j_active, &journal->j_lock, PRIBIO,
		    "e4jdrain", INFSLP);
	running = journal->j_running;
	committing = journal->j_committing;
	journal->j_running = NULL;
	journal->j_committing = NULL;
	fs->m_journal = NULL;
	mtx_leave(&journal->j_lock);

	ext4fs_journal_transaction_free(running);
	ext4fs_journal_transaction_free(committing);
	if (journal->j_blockset != NULL)
		free(journal->j_blockset, M_TEMP,
		    (journal->j_blockset_mask + 1) *
		    sizeof(*journal->j_blockset));
	if (journal->j_blockmap != NULL)
		free(journal->j_blockmap, M_TEMP,
		    journal->j_blockmap_count * sizeof(*journal->j_blockmap));
	free(journal, M_UFSMNT, sizeof(*journal));
}

void
ext4fs_journal_abort (struct mount *mp, int error)
{
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;

	fs = VFSTOUFS(mp)->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL)
		return;
	if (error == 0)
		error = EIO;

	mtx_enter(&journal->j_lock);
	ext4fs_journal_state_abort(&journal->j_aborted, &journal->j_error,
	    error);
	wakeup(&journal->j_active);
	mtx_leave(&journal->j_lock);
}

int
ext4fs_journal_begin (struct mount *mp, unsigned int credits,
    struct ext4fs_journal_handle **handlep)
{
	struct ext4fs_journal *journal;
	struct ext4fs_journal_handle *handle;
	struct ext4fs_journal_transaction *candidate, *tx;
	struct m_ext4fs *fs;
	int error;

	if (handlep == NULL || credits == 0)
		return (EINVAL);
	*handlep = NULL;
	fs = VFSTOUFS(mp)->um_e4fs;
	if (fs->m_read_only || (mp->mnt_flag & MNT_RDONLY))
		return (EROFS);
	journal = fs->m_journal;
	if (journal == NULL)
		return (EOPNOTSUPP);

	handle = malloc(sizeof(*handle), M_UFSMNT, M_WAITOK | M_ZERO);
	candidate = malloc(sizeof(*candidate), M_UFSMNT, M_WAITOK | M_ZERO);
	TAILQ_INIT(&candidate->jt_metadata);
	TAILQ_INIT(&candidate->jt_revokes);
	TAILQ_INIT(&candidate->jt_ordered);

	mtx_enter(&journal->j_lock);
	while ((error = ext4fs_journal_state_admission(
	    journal->j_active != NULL, journal->j_aborted, journal->j_error,
	    journal->j_shutting_down)) == EBUSY)
		msleep_nsec(&journal->j_active, &journal->j_lock, PRIBIO,
		    "e4jbegin", INFSLP);
	if (error == 0) {
		tx = journal->j_running != NULL ? journal->j_running : candidate;
		error = ext4fs_journal_state_reserve(
		    journal->j_max_transaction_credits, tx->jt_credits_used,
		    &tx->jt_credits_reserved, credits);
		if (error == 0) {
			if (journal->j_running == NULL) {
				/* Commit transition consumes this sequence. */
				tx->jt_sequence = ext4fs_journal_state_sequence(
				    journal->j_next_sequence);
				journal->j_running = tx;
				candidate = NULL;
			}
			handle->jh_journal = journal;
			handle->jh_transaction = tx;
			handle->jh_credits = credits;
			journal->j_active = handle;
		}
	}
	mtx_leave(&journal->j_lock);

	if (candidate != NULL)
		ext4fs_journal_transaction_free(candidate);
	if (error) {
		free(handle, M_UFSMNT, sizeof(*handle));
		return (error);
	}
	*handlep = handle;
	return (0);
}

int
ext4fs_journal_add_ordered (struct ext4fs_journal_handle *handle,
    struct vnode *vp)
{
	struct ext4fs_journal_ordered *candidate, *ordered;
	struct ext4fs_journal *journal;
	int error;

	if (handle == NULL || vp == NULL || handle->jh_journal == NULL)
		return (EINVAL);
	journal = handle->jh_journal;
	if (vp->v_type != VREG || vp->v_mount != journal->j_mp)
		return (EINVAL);

	candidate = malloc(sizeof(*candidate), M_UFSMNT, M_WAITOK | M_ZERO);
	candidate->jo_vnode = vp;
	vref(vp);

	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	TAILQ_FOREACH(ordered, &handle->jh_transaction->jt_ordered,
	    jo_entry) {
		if (ordered->jo_vnode == vp) {
			error = 0;
			goto out;
		}
	}
	TAILQ_INSERT_TAIL(&handle->jh_transaction->jt_ordered, candidate,
	    jo_entry);
	candidate = NULL;
	error = 0;

out:
	mtx_leave(&journal->j_lock);
	if (candidate != NULL) {
		vrele(candidate->jo_vnode);
		free(candidate, M_UFSMNT, sizeof(*candidate));
	}
	return (error);
}

int
ext4fs_journal_get_write_access (struct ext4fs_journal_handle *handle,
    struct buf *bp, u_int64_t fsblock)
{
	struct ext4fs_journal_metadata *candidate, *metadata;
	struct ext4fs_journal_revoke *revoke;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	int error;

	if (handle == NULL || bp == NULL || handle->jh_journal == NULL ||
	    bp->b_data == NULL || !ISSET(bp->b_flags, B_BUSY))
		return (EINVAL);
	journal = handle->jh_journal;
	if (bp->b_bcount != journal->j_blocksize)
		return (EINVAL);
	fs = VFSTOUFS(journal->j_mp)->um_e4fs;
	if (fsblock >= fs->m_blocks_count ||
	    ext4fs_journal_block_member(journal, fsblock))
		return (EINVAL);
	candidate = malloc(sizeof(*candidate), M_UFSMNT, M_WAITOK | M_ZERO);
	candidate->jm_buf = bp;
	candidate->jm_owner = handle;
	candidate->jm_fsblock = fsblock;

	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	TAILQ_FOREACH(metadata, &handle->jh_transaction->jt_metadata,
	    jm_entry) {
		error = ext4fs_journal_state_metadata_relation(metadata->jm_buf,
		    metadata->jm_fsblock, bp, fsblock);
		if (error != ENOENT)
			goto out;
	}
	TAILQ_FOREACH(revoke, &handle->jh_transaction->jt_revokes, jr_entry) {
		if (ext4fs_journal_state_block_relation(revoke->jr_fsblock,
		    fsblock) == 0) {
			error = EBUSY;
			goto out;
		}
	}
	TAILQ_INSERT_TAIL(&handle->jh_transaction->jt_metadata, candidate,
	    jm_entry);
	/* The handle now owns the busy buffer until dirty_metadata() or end(). */
	candidate = NULL;
	error = 0;

out:
	mtx_leave(&journal->j_lock);
	if (candidate != NULL)
		free(candidate, M_UFSMNT, sizeof(*candidate));
	return (error);
}

int
ext4fs_journal_dirty_metadata (struct ext4fs_journal_handle *handle,
    struct buf *bp)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal *journal;
	int error;

	if (handle == NULL || bp == NULL || handle->jh_journal == NULL ||
	    !ISSET(bp->b_flags, B_BUSY))
		return (EINVAL);
	journal = handle->jh_journal;
	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	TAILQ_FOREACH(metadata, &handle->jh_transaction->jt_metadata,
	    jm_entry) {
		if (metadata->jm_buf != bp)
			continue;
		if (metadata->jm_dirty) {
			error = 0;
			goto out;
		}
		if (metadata->jm_owner != handle) {
			error = EBUSY;
			goto out;
		}
		error = ext4fs_journal_state_consume(&handle->jh_credits,
		    &handle->jh_transaction->jt_credits_reserved,
		    &handle->jh_transaction->jt_credits_used);
		if (error)
			goto out;
		metadata->jm_dirty = 1;
		/* Transfer the busy buffer from the handle to the transaction. */
		metadata->jm_owner = NULL;
		goto out;
	}
	error = EINVAL;

out:
	mtx_leave(&journal->j_lock);
	return (error);
}

int
ext4fs_journal_revoke (struct ext4fs_journal_handle *handle,
    u_int64_t fsblock)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal_revoke *candidate, *revoke;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	int error;

	if (handle == NULL || handle->jh_journal == NULL)
		return (EINVAL);
	journal = handle->jh_journal;
	fs = VFSTOUFS(journal->j_mp)->um_e4fs;
	if (fsblock >= fs->m_blocks_count ||
	    ext4fs_journal_block_member(journal, fsblock))
		return (EINVAL);
	candidate = malloc(sizeof(*candidate), M_UFSMNT, M_WAITOK | M_ZERO);
	candidate->jr_fsblock = fsblock;

	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	TAILQ_FOREACH(revoke, &handle->jh_transaction->jt_revokes, jr_entry) {
		if (ext4fs_journal_state_block_relation(revoke->jr_fsblock,
		    fsblock) == 0) {
			error = 0;
			goto out;
		}
	}
	TAILQ_FOREACH(metadata, &handle->jh_transaction->jt_metadata,
	    jm_entry) {
		if (ext4fs_journal_state_block_relation(metadata->jm_fsblock,
		    fsblock) == 0) {
			error = EBUSY;
			goto out;
		}
	}
	error = ext4fs_journal_state_consume(&handle->jh_credits,
	    &handle->jh_transaction->jt_credits_reserved,
	    &handle->jh_transaction->jt_credits_used);
	if (error)
		goto out;
	TAILQ_INSERT_TAIL(&handle->jh_transaction->jt_revokes, candidate,
	    jr_entry);
	candidate = NULL;

out:
	mtx_leave(&journal->j_lock);
	if (candidate != NULL)
		free(candidate, M_UFSMNT, sizeof(*candidate));
	return (error);
}

int
ext4fs_journal_end (struct ext4fs_journal_handle *handle)
{
	struct ext4fs_journal_metadata_head unused;
	struct ext4fs_journal_metadata *metadata, *next;
	struct ext4fs_journal_transaction *empty;
	struct ext4fs_journal *journal;
	int error, release_error;

	if (handle == NULL || handle->jh_journal == NULL || handle->jh_ended)
		return (EINVAL);
	journal = handle->jh_journal;
	empty = NULL;
	TAILQ_INIT(&unused);

	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error == ESHUTDOWN)
		error = 0;
	if (journal->j_active != handle ||
	    journal->j_running != handle->jh_transaction) {
		mtx_leave(&journal->j_lock);
		return (EINVAL);
	}
	TAILQ_FOREACH_SAFE(metadata, &handle->jh_transaction->jt_metadata,
	    jm_entry, next) {
		if (metadata->jm_owner != handle || metadata->jm_dirty)
			continue;
		TAILQ_REMOVE(&handle->jh_transaction->jt_metadata, metadata,
		    jm_entry);
		TAILQ_INSERT_TAIL(&unused, metadata, jm_entry);
	}
	release_error = ext4fs_journal_state_release(
	    &handle->jh_transaction->jt_credits_reserved,
	    &handle->jh_credits);
	KASSERT(release_error == 0);
	if (error == 0)
		error = release_error;
	handle->jh_ended = 1;
	journal->j_active = NULL;
	if (TAILQ_EMPTY(&handle->jh_transaction->jt_metadata) &&
	    TAILQ_EMPTY(&handle->jh_transaction->jt_revokes)) {
		KASSERT(handle->jh_transaction->jt_credits_used == 0);
		KASSERT(handle->jh_transaction->jt_credits_reserved == 0);
		empty = handle->jh_transaction;
		journal->j_running = NULL;
	}
	wakeup(&journal->j_active);
	mtx_leave(&journal->j_lock);

	while ((metadata = TAILQ_FIRST(&unused)) != NULL) {
		TAILQ_REMOVE(&unused, metadata, jm_entry);
		KASSERT(metadata->jm_owner == handle);
		KASSERT(!metadata->jm_dirty);
		KASSERT(ISSET(metadata->jm_buf->b_flags, B_BUSY));
		brelse(metadata->jm_buf);
		free(metadata, M_UFSMNT, sizeof(*metadata));
	}
	ext4fs_journal_transaction_free(empty);
	free(handle, M_UFSMNT, sizeof(*handle));
	return (error);
}

int
ext4fs_journal_force_commit (struct mount *mp)
{
	struct ext4fs_journal_transaction *empty;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	int error;

	fs = VFSTOUFS(mp)->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL)
		return (0);
	empty = NULL;

	mtx_enter(&journal->j_lock);
	while ((error = ext4fs_journal_state_admission(
	    journal->j_active != NULL, journal->j_aborted, journal->j_error,
	    journal->j_shutting_down)) == EBUSY)
		msleep_nsec(&journal->j_active, &journal->j_lock, PRIBIO,
		    "e4jcommit", INFSLP);
	if (error == 0 && journal->j_committing != NULL)
		error = EBUSY;
	else if (error == 0 && journal->j_running == NULL)
		error = 0;
	else if (error == 0 &&
	    (!TAILQ_EMPTY(&journal->j_running->jt_metadata) ||
	    !TAILQ_EMPTY(&journal->j_running->jt_revokes)))
		error = EOPNOTSUPP;
	else if (error == 0) {
		empty = journal->j_running;
		journal->j_running = NULL;
	}
	mtx_leave(&journal->j_lock);

	ext4fs_journal_transaction_free(empty);
	return (error);
}
