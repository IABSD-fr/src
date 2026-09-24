# ext4fs Journalling Plan

## Goal

Add reliable JBD2 journalling support to IABSD's ext4fs implementation.
The first supported runtime mode will be metadata-only `data=ordered`
journalling with a single filesystem-wide transaction. More advanced modes and
concurrency can be added after recovery and crash consistency are proven.

## Current state

The tree contains mount-time and `fsck_ext4fs` JBD2 recovery using a bounded,
three-pass scan, revoke, and replay flow.  It also contains a serialized
runtime core and an ordered on-disk commit/checkpoint writer.  Runtime
filesystem operations do not use journal handles yet, however; metadata
buffers still reach their home locations directly through `bwrite()`,
`bdwrite()`, and `bawrite()`.

Recovery hardening and its production-kernel regression gate are complete.
The Phase 3 writer is implemented but cannot be considered activated or
production-tested until the first Phase 4 metadata path uses it.

### Phase 1 implementation status (2026-09-23)

The kernel and `fsck_ext4fs` recovery implementations now compile from the
same documented JBD2 format definitions and the same ext4 checksum
implementation. Recovery uses a validation pass before the first home-block
write, validates the ext4 superblock, group descriptors, journal inode,
external journal extent nodes, journal geometry and supported feature masks,
and propagates read, write, and durability-flush failures. Journal-target and
revoke membership use bounded hash tables, and physically aliased journal
extents are rejected.

Disposable images generated with e2fsprogs have been replayed successfully for
no-checksum, multi-tag checksum-v2, and checksum-v3 journals. The resulting
home block matched the journal payload and `e2fsck -fn` accepted each recovered
image. Read-only validation rejected replay without changing the image hash.

The automated `fsck_ext4fs` regression suite now covers checksum formats,
descriptor boundaries, deleted tags, revoke ordering, log and transaction-ID
wraparound, failure atomicity, malformed structures, and seeded mutation runs.

Recovery now treats `RECOVER` with `s_start == 0` as a restart after the durable
journal-clean marker, accepts e2fsprogs checksum-v2 multi-tag descriptors, and
implements restartable classic-list and orphan-file recovery. Orphan recovery
keeps the filesystem clean while checkpointing, processes classic lists from
the tail, authenticates orphan-file and extent metadata before mutation, and
rebuilds allocation counters before removing each durable recovery reference.

Phase 1 recovery hardening and its regression gate are complete. On
2026-09-23, the expanded root-only mount suite passed against the booted
production IABSD kernel. It exercised every userland recovery fixture through
the kernel path, modeled every persisted recovery durability state, verified
restartable classic-list and orphan-file cleanup, and remounted completed
images to prove idempotence. Corrupt recovery inputs remained byte-for-byte
unchanged with `RECOVER` or `ORPHAN_PRESENT` preserved, and `e2fsck -fn`
accepted every successfully recovered image.

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
      invalid flag combinations, and invalid revoke lengths as errors.  A
      structurally valid transaction tail without a commit record is instead
      discarded without replay because that is the normal power-loss case.
- [x] Flush replayed home blocks before marking the journal clean.
- [x] Clear `EXT4FS_FEATURE_INCOMPAT_RECOVER` only after replay and all required
      flushes have succeeded.
- [x] Apply the same recovery rules to `fsck_ext4fs`, preferably through shared
      format/parsing helpers where kernel/userland boundaries permit it.
- [x] Define and implement restartable handling when `RECOVER` is set but the
      journal superblock has `s_start == 0`.
- [x] Accept e2fsprogs checksum-v2 descriptor blocks containing multiple tags.
- [x] Implement safe, restartable recovery of the classic orphan list and the
      orphan file, persisting recovery progress before clearing either orphan
      root or `ORPHAN_PRESENT`.

### Phase 1 tests

- [x] Replay e2fsprogs-generated journals using no checksum, single-tag
      checksum v2, and checksum v3 formats.
- [x] Replay an e2fsprogs-generated checksum-v2 descriptor containing multiple
      tags.
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
- [x] Exercise every userland recovery fixture through the kernel mount path.
- [x] Test `RECOVER` with `s_start == 0`, including failures injected before
      and after each durability boundary, and prove that retry is idempotent.
- [x] Run the kernel-mount recovery matrix and modeled power-loss durability
      states against the booted production IABSD kernel.

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
int  ext4fs_journal_begin (struct mount *, unsigned int,
    struct ext4fs_journal_handle **);
int  ext4fs_journal_add_ordered (struct ext4fs_journal_handle *,
    struct vnode *);
int  ext4fs_journal_get_write_access (struct ext4fs_journal_handle *,
    struct buf *, u_int64_t);
int  ext4fs_journal_dirty_metadata (struct ext4fs_journal_handle *,
    struct buf *);
