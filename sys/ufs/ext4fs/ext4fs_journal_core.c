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

/*
 * Serialized runtime journal state for ext4fs.
 *
 * The writer keeps one active handle, one running transaction, and one
 * committing transaction per mount.  Durable committed transactions
 * retain their log space and immutable metadata until a bounded batch
 * checkpoint advances the journal tail.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/specdev.h>
#include <sys/task.h>
#include <sys/timeout.h>
#include <sys/tree.h>
#include <sys/vnode.h>

#include <ufs/ufs/quota.h>
#include <ufs/ufs/ufsmount.h>

#include <ufs/ext4fs/ext4fs.h>
#include <ufs/ext4fs/ext4fs_crc32c.h>
#include <ufs/ext4fs/ext4fs_dinode.h>
#include <ufs/ext4fs/ext4fs_journal.h>
#include <ufs/ext4fs/ext4fs_journal_state.h>

enum ext4fs_journal_update_kind {
	EXT4FS_JOURNAL_UPDATE_NONE = 0,
	EXT4FS_JOURNAL_UPDATE_INODE = 1,
	EXT4FS_JOURNAL_UPDATE_GROUP_DESCRIPTOR,
	EXT4FS_JOURNAL_UPDATE_SUPERBLOCK
};

/*
 * Mount validation limits blocks to 4096 and descriptors to 32 or 64.
 */
#define EXT4FS_JOURNAL_UPDATE_SLOTS_MAX				\
	((1U << (EXT4FS_LOG_MIN_BLOCK_SIZE + 2)) /		\
	EXT4FS_BGD_SIZE_32)
#define EXT4FS_JOURNAL_UPDATE_BYTES				\
	howmany(EXT4FS_JOURNAL_UPDATE_SLOTS_MAX, NBBY)

struct ext4fs_journal_metadata {
	TAILQ_ENTRY(ext4fs_journal_metadata) jm_entry;
	RB_ENTRY(ext4fs_journal_metadata) jm_block_entry;
	RB_ENTRY(ext4fs_journal_metadata) jm_buffer_entry;
	/* Kept B_BUSY while journal-owned. */
	struct buf	*jm_buf;
	/* Frozen image shared by logging and checkpointing. */
	void		*jm_snapshot;
	/* Handle until dirtied. */
	struct ext4fs_journal_handle *jm_owner;
	u_int64_t	 jm_fsblock;
	u_int32_t	 jm_snapshot_size;
	u_int32_t	 jm_validated;
	int		 jm_dirty;
	/* Deferred checksum and copy finalization within this block. */
	u_int8_t	 jm_update_bits[EXT4FS_JOURNAL_UPDATE_BYTES];
	u_int32_t	 jm_update_base;
	u_int32_t	 jm_update_slots;
	enum ext4fs_journal_update_kind jm_update_kind;
	int		 jm_update_pending;
};

struct ext4fs_journal_revoke {
	TAILQ_ENTRY(ext4fs_journal_revoke) jr_entry;
	RB_ENTRY(ext4fs_journal_revoke) jr_block_entry;
	u_int64_t	 jr_fsblock;
};

struct ext4fs_journal_ordered {
	TAILQ_ENTRY(ext4fs_journal_ordered) jo_entry;
	struct vnode	*jo_vnode;
};

TAILQ_HEAD(ext4fs_journal_metadata_head, ext4fs_journal_metadata);
TAILQ_HEAD(ext4fs_journal_revoke_head, ext4fs_journal_revoke);
TAILQ_HEAD(ext4fs_journal_ordered_head, ext4fs_journal_ordered);
RB_HEAD(ext4fs_journal_metadata_block_tree,
    ext4fs_journal_metadata);
RB_HEAD(ext4fs_journal_metadata_buffer_tree,
    ext4fs_journal_metadata);
RB_HEAD(ext4fs_journal_revoke_block_tree, ext4fs_journal_revoke);

struct ext4fs_journal_io_batch {
	struct mutex	 jib_lock;
	u_int32_t	 jib_pending;
	int		 jib_error;
};

struct ext4fs_journal_transaction {
	TAILQ_ENTRY(ext4fs_journal_transaction) jt_entry;
	struct ext4fs_journal_metadata_head jt_metadata;
	struct ext4fs_journal_revoke_head jt_revokes;
	struct ext4fs_journal_ordered_head jt_ordered;
	struct ext4fs_journal_metadata_block_tree jt_metadata_blocks;
	struct ext4fs_journal_metadata_buffer_tree jt_metadata_buffers;
	struct ext4fs_journal_revoke_block_tree jt_revoke_blocks;
	struct ext4fs_journal_io_batch jt_data_io;
	enum ext4fs_journal_transaction_state jt_state;
	u_int32_t	 jt_sequence;
	u_int32_t	 jt_credits_reserved;
	u_int32_t	 jt_credits_used;
	u_int32_t	 jt_log_start;
	u_int32_t	 jt_log_end;
	u_int32_t	 jt_log_blocks;
};

TAILQ_HEAD(ext4fs_journal_transaction_head,
    ext4fs_journal_transaction);

static int	ext4fs_journal_metadata_block_compare (
		    struct ext4fs_journal_metadata *,
		    struct ext4fs_journal_metadata *);
static int	ext4fs_journal_metadata_buffer_compare (
		    struct ext4fs_journal_metadata *,
		    struct ext4fs_journal_metadata *);
static int	ext4fs_journal_revoke_block_compare (
		    struct ext4fs_journal_revoke *,
		    struct ext4fs_journal_revoke *);

RB_PROTOTYPE_STATIC(ext4fs_journal_metadata_block_tree,
    ext4fs_journal_metadata, jm_block_entry,
    ext4fs_journal_metadata_block_compare);
RB_GENERATE_STATIC(ext4fs_journal_metadata_block_tree,
    ext4fs_journal_metadata, jm_block_entry,
    ext4fs_journal_metadata_block_compare);
RB_PROTOTYPE_STATIC(ext4fs_journal_metadata_buffer_tree,
    ext4fs_journal_metadata, jm_buffer_entry,
    ext4fs_journal_metadata_buffer_compare);
RB_GENERATE_STATIC(ext4fs_journal_metadata_buffer_tree,
    ext4fs_journal_metadata, jm_buffer_entry,
    ext4fs_journal_metadata_buffer_compare);
RB_PROTOTYPE_STATIC(ext4fs_journal_revoke_block_tree,
    ext4fs_journal_revoke, jr_block_entry,
    ext4fs_journal_revoke_block_compare);
RB_GENERATE_STATIC(ext4fs_journal_revoke_block_tree,
    ext4fs_journal_revoke, jr_block_entry,
    ext4fs_journal_revoke_block_compare);

#define EXT4FS_JOURNAL_COMMITTED_MAX	8
#define EXT4FS_JOURNAL_COMMITTED_BYTES_MAX	(16U * 1024U * 1024U)
#define EXT4FS_JOURNAL_DIRTY_METADATA_MAX	(4U * 1024U * 1024U)
#define EXT4FS_JOURNAL_TRANSACTION_AGE_MSEC	100

struct ext4fs_journal {
	struct mutex	 j_lock;
	struct mount	*j_mp;
	struct ext4fs_journal_handle *j_active;
	struct proc	*j_handoff;
	struct ext4fs_journal_transaction *j_running;
	struct ext4fs_journal_transaction *j_committing;
	struct ext4fs_journal_transaction_head j_committed;
	struct timeout	 j_commit_timeout;
	struct task	 j_commit_task;
	struct taskq	*j_commit_taskq;
	/* Serialized commit workspace; queued buffers copy its data. */
	void		*j_scratch_block;

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
	u_int32_t	 j_committed_count;
	u_int32_t	 j_committed_blocks;
	u_int32_t	 j_max_transaction_credits;
	u_int32_t	 j_expired_sequence;
	u_int32_t	 j_features_compat;
	u_int32_t	 j_features_incompat;
	u_int32_t	 j_features_ro_compat;
	u_int32_t	 j_checksum_seed;
	u_int8_t	 j_uuid[16];
	enum ext4fs_journal_stage j_stage;
	int		 j_aborted;
	int		 j_error;
	int		 j_commit_busy;
	int		 j_scratch_busy;
	int		 j_timeout_draining;
	int		 j_shutting_down;
};

struct ext4fs_journal_handle {
	struct ext4fs_journal *jh_journal;
	struct ext4fs_journal_transaction *jh_transaction;
	u_int32_t	 jh_credits;
	int		 jh_ended;
};

enum ext4fs_journal_fence {
	EXT4FS_JOURNAL_FENCE_NONE = 0,
	EXT4FS_JOURNAL_FENCE_MATCH,
	EXT4FS_JOURNAL_FENCE_BEFORE,
	EXT4FS_JOURNAL_FENCE_WAIT
};

struct ext4fs_journal_abort_info {
	const char	*jai_caller;
	enum ext4fs_journal_stage jai_stage;
	u_int32_t	 jai_sequence;
	u_int32_t	 jai_head;
	u_int32_t	 jai_tail;
	u_int32_t	 jai_free;
};

static void	ext4fs_journal_transaction_free (
		    struct ext4fs_journal_transaction *);
static void	ext4fs_journal_update_reset (
		    struct ext4fs_journal_metadata *);
static int	ext4fs_journal_materialize_record (
		    struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *,
		    struct ext4fs_journal_metadata *);
static int	ext4fs_journal_materialize_transaction (
		    struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *);
static int	ext4fs_journal_block_member (struct ext4fs_journal *,
		    u_int64_t);
static int	ext4fs_journal_handle_error (
		    struct ext4fs_journal_handle *);
static int	ext4fs_journal_metadata_validation (
		    struct ext4fs_journal_handle *, struct buf *,
		    u_int32_t, int, int *);
static int	ext4fs_journal_abort_locked (
		    struct ext4fs_journal *, int, const char *,
		    struct ext4fs_journal_abort_info *);
static void	ext4fs_journal_abort_policy (struct mount *, int,
		    const struct ext4fs_journal_abort_info *);
static void	ext4fs_journal_set_stage (struct ext4fs_journal *,
		    enum ext4fs_journal_stage);
static void	ext4fs_journal_write_iodone (struct buf *);
static void	ext4fs_journal_io_init (
		    struct ext4fs_journal_io_batch *);
static void	ext4fs_journal_io_submit (
		    struct ext4fs_journal_io_batch *, struct buf *);
static void	ext4fs_journal_io_submit_raw (
		    struct ext4fs_journal_io_batch *, struct vnode *,
		    struct buf *);
static int	ext4fs_journal_io_wait (
		    struct ext4fs_journal_io_batch *);
static void	ext4fs_journal_scratch_acquire (
		    struct ext4fs_journal *, void **);
static void	ext4fs_journal_scratch_release (
		    struct ext4fs_journal *);
static void	ext4fs_journal_checksum_ctx (struct ext4fs_journal *,
		    struct jbd2_replay_ctx *);
static u_int32_t	ext4fs_journal_next_block (
		    struct ext4fs_journal *, u_int32_t);
static int	ext4fs_journal_prepare_block (struct ext4fs_journal *,
		    u_int32_t, const void *, struct buf **);
static int	ext4fs_journal_write_block (struct ext4fs_journal *,
		    u_int32_t, const void *);
static int	ext4fs_journal_queue_block (struct ext4fs_journal *,
		    u_int32_t, const void *,
		    struct ext4fs_journal_io_batch *);
static int	ext4fs_journal_super_valid (struct ext4fs_journal *,
		    struct jbd2_superblock *);
static int	ext4fs_journal_write_super (struct ext4fs_journal *,
		    u_int32_t, u_int32_t, u_int32_t);
static int	ext4fs_journal_enable_revoke (
		    struct ext4fs_journal *);
static int	ext4fs_journal_set_recover (struct ext4fs_journal *);
static u_int32_t
		ext4fs_journal_tag_bytes (struct ext4fs_journal *, int);
static u_int32_t	ext4fs_journal_descriptor_count (
		    struct ext4fs_journal *,
		    struct ext4fs_journal_metadata *);
static int	ext4fs_journal_log_blocks (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *, u_int32_t *);
static int	ext4fs_journal_snapshot_metadata (
		    struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *);
static int	ext4fs_journal_write_metadata (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *, u_int32_t *,
		    void *, struct ext4fs_journal_io_batch *);
static int	ext4fs_journal_write_revokes (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *, u_int32_t *,
		    void *, struct ext4fs_journal_io_batch *);
static int	ext4fs_journal_write_commit (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *, u_int32_t *,
		    void *);
static int	ext4fs_journal_flush_ordered (
		    struct ext4fs_journal_transaction *);
static int	ext4fs_journal_release_metadata (
		    struct ext4fs_journal_transaction *);
static int	ext4fs_journal_metadata_revoked (
		    struct ext4fs_journal_transaction *, u_int64_t);
static struct ext4fs_journal_transaction *
		ext4fs_journal_committed_find (struct ext4fs_journal *,
		    u_int32_t);
static struct ext4fs_journal_transaction *
		ext4fs_journal_committed_before (struct ext4fs_journal *,
		    u_int32_t);
static int	ext4fs_journal_checkpoint (struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *);
static int	ext4fs_journal_checkpoint_committed (
		    struct ext4fs_journal *,
		    struct ext4fs_journal_transaction_head *,
		    struct ext4fs_journal_transaction *);
static int	ext4fs_journal_should_checkpoint (
		    struct ext4fs_journal *);
static int	ext4fs_journal_commit_transaction (
		    struct ext4fs_journal *,
		    struct ext4fs_journal_transaction *);
static int	ext4fs_journal_commit_internal (struct mount *,
		    enum ext4fs_journal_commit_reason, u_int32_t,
		    enum ext4fs_journal_fence);
