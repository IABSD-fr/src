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
 * The initial implementation keeps one active handle, one running
 * transaction, and one committing transaction per mount.  Commits are
 * synchronous and checkpoint metadata before releasing the log space.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/lock.h>
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
	int		 j_commit_busy;
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
static void	ext4fs_journal_abort_locked (struct ext4fs_journal *, int);
static void	ext4fs_journal_checksum_ctx (struct ext4fs_journal *,
		    struct jbd2_replay_ctx *);
static u_int32_t ext4fs_journal_next_block (struct ext4fs_journal *,
		    u_int32_t);
static int	ext4fs_journal_write_block (struct ext4fs_journal *, u_int32_t,
		    const void *);
static int	ext4fs_journal_write_super (struct ext4fs_journal *, u_int32_t,
		    u_int32_t, u_int32_t);
static int	ext4fs_journal_set_recover (struct ext4fs_journal *);
static u_int32_t ext4fs_journal_tag_bytes (struct ext4fs_journal *, int);
static u_int32_t ext4fs_journal_descriptor_count (
		    struct ext4fs_journal *, struct ext4fs_journal_metadata *);
static int	ext4fs_journal_log_blocks (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *, u_int32_t *);
static int	ext4fs_journal_write_metadata (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *, u_int32_t *, void *,
		    void *);
static int	ext4fs_journal_write_revokes (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *, u_int32_t *, void *);
static int	ext4fs_journal_write_commit (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *, u_int32_t *, void *);
static int	ext4fs_journal_flush_ordered (
		    struct ext4fs_journal_transaction *);
static int	ext4fs_journal_checkpoint (struct ext4fs_journal_transaction *);
static int	ext4fs_journal_commit_transaction (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *, u_int32_t *);

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

static void
ext4fs_journal_abort_locked (struct ext4fs_journal *journal, int error)
{
	struct m_ext4fs *fs;

	fs = VFSTOUFS(journal->j_mp)->um_e4fs;
	ext4fs_journal_state_abort(&journal->j_aborted, &journal->j_error,
	    error);
	fs->m_state = EXT4FS_STATE_ERROR;
	fs->m_sble.sb_state = htole16(fs->m_state);
	fs->m_fs_was_modified = 1;
	journal->j_mp->mnt_flag |= MNT_RDONLY;
	wakeup(&journal->j_active);
	wakeup(&journal->j_committing);
}

static void
ext4fs_journal_checksum_ctx (struct ext4fs_journal *journal,
    struct jbd2_replay_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->rc_blocksize = journal->j_blocksize;
	ctx->rc_first = journal->j_first;
	ctx->rc_maxlen = journal->j_maxlen;
	ctx->rc_features_compat = journal->j_features_compat;
	ctx->rc_features_incompat = journal->j_features_incompat;
	ctx->rc_features_ro_compat = journal->j_features_ro_compat;
	ctx->rc_checksum_seed = journal->j_checksum_seed;
	memcpy(ctx->rc_uuid, journal->j_uuid, sizeof(ctx->rc_uuid));
}

static u_int32_t
ext4fs_journal_next_block (struct ext4fs_journal *journal, u_int32_t block)
{
	block++;
	if (block >= journal->j_maxlen)
		block = journal->j_first;
	return (block);
}

static int
ext4fs_journal_write_block (struct ext4fs_journal *journal,
    u_int32_t jblock, const void *data)
{
	struct m_ext4fs *fs;
	struct ufsmount *ump;
	struct buf *bp;
	u_int64_t fsblock;

	if (jblock < journal->j_first ||
	    jblock >= journal->j_blockmap_count)
		return (EINVAL);
	ump = VFSTOUFS(journal->j_mp);
	fs = ump->um_e4fs;
	fsblock = journal->j_blockmap[jblock].jb_fsblock;
	if (fsblock == 0)
		return (EIO);
	bp = getblk(ump->um_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, fsblock), journal->j_blocksize,
	    0, INFSLP);
	memcpy(bp->b_data, data, journal->j_blocksize);
	/* Journal ordering must not be weakened by an asynchronous mount. */
	SET(bp->b_flags, B_NOCACHE);
	return (bwrite(bp));
}

