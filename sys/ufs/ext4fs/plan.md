# ext4fs Journalling Plan

## Goal

Add reliable JBD2 journalling support to IABSD's ext4fs implementation.
The first supported runtime mode will be metadata-only `data=ordered`
journalling with a single filesystem-wide transaction. More advanced modes and
concurrency can be added after recovery and crash consistency are proven.

## Current state

The tree already contains mount-time JBD2 replay in
`sys/ufs/ext4fs/ext4fs_journal.c`. It implements a basic three-pass scan,
revoke, and replay flow. Runtime filesystem operations do not write JBD2
transactions, however; metadata buffers still reach their home locations
directly through `bwrite()`, `bdwrite()`, and `bawrite()`.

The existing replay implementation also needs hardening before it is safe to
use as the recovery side of a journal writer. In particular, it assumes a
mostly linear `descriptor -> data -> commit` transaction and does not validate
all modern JBD2 checksums and features.

## Phase 1: Harden journal recovery

- [ ] Validate the journal superblock geometry:
  - block size matches the filesystem block size;
  - `s_first`, `s_start`, and `s_maxlen` are internally consistent;
  - the journal fits within inode 8;
  - allocation-size calculations cannot overflow.
- [ ] Compare the journal UUID with `sb_journal_uuid`.
- [ ] Define supported JBD2 feature masks and reject unknown incompatible
      features, including fast commits until they are implemented.
- [ ] Verify the journal superblock checksum.
- [ ] Verify descriptor, revoke, commit, and data-block checksums for checksum
      v2 and v3 journals.
- [ ] Support transactions containing multiple descriptor and revoke blocks
      before the commit block.
- [ ] Bound every pass by the journal length so malformed logs cannot loop
      forever.
- [ ] Use wrap-safe transaction-ID comparisons.
- [ ] Validate every replay target against the filesystem block count and
      reject targets inside the journal itself where appropriate.
- [ ] Handle journal inode extent trees up to `EXT4FS_EXTENT_DEPTH_MAX`, or
      reject unsupported depths explicitly without modifying the filesystem.
- [ ] Treat malformed tags, missing `LAST_TAG`, invalid UUID fields, invalid
      revoke lengths, and incomplete transactions as errors.
- [ ] Flush replayed home blocks before marking the journal clean.
- [ ] Clear `EXT4FS_FEATURE_INCOMPAT_RECOVER` only after replay and all required
      flushes have succeeded.
- [ ] Apply the same recovery rules to `fsck_ext4fs`, preferably through shared
      format/parsing helpers where kernel/userland boundaries permit it.

### Phase 1 tests

- [ ] Replay Linux-generated journals using no checksum, checksum v2, and
      checksum v3 formats.
- [ ] Cover multiple descriptor blocks, revoke-only transactions, journal
      wraparound, escaped data, and transaction-ID wraparound.
- [ ] Confirm incomplete or checksum-invalid transactions are not replayed.
- [ ] Fuzz journal headers, tag counts, revoke lengths, and geometry fields.
- [ ] Verify corrupted journals fail the mount without clearing `RECOVER`.

## Phase 2: Introduce the runtime journal core

Start with a serialized implementation: one running transaction and one
committing transaction per mounted filesystem. Correctness is more important
than batching or throughput in this phase.

Add journal state to `struct m_ext4fs`, including:

- journal geometry and logical-to-physical block mapping;
- head, tail, free-space, and next transaction sequence;
- running and committing transaction state;
- a lock and wait mechanism;
- lists of metadata buffers, ordered-data dependencies, and revokes;
- sticky journal-abort/error state.

Introduce an interface along these lines:

```c
int  ext4fs_journal_begin(struct mount *, unsigned int,
    struct ext4fs_journal_handle **);
int  ext4fs_journal_get_write_access(struct ext4fs_journal_handle *,
    struct buf *, u_int64_t);
int  ext4fs_journal_dirty_metadata(struct ext4fs_journal_handle *,
    struct buf *);
int  ext4fs_journal_revoke(struct ext4fs_journal_handle *, u_int64_t);
int  ext4fs_journal_end(struct ext4fs_journal_handle *);
int  ext4fs_journal_force_commit(struct mount *);
void ext4fs_journal_abort(struct mount *, int);
```

Each operation must reserve enough journal credits before modifying metadata.
Initial credit estimates can be conservative while the implementation is
serialized.