static int	ext4fs_journal_end_internal (
		    struct ext4fs_journal_handle *, u_int32_t *, int *);
static void	ext4fs_journal_timeout_drain (
		    struct ext4fs_journal *);
static void	ext4fs_journal_commit_task (void *);
static void	ext4fs_journal_commit_timeout (void *);

static int
ext4fs_journal_metadata_block_compare (
    struct ext4fs_journal_metadata *left,
    struct ext4fs_journal_metadata *right)
{
	if (left->jm_fsblock < right->jm_fsblock)
		return (-1);
	if (left->jm_fsblock > right->jm_fsblock)
		return (1);
	return (0);
}

static int
ext4fs_journal_metadata_buffer_compare (
    struct ext4fs_journal_metadata *left,
    struct ext4fs_journal_metadata *right)
{
	if ((uintptr_t)left->jm_buf < (uintptr_t)right->jm_buf)
		return (-1);
	if ((uintptr_t)left->jm_buf > (uintptr_t)right->jm_buf)
		return (1);
	return (0);
}

static int
ext4fs_journal_revoke_block_compare (
    struct ext4fs_journal_revoke *left,
    struct ext4fs_journal_revoke *right)
{
	if (left->jr_fsblock < right->jr_fsblock)
		return (-1);
	if (left->jr_fsblock > right->jr_fsblock)
		return (1);
	return (0);
}

static struct ext4fs_journal_metadata *
ext4fs_journal_metadata_block_find (
    struct ext4fs_journal_transaction *tx, u_int64_t fsblock)
{
	struct ext4fs_journal_metadata key;

	key.jm_fsblock = fsblock;
	return (RB_FIND(ext4fs_journal_metadata_block_tree,
	    &tx->jt_metadata_blocks, &key));
}

static struct ext4fs_journal_metadata *
ext4fs_journal_metadata_buffer_find (
    struct ext4fs_journal_transaction *tx, struct buf *bp)
{
	struct ext4fs_journal_metadata key;

	key.jm_buf = bp;
	return (RB_FIND(ext4fs_journal_metadata_buffer_tree,
	    &tx->jt_metadata_buffers, &key));
}

static struct ext4fs_journal_revoke *
ext4fs_journal_revoke_block_find (
    struct ext4fs_journal_transaction *tx, u_int64_t fsblock)
{
	struct ext4fs_journal_revoke key;

	key.jr_fsblock = fsblock;
	return (RB_FIND(ext4fs_journal_revoke_block_tree,
	    &tx->jt_revoke_blocks, &key));
}

static void
ext4fs_journal_metadata_remove (
    struct ext4fs_journal_transaction *tx,
    struct ext4fs_journal_metadata *metadata)
{
	struct ext4fs_journal_metadata *removed;

	KASSERT(! metadata->jm_update_pending);
	removed = RB_REMOVE(ext4fs_journal_metadata_block_tree,
	    &tx->jt_metadata_blocks, metadata);
	KASSERT(removed == metadata);
	if (metadata->jm_buf != NULL) {
		removed = RB_REMOVE(
		    ext4fs_journal_metadata_buffer_tree,
		    &tx->jt_metadata_buffers, metadata);
		KASSERT(removed == metadata);
	}
	TAILQ_REMOVE(&tx->jt_metadata, metadata, jm_entry);
}

static void
ext4fs_journal_revoke_remove (
    struct ext4fs_journal_transaction *tx,
    struct ext4fs_journal_revoke *revoke)
{
	struct ext4fs_journal_revoke *removed;

	removed = RB_REMOVE(ext4fs_journal_revoke_block_tree,
	    &tx->jt_revoke_blocks, revoke);
	KASSERT(removed == revoke);
	TAILQ_REMOVE(&tx->jt_revokes, revoke, jr_entry);
}

static void
ext4fs_journal_update_reset (
    struct ext4fs_journal_metadata *metadata)
{
	memset(metadata->jm_update_bits, 0,
	    sizeof(metadata->jm_update_bits));
	metadata->jm_update_base = 0;
	metadata->jm_update_slots = 0;
	metadata->jm_update_kind = EXT4FS_JOURNAL_UPDATE_NONE;
	metadata->jm_update_pending = 0;
}

static int
ext4fs_journal_update_location (struct m_ext4fs *fs,
    enum ext4fs_journal_update_kind kind, u_int32_t key,
    u_int64_t *fsblockp, u_int32_t *offsetp)
{
	struct ext4fs_block_group_descriptor *gd;
	u_int64_t table;
	u_int32_t group, index;
	size_t offset, size;
	int error;

	switch (kind) {
	case EXT4FS_JOURNAL_UPDATE_INODE:
		if (key == 0 || key > fs->m_inodes_count ||
		    fs->m_inodes_per_group == 0 ||
		    fs->m_inodes_per_block == 0 ||
		    fs->m_inode_size >
		    sizeof(struct ext4fs_dinode_256))
			return (EINVAL);
		group = (key - 1) / fs->m_inodes_per_group;
		index = (key - 1) % fs->m_inodes_per_group;
		if (group >= fs->m_block_group_count)
			return (EINVAL);
		gd = &fs->m_gd[group];
		table = ext4fs_bgd_get_block(fs, gd,
		    EXT4FS_BGD_INODE_TABLE);
		*fsblockp = table + index / fs->m_inodes_per_block;
		offset = (size_t)(index % fs->m_inodes_per_block) *
		    fs->m_inode_size;
		size = fs->m_inode_size;
		break;
	case EXT4FS_JOURNAL_UPDATE_GROUP_DESCRIPTOR:
		if (key >= fs->m_block_group_count)
			return (EINVAL);
		error = ext4fs_bgd_location(fs, key, fsblockp,
		    &offset);
		if (error)
			return (error);
		size = fs->m_block_group_descriptor_size;
		break;
	case EXT4FS_JOURNAL_UPDATE_SUPERBLOCK:
		if (key != 0)
			return (EINVAL);
		*fsblockp = EXT4FS_SUPER_BLOCK_OFFSET /
		    fs->m_block_size;
		offset = EXT4FS_SUPER_BLOCK_OFFSET %
		    fs->m_block_size;
		size = sizeof(struct ext4fs);
		break;
	default:
		return (EINVAL);
	}
	if (*fsblockp >= fs->m_blocks_count ||
	    offset > fs->m_block_size ||
	    size > fs->m_block_size - offset ||
	    offset > UINT32_MAX)
		return (EINVAL);
	*offsetp = (u_int32_t)offset;
	return (0);
}

static int
ext4fs_journal_materialize_update (struct ext4fs_journal *journal,
    struct ext4fs_journal_metadata *metadata, u_int32_t key)
{
	struct ext4fs_block_group_descriptor *gd;
	struct ext4fs_dinode_256 dinode, *disk_inode;
	struct m_ext4fs *fs;
	struct ext4fs *sble;
	struct buf *bp;
	u_int32_t checksum, offset;
	u_int64_t fsblock;
	int error;

	fs = VFSTOUFS(journal->j_mp)->um_e4fs;
	bp = metadata->jm_buf;
	error = ext4fs_journal_update_location(fs,
	    metadata->jm_update_kind, key, &fsblock, &offset);
	if (error)
		return (error);
	if (bp == NULL || bp->b_data == NULL ||
	    metadata->jm_fsblock != fsblock)
		return (EINVAL);

	switch (metadata->jm_update_kind) {
	case EXT4FS_JOURNAL_UPDATE_INODE:
		memset(&dinode, 0, sizeof(dinode));
		memcpy(&dinode, (char *)bp->b_data + offset,
		    fs->m_inode_size);
		checksum = ext4fs_inode_csum(fs, &dinode, key);
		disk_inode = (struct ext4fs_dinode_256 *)
		    ((char *)bp->b_data + offset);
		disk_inode->dinode.i_checksum_lo =
		    htole16(checksum & 0xffff);
		if (ext4fs_inode_has_csum_hi(&dinode))
			disk_inode->dinode.i_checksum_hi =
			    htole16(checksum >> 16);
		break;
	case EXT4FS_JOURNAL_UPDATE_GROUP_DESCRIPTOR:
		gd = &fs->m_gd[key];
		gd->bgd_checksum = htole16(ext4fs_bgd_csum(fs, gd,
		    key));
		memcpy((char *)bp->b_data + offset, gd,
		    fs->m_block_group_descriptor_size);
		break;
	case EXT4FS_JOURNAL_UPDATE_SUPERBLOCK:
		sble = (struct ext4fs *)((char *)bp->b_data + offset);
		memcpy(sble, &fs->m_sble, sizeof(*sble));
		ext4fs_sbprepare(fs, sble);
		memcpy(&fs->m_sble, sble, sizeof(fs->m_sble));
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

static int
ext4fs_journal_materialize_record (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx,
    struct ext4fs_journal_metadata *metadata)
{
	struct ext4fs_journal_metadata *found;
	u_int32_t bytes, i, key;
	int error;

	if (! metadata->jm_update_pending)
		return (0);
	if (metadata->jm_buf == NULL || ! metadata->jm_dirty ||
	    metadata->jm_owner != NULL ||
	    ! ISSET(metadata->jm_buf->b_flags, B_BUSY) ||
	    ! ISSET(metadata->jm_buf->b_flags, B_DONE))
		return (EINVAL);
	found = ext4fs_journal_metadata_buffer_find(tx,
	    metadata->jm_buf);
	if (found != metadata)
		return (EINVAL);
	if (metadata->jm_update_kind ==
	    EXT4FS_JOURNAL_UPDATE_SUPERBLOCK) {
		if (metadata->jm_update_base != 0 ||
		    metadata->jm_update_slots != 0)
			return (EINVAL);
		error = ext4fs_journal_materialize_update(journal,
		    metadata, 0);
		if (error)
			return (error);
	} else {
		bytes = howmany(metadata->jm_update_slots, NBBY);
		if (metadata->jm_update_slots == 0 ||
		    bytes > sizeof(metadata->jm_update_bits))
			return (EINVAL);
		for (i = 0; i < metadata->jm_update_slots; i++) {
			if (isclr(metadata->jm_update_bits, i))
				continue;
			if (metadata->jm_update_base > UINT32_MAX - i)
				return (EOVERFLOW);
			key = metadata->jm_update_base + i;
			error = ext4fs_journal_materialize_update(journal,
			    metadata, key);
			if (error)
				return (error);
		}
		memset(metadata->jm_update_bits, 0,
		    bytes);
	}
	metadata->jm_update_pending = 0;
	return (0);
}

static int
ext4fs_journal_materialize_transaction (
    struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx)
{
	struct ext4fs_journal_metadata *metadata;
	int error;

	TAILQ_FOREACH(metadata, &tx->jt_metadata, jm_entry) {
		if (! metadata->jm_update_pending)
			continue;
		error = ext4fs_journal_materialize_record(journal, tx,
		    metadata);
		if (error)
			return (error);
	}
	return (0);
}

static void
ext4fs_journal_transaction_free (struct ext4fs_journal_transaction *tx)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal_ordered *ordered;
	struct ext4fs_journal_revoke *revoke;

	if (tx == NULL)
		return;
	(void)ext4fs_journal_io_wait(&tx->jt_data_io);
	while ((metadata = TAILQ_FIRST(&tx->jt_metadata)) != NULL) {
		ext4fs_journal_update_reset(metadata);
		ext4fs_journal_metadata_remove(tx, metadata);
		/*
		 * Only dirtied, transaction-owned buffers survive a
		 * handle.  Teardown without a checkpoint must discard
		 * their contents.
		 */
		KASSERT(metadata->jm_dirty);
		KASSERT(metadata->jm_owner == NULL);
		if (metadata->jm_buf != NULL) {
			KASSERT(ISSET(metadata->jm_buf->b_flags, B_BUSY));
			SET(metadata->jm_buf->b_flags, B_INVAL);
			brelse(metadata->jm_buf);
		}
		if (metadata->jm_snapshot != NULL)
			free(metadata->jm_snapshot, M_UFSMNT,
			    metadata->jm_snapshot_size);
		free(metadata, M_UFSMNT, sizeof(*metadata));
	}
	while ((revoke = TAILQ_FIRST(&tx->jt_revokes)) != NULL) {
		ext4fs_journal_revoke_remove(tx, revoke);
		free(revoke, M_UFSMNT, sizeof(*revoke));
	}
	while ((ordered = TAILQ_FIRST(&tx->jt_ordered)) != NULL) {
		TAILQ_REMOVE(&tx->jt_ordered, ordered, jo_entry);
		vrele(ordered->jo_vnode);
		free(ordered, M_UFSMNT, sizeof(*ordered));
	}
	KASSERT(RB_EMPTY(&tx->jt_metadata_blocks));
	KASSERT(RB_EMPTY(&tx->jt_metadata_buffers));
	KASSERT(RB_EMPTY(&tx->jt_revoke_blocks));
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

	if (handle == NULL || handle->jh_journal == NULL ||
	    handle->jh_ended)
		return (EINVAL);
	journal = handle->jh_journal;
	if (journal->j_active != handle ||
	    journal->j_running != handle->jh_transaction ||
	    handle->jh_transaction->jt_state !=
	    EXT4FS_JOURNAL_TRANSACTION_RUNNING)
		return (EINVAL);
	return (ext4fs_journal_state_admission(0, journal->j_aborted,
	    journal->j_error, journal->j_shutting_down));
}

static int
ext4fs_journal_abort_locked (struct ext4fs_journal *journal, int error,
    const char *caller, struct ext4fs_journal_abort_info *info)
{
	struct m_ext4fs *fs;
	int first;

	fs = VFSTOUFS(journal->j_mp)->um_e4fs;
	first = ext4fs_journal_state_abort(&journal->j_aborted,
	    &journal->j_error, error);
	fs->m_state = EXT4FS_STATE_ERROR;
	fs->m_sble.sb_state = htole16(fs->m_state);
	fs->m_fs_was_modified = 1;
	journal->j_mp->mnt_flag |= MNT_RDONLY;
	journal->j_handoff = NULL;
	wakeup(&journal->j_active);
	wakeup(&journal->j_committing);
	if (first) {
		info->jai_caller = caller;
		info->jai_stage = journal->j_stage;
		if (journal->j_committing != NULL)
			info->jai_sequence =
			    journal->j_committing->jt_sequence;
		else if (journal->j_running != NULL)
			info->jai_sequence =
			    journal->j_running->jt_sequence;
		else
			info->jai_sequence = journal->j_next_sequence;
		info->jai_head = journal->j_head;
		info->jai_tail = journal->j_tail;
		info->jai_free = journal->j_free;
	}
	return (first);
}

static void
ext4fs_journal_abort_policy (struct mount *mp, int error,
    const struct ext4fs_journal_abort_info *info)
{
	struct m_ext4fs *fs;
	const char *policy, *stage;

	fs = VFSTOUFS(mp)->um_e4fs;
	stage = ext4fs_journal_stage_name(info->jai_stage);
	if (fs->m_errors == EXT4FS_ERRORS_PANIC)
		panic("ext4fs: journal abort on %s: %s at %s, "
		    "error %d, sequence %u, head %u, tail %u, "
		    "free %u", mp->mnt_stat.f_mntonname,
		    info->jai_caller, stage, error,
		    info->jai_sequence, info->jai_head,
		    info->jai_tail, info->jai_free);
	if (fs->m_errors == EXT4FS_ERRORS_CONTINUE)
		policy = "errors=continue cannot continue without a "
		    "journal; read-only";
	else
		policy = "errors=remount-ro; read-only";
	printf("ext4fs: journal abort on %s: %s at %s, error %d; "
	    "sequence %u, head %u, tail %u, free %u; %s\n",
	    mp->mnt_stat.f_mntonname, info->jai_caller, stage, error,
	    info->jai_sequence, info->jai_head, info->jai_tail,
	    info->jai_free, policy);
}

static void
ext4fs_journal_set_stage (struct ext4fs_journal *journal,
    enum ext4fs_journal_stage stage)
{
	mtx_enter(&journal->j_lock);
	journal->j_stage = stage;
	mtx_leave(&journal->j_lock);
}

static void
ext4fs_journal_write_iodone (struct buf *bp)
{
	struct ext4fs_journal_io_batch *batch;
	int error;

	batch = bp->b_saveaddr;
	KASSERT(batch != NULL);
	if (ISSET(bp->b_flags, B_EINTR))
		error = EINTR;
	else if (ISSET(bp->b_flags, B_ERROR))
		error = bp->b_error != 0 ? bp->b_error : EIO;
	else
		error = 0;
	bp->b_saveaddr = NULL;
	bp->b_iodone = NULL;
	brelse(bp);

	mtx_enter(&batch->jib_lock);
	if (batch->jib_error == 0 && error != 0)
		batch->jib_error = error;
	KASSERT(batch->jib_pending != 0);
	batch->jib_pending--;
	if (batch->jib_pending == 0)
		wakeup(batch);
	mtx_leave(&batch->jib_lock);
}

static void
ext4fs_journal_io_init (struct ext4fs_journal_io_batch *batch)
{
	mtx_init(&batch->jib_lock, IPL_BIO);
	batch->jib_pending = 0;
	batch->jib_error = 0;
}

static void
ext4fs_journal_io_submit (struct ext4fs_journal_io_batch *batch,
    struct buf *bp)
{
	KASSERT(bp != NULL);
	KASSERT(ISSET(bp->b_flags, B_BUSY));
	KASSERT(bp->b_iodone == NULL);