static int
ext4fs_journal_write_super (struct ext4fs_journal *journal,
    u_int32_t start, u_int32_t sequence, u_int32_t head)
{
	struct jbd2_replay_ctx ctx;
	struct jbd2_superblock *jsb;
	struct m_ext4fs *fs;
	struct ufsmount *ump;
	struct buf *bp;
	int error;

	ump = VFSTOUFS(journal->j_mp);
	fs = ump->um_e4fs;
	bp = NULL;
	error = bread(ump->um_devvp,
	    (daddr_t)EXT4FS_FSBTODB(fs, journal->j_superblock),
	    journal->j_blocksize, &bp);
	if (error) {
		if (bp != NULL)
			brelse(bp);
		return (error);
	}
	jsb = (struct jbd2_superblock *)bp->b_data;
	ext4fs_journal_checksum_ctx(journal, &ctx);
	if (betoh32(jsb->s_header.h_magic) != JBD2_MAGIC ||
	    betoh32(jsb->s_header.h_blocktype) != JBD2_SUPERBLOCK_V2 ||
	    betoh32(jsb->s_blocksize) != journal->j_blocksize ||
	    betoh32(jsb->s_maxlen) != journal->j_maxlen ||
	    betoh32(jsb->s_first) != journal->j_first ||
	    betoh32(jsb->s_feature_compat) != journal->j_features_compat ||
	    betoh32(jsb->s_feature_incompat) !=
	    journal->j_features_incompat ||
	    betoh32(jsb->s_feature_ro_compat) !=
	    journal->j_features_ro_compat ||
	    memcmp(jsb->s_uuid, journal->j_uuid, sizeof(journal->j_uuid)) != 0) {
		brelse(bp);
		return (EINVAL);
	}
	if (!jbd2_superblock_csum_verify(&ctx, jsb)) {
		brelse(bp);
		return (EINVAL);
	}
	jsb->s_start = htobe32(start);
	jsb->s_sequence = htobe32(sequence);
	jsb->s_head = htobe32(head);
	jbd2_superblock_csum_set(&ctx, jsb);
	SET(bp->b_flags, B_NOCACHE);
	return (bwrite(bp));
}

static int
ext4fs_journal_set_recover (struct ext4fs_journal *journal)
{
	struct m_ext4fs *fs;
	struct ufsmount *ump;
	int error;

	ump = VFSTOUFS(journal->j_mp);
	fs = ump->um_e4fs;
	if (fs->m_feature_incompat & EXT4FS_FEATURE_INCOMPAT_RECOVER)
		return (0);
	fs->m_feature_incompat |= EXT4FS_FEATURE_INCOMPAT_RECOVER;
	fs->m_sble.sb_feature_incompat = htole32(fs->m_feature_incompat);
	fs->m_fs_was_modified = 1;
	error = ext4fs_sbwrite(journal->j_mp);
	if (error == 0)
		error = jbd2_flush_device(ump->um_devvp, curproc);
	return (error);
}

static void
ext4fs_journal_put16 (void *buf, u_int32_t offset, u_int16_t value)
{
	value = htobe16(value);
	memcpy((char *)buf + offset, &value, sizeof(value));
}

static void
ext4fs_journal_put32 (void *buf, u_int32_t offset, u_int32_t value)
{
	value = htobe32(value);
	memcpy((char *)buf + offset, &value, sizeof(value));
}

static u_int32_t
ext4fs_journal_tag_bytes (struct ext4fs_journal *journal, int same_uuid)
{
	u_int32_t bytes;

	if (journal->j_features_incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3)
		bytes = sizeof(struct jbd2_block_tag3);
	else {
		bytes = 8;
		if (journal->j_features_incompat &
		    JBD2_FEATURE_INCOMPAT_64BIT)
			bytes += 4;
		if (journal->j_features_incompat &
		    JBD2_FEATURE_INCOMPAT_CSUM_V2)
			bytes += JBD2_CSUM_V2_TAG_EXTRA;
	}
	if (!same_uuid)
		bytes += sizeof(journal->j_uuid);
	return (bytes);
}

static u_int32_t
ext4fs_journal_descriptor_count (struct ext4fs_journal *journal,
    struct ext4fs_journal_metadata *first)
{
	struct ext4fs_journal_metadata *metadata;
	struct jbd2_replay_ctx ctx;
	u_int32_t bytes, count, limit, tag_bytes;