## Phase 3: Implement ordered commits

Implement metadata-only `data=ordered` commits in this order:

```text
flush affected regular-file data to home locations
    -> write descriptor blocks, metadata payloads, and revoke blocks
    -> issue a durability flush
    -> write the commit block
    -> issue a durability flush
    -> checkpoint metadata to its home locations
    -> issue a durability flush
    -> advance the journal tail and persist journal state
```

Additional requirements:

- [ ] Escape payload blocks beginning with `JBD2_MAGIC` and set `ESCAPE`.
- [ ] Generate descriptor tags and checksum tails for the selected format.
- [ ] Split transactions across as many descriptor blocks as necessary.
- [ ] Prevent the head from overwriting uncheckpointed transactions.
- [ ] Block or force a checkpoint when journal space is exhausted.
- [ ] Abort the journal and force the filesystem read-only after an I/O or
      invariant failure.
- [ ] Set `EXT4FS_FEATURE_INCOMPAT_RECOVER` before the first live transaction
      can become durable.
- [ ] Keep `EXT4FS_STATE_VALID` clear throughout a writable mount.
- [ ] On clean unmount, commit and checkpoint everything, mark the journal
      empty, clear `RECOVER`, and finally set `EXT4FS_STATE_VALID`.

## Phase 4: Convert metadata writers

Audit every metadata write in `sys/ufs/ext4fs` and route it through a journal
handle. This includes:

- [ ] inode-table blocks and inode timestamps;
- [ ] block bitmaps;
- [ ] inode bitmaps;
- [ ] block-group descriptors;
- [ ] extent-tree roots, index blocks, and leaf blocks;
- [ ] directory blocks and checksum tails;
- [ ] orphan-list and orphan-file updates;
- [ ] allocation and free counters;
- [ ] the ext4 superblock.

Direct `bwrite()`, `bdwrite()`, or `bawrite()` calls must remain only for
regular-file data, the journal's own I/O, recovery, checkpointing, or another
explicitly documented exception.

Wrap each compound namespace operation in one transaction:

- create and mknod;
- link and unlink;
- mkdir and rmdir;
- rename;
- symlink;
- truncate and extent allocation/free.

## Phase 5: VFS semantics

- [ ] Make `fsync()` commit the transaction containing the inode and wait for
      ordered data and the commit record to become durable.
- [ ] Make synchronous mounts and `O_SYNC` writes force the required commit.
- [ ] Make VFS sync commit and checkpoint outstanding journal work.
- [ ] Ensure read-only mounts may replay recovery only when the device can be
      safely opened for writing; otherwise fail without modifying it.
- [ ] Define remount read-only/read-write behavior.
- [ ] Implement consistent journal abort and ext4 error-policy handling.
- [ ] Expose useful journal state and failure diagnostics without excessive
      normal-operation logging.

## Phase 6: Crash-consistency test matrix

Use filesystem images created by Linux tools and run IABSD in a VM. Inject an
abrupt power loss after each commit phase and at journal wraparound boundaries.

For every recovered image:

1. boot or remount it on IABSD;
2. verify expected namespace and file-data outcomes;
3. run `e2fsck -fn`;
4. mount it on Linux and repeat integrity checks;
5. confirm replay is idempotent by attempting recovery again.

Exercise at least:

- create, write, fsync, and rename;
- unlink of open files and orphan cleanup;
- directory growth and removal;
- extent-tree growth and splitting;
- truncate, block reuse, and revoke processing;
- journal exhaustion and wraparound;
- checksum corruption and injected read/write/flush errors;
- clean and unclean unmounts;
- 1 KiB, 2 KiB, and 4 KiB filesystem blocks;
- 32-bit and 64-bit filesystem block numbers.

## Deferred work

Do not include these in the first working milestone:

- full `data=journal` mode;
- `data=writeback` mode;
- concurrent transaction handles and sophisticated batching;
- fast commits;
- external journal devices;
- shared journals;
- online journal creation or resizing.

## References

- Linux ext4 JBD2 format documentation:
  <https://cdn.kernel.org/doc/html/latest/filesystems/ext4/journal.html>
- Linux JBD2 recovery implementation:
  <https://github.com/torvalds/linux/blob/master/fs/jbd2/recovery.c>
- Linux JBD2 on-disk definitions:
  <https://github.com/torvalds/linux/blob/master/include/linux/jbd2.h>