	SET(bp->b_flags, B_NOCACHE | B_ASYNC | B_CALL);
	bp->b_saveaddr = batch;
	bp->b_iodone = ext4fs_journal_write_iodone;
	mtx_enter(&batch->jib_lock);
	KASSERT(batch->jib_pending != UINT32_MAX);
	batch->jib_pending++;
	mtx_leave(&batch->jib_lock);
	/* The device bufq bounds writes at its high-water mark. */
	bawrite(bp);
}

static void
ext4fs_journal_io_submit_raw (
    struct ext4fs_journal_io_batch *batch, struct vnode *devvp,
    struct buf *bp)
{
	KASSERT(devvp != NULL);
	KASSERT(devvp->v_type == VBLK);
	KASSERT(bp != NULL);
	KASSERT(bp->b_vp == NULL);
	KASSERT(ISSET(bp->b_flags, B_BUSY));
	KASSERT(bp->b_iodone == NULL);

	CLR(bp->b_flags, B_READ | B_DONE | B_ERROR | B_EINTR |
	    B_INVAL | B_DELWRI);
	SET(bp->b_flags, B_NOCACHE | B_ASYNC | B_CALL | B_RAW);
	bp->b_dev = devvp->v_rdev;
	bp->b_error = 0;
	bp->b_resid = 0;
	bp->b_saveaddr = batch;
	bp->b_iodone = ext4fs_journal_write_iodone;
	mtx_enter(&batch->jib_lock);
	KASSERT(batch->jib_pending != UINT32_MAX);
	batch->jib_pending++;
	mtx_leave(&batch->jib_lock);
	VOP_STRATEGY(devvp, bp);
	if (bp->b_bq != NULL)
		bufq_wait(bp->b_bq);
}

static int
ext4fs_journal_io_wait (struct ext4fs_journal_io_batch *batch)
{
	int error;

	mtx_enter(&batch->jib_lock);
	while (batch->jib_pending != 0)
		msleep_nsec(batch, &batch->jib_lock, PRIBIO,
		    "e4jwrite", INFSLP);
	error = batch->jib_error;
	mtx_leave(&batch->jib_lock);
	return (error);
}

int
ext4fs_journal_submit_data (struct ext4fs_journal_handle *handle,
    struct buf *bp)
{
	struct ext4fs_journal_transaction *tx;
	struct ext4fs_journal *journal;
	int error;

	if (handle == NULL || handle->jh_journal == NULL || bp == NULL ||
	    bp->b_data == NULL || ! ISSET(bp->b_flags, B_BUSY) ||
	    bp->b_iodone != NULL)
		return (EINVAL);
	journal = handle->jh_journal;
	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	tx = handle->jh_transaction;
	mtx_leave(&journal->j_lock);
	if (error)
		return (error);

	mtx_enter(&tx->jt_data_io.jib_lock);
	error = tx->jt_data_io.jib_error;
	mtx_leave(&tx->jt_data_io.jib_lock);
	if (error) {
		ext4fs_journal_abort_impl(journal->j_mp, error, __func__);
		return (error);
	}
	ext4fs_journal_io_submit(&tx->jt_data_io, bp);
	return (0);
}

int
ext4fs_journal_wait_data (struct ext4fs_journal_handle *handle)
{
	struct ext4fs_journal_transaction *tx;
	struct ext4fs_journal *journal;
	int error;

	if (handle == NULL || handle->jh_journal == NULL)
		return (EINVAL);
	journal = handle->jh_journal;
	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	tx = handle->jh_transaction;
	mtx_leave(&journal->j_lock);
	if (error)
		return (error);
	error = ext4fs_journal_io_wait(&tx->jt_data_io);
	if (error)
		ext4fs_journal_abort_impl(journal->j_mp, error, __func__);
	return (error);
}

int
ext4fs_journal_wait_pending_data (struct mount *mp)
{
	struct ext4fs_journal_transaction *tx;
	struct ext4fs_journal_handle *handle;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	u_int32_t pending;
	int end_error, error, io_error;

	fs = VFSTOUFS(mp)->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL)
		return (0);
	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_state_admission(0,
	    journal->j_aborted, journal->j_error,
	    journal->j_shutting_down);
	tx = journal->j_running;
	pending = 0;
	io_error = 0;
	if (error == 0 && tx != NULL) {
		mtx_enter(&tx->jt_data_io.jib_lock);
		if (tx->jt_data_io.jib_pending != 0)
			pending = 1;
		io_error = tx->jt_data_io.jib_error;
		mtx_leave(&tx->jt_data_io.jib_lock);
	}
	tx = journal->j_committing;
	if (error == 0 && tx != NULL) {
		mtx_enter(&tx->jt_data_io.jib_lock);
		if (tx->jt_data_io.jib_pending != 0)
			pending = 1;
		if (io_error == 0)
			io_error = tx->jt_data_io.jib_error;
		mtx_leave(&tx->jt_data_io.jib_lock);
	}
	mtx_leave(&journal->j_lock);
	if (error)
		return (error);
	if (io_error) {
		ext4fs_journal_abort_impl(mp, io_error, __func__);
		return (io_error);
	}
	if (pending == 0)
		return (0);

	handle = NULL;
	error = ext4fs_journal_begin(mp, 1, &handle);
	if (error)
		return (error);
	error = ext4fs_journal_wait_data(handle);
	end_error = ext4fs_journal_end(handle);
	if (error == 0)
		error = end_error;
	return (error);
}

static void
ext4fs_journal_scratch_acquire (struct ext4fs_journal *journal,
    void **blockp)
{
	mtx_enter(&journal->j_lock);
	KASSERT(journal->j_commit_busy);
	KASSERT(! journal->j_scratch_busy);
	KASSERT(journal->j_scratch_block != NULL);
	journal->j_scratch_busy = 1;
	*blockp = journal->j_scratch_block;
	mtx_leave(&journal->j_lock);
}

static void
ext4fs_journal_scratch_release (struct ext4fs_journal *journal)
{
	mtx_enter(&journal->j_lock);
	KASSERT(journal->j_commit_busy);
	KASSERT(journal->j_scratch_busy);
	journal->j_scratch_busy = 0;
	mtx_leave(&journal->j_lock);
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
ext4fs_journal_next_block (struct ext4fs_journal *journal,
    u_int32_t block)
{
	block++;
	if (block >= journal->j_maxlen)
		block = journal->j_first;
	return (block);
}

static int
ext4fs_journal_prepare_block (struct ext4fs_journal *journal,
    u_int32_t jblock, const void *data, struct buf **bpp)
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
	*bpp = bp;
	return (0);
}

static int
ext4fs_journal_write_block (struct ext4fs_journal *journal,
    u_int32_t jblock, const void *data)
{
	struct buf *bp;
	int error;

	error = ext4fs_journal_prepare_block(journal, jblock, data,
	    &bp);
	if (error)
		return (error);
	/* Do not weaken journal ordering on an asynchronous mount. */
	SET(bp->b_flags, B_NOCACHE);
	return (bwrite(bp));
}

static int
ext4fs_journal_queue_block (struct ext4fs_journal *journal,
    u_int32_t jblock, const void *data,
    struct ext4fs_journal_io_batch *batch)
{
	struct buf *bp;
	int error;

	error = ext4fs_journal_prepare_block(journal, jblock, data,
	    &bp);
	if (error)
		return (error);
	ext4fs_journal_io_submit(batch, bp);
	return (0);
}

static int
ext4fs_journal_super_valid (struct ext4fs_journal *journal,
    struct jbd2_superblock *jsb)
{
	struct jbd2_replay_ctx ctx;

	ext4fs_journal_checksum_ctx(journal, &ctx);
	if (betoh32(jsb->s_header.h_magic) != JBD2_MAGIC ||
	    betoh32(jsb->s_header.h_blocktype) !=
	    JBD2_SUPERBLOCK_V2 ||
	    betoh32(jsb->s_blocksize) != journal->j_blocksize ||
	    betoh32(jsb->s_maxlen) != journal->j_maxlen ||
	    betoh32(jsb->s_first) != journal->j_first ||
	    betoh32(jsb->s_feature_compat) !=
	    journal->j_features_compat ||
	    betoh32(jsb->s_feature_incompat) !=
	    journal->j_features_incompat ||
	    betoh32(jsb->s_feature_ro_compat) !=
	    journal->j_features_ro_compat ||
	    memcmp(jsb->s_uuid, journal->j_uuid,
	    sizeof(journal->j_uuid)) != 0)
		return (0);
	return (jbd2_superblock_csum_verify(&ctx, jsb));
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
	if (! ext4fs_journal_super_valid(journal, jsb)) {
		brelse(bp);
		return (EINVAL);
	}
	jsb->s_start = htobe32(start);
	jsb->s_sequence = htobe32(sequence);
	jsb->s_head = htobe32(head);
	ext4fs_journal_checksum_ctx(journal, &ctx);
	jbd2_superblock_csum_set(&ctx, jsb);
	SET(bp->b_flags, B_NOCACHE);
	return (bwrite(bp));
}