	ext4fs_journal_checksum_ctx(journal, &ctx);
	limit = jbd2_descriptor_limit(&ctx);
	bytes = sizeof(struct jbd2_header);
	count = 0;
	for (metadata = first; metadata != NULL;
	    metadata = TAILQ_NEXT(metadata, jm_entry)) {
		tag_bytes = ext4fs_journal_tag_bytes(journal, count != 0);
		if (bytes > limit || tag_bytes > limit - bytes)
			break;
		bytes += tag_bytes;
		count++;
	}
	return (count);
}

static int
ext4fs_journal_log_blocks (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx, u_int32_t *blocksp)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal_revoke *revoke;
	struct jbd2_replay_ctx ctx;
	u_int32_t count, descriptors, metadata_blocks, record_size;
	u_int32_t revoke_blocks, revoke_count, revoke_per_block;
	u_int64_t total;

	descriptors = 0;
	metadata_blocks = 0;
	metadata = TAILQ_FIRST(&tx->jt_metadata);
	while (metadata != NULL) {
		u_int32_t i;

		count = ext4fs_journal_descriptor_count(journal, metadata);
		if (count == 0)
			return (EINVAL);
		descriptors++;
		metadata_blocks += count;
		for (i = 0; i < count; i++) {
			if (!metadata->jm_dirty || metadata->jm_owner != NULL ||
			    metadata->jm_buf == NULL ||
			    !ISSET(metadata->jm_buf->b_flags, B_BUSY))
				return (EINVAL);
			metadata = TAILQ_NEXT(metadata, jm_entry);
		}
	}

	revoke_count = 0;
	TAILQ_FOREACH(revoke, &tx->jt_revokes, jr_entry)
		revoke_count++;
	revoke_blocks = 0;
	if (revoke_count != 0) {
		if (!(journal->j_features_incompat &
		    JBD2_FEATURE_INCOMPAT_REVOKE))
			return (EOPNOTSUPP);
		ext4fs_journal_checksum_ctx(journal, &ctx);
		record_size = (journal->j_features_incompat &
		    JBD2_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
		if (jbd2_descriptor_limit(&ctx) <=
		    sizeof(struct jbd2_revoke_header))
			return (EINVAL);
		revoke_per_block = (jbd2_descriptor_limit(&ctx) -
		    sizeof(struct jbd2_revoke_header)) / record_size;
		if (revoke_per_block == 0)
			return (EINVAL);
		revoke_blocks = howmany(revoke_count, revoke_per_block);
	}
	if (tx->jt_credits_reserved != 0 ||
	    (u_int64_t)metadata_blocks + revoke_count != tx->jt_credits_used)
		return (EINVAL);

	total = (u_int64_t)descriptors + metadata_blocks + revoke_blocks + 1;
	if (total > 0xffffffffU || total > journal->j_free)
		return (ENOSPC);
	*blocksp = (u_int32_t)total;
	return (0);
}

