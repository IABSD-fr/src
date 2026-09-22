# ext4fs Journalling Plan

## Goal

Add reliable JBD2 journalling support to IABSD's ext4fs implementation.
The first supported runtime mode will be metadata-only `data=ordered`
journalling with a single filesystem-wide transaction. More advanced modes and
concurrency can be added after recovery and crash consistency are proven.

## Current state

The tree contains mount-time and `fsck_ext4fs` JBD2 recovery using a bounded,
three-pass scan, revoke, and replay flow. Runtime filesystem operations do not
write JBD2 transactions, however; metadata buffers still reach their home
locations directly through `bwrite()`, `bdwrite()`, and `bawrite()`.

Recovery hardening is in progress and must be completed before this reader is
used as the recovery side of a journal writer.

### Phase 1 implementation status (2026-09-21)

The kernel and `fsck_ext4fs` recovery implementations now compile from the
same documented JBD2 format definitions. Recovery uses a validation pass before
the first home-block write, validates the ext4 superblock, group descriptors,
journal inode, external journal extent nodes, journal geometry and supported
feature masks, and propagates read, write, and durability-flush failures.
Journal-target and revoke membership use bounded hash tables, and physically
aliased journal extents are rejected.

Disposable images generated with e2fsprogs have been replayed successfully for
no-checksum, checksum-v2, and checksum-v3 journals. The resulting home block
matched the journal payload and `e2fsck -fn` accepted each recovered image.
Read-only validation rejected replay without changing the image hash.

The automated `fsck_ext4fs` regression suite now covers checksum formats,
descriptor boundaries, deleted tags, revoke ordering, log and transaction-ID
wraparound, failure atomicity, malformed structures, and seeded mutation runs.

Phase 1 is not complete. The remaining release blockers are:

- define and test restartable handling for `RECOVER` with journal `s_start == 0`;
- accept e2fsprogs checksum-v2 descriptors containing multiple tags;
- exercise the userland recovery fixtures through the kernel mount path;
- finish safe, restartable classic-orphan and orphan-file recovery (mount now
  fails closed when orphan cleanup would be required);
- run kernel mount and injected-power-loss tests in an IABSD VM.

## Phase 1: Harden journal recovery

- [x] Validate the journal superblock geometry:
  - block size matches the filesystem block size;
  - `s_first`, `s_start`, and `s_maxlen` are internally consistent;
  - the journal fits within inode 8;
  - allocation-size calculations cannot overflow.
- [x] Compare the journal UUID with `sb_journal_uuid`.
- [x] Define supported JBD2 feature masks and reject unknown incompatible
      features, including fast commits until they are implemented.
- [x] Verify the journal superblock checksum.
- [x] Verify descriptor, revoke, commit, and data-block checksums for checksum
      v2 and v3 journals.
- [x] Support transactions containing multiple descriptor and revoke blocks
      before the commit block.
- [x] Bound every pass by the journal length so malformed logs cannot loop
      forever.
- [x] Use wrap-safe transaction-ID comparisons.
- [x] Validate every replay target against the filesystem block count and
      reject targets inside the journal itself where appropriate.
- [x] Handle journal inode extent trees up to `EXT4FS_EXTENT_DEPTH_MAX`, or
      reject unsupported depths explicitly without modifying the filesystem.
- [x] Treat malformed tags, missing `LAST_TAG` in a non-full descriptor,
      invalid flag combinations, invalid revoke lengths, and incomplete
      transactions as errors.
- [x] Flush replayed home blocks before marking the journal clean.
- [x] Clear `EXT4FS_FEATURE_INCOMPAT_RECOVER` only after replay and all required
      flushes have succeeded.
- [x] Apply the same recovery rules to `fsck_ext4fs`, preferably through shared
      format/parsing helpers where kernel/userland boundaries permit it.

### Phase 1 tests

- [x] Replay e2fsprogs-generated journals using no checksum, checksum v2, and
      checksum v3 formats.
- [x] Cover escaped data and a committed revoke-only transaction that
      suppresses an earlier logged home-block write.
- [x] Replay a checksum-v3 transaction spanning multiple descriptor blocks.
- [x] Replay generated journals on 1 KiB, 2 KiB, and 4 KiB filesystems.
- [x] Cover journal wraparound and transaction-ID wraparound.
- [x] Cover deleted tags and the exact descriptor-capacity boundary.
- [x] Corrupt the journal superblock, descriptor, data, revoke, and commit
      checksum regions independently.
- [x] Confirm a checksum-invalid transaction is not replayed and the image is
      byte-for-byte unchanged.
- [x] Confirm incomplete transactions are not replayed and the image is
      byte-for-byte unchanged.
- [x] Confirm a bad payload in transaction 2 prevents transaction 1 from being
      applied, proving the validation pass precedes all home-block writes.
- [x] Fuzz journal headers, tag counts, revoke lengths, and geometry fields.
- [x] Verify corrupted journals fail the mount without clearing `RECOVER`.
      The root-only `run-regress-journal-mount` target checks a valid replay
      control, byte-for-byte failure atomicity, and preservation of `RECOVER`.

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

- Published ext4 JBD2 on-disk format documentation:
  <https://cdn.kernel.org/doc/html/latest/filesystems/ext4/journal.html>
- IABSD's existing ext4fs implementation and locally generated filesystem
  images. Linux source code is intentionally not used as implementation input.