static int
ext4fs_journal_enable_revoke (struct ext4fs_journal *journal)
{
	struct jbd2_replay_ctx ctx;
	struct jbd2_superblock *jsb;
	struct m_ext4fs *fs;
	struct ufsmount *ump;
	struct buf *bp;
	u_int32_t features;
	int error;

	/*
	 * Upgrade only a clean journal.  The new feature reaches stable
	 * storage before runtime code may emit a revoke record and before
	 * the filesystem RECOVER flag is set.
	 */
	if (journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_REVOKE)
		return (0);

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
	if (! ext4fs_journal_super_valid(journal, jsb) ||
	    betoh32(jsb->s_start) != 0) {
		brelse(bp);
		return (EINVAL);
	}

	features = journal->j_features_incompat |
	    JBD2_FEATURE_INCOMPAT_REVOKE;
	jsb->s_feature_incompat = htobe32(features);
	ext4fs_journal_checksum_ctx(journal, &ctx);
	ctx.rc_features_incompat = features;
	jbd2_superblock_csum_set(&ctx, jsb);
	SET(bp->b_flags, B_NOCACHE);
	error = bwrite(bp);
	if (error == 0)
		error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error == 0)
		journal->j_features_incompat = features;
	return (error);
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
	fs->m_sble.sb_feature_incompat =
	    htole32(fs->m_feature_incompat);
	fs->m_fs_was_modified = 1;
	error = ext4fs_sbwrite_lifecycle(journal->j_mp);
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

	if (journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_CSUM_V3)
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
	if (! same_uuid)
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
		tag_bytes = ext4fs_journal_tag_bytes(journal,
		    count != 0);
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

		count = ext4fs_journal_descriptor_count(journal,
		    metadata);
		if (count == 0)
			return (EINVAL);
		descriptors++;
		metadata_blocks += count;
		for (i = 0; i < count; i++) {
			if (! metadata->jm_dirty ||
			    metadata->jm_owner != NULL ||
			    metadata->jm_buf == NULL ||
			    ! ISSET(metadata->jm_buf->b_flags, B_BUSY))
				return (EINVAL);
			metadata = TAILQ_NEXT(metadata, jm_entry);
		}
	}

	revoke_count = 0;
	TAILQ_FOREACH(revoke, &tx->jt_revokes, jr_entry)
		revoke_count++;
	revoke_blocks = 0;
	if (revoke_count != 0) {
		if (! (journal->j_features_incompat &
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
	    (u_int64_t)metadata_blocks + revoke_count !=
	    tx->jt_credits_used)
		return (EINVAL);

	total = (u_int64_t)descriptors + metadata_blocks +
	    revoke_blocks + 1;
	if (total > 0xffffffffU || total > journal->j_free)
		return (ENOSPC);
	*blocksp = (u_int32_t)total;
	return (0);
}

static int
ext4fs_journal_snapshot_metadata (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx)
{
	struct ext4fs_journal_metadata *metadata;

	TAILQ_FOREACH(metadata, &tx->jt_metadata, jm_entry) {
		if (metadata->jm_snapshot != NULL ||
		    metadata->jm_buf == NULL || ! metadata->jm_dirty ||
		    metadata->jm_owner != NULL ||
		    metadata->jm_update_pending ||
		    ! ISSET(metadata->jm_buf->b_flags, B_BUSY))
			return (EINVAL);
		metadata->jm_snapshot = malloc(journal->j_blocksize,
		    M_UFSMNT, M_WAITOK);
		metadata->jm_snapshot_size = journal->j_blocksize;
		memcpy(metadata->jm_snapshot, metadata->jm_buf->b_data,
		    journal->j_blocksize);
	}
	return (0);
}

static int
ext4fs_journal_write_metadata (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx, u_int32_t *jblockp,
    void *descriptor,
    struct ext4fs_journal_io_batch *batch)
{
	struct ext4fs_journal_metadata *first, *metadata, *next;
	struct jbd2_block_tail *tail;
	struct jbd2_header *header;
	struct jbd2_replay_ctx ctx;
	const void *payload;
	u_int32_t checksum, count, flags, i, limit, offset, word;
	int error, escaped, has_64bit, has_csum_v2, has_csum_v3;

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
			/*
			 * Checksum the immutable snapshot directly.  Escaping
			 * logically replaces its first word with zero.
			 */
			flags = i == count - 1 ? JBD2_FLAG_LAST_TAG : 0;
			if (i != 0)
				flags |= JBD2_FLAG_SAME_UUID;
			memcpy(&word, metadata->jm_snapshot, sizeof(word));
			escaped = word == htobe32(JBD2_MAGIC);
			if (escaped)
				flags |= JBD2_FLAG_ESCAPE;
			if (! jbd2_has_csum_v2or3(&ctx))
				checksum = 0;
			else if (escaped)
				checksum =
				    jbd2_data_block_checksum_escaped(&ctx,
				    metadata->jm_snapshot,
				    tx->jt_sequence);
			else
				checksum = jbd2_data_block_checksum(&ctx,
				    metadata->jm_snapshot,
				    tx->jt_sequence);

			if (has_csum_v3) {
				ext4fs_journal_put32(descriptor, offset,
				    (u_int32_t)metadata->jm_fsblock);
				ext4fs_journal_put32(
				    descriptor, offset + 4, flags);
				ext4fs_journal_put32(
				    descriptor, offset + 8,
				    has_64bit ? (u_int32_t)
				    (metadata->jm_fsblock >> 32) : 0);
				ext4fs_journal_put32(
				    descriptor, offset + 12, checksum);
				offset +=
				    sizeof(struct jbd2_block_tag3);
			} else {
				ext4fs_journal_put32(descriptor, offset,
				    (u_int32_t)metadata->jm_fsblock);
				ext4fs_journal_put16(
				    descriptor, offset + 4,
				    (u_int16_t)checksum);
				ext4fs_journal_put16(
				    descriptor, offset + 6,
				    (u_int16_t)flags);
				offset += 8;
				if (has_64bit) {
					ext4fs_journal_put32(
					    descriptor, offset,
					    (u_int32_t)
				    (metadata->jm_fsblock >> 32));
					offset += 4;
				}
				if (has_csum_v2)
					offset +=
					    JBD2_CSUM_V2_TAG_EXTRA;
			}
			if (i == 0) {
				memcpy((char *)descriptor + offset,
				    journal->j_uuid,
				    sizeof(journal->j_uuid));
				offset += sizeof(journal->j_uuid);
			}
			metadata = TAILQ_NEXT(metadata, jm_entry);
		}
		KASSERT(offset <= limit);
		if (jbd2_has_csum_v2or3(&ctx)) {
			tail = (struct jbd2_block_tail *)
			    ((char *)descriptor + journal->j_blocksize -
			    sizeof(*tail));
			tail->t_checksum = htobe32(
			    jbd2_block_checksum(&ctx, descriptor,
			    journal->j_blocksize));
		}
		error = ext4fs_journal_queue_block(journal, *jblockp,
		    descriptor, batch);
		if (error)
			return (error);
		*jblockp = ext4fs_journal_next_block(journal, *jblockp);

		metadata = first;
		for (i = 0; i < count; i++) {
			/*
			 * The queued buffer copies its input.  Only escaped
			 * payloads need the reusable workspace.
			 */
			next = TAILQ_NEXT(metadata, jm_entry);
			payload = metadata->jm_snapshot;
			memcpy(&word, payload, sizeof(word));
			if (word == htobe32(JBD2_MAGIC)) {
				memcpy(descriptor, payload,
				    journal->j_blocksize);
				memset(descriptor, 0, sizeof(word));
				payload = descriptor;
			}
			error = ext4fs_journal_queue_block(journal,
			    *jblockp, payload, batch);
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
    struct ext4fs_journal_transaction *tx, u_int32_t *jblockp,
    void *block, struct ext4fs_journal_io_batch *batch)
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
	if (! (journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_REVOKE))
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
		header->r_header.h_blocktype =
		    htobe32(JBD2_REVOKE_BLOCK);
		header->r_header.h_sequence = htobe32(tx->jt_sequence);
		offset = sizeof(*header);
		while (revoke != NULL &&
		    record_size <= limit - offset) {
			if (has_64bit) {
				ext4fs_journal_put32(block, offset,
				    (u_int32_t)
				    (revoke->jr_fsblock >> 32));
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
			tail = (struct jbd2_block_tail *)
			    ((char *)block + journal->j_blocksize -
			    sizeof(*tail));
			tail->t_checksum = htobe32(
			    jbd2_block_checksum(&ctx, block,
			    journal->j_blocksize));
		}
		error = ext4fs_journal_queue_block(journal,
		    *jblockp, block, batch);
		if (error)
			return (error);
		*jblockp = ext4fs_journal_next_block(journal, *jblockp);
	}
	return (0);
}

static int
ext4fs_journal_write_commit (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx, u_int32_t *jblockp,
    void *block)
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
		commit->h_checksum[0] = htobe32(
		    jbd2_block_checksum(&ctx, block,
		    journal->j_blocksize));
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

	error = ext4fs_journal_io_wait(&tx->jt_data_io);
	if (error)
		return (error);
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
ext4fs_journal_release_metadata (
    struct ext4fs_journal_transaction *tx)
{
	struct ext4fs_journal_metadata *metadata, *removed;

	TAILQ_FOREACH(metadata, &tx->jt_metadata, jm_entry) {
		if (metadata->jm_buf == NULL || ! metadata->jm_dirty ||
		    metadata->jm_owner != NULL ||
		    ! ISSET(metadata->jm_buf->b_flags, B_BUSY) ||
		    ! ISSET(metadata->jm_buf->b_flags, B_DONE) ||
		    metadata->jm_snapshot == NULL ||
		    metadata->jm_snapshot_size !=
		    (u_int32_t)metadata->jm_buf->b_bcount)
			return (EINVAL);
	}
	TAILQ_FOREACH(metadata, &tx->jt_metadata, jm_entry) {
		/* The durable log now permits an asynchronous home write. */
		removed = RB_REMOVE(
		    ext4fs_journal_metadata_buffer_tree,
		    &tx->jt_metadata_buffers, metadata);
		if (removed != metadata)
			return (EINVAL);
		bdwrite(metadata->jm_buf);
		metadata->jm_buf = NULL;
	}
	return (0);
}

static int
ext4fs_journal_metadata_revoked (
    struct ext4fs_journal_transaction *tx, u_int64_t fsblock)
{
	struct ext4fs_journal_transaction *later;

	for (later = TAILQ_NEXT(tx, jt_entry); later != NULL;
	    later = TAILQ_NEXT(later, jt_entry)) {
		if (ext4fs_journal_revoke_block_find(later,
		    fsblock) != NULL)
			return (1);
	}
	return (0);
}

static int
ext4fs_journal_checkpoint (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx)
{
	struct ext4fs_journal_io_batch batch;
	struct ext4fs_journal_metadata *metadata;
	struct m_ext4fs *fs;
	struct ufsmount *ump;
	struct buf *bp;

	ump = VFSTOUFS(journal->j_mp);
	fs = ump->um_e4fs;
	TAILQ_FOREACH(metadata, &tx->jt_metadata, jm_entry) {
		if (metadata->jm_buf != NULL || ! metadata->jm_dirty ||
		    metadata->jm_owner != NULL ||
		    metadata->jm_snapshot == NULL ||
		    metadata->jm_snapshot_size != journal->j_blocksize ||
		    metadata->jm_fsblock >= fs->m_blocks_count)
			return (EINVAL);
	}
	ext4fs_journal_io_init(&batch);
	while ((metadata = TAILQ_FIRST(&tx->jt_metadata)) != NULL) {
		if (ext4fs_journal_metadata_revoked(tx,
		    metadata->jm_fsblock)) {
			ext4fs_journal_update_reset(metadata);
			ext4fs_journal_metadata_remove(tx, metadata);
			free(metadata->jm_snapshot, M_UFSMNT,
			    metadata->jm_snapshot_size);
			free(metadata, M_UFSMNT, sizeof(*metadata));
			continue;
		}
		bp = geteblk(metadata->jm_snapshot_size);
		bp->b_blkno = (daddr_t)EXT4FS_FSBTODB(fs,
		    metadata->jm_fsblock);
		bp->b_lblkno = bp->b_blkno;
		memcpy(bp->b_data, metadata->jm_snapshot,
		    metadata->jm_snapshot_size);
		ext4fs_journal_update_reset(metadata);
		ext4fs_journal_metadata_remove(tx, metadata);
		free(metadata->jm_snapshot, M_UFSMNT,
		    metadata->jm_snapshot_size);
		metadata->jm_snapshot = NULL;
		metadata->jm_snapshot_size = 0;
		ext4fs_journal_io_submit_raw(&batch, ump->um_devvp, bp);
		free(metadata, M_UFSMNT, sizeof(*metadata));
	}
	return (ext4fs_journal_io_wait(&batch));
}

static int
ext4fs_journal_should_checkpoint (struct ext4fs_journal *journal)
{
	u_int32_t block_limit, usable;

	usable = journal->j_maxlen - journal->j_first;
	block_limit = EXT4FS_JOURNAL_COMMITTED_BYTES_MAX /
	    journal->j_blocksize;
	if (block_limit == 0)
		block_limit = 1;
	if (! (journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_REVOKE))
		return (1);
	if (journal->j_mp->mnt_flag & MNT_SYNCHRONOUS)
		return (1);
	if (journal->j_committed_count >=
	    EXT4FS_JOURNAL_COMMITTED_MAX)
		return (1);
	if (journal->j_committed_blocks >= block_limit)
		return (1);
	return (journal->j_free <= usable / 4);
}

/* The caller holds j_lock while inspecting the committed queue. */
static struct ext4fs_journal_transaction *
ext4fs_journal_committed_find (struct ext4fs_journal *journal,
    u_int32_t sequence)
{
	struct ext4fs_journal_transaction *tx;

	TAILQ_FOREACH(tx, &journal->j_committed, jt_entry) {
		if (tx->jt_sequence == sequence)
			return (tx);
	}
	return (NULL);
}

/* Return the last committed transaction strictly before the fence. */
static struct ext4fs_journal_transaction *
ext4fs_journal_committed_before (struct ext4fs_journal *journal,
    u_int32_t sequence)
{
	struct ext4fs_journal_transaction *last, *tx;

	last = NULL;
	TAILQ_FOREACH(tx, &journal->j_committed, jt_entry) {
		if (! ext4fs_journal_state_sequence_before(
		    tx->jt_sequence, sequence))
			break;
		last = tx;
	}
	return (last);
}