static int
ext4fs_journal_write_metadata (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx, u_int32_t *jblockp,
    void *descriptor, void *data)
{
	struct ext4fs_journal_metadata *first, *metadata, *next;
	struct jbd2_block_tail *tail;
	struct jbd2_header *header;
	struct jbd2_replay_ctx ctx;
	u_int32_t checksum, count, flags, i, limit, offset, word;
	int error, has_64bit, has_csum_v2, has_csum_v3;

	ext4fs_journal_checksum_ctx(journal, &ctx);
	limit = jbd2_descriptor_limit(&ctx);
	has_64bit = journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT;
	has_csum_v2 = journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_CSUM_V2;
	has_csum_v3 = journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_CSUM_V3;

	first = TAILQ_FIRST(&tx->jt_metadata);
	while (first != NULL) {
		count = ext4fs_journal_descriptor_count(journal, first);
		if (count == 0)
			return (EINVAL);
		memset(descriptor, 0, journal->j_blocksize);
		header = descriptor;
		header->h_magic = htobe32(JBD2_MAGIC);
		header->h_blocktype = htobe32(JBD2_DESCRIPTOR_BLOCK);
		header->h_sequence = htobe32(tx->jt_sequence);
		offset = sizeof(*header);
		metadata = first;
		for (i = 0; i < count; i++) {
			flags = i == count - 1 ? JBD2_FLAG_LAST_TAG : 0;
			if (i != 0)
				flags |= JBD2_FLAG_SAME_UUID;
			memcpy(data, metadata->jm_buf->b_data,
			    journal->j_blocksize);
			memcpy(&word, data, sizeof(word));
			if (word == htobe32(JBD2_MAGIC)) {
				memset(data, 0, sizeof(word));
				flags |= JBD2_FLAG_ESCAPE;
			}
			checksum = jbd2_has_csum_v2or3(&ctx) ?
			    jbd2_data_block_checksum(&ctx, data,
			    tx->jt_sequence) : 0;

			if (has_csum_v3) {
				ext4fs_journal_put32(descriptor, offset,
				    (u_int32_t)metadata->jm_fsblock);
				ext4fs_journal_put32(descriptor, offset + 4,
				    flags);
				ext4fs_journal_put32(descriptor, offset + 8,
				    has_64bit ?
				    (u_int32_t)(metadata->jm_fsblock >> 32) : 0);
				ext4fs_journal_put32(descriptor, offset + 12,
				    checksum);
				offset += sizeof(struct jbd2_block_tag3);
			} else {
				ext4fs_journal_put32(descriptor, offset,
				    (u_int32_t)metadata->jm_fsblock);
				ext4fs_journal_put16(descriptor, offset + 4,
				    (u_int16_t)checksum);
				ext4fs_journal_put16(descriptor, offset + 6,
				    (u_int16_t)flags);
				offset += 8;
				if (has_64bit) {
					ext4fs_journal_put32(descriptor, offset,
					    (u_int32_t)(metadata->jm_fsblock >> 32));
					offset += 4;
				}
				if (has_csum_v2)
					offset += JBD2_CSUM_V2_TAG_EXTRA;
			}
			if (i == 0) {
				memcpy((char *)descriptor + offset, journal->j_uuid,
				    sizeof(journal->j_uuid));
				offset += sizeof(journal->j_uuid);
			}
			metadata = TAILQ_NEXT(metadata, jm_entry);
		}
		KASSERT(offset <= limit);
		if (jbd2_has_csum_v2or3(&ctx)) {
			tail = (struct jbd2_block_tail *)((char *)descriptor +
			    journal->j_blocksize - sizeof(*tail));
			tail->t_checksum = htobe32(jbd2_block_checksum(&ctx,
			    descriptor, journal->j_blocksize));
		}
		error = ext4fs_journal_write_block(journal, *jblockp,
		    descriptor);
		if (error)
			return (error);
		*jblockp = ext4fs_journal_next_block(journal, *jblockp);

		metadata = first;
		for (i = 0; i < count; i++) {
			next = TAILQ_NEXT(metadata, jm_entry);
			memcpy(data, metadata->jm_buf->b_data,
			    journal->j_blocksize);
			memcpy(&word, data, sizeof(word));
			if (word == htobe32(JBD2_MAGIC))
				memset(data, 0, sizeof(word));
			error = ext4fs_journal_write_block(journal, *jblockp,
			    data);
			if (error)
				return (error);
			*jblockp = ext4fs_journal_next_block(journal,
			    *jblockp);
			metadata = next;
		}
		first = metadata;
	}
	return (0);
}

static int
ext4fs_journal_write_revokes (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx, u_int32_t *jblockp, void *block)
{
	struct ext4fs_journal_revoke *revoke;
	struct jbd2_block_tail *tail;
	struct jbd2_replay_ctx ctx;
	struct jbd2_revoke_header *header;
	u_int32_t limit, offset, record_size;
	int error, has_64bit;

	revoke = TAILQ_FIRST(&tx->jt_revokes);
	if (revoke == NULL)
		return (0);
	if (!(journal->j_features_incompat & JBD2_FEATURE_INCOMPAT_REVOKE))
		return (EOPNOTSUPP);
	ext4fs_journal_checksum_ctx(journal, &ctx);
	limit = jbd2_descriptor_limit(&ctx);
	has_64bit = journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT;
	record_size = has_64bit ? 8 : 4;

	while (revoke != NULL) {
		memset(block, 0, journal->j_blocksize);
		header = block;
		header->r_header.h_magic = htobe32(JBD2_MAGIC);
		header->r_header.h_blocktype = htobe32(JBD2_REVOKE_BLOCK);
		header->r_header.h_sequence = htobe32(tx->jt_sequence);
		offset = sizeof(*header);
		while (revoke != NULL && record_size <= limit - offset) {
			if (has_64bit) {
				ext4fs_journal_put32(block, offset,
				    (u_int32_t)(revoke->jr_fsblock >> 32));
				ext4fs_journal_put32(block, offset + 4,
				    (u_int32_t)revoke->jr_fsblock);
			} else
				ext4fs_journal_put32(block, offset,
				    (u_int32_t)revoke->jr_fsblock);
			offset += record_size;
			revoke = TAILQ_NEXT(revoke, jr_entry);
		}
		header->r_count = htobe32(offset);
		if (jbd2_has_csum_v2or3(&ctx)) {
			tail = (struct jbd2_block_tail *)((char *)block +
			    journal->j_blocksize - sizeof(*tail));
			tail->t_checksum = htobe32(jbd2_block_checksum(&ctx,
			    block, journal->j_blocksize));
		}
		error = ext4fs_journal_write_block(journal, *jblockp, block);
		if (error)
			return (error);
		*jblockp = ext4fs_journal_next_block(journal, *jblockp);
	}
	return (0);
}