int  ext4fs_journal_revoke (struct ext4fs_journal_handle *, u_int64_t);
int  ext4fs_journal_end (struct ext4fs_journal_handle *);
int  ext4fs_journal_force_commit (struct mount *);
void ext4fs_journal_abort (struct mount *, int);
```

Each operation must reserve enough journal credits before modifying metadata.
Initial credit estimates can be conservative while the implementation is
serialized.

### Phase 2 implementation status (updated 2026-09-24)

The runtime core is present.  Mount-time recovery and runtime
initialization share one journal-superblock validator and one complete
logical-to-physical journal block map.  A mounted filesystem now owns an
opaque journal object with serialized handles, conservative credit
reservation, owned metadata buffers, ordered-data dependencies, revoke
tracking, journal-block exclusion, and a sticky abort error.  Mount failure
and unmount paths tear this state down.

No live metadata writer uses these handles yet.  Phase 3 now supplies the
ordered on-disk writer used by `ext4fs_journal_force_commit()`, but normal
filesystem operations will not generate runtime transactions until Phase 4
converts their metadata writes.

On 2026-09-24, the root-only journal mount suite passed in full against the
rebuilt and booted production `GENERIC.MP` kernel.  This exercised runtime
journal initialization and teardown after successful recovery across all
supported checksum formats and block sizes, as well as the existing
non-mutating corruption and restartable orphan-recovery cases.

The non-root `regress/sys/ext4fs` suite builds a separate
`journal_core_test` program and exercises the same side-effect-free state
policy implementation compiled into the kernel core.  This keeps test-only
entry points and configuration out of the production kernel.

- [x] Share validated journal geometry, features, checksum state, block map,
      and journal-block membership with recovery.
- [x] Add per-mount running/committing state, locking, wait channels, journal
      position/free-space fields, and sticky abort state.
- [x] Add serialized handles with overflow-safe conservative credit
      reservation.
- [x] Track unique metadata buffers and revokes, rejecting aliases, conflicts,
      out-of-range blocks, and journal blocks.
- [x] Initialize and destroy runtime journal state in the mount lifecycle.
- [x] Define ownership and lifetime rules for metadata buffers after a handle
      ends.  Successful write-access registration transfers a busy buffer to
      the handle; dirtying transfers it to the transaction; ending releases
      unused access, while abort teardown invalidates uncheckpointed contents.
- [x] Add transaction-owned ordered-data dependencies.  Dependencies are
      deduplicated regular-file vnodes held by reference until commit or
      teardown; the initial ordered mode will conservatively flush the whole
      vnode.
- [x] Add focused shared-state regression tests for serialized handle
      admission, credit reservation/exhaustion/release, duplicate and alias
      decisions, revoke conflicts, sticky aborts, and sequence wraparound.
- [x] Rebuild, boot, and run the root-only kernel mount regression suite
      against the production `GENERIC.MP` kernel.

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

- [x] Escape payload blocks beginning with `JBD2_MAGIC` and set `ESCAPE`.
- [x] Generate descriptor tags and checksum tails for the selected format.
- [x] Split transactions across as many descriptor blocks as necessary.
- [x] Prevent the head from overwriting uncheckpointed transactions.
- [x] Block or force a checkpoint when journal space is exhausted.
- [x] Abort the journal and force the filesystem read-only after an I/O or
      invariant failure.
- [x] Set `EXT4FS_FEATURE_INCOMPAT_RECOVER` before the first live transaction
      can become durable.
- [x] Keep `EXT4FS_STATE_VALID` clear throughout a writable mount.
- [x] On clean unmount, commit and checkpoint everything, mark the journal
      empty, clear `RECOVER`, and finally set `EXT4FS_STATE_VALID`.
- [ ] Exercise actual handle wait/wakeup, owned-buffer teardown, ordered-vnode
      lifetime, abort teardown, and commit wakeups through the first
      production journaled metadata path.

### Phase 3 implementation status (2026-09-24)

The serialized writer now emits bounded descriptor and revoke records,
checksum-v2/v3 tails, escaped metadata payloads, and a checksummed commit
record.  It flushes ordered regular-file buffers before journal metadata,
persists `s_start` before the commit can become durable, checkpoints home
metadata only after the commit flush, and advances the clean journal head only
after the checkpoint flush.  A zeroed future commit slot makes a crash during
record construction an unambiguous incomplete tail.

Every successful commit checkpoints synchronously, so the next commit cannot
overwrite live log records.  Credit exhaustion commits and checkpoints the
current running transaction before retrying admission.  I/O and invariant
failures are sticky, leave `RECOVER` set, force the mount read-only, and retain
uncheckpointed buffers for invalidating teardown.  Teardown separately waits
for in-flight commit I/O, including the aborted-transaction case.

Writable initialization persists `RECOVER` before transaction data can become
durable.  Clean unmount forces all work through the writer, verifies that the
journal is empty, then durably clears `RECOVER` and sets `VALID`.  The kernel
objects compile with production `-Werror` flags on amd64 and i386, and the
non-root journal recovery and journal-core regression suites pass.

On 2026-09-24, the root-only `run-regress-journal-mount` suite also passed in
full against the rebuilt and booted production kernel.  This revalidates the
kernel recovery matrix, runtime initialization, clean teardown, and the new
incomplete-tail recovery rules without a test-only kernel configuration.  The
final Phase 3 item remains open because no production metadata path invokes
the writer yet; activation and writer-specific production-kernel tests begin
with Phase 4.

## Pre-Phase 4 ext4fs audit and baseline tests (2026-09-24)

The existing non-journal filesystem paths were audited before connecting them
to the runtime journal.  The audit used only IABSD/OpenBSD/BSD source and the
published ext4 on-disk format; Linux source was not used.  The implementation
is not yet safe to declare production-ready.  Journalling the current writers
would make several existing semantic and corruption bugs durable, so the
following issues are pre-Phase 4 blockers:

- validate normal extent trees as strictly as journal and orphan extent trees,
  including depth, entry capacity, ordering, physical ranges, checksums, and
  unwritten extents; reject writes and truncates of unsupported depth-2-or-
  deeper trees rather than treating index blocks as leaves;
- validate mount geometry and every block-group metadata location with
  overflow-safe arithmetic on both 32-bit and 64-bit systems, authenticate
  allocation bitmaps before use, bound the short final group, and make
  `FLEX_BG` plus uninitialized-group reconstruction safe;
- validate directory records before dereferencing them, verify directory
  checksums, and either maintain indexed directories or reject their mutation;
- restore BSD namespace and protection semantics, particularly sticky
  directories, same-inode and type-changing rename cases, immutable/append
  flags, special vnode initialization, and error propagation during deletion;
- serialize allocation/free accounting, avoid publishing initialized extents
  before their data is written, retain dirty inode state after I/O errors, and
  handle external extended-attribute block lifetime;
- define the writable ext4 feature profile explicitly: extent and file-type
  requirements, 32-byte versus 64-byte group descriptors, journal-less ext4,
  checksum feature dependencies, and indexed directories;
- remove signed `off_t` decoding and timestamp-shift undefined behavior, and
  use the correct `HUGE_FILE` block-count units, with i386 coverage.

A new root-only `regress/sys/ext4fsops` suite provides a repeatable baseline
through the booted production kernel.  It builds a separate `PROG=ext4fsops`
workload helper and runs it on 1 KiB, 2 KiB, and 4 KiB ext4 images.  Each
mutation stage is followed by an unmounted `e2fsck -fn` check and a remount
verification.  Coverage includes partial and multi-block I/O, append and
overwrite, sparse files above 4 GiB, truncate shrink/regrow, depth-1 extent-
tree promotion, directory growth/deletion/reuse, hard links, fast and
block-backed symlinks, FIFO creation, same- and cross-directory rename,
replacement, directory-loop rejection, open-unlinked lifetime, mode and
timestamp persistence, maximum-length names, and read-only mutation rejection.
The read-only pass also requires the complete image hash to remain unchanged.

- [x] Integrate the ordinary-operation suite into `regress/sys` with its own
      `PROG=` and a `REGRESS_ROOT_TARGETS` gate.
- [x] Build the helper with `-Wall -Werror -Wextra`, validate the shell driver,
      and smoke-test every writable helper state without root privileges.
- [x] Run `run-regress-ext4fsops` as root against the booted production kernel.

Expand the baseline with the following six groups of tests.  All kernel tests
must exercise the booted production kernel, without a test-only kernel
configuration or instrumentation.  Root-only cases must remain behind the
BSD regress `REGRESS_ROOT_TARGETS` gate.  Fixtures must be ext4 filesystems;
ext2 and ext3 compatibility is outside this project.

### 1. Malformed metadata and non-mutation

- [ ] Generate extent fixtures with bad magic, invalid depth, impossible
      `eh_entries`/`eh_max`, unordered or overlapping logical ranges,
      out-of-filesystem physical ranges, bad index targets, invalid unwritten
      extents, and bad extent-block checksums.
- [ ] Exercise corrupt depth-1 and depth-2 extent index/leaf blocks, including
      self-reference and cyclic-reference cases, under a bounded timeout.
- [ ] Generate directory blocks with zero, undersized, unaligned, and
      over-running record lengths; inconsistent name lengths and inode
      numbers; invalid file types; and damaged checksum tails.
- [ ] Corrupt block and inode bitmap checksums and construct bitmaps which mark
      reserved or out-of-final-group objects available.
- [ ] Corrupt 32-byte and 64-byte group descriptors, their checksums, and each
      bitmap/inode-table pointer independently.
- [ ] Exercise overflowing group counts, truncated final groups, invalid
      `FLEX_BG` placement, inconsistent uninitialized-group flags, and
      filesystem geometry near 32-bit arithmetic boundaries.
- [ ] Require every malformed fixture to fail in bounded time without a panic
      or loop, and compare complete pre- and post-test image hashes to prove
      that rejection did not modify the filesystem.

### 2. ENOSPC and failure rollback

- [ ] Exhaust data blocks and verify the results of a partial write, append,
      sparse-file extension, truncate growth, and extent-tree split.
- [ ] Exhaust free inodes and verify create, mkdir, mknod, symlink, and hard
      link failure paths.
- [ ] Force directory growth, rename replacement, and cross-directory rename
      at low-space boundaries, checking both the old and new namespace after
      each failure.
- [ ] Hold an unlinked file open while exhausting space, then close it and
      verify that its inode and blocks become reusable exactly once.
- [ ] After every injected resource failure, verify the expected errno,
      unmount cleanly, run `e2fsck -fn`, remount, validate surviving file data
      and names, and check free-space/free-inode accounting.
- [ ] Cover real device I/O error handling when a production-kernel mechanism
      can provide deterministic failures without ext4fs instrumentation; keep
      these cases separate from ordinary ENOSPC tests.

### 3. Namespace, protection, and special files

- [ ] Run credential-aware operations as multiple unprivileged UIDs and GIDs
      from the root-gated helper, including owner, group, and supplementary-
      group permission checks.
- [ ] Verify sticky-directory unlink and rename rules for the directory owner,
      file owner, unrelated users, and root.
- [ ] Verify immutable and append-only behavior for write, truncate, link,
      unlink, rename, and directory mutation, including persistence across a
      remount.
- [ ] Cover rename where source and destination are the same inode, all valid
      file/directory source-target combinations, empty and non-empty directory
      replacement, `.`/`..` rejection, and ancestor-cycle rejection.
- [ ] Exercise FIFO blocking and non-blocking I/O, UNIX-domain socket creation
      and removal, and safe character/block-device node metadata operations
      without opening arbitrary devices.
- [ ] Repeat rejected namespace operations enough times to expose vnode,
      buffer, or reference leaks, then require an ordinary non-forced unmount.

### 4. Parallel allocation and free stress

- [ ] Add a deterministic seeded multi-process workload which concurrently
      creates, writes, truncates, links, renames, unlinks, and recreates files
      in shared and disjoint directories.
- [ ] Run allocation/free races on both a mostly empty image and an image near
      block and inode exhaustion.
- [ ] Include competing directory growth, same-name creation, rename
      replacement, and open-unlinked-file workloads.
- [ ] Keep a userspace model of successful operations and verify namespace,
      lengths, link counts, and file contents after sync and remount.
- [ ] Bound every worker and the overall run with timeouts; record the seed on
      failure so every random schedule/workload can be reproduced.
- [ ] Finish every stress pass with a clean unmount, `e2fsck -fn`, a remount
      verification pass, and another clean unmount.

### 5. Format and boundary matrix

- [ ] Test valid unwritten extents for reads, partial conversion, truncate,
      hole punching where supported, and zero exposure at block boundaries.
- [ ] Test read-only access to depth-2-or-deeper extent trees and prove that
      unsupported mutations fail without changing the image; convert these to
      success tests when deep-tree mutation is implemented.
- [ ] Test indexed directories at one and multiple index levels; require
      unsupported mutations to be rejected without silently damaging the
      index.
- [ ] Exercise 32-byte and 64-byte group descriptors, `FLEX_BG`, `UNINIT_BG`,
      `metadata_csum`, and the explicitly supported feature combinations.
- [ ] Exercise one-group and multi-group filesystems, a short final block
      group, sparse large images, and logical/physical values around 2^31 and
      2^32 without requiring fully allocated multi-terabyte storage.
- [ ] Run each applicable case with 1 KiB, 2 KiB, and 4 KiB filesystem block
      sizes and verify `HUGE_FILE` block accounting and timestamp boundaries.

### 6. Architecture coverage

- [ ] Build the kernel and both regression helpers with warnings treated as
      errors on amd64 and i386, fixing narrowing, signedness, shift, and format
      issues rather than suppressing them.
- [ ] Run the complete ordinary-operation, malformed-metadata, ENOSPC,
      namespace, and stress suites through the booted production kernel on
      amd64 and i386.
- [ ] Use identical deterministic seeds and fixture manifests on both
      architectures, and compare expected errno values, namespace results,
      file contents, and `e2fsck -fn` results.
- [ ] Record architecture, filesystem block size, feature set, random seed,
      and the exact failed stage in retained-fixture diagnostics.

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
  images. Source code with an incompatible licence is intentionally not
  used as implementation input.