static int
ext4fs_journal_checkpoint_committed (
    struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction_head *done,
    struct ext4fs_journal_transaction *last)
{
	struct ext4fs_journal_transaction *next, *tx;
	struct ufsmount *ump;
	u_int32_t count, released, sequence, start, usable;
	int error, found, state_error;

	if (TAILQ_EMPTY(&journal->j_committed))
		return (0);
	ump = VFSTOUFS(journal->j_mp);

	mtx_enter(&journal->j_lock);
	if (last == NULL)
		last = TAILQ_LAST(&journal->j_committed,
		    ext4fs_journal_transaction_head);
	found = 0;
	TAILQ_FOREACH(tx, &journal->j_committed, jt_entry) {
		if (tx->jt_state !=
		    EXT4FS_JOURNAL_TRANSACTION_COMMITTED) {
			mtx_leave(&journal->j_lock);
			return (EINVAL);
		}
		if (tx == last) {
			found = 1;
			break;
		}
	}
	if (! found) {
		mtx_leave(&journal->j_lock);
		return (EINVAL);
	}
	TAILQ_FOREACH(tx, &journal->j_committed, jt_entry) {
		state_error = ext4fs_journal_transaction_transition(
		    &tx->jt_state,
		    EXT4FS_JOURNAL_TRANSACTION_CHECKPOINTING);
		if (state_error != 0)
			panic("ext4fs: %s: journal checkpoint "
			    "transition", journal->j_mp->mnt_stat.f_mntonname);
		if (tx == last)
			break;
	}
	wakeup(&journal->j_committing);
	mtx_leave(&journal->j_lock);

	/*
	 * Wait for each transaction in the selected prefix before
	 * submitting its successor.  This preserves home-block order
	 * when consecutive transactions contain different frozen images
	 * of the same metadata block.  One durability flush covers the
	 * selected ordered batch.
	 */
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_CHECKPOINT);
	TAILQ_FOREACH(tx, &journal->j_committed, jt_entry) {
		error = ext4fs_journal_checkpoint(journal, tx);
		if (error)
			return (error);
		if (tx == last)
			break;
	}
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_CHECKPOINT_FLUSH);
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		return (error);

	next = TAILQ_NEXT(last, jt_entry);
	if (next == NULL) {
		start = 0;
		sequence = last->jt_sequence + 1;
	} else {
		start = next->jt_log_start;
		sequence = next->jt_sequence;
	}
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_CLEAR);
	error = ext4fs_journal_write_super(journal, start, sequence,
	    journal->j_head);
	if (error)
		return (error);
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_CLEAR_FLUSH);
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		return (error);

	usable = journal->j_maxlen - journal->j_first;
	count = 0;
	released = 0;
	mtx_enter(&journal->j_lock);
	while ((tx = TAILQ_FIRST(&journal->j_committed)) != NULL) {
		KASSERT(tx->jt_log_blocks <= UINT32_MAX - released);
		released += tx->jt_log_blocks;
		count++;
		state_error = ext4fs_journal_transaction_transition(
		    &tx->jt_state,
		    EXT4FS_JOURNAL_TRANSACTION_DONE);
		if (state_error != 0)
			panic("ext4fs: %s: journal completion transition",
			    journal->j_mp->mnt_stat.f_mntonname);
		TAILQ_REMOVE(&journal->j_committed, tx, jt_entry);
		TAILQ_INSERT_TAIL(done, tx, jt_entry);
		if (tx == last)
			break;
	}
	journal->j_tail = next != NULL ? next->jt_log_start :
	    journal->j_head;
	state_error = ext4fs_journal_state_checkpoint_release(usable,
	    &journal->j_free, &journal->j_committed_count,
	    &journal->j_committed_blocks, count, released);
	if (state_error != 0)
		panic("ext4fs: %s: journal checkpoint accounting",
		    journal->j_mp->mnt_stat.f_mntonname);
	mtx_leave(&journal->j_lock);

	return (0);
}

static int
ext4fs_journal_commit_transaction (struct ext4fs_journal *journal,
    struct ext4fs_journal_transaction *tx)
{
	struct ext4fs_journal_io_batch batch;
	struct ext4fs_journal_transaction *oldest;
	struct ufsmount *ump;
	void *block;
	u_int32_t commit_block, expected_head, jblock, required;
	u_int32_t expose_sequence, expose_start;
	u_int32_t start, usable;
	int error, wait_error;

	ump = VFSTOUFS(journal->j_mp);
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_VALIDATE);
	error = ext4fs_journal_log_blocks(journal, tx, &required);
	if (error)
		return (error);
	error = ext4fs_journal_materialize_transaction(journal, tx);
	if (error)
		return (error);
	error = ext4fs_journal_snapshot_metadata(journal, tx);
	if (error)
		return (error);
	start = jblock = journal->j_head;
	usable = journal->j_maxlen - journal->j_first;
	commit_block = journal->j_first +
	    ((u_int64_t)(start - journal->j_first) + required - 1) %
	    usable;
	expected_head = ext4fs_journal_next_block(journal,
	    commit_block);
	ext4fs_journal_scratch_acquire(journal, &block);
	ext4fs_journal_io_init(&batch);

	/*
	 * A commit uses the first two durability barriers.  A later
	 * checkpoint batch uses the final two:
	 *
	 * 1. ordered data, log records, and the cleared commit slot;
	 * 2. journal exposure and the new commit record;
	 * 3. checkpointed home metadata for committed transactions;
	 * 4. the advanced or clean journal superblock.
	 *
	 * Writes within either of the first two groups may reach the
	 * device in any order.  The first flush makes ordered data and
	 * the complete log durable before a commit can be written.  The
	 * second makes exposure and commit durable before the transaction
	 * can enter the committed queue.
	 */
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_ORDERED_DATA);
	error = ext4fs_journal_flush_ordered(tx);
	if (error)
		goto out;

	/*
	 * All vnode-dependent work is complete and the frozen metadata
	 * image cannot change.  Independent operations may now build a
	 * successor transaction.  Access to an aliased busy metadata
	 * buffer continues to wait until this commit becomes durable.
	 */
	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_transaction_transition(&tx->jt_state,
	    EXT4FS_JOURNAL_TRANSACTION_FROZEN);
	if (error == 0)
		wakeup(&journal->j_committing);
	mtx_leave(&journal->j_lock);
	if (error)
		goto out;

	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_METADATA);
	error = ext4fs_journal_write_metadata(journal, tx, &jblock,
	    block, &batch);
	if (error)
		goto precommit_wait;
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_REVOKES);
	error = ext4fs_journal_write_revokes(journal, tx, &jblock,
	    block, &batch);
	if (error)
		goto precommit_wait;
	if (jblock != commit_block) {
		error = EINVAL;
		goto precommit_wait;
	}
	/* Remove an old commit header before exposure. */
	memset(block, 0, journal->j_blocksize);
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_COMMIT_CLEAR);
	error = ext4fs_journal_queue_block(journal, jblock, block,
	    &batch);
	if (error)
		goto precommit_wait;

precommit_wait:
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_PRECOMMIT_FLUSH);
	wait_error = ext4fs_journal_io_wait(&batch);
	if (error == 0)
		error = wait_error;
	if (error)
		goto out;
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		goto out;

	/*
	 * Only expose the transaction after every pre-commit record is
	 * durable.  This lets recovery ignore a crash during descriptor
	 * construction while still making s_start durable before the
	 * commit block can reach disk.
	 */
	oldest = TAILQ_FIRST(&journal->j_committed);
	if (oldest == NULL) {
		expose_start = start;
		expose_sequence = tx->jt_sequence;
	} else {
		expose_start = oldest->jt_log_start;
		expose_sequence = oldest->jt_sequence;
	}
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_EXPOSE);
	error = ext4fs_journal_write_super(journal, expose_start,
	    expose_sequence, start);
	if (error)
		goto out;

	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_COMMIT);
	error = ext4fs_journal_write_commit(journal, tx, &jblock,
	    block);
	if (error)
		goto out;
	if (jblock != expected_head) {
		error = EINVAL;
		goto out;
	}
	ext4fs_journal_set_stage(journal,
	    EXT4FS_JOURNAL_STAGE_COMMIT_FLUSH);
	error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error)
		goto out;
	tx->jt_log_start = start;
	tx->jt_log_end = expected_head;
	tx->jt_log_blocks = required;
	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_transaction_transition(&tx->jt_state,
	    EXT4FS_JOURNAL_TRANSACTION_COMMITTED);
	mtx_leave(&journal->j_lock);
	if (error)
		goto out;

	/*
	 * The durable journal now owns the frozen metadata image.  The
	 * live cache buffer may be released before its home write, so a
	 * later transaction can modify the cached successor while this
	 * transaction checkpoints its immutable predecessor.
	 */
	error = ext4fs_journal_release_metadata(tx);

out:
	ext4fs_journal_scratch_release(journal);
	return (error);
}

static void
ext4fs_journal_timeout_drain (struct ext4fs_journal *journal)
{
	timeout_del_barrier(&journal->j_commit_timeout);

	mtx_enter(&journal->j_lock);
	KASSERT(journal->j_timeout_draining);
	journal->j_timeout_draining = 0;
	wakeup(&journal->j_timeout_draining);
	mtx_leave(&journal->j_lock);
}

static void
ext4fs_journal_commit_timeout (void *arg)
{
	struct ext4fs_journal *journal = arg;
	int queue;

	queue = 0;
	mtx_enter(&journal->j_lock);
	if (! journal->j_shutting_down &&
	    journal->j_running != NULL) {
		journal->j_expired_sequence =
		    journal->j_running->jt_sequence;
		queue = 1;
	}
	mtx_leave(&journal->j_lock);
	if (queue)
		task_add(journal->j_commit_taskq,
		    &journal->j_commit_task);
}

static void
ext4fs_journal_commit_task (void *arg)
{
	struct ext4fs_journal *journal = arg;
	u_int32_t sequence;
	int commit;

	commit = 0;
	mtx_enter(&journal->j_lock);
	if (! journal->j_shutting_down) {
		sequence = journal->j_expired_sequence;
		commit = 1;
	}
	mtx_leave(&journal->j_lock);
	if (commit)
		(void)ext4fs_journal_commit_internal(journal->j_mp,
		    EXT4FS_JOURNAL_COMMIT_TRANSACTION_AGE,
		    sequence, EXT4FS_JOURNAL_FENCE_MATCH);
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
	if (! (fs->m_feature_compat &
	    EXT4FS_FEATURE_COMPAT_HAS_JOURNAL))
		return (0);

	error = jbd2_journal_open(ump->um_devvp, fs, &ctx, &superblock);
	if (error)
		return (error);
	if (ctx.rc_start != 0) {
		ext4fs_print(mp, "refusing runtime journal with "
		    "non-empty log\n");
		jbd2_journal_close(&ctx);
		return (EINVAL);
	}

	journal = malloc(sizeof(*journal), M_UFSMNT, M_WAITOK | M_ZERO);
	mtx_init(&journal->j_lock, IPL_NONE);
	TAILQ_INIT(&journal->j_committed);
	journal->j_commit_taskq = taskq_create("ext4fsj", 1,
	    IPL_NONE, 0);
	if (journal->j_commit_taskq == NULL) {
		free(journal, M_UFSMNT, sizeof(*journal));
		jbd2_journal_close(&ctx);
		return (ENOMEM);
	}
	timeout_set(&journal->j_commit_timeout,
	    ext4fs_journal_commit_timeout, journal);
	task_set(&journal->j_commit_task,
	    ext4fs_journal_commit_task, journal);
	journal->j_mp = mp;
	journal->j_superblock = superblock;
	journal->j_blocksize = ctx.rc_blocksize;
	journal->j_scratch_block = malloc(journal->j_blocksize,
	    M_UFSMNT, M_WAITOK);
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
	 * Each metadata credit may need one descriptor block
	 * and one payload block.  Keep one further block for the commit
	 * record.
	 */
	journal->j_max_transaction_credits =
	    ext4fs_journal_state_credit_limit(usable,
	    ctx.rc_max_transaction, journal->j_blocksize,
	    EXT4FS_JOURNAL_DIRTY_METADATA_MAX);

	journal->j_blockmap = ctx.rc_blockmap;
	journal->j_blockmap_count = ctx.rc_blockmap_count;
	journal->j_blockset = ctx.rc_blockset;
	journal->j_blockset_mask = ctx.rc_blockset_mask;
	ctx.rc_blockmap = NULL;
	ctx.rc_blockset = NULL;
	jbd2_journal_close(&ctx);

	fs->m_journal = journal;
	if (! fs->m_read_only) {
		error = ext4fs_journal_enable_revoke(journal);
		if (error == 0)
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
	struct ext4fs_journal_transaction *committed, *committing;
	struct ext4fs_journal_transaction *running;
	struct m_ext4fs *fs;

	fs = VFSTOUFS(mp)->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL)
		return;

	mtx_enter(&journal->j_lock);
	journal->j_shutting_down = 1;
	journal->j_handoff = NULL;
	wakeup(&journal->j_active);
	mtx_leave(&journal->j_lock);

	timeout_del_barrier(&journal->j_commit_timeout);
	taskq_del_barrier(journal->j_commit_taskq,
	    &journal->j_commit_task);

	mtx_enter(&journal->j_lock);
	while (journal->j_active != NULL)
		msleep_nsec(&journal->j_active, &journal->j_lock,
		    PRIBIO, "e4jdrain", INFSLP);
	while (journal->j_commit_busy)
		msleep_nsec(&journal->j_committing,
		    &journal->j_lock, PRIBIO, "e4jiodrn", INFSLP);
	KASSERT(! journal->j_scratch_busy);
	running = journal->j_running;
	committing = journal->j_committing;
	journal->j_running = NULL;
	journal->j_committing = NULL;
	fs->m_journal = NULL;
	mtx_leave(&journal->j_lock);

	ext4fs_journal_transaction_free(running);
	ext4fs_journal_transaction_free(committing);
	while ((committed = TAILQ_FIRST(&journal->j_committed)) != NULL) {
		TAILQ_REMOVE(&journal->j_committed, committed, jt_entry);
		ext4fs_journal_transaction_free(committed);
	}
	if (journal->j_blockset != NULL)
		free(journal->j_blockset, M_TEMP,
		    (journal->j_blockset_mask + 1) *
		    sizeof(*journal->j_blockset));
	if (journal->j_blockmap != NULL)
		free(journal->j_blockmap, M_TEMP,
		    journal->j_blockmap_count *
		    sizeof(*journal->j_blockmap));
	free(journal->j_scratch_block, M_UFSMNT,
	    journal->j_blocksize);
	taskq_destroy(journal->j_commit_taskq);
	free(journal, M_UFSMNT, sizeof(*journal));
}