static int
ext4fs_journal_write_commit (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx, u_int32_t *jblockp, void *block)
{
	struct jbd2_commit_header *commit;
	struct jbd2_replay_ctx ctx;
	struct timespec ts;
	int error;

	memset(block, 0, journal->j_blocksize);
	commit = block;
	commit->h_header.h_magic = htobe32(JBD2_MAGIC);
	commit->h_header.h_blocktype = htobe32(JBD2_COMMIT_BLOCK);
	commit->h_header.h_sequence = htobe32(tx->jt_sequence);
	getnanotime(&ts);
	commit->h_commit_sec = htobe64((u_int64_t)ts.tv_sec);
	commit->h_commit_nsec = htobe32((u_int32_t)ts.tv_nsec);
	ext4fs_journal_checksum_ctx(journal, &ctx);
	if (jbd2_has_csum_v2or3(&ctx)) {
		commit->h_checksum_type = JBD2_CHECKSUM_CRC32C;
		commit->h_checksum_size = JBD2_CHECKSUM_SIZE;
		commit->h_checksum[0] = htobe32(jbd2_block_checksum(&ctx,
		    block, journal->j_blocksize));
	}
	error = ext4fs_journal_write_block(journal, *jblockp, block);
	if (error)
		return (error);
	*jblockp = ext4fs_journal_next_block(journal, *jblockp);
	return (0);
}

static int
ext4fs_journal_flush_ordered (struct ext4fs_journal_transaction *tx)
{
	struct ext4fs_journal_ordered *ordered;
	struct vnode *vp;
	int error, s;

	TAILQ_FOREACH(ordered, &tx->jt_ordered, jo_entry) {
		vp = ordered->jo_vnode;
		error = vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
		if (error)
			return (error);
		vflushbuf(vp, 1);
		s = splbio();
		error = ISSET(vp->v_bioflag, VBIOERROR) ? EIO : 0;
		splx(s);
		VOP_UNLOCK(vp);
		if (error)
			return (error);
	}
	return (0);
}

static int
ext4fs_journal_checkpoint (struct ext4fs_journal_transaction *tx)
{
	struct ext4fs_journal_metadata *metadata;
	struct buf *bp;
	int error;

	while ((metadata = TAILQ_FIRST(&tx->jt_metadata)) != NULL) {
		TAILQ_REMOVE(&tx->jt_metadata, metadata, jm_entry);
		bp = metadata->jm_buf;
		metadata->jm_buf = NULL;
		KASSERT(metadata->jm_dirty);
		KASSERT(metadata->jm_owner == NULL);
		KASSERT(ISSET(bp->b_flags, B_BUSY));
		SET(bp->b_flags, B_NOCACHE);
		error = bwrite(bp);
		free(metadata, M_UFSMNT, sizeof(*metadata));
		if (error)
			return (error);
	}
	return (0);
}