void
ext4fs_journal_abort_impl (struct mount *mp, int error,
    const char *caller)
{
	struct ext4fs_journal_abort_info info;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	int first;

	fs = VFSTOUFS(mp)->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL)
		return;
	if (error == 0)
		error = EIO;

	mtx_enter(&journal->j_lock);
	first = ext4fs_journal_abort_locked(journal, error, caller,
	    &info);
	mtx_leave(&journal->j_lock);
	if (first)
		ext4fs_journal_abort_policy(mp, error, &info);
}

int
ext4fs_journal_error (struct mount *mp)
{
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	int error;

	fs = VFSTOUFS(mp)->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL)
		return (0);
	mtx_enter(&journal->j_lock);
	error = journal->j_aborted ? journal->j_error : 0;
	if (journal->j_aborted && error == 0)
		error = EIO;
	mtx_leave(&journal->j_lock);
	return (error);
}

int
ext4fs_journal_begin (struct mount *mp, unsigned int credits,
    struct ext4fs_journal_handle **handlep)
{
	struct ext4fs_journal *journal;
	struct ext4fs_journal_handle *handle;
	struct ext4fs_journal_transaction *candidate, *tx;
	struct m_ext4fs *fs;
	int allocate, arm, commit, error;

	if (handlep == NULL || credits == 0)
		return (EINVAL);
	*handlep = NULL;
	fs = VFSTOUFS(mp)->um_e4fs;
	if (fs->m_read_only)
		return (EROFS);
	journal = fs->m_journal;
	if (journal == NULL)
		return (EOPNOTSUPP);

	handle = malloc(sizeof(*handle), M_UFSMNT, M_WAITOK | M_ZERO);
	candidate = NULL;

	for (;;) {
		allocate = 0;
		arm = 0;
		commit = 0;
		mtx_enter(&journal->j_lock);
		for (;;) {
			if (journal->j_timeout_draining) {
				msleep_nsec(&journal->j_timeout_draining,
				    &journal->j_lock, PRIBIO,
				    "e4jtimer", INFSLP);
				continue;
			}
			error = ext4fs_journal_state_admission(
			    journal->j_active != NULL ||
			    journal->j_handoff != NULL,
			    journal->j_aborted, journal->j_error,
			    journal->j_shutting_down);
			if (error == EBUSY) {
				msleep_nsec(&journal->j_active,
				    &journal->j_lock, PRIBIO,
				    "e4jbegin", INFSLP);
				continue;
			}
			if (error != 0 || journal->j_committing == NULL ||
			    ext4fs_journal_transaction_can_overlap(
			    journal->j_committing->jt_state))
				break;
			msleep_nsec(&journal->j_committing,
			    &journal->j_lock, PRIBIO, "e4jwait",
			    INFSLP);
		}
		if (error == 0 && journal->j_running != NULL) {
			tx = journal->j_running;
			error = ext4fs_journal_state_reserve(
			    journal->j_max_transaction_credits,
			    tx->jt_credits_used,
			    &tx->jt_credits_reserved, credits);
			if (error == ENOSPC) {
				commit = 1;
			} else if (error == 0) {
				handle->jh_journal = journal;
				handle->jh_transaction = tx;
				handle->jh_credits = credits;
				journal->j_active = handle;
			}
		} else if (error == 0 && candidate == NULL) {
			allocate = 1;
		} else if (error == 0) {
			tx = candidate;
			error = ext4fs_journal_state_reserve(
			    journal->j_max_transaction_credits,
			    tx->jt_credits_used,
			    &tx->jt_credits_reserved, credits);
			if (error == 0) {
				error =
				    ext4fs_journal_transaction_transition(
				    &tx->jt_state,
				    EXT4FS_JOURNAL_TRANSACTION_RUNNING);
			}
			if (error == 0) {
				/* Consume this sequence. */
				tx->jt_sequence =
				    ext4fs_journal_state_sequence(
					    journal->j_next_sequence);
				journal->j_running = tx;
				candidate = NULL;
				handle->jh_journal = journal;
				handle->jh_transaction = tx;
				handle->jh_credits = credits;
				journal->j_active = handle;
				arm = 1;
			}
		}
		mtx_leave(&journal->j_lock);
		if (allocate) {
			candidate = malloc(sizeof(*candidate), M_UFSMNT,
			    M_WAITOK | M_ZERO);
			TAILQ_INIT(&candidate->jt_metadata);
			TAILQ_INIT(&candidate->jt_revokes);
			TAILQ_INIT(&candidate->jt_ordered);
			RB_INIT(&candidate->jt_metadata_blocks);
			RB_INIT(&candidate->jt_metadata_buffers);
			RB_INIT(&candidate->jt_revoke_blocks);
			ext4fs_journal_io_init(&candidate->jt_data_io);
			continue;
		}
		if (arm)
			timeout_add_msec(&journal->j_commit_timeout,
			    EXT4FS_JOURNAL_TRANSACTION_AGE_MSEC);
		if (! commit)
			break;
		error = ext4fs_journal_commit(mp,
		    EXT4FS_JOURNAL_COMMIT_CREDIT_EXHAUSTION);
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

	candidate = malloc(sizeof(*candidate), M_UFSMNT,
	    M_WAITOK | M_ZERO);
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
	TAILQ_INSERT_TAIL(&handle->jh_transaction->jt_ordered,
	    candidate, jo_entry);
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
ext4fs_journal_read_metadata (struct mount *mp, struct vnode *devvp,
    u_int64_t fsblock, struct buf **bpp)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	struct ufsmount *ump;
	struct buf *copy;
	int error;

	if (mp == NULL || devvp == NULL || bpp == NULL)
		return (EINVAL);
	*bpp = NULL;
	ump = VFSTOUFS(mp);
	fs = ump->um_e4fs;
	if (devvp != ump->um_devvp || fsblock >= fs->m_blocks_count)
		return (EINVAL);
	journal = fs->m_journal;
	if (journal == NULL)
		return (bread(devvp,
		    (daddr_t)EXT4FS_FSBTODB(fs, fsblock),
		    fs->m_block_size, bpp));
	if (ext4fs_journal_block_member(journal, fsblock))
		return (EINVAL);
	if (fsblock > 0xffffffffULL &&
	    ! (journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT))
		return (EFBIG);

	copy = NULL;
	for (;;) {
		mtx_enter(&journal->j_lock);
		error = ext4fs_journal_state_admission(0,
		    journal->j_aborted, journal->j_error,
		    journal->j_shutting_down);
		if (error)
			goto out;
		/*
		 * A handle owns the live buffers while it is active and may
		 * change them without holding j_lock.  Wait for the handle
		 * boundary before treating a running buffer as stable.
		 */
		if (journal->j_active != NULL) {
			msleep_nsec(&journal->j_active,
			    &journal->j_lock, PRIBIO, "e4jread",
			    INFSLP);
			mtx_leave(&journal->j_lock);
			continue;
		}
		metadata = journal->j_running != NULL ?
		    ext4fs_journal_metadata_block_find(
		    journal->j_running, fsblock) : NULL;
		if (metadata == NULL) {
			mtx_leave(&journal->j_lock);
			if (copy != NULL)
				brelse(copy);
			return (bread(devvp,
			    (daddr_t)EXT4FS_FSBTODB(fs, fsblock),
			    fs->m_block_size, bpp));
		}
		if (metadata->jm_update_pending) {
			error = ext4fs_journal_materialize_record(journal,
			    journal->j_running, metadata);
			if (error)
				goto out;
		}
		if (metadata->jm_buf == NULL ||
		    metadata->jm_buf->b_vp != devvp ||
		    metadata->jm_owner != NULL ||
		    ! metadata->jm_dirty ||
		    ! ISSET(metadata->jm_buf->b_flags, B_BUSY) ||
		    ! ISSET(metadata->jm_buf->b_flags, B_DONE) ||
		    metadata->jm_buf->b_bcount !=
		    (long)fs->m_block_size) {
			error = EINVAL;
			goto out;
		}
		if (copy == NULL) {
			/*
			 * Buffer allocation may sleep.  Revalidate the running
			 * record after reacquiring j_lock.
			 */
			mtx_leave(&journal->j_lock);
			copy = geteblk(fs->m_block_size);
			continue;
		}
		/* j_lock keeps the transaction and its live buffer stable. */
		memcpy(copy->b_data, metadata->jm_buf->b_data,
		    fs->m_block_size);
		copy->b_blkno = (daddr_t)EXT4FS_FSBTODB(fs,
		    fsblock);
		copy->b_lblkno = copy->b_blkno;
		copy->b_error = 0;
		copy->b_resid = 0;
		CLR(copy->b_flags, B_ERROR | B_EINTR | B_INVAL |
		    B_READ);
		SET(copy->b_flags, B_DONE | B_NOCACHE);
		*bpp = copy;
		mtx_leave(&journal->j_lock);
		return (0);
	}

out:
	mtx_leave(&journal->j_lock);
	if (copy != NULL)
		brelse(copy);
	return (error);
}

int
ext4fs_journal_get_metadata (struct ext4fs_journal_handle *handle,
    struct vnode *devvp, u_int64_t fsblock, struct buf **bpp)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	struct buf *bp;
	int error;

	if (handle == NULL || devvp == NULL || bpp == NULL ||
	    handle->jh_journal == NULL)
		return (EINVAL);
	*bpp = NULL;
	journal = handle->jh_journal;
	fs = VFSTOUFS(journal->j_mp)->um_e4fs;
	if (devvp != VFSTOUFS(journal->j_mp)->um_devvp ||
	    fsblock >= fs->m_blocks_count ||
	    ext4fs_journal_block_member(journal, fsblock))
		return (EINVAL);
	if (fsblock > 0xffffffffULL && ! (journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT))
		return (EFBIG);

	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	metadata = ext4fs_journal_metadata_block_find(
	    handle->jh_transaction, fsblock);
	if (metadata != NULL) {
		if (metadata->jm_buf == NULL ||
		    metadata->jm_buf->b_vp != devvp ||
		    ! ISSET(metadata->jm_buf->b_flags, B_BUSY)) {
			error = EINVAL;
			goto out;
		}
		if (metadata->jm_owner != NULL &&
		    metadata->jm_owner != handle) {
			error = EBUSY;
			goto out;
		}
		*bpp = metadata->jm_buf;
		error = 0;
		goto out;
	}
	mtx_leave(&journal->j_lock);

	error = bread(devvp, (daddr_t)EXT4FS_FSBTODB(fs, fsblock),
	    fs->m_block_size, &bp);
	if (error) {
		if (bp != NULL)
			brelse(bp);
		return (error);
	}
	error = ext4fs_journal_get_write_access(handle, bp, fsblock);
	if (error) {
		brelse(bp);
		return (error);
	}
	*bpp = bp;
	return (0);

out:
	mtx_leave(&journal->j_lock);
	return (error);
}

static int
ext4fs_journal_metadata_validation (
    struct ext4fs_journal_handle *handle, struct buf *bp,
    u_int32_t flag, int set, int *validatedp)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal *journal;
	int error;

	/*
	 * Validation belongs to this transaction's busy metadata
	 * record.  It is never retained across a transaction boundary.
	 */
	if (handle == NULL || bp == NULL ||
	    handle->jh_journal == NULL || validatedp == NULL ||
	    (flag != EXT4FS_JOURNAL_VALID_BLOCK_BITMAP &&
	    flag != EXT4FS_JOURNAL_VALID_INODE_BITMAP))
		return (EINVAL);
	journal = handle->jh_journal;
	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	metadata = ext4fs_journal_metadata_buffer_find(
	    handle->jh_transaction, bp);
	if (metadata != NULL) {
		if (metadata->jm_owner != NULL &&
		    metadata->jm_owner != handle) {
			error = EBUSY;
			goto out;
		}
		if (set)
			SET(metadata->jm_validated, flag);
		*validatedp = ISSET(metadata->jm_validated, flag);
		error = 0;
		goto out;
	}
	error = EINVAL;

out:
	mtx_leave(&journal->j_lock);
	return (error);
}

int
ext4fs_journal_metadata_validated (
    struct ext4fs_journal_handle *handle, struct buf *bp,
    u_int32_t flag, int *validatedp)
{
	return (ext4fs_journal_metadata_validation(handle, bp, flag,
	    0, validatedp));
}

int
ext4fs_journal_metadata_mark_validated (
    struct ext4fs_journal_handle *handle, struct buf *bp,
    u_int32_t flag)
{
	int validated;

	return (ext4fs_journal_metadata_validation(handle, bp, flag,
	    1, &validated));
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

	if (handle == NULL || bp == NULL ||
	    handle->jh_journal == NULL ||
	    bp->b_data == NULL || ! ISSET(bp->b_flags, B_BUSY))
		return (EINVAL);
	journal = handle->jh_journal;
	if (bp->b_bcount != journal->j_blocksize)
		return (EINVAL);
	fs = VFSTOUFS(journal->j_mp)->um_e4fs;
	if (fsblock >= fs->m_blocks_count ||
	    ext4fs_journal_block_member(journal, fsblock))
		return (EINVAL);
	if (fsblock > 0xffffffffULL && ! (journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT))
		return (EFBIG);
	candidate = malloc(sizeof(*candidate), M_UFSMNT,
	    M_WAITOK | M_ZERO);
	candidate->jm_buf = bp;
	candidate->jm_owner = handle;
	candidate->jm_fsblock = fsblock;

	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	metadata = ext4fs_journal_metadata_block_find(
	    handle->jh_transaction, fsblock);
	if (metadata != NULL) {
		error = ext4fs_journal_state_metadata_relation(
		    metadata->jm_buf, metadata->jm_fsblock, bp,
		    fsblock);
		goto out;
	}
	metadata = ext4fs_journal_metadata_buffer_find(
	    handle->jh_transaction, bp);
	if (metadata != NULL) {
		error = ext4fs_journal_state_metadata_relation(
		    metadata->jm_buf, metadata->jm_fsblock, bp,
		    fsblock);
		goto out;
	}
	revoke = ext4fs_journal_revoke_block_find(
	    handle->jh_transaction, fsblock);
	if (revoke != NULL) {
		error = EBUSY;
		goto out;
	}
	metadata = RB_INSERT(ext4fs_journal_metadata_block_tree,
	    &handle->jh_transaction->jt_metadata_blocks, candidate);
	if (metadata != NULL) {
		error = EBUSY;
		goto out;
	}
	metadata = RB_INSERT(ext4fs_journal_metadata_buffer_tree,
	    &handle->jh_transaction->jt_metadata_buffers, candidate);
	if (metadata != NULL) {
		metadata = RB_REMOVE(
		    ext4fs_journal_metadata_block_tree,
		    &handle->jh_transaction->jt_metadata_blocks,
		    candidate);
		KASSERT(metadata == candidate);
		error = EINVAL;
		goto out;
	}
	TAILQ_INSERT_TAIL(&handle->jh_transaction->jt_metadata,
	    candidate, jm_entry);
	/*
	 * The handle now owns the busy buffer until dirty_metadata() or
	 * end().
	 */
	candidate = NULL;
	error = 0;

out:
	mtx_leave(&journal->j_lock);
	if (candidate != NULL)
		free(candidate, M_UFSMNT, sizeof(*candidate));
	return (error);
}

static int
ext4fs_journal_dirty_record_locked (
    struct ext4fs_journal_handle *handle,
    struct ext4fs_journal_metadata *metadata)
{
	int error;

	if (metadata->jm_dirty)
		return (0);
	if (metadata->jm_owner != handle)
		return (EBUSY);
	error = ext4fs_journal_state_consume(&handle->jh_credits,
	    &handle->jh_transaction->jt_credits_reserved,
	    &handle->jh_transaction->jt_credits_used);
	if (error)
		return (error);
	metadata->jm_dirty = 1;
	/* Transfer the busy buffer to the transaction. */
	metadata->jm_owner = NULL;
	return (0);
}

static int
ext4fs_journal_dirty_update (struct ext4fs_journal_handle *handle,
    struct buf *bp, enum ext4fs_journal_update_kind kind,
    u_int32_t key)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	u_int64_t fsblock;
	u_int32_t base, bytes, offset, slot, slots;
	size_t size;
	int error;

	if (handle == NULL || bp == NULL ||
	    handle->jh_journal == NULL ||
	    ! ISSET(bp->b_flags, B_BUSY) ||
	    ! ISSET(bp->b_flags, B_DONE))
		return (EINVAL);
	journal = handle->jh_journal;
	fs = VFSTOUFS(journal->j_mp)->um_e4fs;
	error = ext4fs_journal_update_location(fs, kind, key,
	    &fsblock, &offset);
	if (error)
		return (error);
	base = bytes = slot = slots = 0;
	size = 0;
	switch (kind) {
	case EXT4FS_JOURNAL_UPDATE_INODE:
		size = fs->m_inode_size;
		slots = fs->m_inodes_per_block;
		break;
	case EXT4FS_JOURNAL_UPDATE_GROUP_DESCRIPTOR:
		size = fs->m_block_group_descriptor_size;
		if (size != 0)
			slots = fs->m_block_size / size;
		break;
	case EXT4FS_JOURNAL_UPDATE_SUPERBLOCK:
		break;
	default:
		return (EINVAL);
	}
	if (kind != EXT4FS_JOURNAL_UPDATE_SUPERBLOCK) {
		if (size == 0 || slots == 0 || offset % size != 0)
			return (EINVAL);
		slot = offset / size;
		if (slot >= slots || key < slot)
			return (EINVAL);
		base = key - slot;
		bytes = howmany(slots, NBBY);
		if (bytes > EXT4FS_JOURNAL_UPDATE_BYTES)
			return (EINVAL);
	}

	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	metadata = ext4fs_journal_metadata_buffer_find(
	    handle->jh_transaction, bp);
	if (metadata == NULL || metadata->jm_fsblock != fsblock) {
		error = EINVAL;
		goto out;
	}
	if (metadata->jm_update_kind !=
	    EXT4FS_JOURNAL_UPDATE_NONE &&
	    (metadata->jm_update_kind != kind ||
	    metadata->jm_update_base != base ||
	    metadata->jm_update_slots != slots)) {
		error = EINVAL;
		goto out;
	}
	error = ext4fs_journal_dirty_record_locked(handle, metadata);
	if (error)
		goto out;
	if (metadata->jm_update_kind ==
	    EXT4FS_JOURNAL_UPDATE_NONE) {
		metadata->jm_update_base = base;
		metadata->jm_update_slots = slots;
		metadata->jm_update_kind = kind;
	}
	if (bytes != 0)
		setbit(metadata->jm_update_bits, slot);
	metadata->jm_update_pending = 1;

out:
	mtx_leave(&journal->j_lock);
	return (error);
}

int
ext4fs_journal_dirty_inode (struct ext4fs_journal_handle *handle,
    struct buf *bp, u_int32_t ino)
{
	return (ext4fs_journal_dirty_update(handle, bp,
	    EXT4FS_JOURNAL_UPDATE_INODE, ino));
}

int
ext4fs_journal_dirty_group_descriptor (
    struct ext4fs_journal_handle *handle, struct buf *bp,
    u_int32_t group)
{
	return (ext4fs_journal_dirty_update(handle, bp,
	    EXT4FS_JOURNAL_UPDATE_GROUP_DESCRIPTOR, group));
}

int
ext4fs_journal_dirty_superblock (
    struct ext4fs_journal_handle *handle, struct buf *bp)
{
	return (ext4fs_journal_dirty_update(handle, bp,
	    EXT4FS_JOURNAL_UPDATE_SUPERBLOCK, 0));
}

int
ext4fs_journal_materialize_metadata (
    struct ext4fs_journal_handle *handle, struct buf *bp)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal *journal;
	int error;

	if (handle == NULL || bp == NULL ||
	    handle->jh_journal == NULL)
		return (EINVAL);
	journal = handle->jh_journal;
	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	metadata = ext4fs_journal_metadata_buffer_find(
	    handle->jh_transaction, bp);
	if (metadata == NULL) {
		error = EINVAL;
		goto out;
	}
	if (metadata->jm_update_pending)
		error = ext4fs_journal_materialize_record(journal,
		    handle->jh_transaction, metadata);

out:
	mtx_leave(&journal->j_lock);
	return (error);
}

int
ext4fs_journal_dirty_metadata (struct ext4fs_journal_handle *handle,
    struct buf *bp)
{
	struct ext4fs_journal_metadata *metadata;
	struct ext4fs_journal *journal;
	int error;

	if (handle == NULL || bp == NULL ||
	    handle->jh_journal == NULL ||
	    ! ISSET(bp->b_flags, B_BUSY) ||
	    ! ISSET(bp->b_flags, B_DONE))
		return (EINVAL);
	journal = handle->jh_journal;
	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_handle_error(handle);
	if (error)
		goto out;
	metadata = ext4fs_journal_metadata_buffer_find(
	    handle->jh_transaction, bp);
	if (metadata == NULL)
		error = EINVAL;
	else
		error = ext4fs_journal_dirty_record_locked(handle,
		    metadata);

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
	int error, has_revoke;

	if (handle == NULL || handle->jh_journal == NULL)
		return (EINVAL);
	journal = handle->jh_journal;
	fs = VFSTOUFS(journal->j_mp)->um_e4fs;
	has_revoke = journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_REVOKE;
	if (fsblock >= fs->m_blocks_count ||
	    ext4fs_journal_block_member(journal, fsblock))
		return (EINVAL);
	if (has_revoke && fsblock > 0xffffffffULL &&
	    ! (journal->j_features_incompat &
	    JBD2_FEATURE_INCOMPAT_64BIT))
		return (EFBIG);
	candidate = NULL;
	if (has_revoke) {
		candidate = malloc(sizeof(*candidate), M_UFSMNT,
		    M_WAITOK | M_ZERO);
		candidate->jr_fsblock = fsblock;
	}

	mtx_enter(&journal->j_lock);
	for (;;) {
		error = ext4fs_journal_handle_error(handle);
		if (error)
			goto out;
		if (journal->j_committing == NULL)
			break;
		/* Do not race reuse against an older commit or home write. */
		msleep_nsec(&journal->j_committing, &journal->j_lock,
		    PRIBIO, "e4jrevoke", INFSLP);
	}
	metadata = ext4fs_journal_metadata_block_find(
	    handle->jh_transaction, fsblock);
	if (metadata != NULL) {
		error = EBUSY;
		goto out;
	}
	/*
	 * A journal without revoke support checkpoints every committed
	 * transaction before reuse.  Freeing metadata enlisted in this
	 * transaction remains invalid in either mode.
	 */
	if (! has_revoke) {
		error = 0;
		goto out;
	}
	revoke = ext4fs_journal_revoke_block_find(
	    handle->jh_transaction, fsblock);
	if (revoke != NULL) {
		error = 0;
		goto out;
	}
	revoke = RB_INSERT(ext4fs_journal_revoke_block_tree,
	    &handle->jh_transaction->jt_revoke_blocks, candidate);
	if (revoke != NULL) {
		error = EINVAL;
		goto out;
	}
	error = ext4fs_journal_state_consume(&handle->jh_credits,
	    &handle->jh_transaction->jt_credits_reserved,
	    &handle->jh_transaction->jt_credits_used);
	if (error) {
		revoke = RB_REMOVE(ext4fs_journal_revoke_block_tree,
		    &handle->jh_transaction->jt_revoke_blocks,
		    candidate);
		KASSERT(revoke == candidate);
		goto out;
	}
	TAILQ_INSERT_TAIL(&handle->jh_transaction->jt_revokes,
	    candidate, jr_entry);
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
	return (ext4fs_journal_end_internal(handle, NULL, NULL));
}

static int
ext4fs_journal_end_internal (struct ext4fs_journal_handle *handle,
    u_int32_t *sequencep, int *transactionp)
{
	struct ext4fs_journal_metadata_head unused;
	struct ext4fs_journal_metadata *metadata, *next;
	struct ext4fs_journal_transaction *empty;
	struct ext4fs_journal *journal;
	int cancel, error, release_error;

	if (handle == NULL || handle->jh_journal == NULL ||
	    handle->jh_ended)
		return (EINVAL);
	journal = handle->jh_journal;
	empty = NULL;
	cancel = 0;
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
	if (sequencep != NULL)
		*sequencep = handle->jh_transaction->jt_sequence;
	if (transactionp != NULL)
		*transactionp = 0;
	TAILQ_FOREACH_SAFE(metadata,
	    &handle->jh_transaction->jt_metadata, jm_entry, next) {
		if (metadata->jm_owner != handle || metadata->jm_dirty)
			continue;
		ext4fs_journal_metadata_remove(
		    handle->jh_transaction, metadata);
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
		KASSERT(
		    handle->jh_transaction->jt_credits_reserved == 0);
		empty = handle->jh_transaction;
		journal->j_running = NULL;
		KASSERT(! journal->j_timeout_draining);
		journal->j_timeout_draining = 1;
		cancel = 1;
	} else if (error == 0 && ! journal->j_aborted &&
	    ! journal->j_shutting_down) {
		if (transactionp != NULL)
			*transactionp = 1;
		/*
		 * Only this caller may detach the transaction.  Otherwise a
		 * waiter holding another vnode could commit it and wait for
		 * a vnode still held by this caller.
		 */
		KASSERT(journal->j_handoff == NULL);
		journal->j_handoff = curproc;
	}
	wakeup(&journal->j_active);
	mtx_leave(&journal->j_lock);
	if (cancel)
		ext4fs_journal_timeout_drain(journal);

	while ((metadata = TAILQ_FIRST(&unused)) != NULL) {
		TAILQ_REMOVE(&unused, metadata, jm_entry);
		KASSERT(metadata->jm_owner == handle);
		KASSERT(! metadata->jm_dirty);
		KASSERT(ISSET(metadata->jm_buf->b_flags, B_BUSY));
		ext4fs_journal_update_reset(metadata);
		brelse(metadata->jm_buf);
		free(metadata, M_UFSMNT, sizeof(*metadata));
	}
	ext4fs_journal_transaction_free(empty);
	free(handle, M_UFSMNT, sizeof(*handle));
	return (error);
}