static int
ext4fs_journal_commit_transaction (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx, u_int32_t *new_headp)
{
	struct ufsmount *ump;
	void *block, *data;
	u_int32_t commit_block, expected_head, jblock, required, start, usable;
	int error;

	ump = VFSTOUFS(journal->j_mp);
	error = ext4fs_journal_log_blocks(journal, tx, &required);
	if (error)
		return (error);
	start = jblock = journal->j_head;
	usable = journal->j_maxlen - journal->j_first;
	commit_block = journal->j_first +
	    ((u_int64_t)(start - journal->j_first) + required - 1) % usable;
	expected_head = ext4fs_journal_next_block(journal, commit_block);
	block = malloc(journal->j_blocksize, M_UFSMNT, M_WAITOK);
	data = malloc(journal->j_blocksize, M_UFSMNT, M_WAITOK);

	/* Ordered mode: make all earlier data writes durable before metadata. */
	error = ext4fs_journal_flush_ordered(tx);
	if (error)
		goto out;
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		goto out;

	error = ext4fs_journal_write_metadata(journal, tx, &jblock, block,
	    data);
	if (error)
		goto out;
	error = ext4fs_journal_write_revokes(journal, tx, &jblock, block);
	if (error)
		goto out;
	if (jblock != commit_block) {
		error = EINVAL;
		goto out;
	}
	/* Remove any old commit header before exposing this transaction. */
	memset(block, 0, journal->j_blocksize);
	error = ext4fs_journal_write_block(journal, jblock, block);
	if (error)
		goto out;
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		goto out;

	/*
	 * Only expose the transaction after every pre-commit record is durable.
	 * This lets recovery ignore a crash during descriptor construction while
	 * still making s_start durable before the commit block can reach disk.
	 */
	error = ext4fs_journal_write_super(journal, start, tx->jt_sequence,
	    journal->j_head);
	if (error)
		goto out;
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		goto out;

	error = ext4fs_journal_write_commit(journal, tx, &jblock, block);
	if (error)
		goto out;
	if (jblock != expected_head) {
		error = EINVAL;
		goto out;
	}
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		goto out;

	error = ext4fs_journal_checkpoint(tx);
	if (error)
		goto out;
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		goto out;

	error = ext4fs_journal_write_super(journal, 0,
	    tx->jt_sequence + 1, jblock);
	if (error)
		goto out;
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		goto out;
	*new_headp = jblock;

out:
	free(data, M_UFSMNT, journal->j_blocksize);
	free(block, M_UFSMNT, journal->j_blocksize);
	return (error);
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
	journal->j_head = ctx.rc_head != 0 ? ctx.rc_head : ctx.rc_first;
	journal->j_tail = journal->j_head;
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
	if (ctx.rc_max_transaction != 0) {
		u_int32_t maximum;

		maximum = ctx.rc_max_transaction > 1 ?
		    (ctx.rc_max_transaction - 1) / 2 : 0;
		if (maximum < journal->j_max_transaction_credits)
			journal->j_max_transaction_credits = maximum;
	}

	journal->j_blockmap = ctx.rc_blockmap;
	journal->j_blockmap_count = ctx.rc_blockmap_count;
	journal->j_blockset = ctx.rc_blockset;
	journal->j_blockset_mask = ctx.rc_blockset_mask;
	ctx.rc_blockmap = NULL;
	ctx.rc_blockset = NULL;
	jbd2_journal_close(&ctx);

	fs->m_journal = journal;
	if (!fs->m_read_only && !(mp->mnt_flag & MNT_RDONLY)) {
		error = ext4fs_journal_set_recover(journal);
		if (error) {
			ext4fs_journal_destroy(mp);
			return (error);
		}
	}
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
	while (journal->j_commit_busy)
		msleep_nsec(&journal->j_committing, &journal->j_lock, PRIBIO,
		    "e4jiodrn", INFSLP);
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
	ext4fs_journal_abort_locked(journal, error);
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
	int checkpoint, error;

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

	for (;;) {
		checkpoint = 0;
		mtx_enter(&journal->j_lock);
		while ((error = ext4fs_journal_state_admission(
		    journal->j_active != NULL, journal->j_aborted,
		    journal->j_error, journal->j_shutting_down)) == EBUSY)
			msleep_nsec(&journal->j_active, &journal->j_lock, PRIBIO,
			    "e4jbegin", INFSLP);
		if (error == 0) {
			tx = journal->j_running != NULL ? journal->j_running :
			    candidate;
			error = ext4fs_journal_state_reserve(
			    journal->j_max_transaction_credits,
			    tx->jt_credits_used, &tx->jt_credits_reserved, credits);
			if (error == ENOSPC && journal->j_running != NULL)
				checkpoint = 1;
			else if (error == 0) {
				if (journal->j_running == NULL) {
					/* The commit transition consumes this sequence. */
					tx->jt_sequence =
					    ext4fs_journal_state_sequence(
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
		if (!checkpoint)
			break;
		error = ext4fs_journal_force_commit(mp);
		if (error)
			break;
	}

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
	if (fsblock > 0xffffffffULL && !(journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT))
		return (EFBIG);
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
	if (!(journal->j_features_incompat & JBD2_FEATURE_INCOMPAT_REVOKE))
		return (EOPNOTSUPP);
	if (fsblock >= fs->m_blocks_count ||
	    ext4fs_journal_block_member(journal, fsblock))
		return (EINVAL);
	if (fsblock > 0xffffffffULL && !(journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT))
		return (EFBIG);
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
	struct ext4fs_journal_transaction *tx;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	u_int32_t new_head;
	int error;

	fs = VFSTOUFS(mp)->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL)
		return (0);
	tx = NULL;
	new_head = 0;

	mtx_enter(&journal->j_lock);
	for (;;) {
		error = ext4fs_journal_state_admission(
		    journal->j_active != NULL, journal->j_aborted,
		    journal->j_error, journal->j_shutting_down);
		if (error == EBUSY) {
			msleep_nsec(&journal->j_active, &journal->j_lock,
			    PRIBIO, "e4jcommit", INFSLP);
			continue;
		}
		if (error != 0)
			break;
		if (journal->j_committing != NULL) {
			msleep_nsec(&journal->j_committing, &journal->j_lock,
			    PRIBIO, "e4jwait", INFSLP);
			continue;
		}
		break;
	}
	if (error == 0 && journal->j_running != NULL) {
		tx = journal->j_running;
		journal->j_running = NULL;
		journal->j_committing = tx;
		journal->j_commit_busy = 1;
		ext4fs_journal_state_sequence_advance(
		    &journal->j_next_sequence);
	}
	mtx_leave(&journal->j_lock);
	if (error != 0 || tx == NULL)
		return (error);

	error = ext4fs_journal_commit_transaction(journal, tx, &new_head);
	mtx_enter(&journal->j_lock);
	KASSERT(journal->j_committing == tx);
	KASSERT(journal->j_commit_busy);
	journal->j_commit_busy = 0;
	if (error == 0 && journal->j_aborted)
		error = journal->j_error != 0 ? journal->j_error : EIO;
	if (error != 0) {
		ext4fs_journal_abort_locked(journal, error);
	} else {
		journal->j_committing = NULL;
		journal->j_head = new_head;
		journal->j_tail = new_head;
		journal->j_free = journal->j_maxlen - journal->j_first;
	}
	wakeup(&journal->j_committing);
	mtx_leave(&journal->j_lock);

	if (error == 0)
		ext4fs_journal_transaction_free(tx);
	return (error);
}

int
ext4fs_journal_mark_clean (struct mount *mp)
{
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	struct ufsmount *ump;
	u_int32_t old_incompat;
	u_int16_t old_state;
	int error;

	ump = VFSTOUFS(mp);
	fs = ump->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL || fs->m_read_only || (mp->mnt_flag & MNT_RDONLY))
		return (0);
	error = ext4fs_journal_force_commit(mp);
	if (error)
		return (error);

	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_state_admission(journal->j_active != NULL,
	    journal->j_aborted, journal->j_error, journal->j_shutting_down);
	if (error == 0 && (journal->j_running != NULL ||
	    journal->j_committing != NULL))
		error = EBUSY;
	mtx_leave(&journal->j_lock);
	if (error)
		return (error);

	old_incompat = fs->m_feature_incompat;
	old_state = fs->m_state;
	fs->m_feature_incompat &= ~EXT4FS_FEATURE_INCOMPAT_RECOVER;
	fs->m_sble.sb_feature_incompat = htole32(fs->m_feature_incompat);
	fs->m_state = EXT4FS_STATE_VALID;
	fs->m_fs_was_modified = 1;
	error = ext4fs_sbwrite(mp);
	if (error == 0)
		error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error) {
		fs->m_feature_incompat = old_incompat;
		fs->m_sble.sb_feature_incompat = htole32(old_incompat);
		fs->m_state = old_state;
		fs->m_sble.sb_state = htole16(old_state);
		ext4fs_journal_abort(mp, error);
	}
	return (error);
}