int
ext4fs_journal_end_commit (struct ext4fs_journal_handle *handle,
    enum ext4fs_journal_commit_reason reason)
{
	struct ext4fs_journal *journal;
	struct mount *mp;
	u_int32_t sequence;
	int error, group, transaction;

	if (handle == NULL || handle->jh_journal == NULL)
		return (EINVAL);
	journal = handle->jh_journal;
	mp = journal->j_mp;
	error = ext4fs_journal_end_internal(handle, &sequence,
	    &transaction);
	if (error)
		return (error);
	/*
	 * An fsync has already flushed its vnode data.  Let peer fsync
	 * calls join a metadata-only transaction before one of them
	 * detaches it.  Ordered data retains the vnode-safe handoff.
	 */
	group = 0;
	if (reason == EXT4FS_JOURNAL_COMMIT_FSYNC && transaction) {
		mtx_enter(&journal->j_lock);
		if (journal->j_handoff == curproc &&
		    journal->j_active == NULL &&
		    journal->j_running != NULL &&
		    journal->j_running->jt_sequence == sequence &&
		    TAILQ_EMPTY(&journal->j_running->jt_ordered) &&
		    ! journal->j_aborted &&
		    ! journal->j_shutting_down) {
			journal->j_handoff = NULL;
			wakeup(&journal->j_active);
			group = 1;
		}
		mtx_leave(&journal->j_lock);
	}
	if (group)
		yield();
	return (ext4fs_journal_commit_internal(mp, reason, sequence,
	    transaction ? EXT4FS_JOURNAL_FENCE_WAIT :
	    EXT4FS_JOURNAL_FENCE_BEFORE));
}

int
ext4fs_journal_commit (struct mount *mp,
    enum ext4fs_journal_commit_reason reason)
{
	return (ext4fs_journal_commit_internal(mp, reason, 0,
	    EXT4FS_JOURNAL_FENCE_NONE));
}

static int
ext4fs_journal_commit_internal (struct mount *mp,
    enum ext4fs_journal_commit_reason reason, u_int32_t sequence,
    enum ext4fs_journal_fence fence)
{
	struct ext4fs_journal_abort_info info;
	struct ext4fs_journal_transaction_head done;
	struct ext4fs_journal_transaction *checkpoint_last;
	struct ext4fs_journal_transaction *finished, *tx;
	struct ext4fs_journal *journal;
	struct m_ext4fs *fs;
	u_int32_t required;
	int cancel, checkpoint, drain, error, first, force, queued;
	int satisfied;

	fs = VFSTOUFS(mp)->um_e4fs;
	journal = fs->m_journal;
	if (journal == NULL)
		return (0);
	if (! ext4fs_journal_commit_reason_valid(reason))
		return (EINVAL);
	if (reason == EXT4FS_JOURNAL_COMMIT_ORDINARY &&
	    ISSET(journal->j_mp->mnt_flag, MNT_SYNCHRONOUS))
		reason = EXT4FS_JOURNAL_COMMIT_SYNC_MOUNT;
	if (reason == EXT4FS_JOURNAL_COMMIT_ORDINARY) {
		force = 0;
		mtx_enter(&journal->j_lock);
		error = ext4fs_journal_state_admission(0,
		    journal->j_aborted, journal->j_error,
		    journal->j_shutting_down);
		/*
		 * A reader may hold an ordered vnode while waiting for
		 * one of this transaction's busy extent blocks.  Do not
		 * leave the age worker on the other side of that cycle.
		 */
		if (error == 0 && journal->j_handoff == curproc &&
		    journal->j_running != NULL &&
		    ! TAILQ_EMPTY(&journal->j_running->jt_ordered)) {
			force = 1;
		} else if (journal->j_handoff == curproc) {
			journal->j_handoff = NULL;
			wakeup(&journal->j_active);
		}
		mtx_leave(&journal->j_lock);
		if (! force)
			return (error);
	}
	drain = ext4fs_journal_commit_reason_requires_checkpoint(reason);
	tx = NULL;
	checkpoint_last = NULL;
	TAILQ_INIT(&done);
	cancel = 0;
	checkpoint = 0;
	first = 0;
	queued = 0;
	satisfied = 0;

	mtx_enter(&journal->j_lock);
	/*
	 * Resolve a sequence fence before considering an active handle.
	 * Once its transaction has left the running slot, a successor's
	 * handle or handoff cannot extend the caller's durability wait.
	 */
	for (;;) {
		error = ext4fs_journal_state_admission(0,
		    journal->j_aborted, journal->j_error,
		    journal->j_shutting_down);
		if (error != 0)
			break;
		if (fence == EXT4FS_JOURNAL_FENCE_MATCH &&
		    (journal->j_running == NULL ||
		    journal->j_running->jt_sequence != sequence)) {
			satisfied = 1;
			break;
		}
		if (fence == EXT4FS_JOURNAL_FENCE_BEFORE) {
			checkpoint_last =
			    ext4fs_journal_committed_before(journal,
			    sequence);
			if (drain && checkpoint_last != NULL) {
				if (journal->j_committing != NULL) {
					msleep_nsec(&journal->j_committing,
					    &journal->j_lock, PRIBIO,
					    "e4jwait", INFSLP);
					continue;
				}
				tx = TAILQ_FIRST(&journal->j_committed);
				journal->j_committing = tx;
				journal->j_commit_busy = 1;
				checkpoint = 1;
				queued = 1;
				break;
			}
			if (drain && journal->j_committing != NULL &&
			    ext4fs_journal_state_sequence_before(
			    journal->j_committing->jt_sequence,
			    sequence)) {
				msleep_nsec(&journal->j_committing,
				    &journal->j_lock, PRIBIO,
				    "e4jwait", INFSLP);
				continue;
			}
			satisfied = 1;
			break;
		}
		if (fence == EXT4FS_JOURNAL_FENCE_WAIT &&
		    (journal->j_running == NULL ||
		    journal->j_running->jt_sequence != sequence)) {
			if (journal->j_committing != NULL &&
			    journal->j_committing->jt_sequence ==
			    sequence) {
				msleep_nsec(&journal->j_committing,
				    &journal->j_lock, PRIBIO,
				    "e4jwait", INFSLP);
				continue;
			}
			checkpoint_last =
			    ext4fs_journal_committed_find(journal,
			    sequence);
			if (drain && checkpoint_last != NULL) {
				if (journal->j_committing != NULL) {
					msleep_nsec(&journal->j_committing,
					    &journal->j_lock, PRIBIO,
					    "e4jwait", INFSLP);
					continue;
				}
				tx = TAILQ_FIRST(&journal->j_committed);
				journal->j_committing = tx;
				journal->j_commit_busy = 1;
				checkpoint = 1;
				queued = 1;
				break;
			}
			satisfied = 1;
			break;
		}
		error = ext4fs_journal_state_admission(
		    journal->j_active != NULL ||
		    (journal->j_handoff != NULL &&
		    journal->j_handoff != curproc),
		    journal->j_aborted, journal->j_error,
		    journal->j_shutting_down);
		if (error == EBUSY) {
			msleep_nsec(&journal->j_active,
			    &journal->j_lock, PRIBIO, "e4jcommit",
			    INFSLP);
			continue;
		}
		if (error != 0)
			break;
		if (journal->j_committing != NULL) {
			msleep_nsec(&journal->j_committing,
			    &journal->j_lock, PRIBIO, "e4jwait",
			    INFSLP);
			continue;
		}
		break;
	}
	if (error == 0 && ! satisfied && ! checkpoint &&
	    journal->j_running != NULL) {
		tx = journal->j_running;
		error = ext4fs_journal_transaction_transition(
		    &tx->jt_state,
		    EXT4FS_JOURNAL_TRANSACTION_COMMITTING);
		if (error == 0) {
			journal->j_running = NULL;
			journal->j_committing = tx;
			journal->j_commit_busy = 1;
			KASSERT(! journal->j_timeout_draining);
			journal->j_timeout_draining = 1;
			cancel = 1;
			ext4fs_journal_state_sequence_advance(
			    &journal->j_next_sequence);
		}
	} else if (error == 0 && ! satisfied && ! checkpoint && drain &&
	    ! TAILQ_EMPTY(&journal->j_committed)) {
		tx = TAILQ_FIRST(&journal->j_committed);
		journal->j_committing = tx;
		journal->j_commit_busy = 1;
		checkpoint = 1;
		queued = 1;
		checkpoint_last = NULL;
	}
	if (journal->j_handoff == curproc) {
		journal->j_handoff = NULL;
		wakeup(&journal->j_active);
	}
	mtx_leave(&journal->j_lock);
	if (cancel)
		ext4fs_journal_timeout_drain(journal);
	if (error != 0 || tx == NULL)
		return (error);

	if (! checkpoint) {
		error = ext4fs_journal_log_blocks(journal, tx,
		    &required);
		if (error == ENOSPC &&
		    ! TAILQ_EMPTY(&journal->j_committed)) {
			error = ext4fs_journal_checkpoint_committed(
			    journal, &done, NULL);
			if (error == 0)
				error = ext4fs_journal_log_blocks(
				    journal, tx, &required);
		}
		if (error == 0)
			error = ext4fs_journal_commit_transaction(
			    journal, tx);
		if (error == 0) {
			mtx_enter(&journal->j_lock);
			if (journal->j_aborted)
				error = journal->j_error != 0 ?
				    journal->j_error : EIO;
			if (error == 0 &&
			    tx->jt_log_blocks > journal->j_free)
				error = EINVAL;
			if (error == 0 &&
			    journal->j_committed_blocks >
			    UINT32_MAX - tx->jt_log_blocks)
				error = EOVERFLOW;
			if (error == 0) {
				if (TAILQ_EMPTY(&journal->j_committed))
					journal->j_tail =
					    tx->jt_log_start;
				journal->j_head = tx->jt_log_end;
				journal->j_free -= tx->jt_log_blocks;
				KASSERT(journal->j_committed_count !=
				    UINT32_MAX);
				journal->j_committed_count++;
				journal->j_committed_blocks +=
				    tx->jt_log_blocks;
				TAILQ_INSERT_TAIL(
				    &journal->j_committed, tx, jt_entry);
				queued = 1;
				checkpoint =
				    ext4fs_journal_should_checkpoint(
				    journal);
				if (drain) {
					checkpoint = 1;
					if (fence ==
					    EXT4FS_JOURNAL_FENCE_WAIT)
						checkpoint_last = tx;
					else
						checkpoint_last = NULL;
				}
			}
			mtx_leave(&journal->j_lock);
		}
	}
	if (error == 0 && checkpoint)
		error = ext4fs_journal_checkpoint_committed(journal,
		    &done, checkpoint_last);

	mtx_enter(&journal->j_lock);
	KASSERT(journal->j_committing == tx);
	KASSERT(journal->j_commit_busy);
	journal->j_commit_busy = 0;
	if (error == 0 && journal->j_aborted)
		error = journal->j_error != 0 ? journal->j_error : EIO;
	if (error != 0) {
		first = ext4fs_journal_abort_locked(journal, error,
		    __func__, &info);
	} else
		journal->j_stage = EXT4FS_JOURNAL_STAGE_OUTSIDE;
	journal->j_committing = NULL;
	wakeup(&journal->j_committing);
	mtx_leave(&journal->j_lock);
	if (first)
		ext4fs_journal_abort_policy(mp, error, &info);

	while ((finished = TAILQ_FIRST(&done)) != NULL) {
		TAILQ_REMOVE(&done, finished, jt_entry);
		ext4fs_journal_transaction_free(finished);
	}
	if (! queued)
		ext4fs_journal_transaction_free(tx);
	return (error);
}

int
ext4fs_journal_mark_clean (struct mount *mp,
    enum ext4fs_journal_commit_reason reason)
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
	if (journal == NULL || fs->m_read_only)
		return (0);
	if (reason != EXT4FS_JOURNAL_COMMIT_REMOUNT &&
	    reason != EXT4FS_JOURNAL_COMMIT_UNMOUNT)
		return (EINVAL);
	error = ext4fs_journal_commit(mp, reason);
	if (error)
		return (error);

	mtx_enter(&journal->j_lock);
	error = ext4fs_journal_state_admission(
	    journal->j_active != NULL, journal->j_aborted,
	    journal->j_error, journal->j_shutting_down);
	if (error == 0 && (journal->j_running != NULL ||
	    journal->j_committing != NULL ||
	    ! TAILQ_EMPTY(&journal->j_committed)))
		error = EBUSY;
	mtx_leave(&journal->j_lock);
	if (error)
		return (error);

	old_incompat = fs->m_feature_incompat;
	old_state = fs->m_state;
	fs->m_feature_incompat &= ~EXT4FS_FEATURE_INCOMPAT_RECOVER;
	fs->m_sble.sb_feature_incompat =
	    htole32(fs->m_feature_incompat);
	fs->m_state = EXT4FS_STATE_VALID;
	fs->m_fs_was_modified = 1;
	error = ext4fs_sbwrite_lifecycle(mp);
	if (error == 0)
		error = jbd2_flush_device(ump->um_devvp, curproc);
	if (error) {
		fs->m_feature_incompat = old_incompat;
		fs->m_sble.sb_feature_incompat = htole32(old_incompat);
		fs->m_state = old_state;
		fs->m_sble.sb_state = htole16(old_state);
		fs->m_sble.sb_checksum = htole32(
		    ext4fs_sb_csum(&fs->m_sble));
		ext4fs_journal_abort(mp, error);
	}
	return (error);
}
