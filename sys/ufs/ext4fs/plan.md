# ext4fs Journalling Plan

## Goal

Add reliable JBD2 journalling support to IABSD's ext4fs implementation.
The first supported runtime mode will be metadata-only `data=ordered`
journalling with a single filesystem-wide transaction. More advanced
modes and concurrency can be added after recovery and crash consistency
are proven.

## Current state

The tree contains mount-time and `fsck_ext4fs` JBD2 recovery using a
bounded, three-pass scan, revoke, and replay flow.  It also contains a
serialized runtime core and an ordered on-disk commit/checkpoint writer.
Runtime journal handles now cover inode updates, regular-file extent
allocation/free and truncate, hard links, unlink/orphan retirement,
create, and mknod.  Remaining namespace and metadata paths must still be
converted before direct metadata writes can be retired.

Recovery hardening and its production-kernel regression gate are
complete. The Phase 3 writer is activated and production-tested by the
converted Phase 4 metadata paths.

### Phase 1 implementation status (2026-09-23)

The kernel and `fsck_ext4fs` recovery implementations now compile from
the same documented JBD2 format definitions and the same ext4 checksum
implementation. Recovery uses a validation pass before the first
home-block write, validates the ext4 superblock, group descriptors,
journal inode, external journal extent nodes, journal geometry and
supported feature masks, and propagates read, write, and
durability-flush failures. Journal-target and revoke membership use
bounded hash tables, and physically aliased journal extents are
rejected.

Disposable images generated with e2fsprogs have been replayed
successfully for no-checksum, multi-tag checksum-v2, and checksum-v3
journals. The resulting home block matched the journal payload and
`e2fsck -fn` accepted each recovered image. Read-only validation
rejected replay without changing the image hash.

The automated `fsck_ext4fs` regression suite now covers checksum
formats, descriptor boundaries, deleted tags, revoke ordering, log and
transaction-ID wraparound, failure atomicity, malformed structures, and
seeded mutation runs.

Recovery now treats `RECOVER` with `s_start == 0` as a restart after the
durable journal-clean marker, accepts e2fsprogs checksum-v2 multi-tag
descriptors, and implements restartable classic-list and orphan-file
recovery. Orphan recovery keeps the filesystem clean while
checkpointing, processes classic lists from the tail, authenticates
orphan-file and extent metadata before mutation, and rebuilds allocation
counters before removing each durable recovery reference.

Phase 1 recovery hardening and its regression gate are complete. On
2026-09-23, the expanded root-only mount suite passed against the booted
production IABSD kernel. It exercised every userland recovery fixture
through the kernel path, modeled every persisted recovery durability
state, verified restartable classic-list and orphan-file cleanup, and
remounted completed images to prove idempotence. Corrupt recovery inputs
remained byte-for-byte unchanged with `RECOVER` or `ORPHAN_PRESENT`
preserved, and `e2fsck -fn` accepted every successfully recovered image.

## Phase 1: Harden journal recovery

- [x] Validate the journal superblock geometry:
  - block size matches the filesystem block size;
  - `s_first`, `s_start`, and `s_maxlen` are internally consistent;
  - the journal fits within inode 8;
  - allocation-size calculations cannot overflow.
- [x] Compare the journal UUID with `sb_journal_uuid`.
- [x] Define supported JBD2 feature masks and reject unknown
      incompatible features, including fast commits until they are
      implemented.
- [x] Verify the journal superblock checksum.
- [x] Verify descriptor, revoke, commit, and data-block checksums for
      checksum v2 and v3 journals.
- [x] Support transactions containing multiple descriptor and revoke
      blocks before the commit block.
- [x] Bound every pass by the journal length so malformed logs cannot
      loop forever.
- [x] Use wrap-safe transaction-ID comparisons.
- [x] Validate every replay target against the filesystem block count
      and reject targets inside the journal itself where appropriate.
- [x] Handle journal inode extent trees up to `EXT4FS_EXTENT_DEPTH_MAX`,
      or reject unsupported depths explicitly without modifying the
      filesystem.
- [x] Treat malformed tags, missing `LAST_TAG` in a non-full descriptor,
      invalid flag combinations, and invalid revoke lengths as errors.
      A structurally valid transaction tail without a commit record is
      instead discarded without replay because that is the normal
      power-loss case.
- [x] Flush replayed home blocks before marking the journal clean.
- [x] Clear `EXT4FS_FEATURE_INCOMPAT_RECOVER` only after replay and all
      required flushes have succeeded.
- [x] Apply the same recovery rules to `fsck_ext4fs`, preferably through
      shared format/parsing helpers where kernel/userland boundaries
      permit it.
- [x] Define and implement restartable handling when `RECOVER` is set
      but the journal superblock has `s_start == 0`.
- [x] Accept e2fsprogs checksum-v2 descriptor blocks containing multiple
      tags.
- [x] Implement safe, restartable recovery of the classic orphan list
      and the orphan file, persisting recovery progress before clearing
      either orphan root or `ORPHAN_PRESENT`.

### Phase 1 tests

- [x] Replay e2fsprogs-generated journals using no checksum, single-tag
      checksum v2, and checksum v3 formats.
- [x] Replay an e2fsprogs-generated checksum-v2 descriptor containing
      multiple tags.
- [x] Cover escaped data and a committed revoke-only transaction that
      suppresses an earlier logged home-block write.
- [x] Replay a checksum-v3 transaction spanning multiple descriptor
      blocks.
- [x] Replay generated journals on 1 KiB, 2 KiB, and 4 KiB filesystems.
- [x] Cover journal wraparound and transaction-ID wraparound.
- [x] Cover deleted tags and the exact descriptor-capacity boundary.
- [x] Corrupt the journal superblock, descriptor, data, revoke, and
      commit checksum regions independently.
- [x] Confirm a checksum-invalid transaction is not replayed and the
      image is byte-for-byte unchanged.
- [x] Confirm incomplete transactions are not replayed and the image is
      byte-for-byte unchanged.
- [x] Confirm a bad payload in transaction 2 prevents transaction 1 from
      being applied, proving the validation pass precedes all home-block
      writes.
- [x] Fuzz journal headers, tag counts, revoke lengths, and geometry
      fields.
- [x] Verify corrupted journals fail the mount without clearing
      `RECOVER`. The root-only `run-regress-journal-mount` target checks
      a valid replay control, byte-for-byte failure atomicity, and
      preservation of `RECOVER`.
- [x] Exercise every userland recovery fixture through the kernel mount
      path.
- [x] Test `RECOVER` with `s_start == 0`, including failures injected
      before and after each durability boundary, and prove that retry is
      idempotent.
- [x] Run the kernel-mount recovery matrix and modeled power-loss
      durability states against the booted production IABSD kernel.

## Phase 2: Introduce the runtime journal core

Start with a serialized implementation: one running transaction and one
committing transaction per mounted filesystem. Correctness is more
important than batching or throughput in this phase.

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

Each operation must reserve enough journal credits before modifying
metadata. Initial credit estimates can be conservative while the
implementation is serialized.

### Phase 2 implementation status (updated 2026-09-24)

The runtime core is present.  Mount-time recovery and runtime
initialization share one journal-superblock validator and one complete
logical-to-physical journal block map.  A mounted filesystem now owns an
opaque journal object with serialized handles, conservative credit
reservation, owned metadata buffers, ordered-data dependencies, revoke
tracking, journal-block exclusion, and a sticky abort error.  Mount
failure and unmount paths tear this state down.

Phase 3 supplies the ordered on-disk writer used by
`ext4fs_journal_force_commit()`.  Converted Phase 4 metadata paths now
use the runtime handles and generate transactions through this writer.

On 2026-09-24, the root-only journal mount suite passed in full against
the rebuilt and booted production `GENERIC.MP` kernel.  This exercised
runtime journal initialization and teardown after successful recovery
across all supported checksum formats and block sizes, as well as the
existing non-mutating corruption and restartable orphan-recovery cases.

The non-root `regress/sys/ext4fs` suite builds a separate
`journal_core_test` program and exercises the same side-effect-free
state policy implementation compiled into the kernel core.  This keeps
test-only entry points and configuration out of the production kernel.

- [x] Share validated journal geometry, features, checksum state, block
      map, and journal-block membership with recovery.
- [x] Add per-mount running/committing state, locking, wait channels,
      journal position/free-space fields, and sticky abort state.
- [x] Add serialized handles with overflow-safe conservative credit
      reservation.
- [x] Track unique metadata buffers and revokes, rejecting aliases,
      conflicts, out-of-range blocks, and journal blocks.
- [x] Initialize and destroy runtime journal state in the mount
      lifecycle.
- [x] Define ownership and lifetime rules for metadata buffers after a
      handle ends.  Successful write-access registration transfers a
      busy buffer to the handle; dirtying transfers it to the
      transaction; ending releases unused access, while abort teardown
      invalidates uncheckpointed contents.
- [x] Add transaction-owned ordered-data dependencies.  Dependencies are
      deduplicated regular-file vnodes held by reference until commit or
      teardown; the initial ordered mode will conservatively flush the
      whole vnode.
- [x] Add focused shared-state regression tests for serialized handle
      admission, credit reservation/exhaustion/release, duplicate and
      alias decisions, revoke conflicts, sticky aborts, and sequence
      wraparound.
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

- [x] Escape payload blocks beginning with `JBD2_MAGIC` and set
      `ESCAPE`.
- [x] Generate descriptor tags and checksum tails for the selected
      format.
- [x] Split transactions across as many descriptor blocks as necessary.
- [x] Prevent the head from overwriting uncheckpointed transactions.
- [x] Block or force a checkpoint when journal space is exhausted.
- [x] Abort the journal and force the filesystem read-only after an I/O
      or invariant failure.
- [x] Set `EXT4FS_FEATURE_INCOMPAT_RECOVER` before the first live
      transaction can become durable.
- [x] Keep `EXT4FS_STATE_VALID` clear throughout a writable mount.
- [x] On clean unmount, commit and checkpoint everything, mark the
      journal empty, clear `RECOVER`, and finally set
      `EXT4FS_STATE_VALID`.
- [ ] Exercise actual handle wait/wakeup, owned-buffer teardown,
      ordered-vnode lifetime, abort teardown, and commit wakeups through
      the first production journaled metadata path.

### Phase 3 implementation status (2026-09-24)

The serialized writer now emits bounded descriptor and revoke records,
checksum-v2/v3 tails, escaped metadata payloads, and a checksummed
commit record.  It flushes ordered regular-file buffers before journal
metadata, persists `s_start` before the commit can become durable,
checkpoints home metadata only after the commit flush, and advances the
clean journal head only after the checkpoint flush.  A zeroed future
commit slot makes a crash during record construction an unambiguous
incomplete tail.

Every successful commit checkpoints synchronously, so the next commit
cannot overwrite live log records.  Credit exhaustion commits and
checkpoints the current running transaction before retrying admission.
I/O and invariant failures are sticky, leave `RECOVER` set, force the
mount read-only, and retain uncheckpointed buffers for invalidating
teardown.  Teardown separately waits for in-flight commit I/O, including
the aborted-transaction case.

Writable initialization persists `RECOVER` before transaction data can
become durable.  Clean unmount forces all work through the writer,
verifies that the journal is empty, then durably clears `RECOVER` and
sets `VALID`.  The kernel objects compile with production `-Werror`
flags on amd64 and i386, and the non-root journal recovery and
journal-core regression suites pass.

On 2026-09-24, the root-only `run-regress-journal-mount` suite also
passed in full against the rebuilt and booted production kernel.  This
revalidates the kernel recovery matrix, runtime initialization, clean
teardown, and the new incomplete-tail recovery rules without a test-only
kernel configuration. Phase 4 subsequently activated the writer through
production metadata paths; the production-kernel runtime regressions now
cover those transactions.

## Pre-Phase 4 ext4fs audit and baseline tests (2026-09-24)

The existing non-journal filesystem paths were audited before connecting
them to the runtime journal.  The audit used only IABSD/OpenBSD/BSD
source and the published ext4 on-disk format; Linux source was not used.
The implementation is not yet safe to declare production-ready.
Journalling the current writers would make several existing semantic and
corruption bugs durable, so the following issues are pre-Phase 4
blockers:

- validate normal extent trees as strictly as journal and orphan extent
  trees, including depth, entry capacity, ordering, physical ranges,
  checksums, and unwritten extents; reject writes and truncates of
  unsupported depth-2-or-deeper trees rather than treating index blocks
  as leaves;
- validate mount geometry and every block-group metadata location with
  overflow-safe arithmetic on both 32-bit and 64-bit systems,
  authenticate allocation bitmaps before use, bound the short final
  group, and make `FLEX_BG` plus uninitialized-group reconstruction
  safe;
- validate directory records before dereferencing them, verify directory
  checksums, and either maintain indexed directories or reject their
  mutation;
- restore BSD namespace and protection semantics, particularly sticky
  directories, same-inode and type-changing rename cases,
  immutable/append flags, special vnode initialization, and error
  propagation during deletion;
- serialize allocation/free accounting, avoid publishing initialized
  extents before their data is written, retain dirty inode state after
  I/O errors, and handle external extended-attribute block lifetime;
- define the writable ext4 feature profile explicitly: extent and
  file-type requirements, 32-byte versus 64-byte group descriptors,
  journal-less ext4, checksum feature dependencies, and indexed
  directories;
- remove signed `off_t` decoding and timestamp-shift undefined behavior,
  and use the correct `HUGE_FILE` block-count units, with i386 coverage.

A new root-only `regress/sys/ext4fsops` suite provides a repeatable
baseline through the booted production kernel.  It builds a separate
`PROG=ext4fsops` workload helper and runs it on 1 KiB, 2 KiB, and 4 KiB
ext4 images.  Each mutation stage is followed by an unmounted `e2fsck
-fn` check and a remount verification.  Coverage includes partial and
multi-block I/O, append and overwrite, sparse files above 4 GiB,
truncate shrink/regrow, depth-1 extent- tree promotion, directory
growth/deletion/reuse, hard links, fast and block-backed symlinks, FIFO
creation, same- and cross-directory rename, replacement, directory-loop
rejection, open-unlinked lifetime, mode and timestamp persistence,
maximum-length names, and read-only mutation rejection. The read-only
pass also requires the complete image hash to remain unchanged.

- [x] Integrate the ordinary-operation suite into `regress/sys` with its
      own `PROG=` and a `REGRESS_ROOT_TARGETS` gate.
- [x] Build the helper with `-Wall -Werror -Wextra`, validate the shell
      driver, and smoke-test every writable helper state without root
      privileges.
- [x] Run `run-regress-ext4fsops` as root against the booted production
      kernel.

Expand the baseline with the following six groups of tests.  All kernel
tests must exercise the booted production kernel, without a test-only
kernel configuration or instrumentation.  Root-only cases must remain
behind the BSD regress `REGRESS_ROOT_TARGETS` gate.  Fixtures must be
ext4 filesystems; ext2 and ext3 compatibility is outside this project.

### 1. Malformed metadata and non-mutation

- [x] Integrate a separate `regress/sys/ext4fs_corrupt` suite with its
      own `PROG=ext4fs_corrupt`, a non-root fixture check, and a
      production-kernel target protected by `REGRESS_ROOT_TARGETS`.
- [x] Add the initial 1 KiB, 2 KiB, and 4 KiB rejection matrix for bad
      superblock and group-descriptor checksums, invalid block size,
      zero block/inode group sizes, invalid inode/descriptor sizes,
      invalid first inode, unsupported incompat features, and `RECOVER`
      without a journal. Each kernel case has a valid read-only control,
      a timeout, and a complete pre/post image hash comparison.
- [x] Run the initial corruption matrix as root through the booted amd64
      production kernel; all valid controls and malformed-image cases
      pass.
- [x] Add offline inline and depth-1 extent controls plus mutations for
      header magic, depth, entry capacity, ordering, overlap, zero
      length, physical and index ranges, external-leaf geometry, and
      external-block checksums. Valid controls are checked with `e2fsck
      -fn` before mutation.
- [x] Run the extent corruption matrix through the booted production
      kernel; each malformed target must fail its first read under a
      timeout, unmount normally, and preserve the complete image hash.
      The amd64 production kernel matrix passed on 2026-09-25.
- [ ] Generate extent fixtures with bad magic, invalid depth, impossible
      `eh_entries`/`eh_max`, unordered or overlapping logical ranges,
      out-of-filesystem physical ranges, bad index targets, invalid
      unwritten extents, and bad extent-block checksums.
- [ ] Exercise corrupt depth-1 and depth-2 extent index/leaf blocks,
      including self-reference and cyclic-reference cases, under a
      bounded timeout.
- [ ] Generate directory blocks with zero, undersized, unaligned, and
      over-running record lengths; inconsistent name lengths and inode
      numbers; invalid file types; and damaged checksum tails.
- [ ] Corrupt block and inode bitmap checksums and construct bitmaps
      which mark reserved or out-of-final-group objects available.
- [ ] Corrupt 32-byte and 64-byte group descriptors, their checksums,
      and each bitmap/inode-table pointer independently.
- [ ] Exercise overflowing group counts, truncated final groups, invalid
      `FLEX_BG` placement, inconsistent uninitialized-group flags, and
      filesystem geometry near 32-bit arithmetic boundaries.
- [ ] Require every malformed fixture to fail in bounded time without a
      panic or loop, and compare complete pre- and post-test image
      hashes to prove that rejection did not modify the filesystem.

### 2. ENOSPC and failure rollback

- [ ] Exhaust data blocks and verify the results of a partial write,
      append, sparse-file extension, truncate growth, and extent-tree
      split.
- [ ] Exhaust free inodes and verify create, mkdir, mknod, symlink, and
      hard link failure paths.
- [ ] Force directory growth, rename replacement, and cross-directory
      rename at low-space boundaries, checking both the old and new
      namespace after each failure.
- [ ] Hold an unlinked file open while exhausting space, then close it
      and verify that its inode and blocks become reusable exactly once.
- [ ] After every injected resource failure, verify the expected errno,
      unmount cleanly, run `e2fsck -fn`, remount, validate surviving
      file data and names, and check free-space/free-inode accounting.
- [ ] Cover real device I/O error handling when a production-kernel
      mechanism can provide deterministic failures without ext4fs
      instrumentation; keep these cases separate from ordinary ENOSPC
      tests.

### 3. Namespace, protection, and special files

- [ ] Run credential-aware operations as multiple unprivileged UIDs and
      GIDs from the root-gated helper, including owner, group, and
      supplementary- group permission checks.
- [ ] Verify sticky-directory unlink and rename rules for the directory
      owner, file owner, unrelated users, and root.
- [ ] Verify immutable and append-only behavior for write, truncate,
      link, unlink, rename, and directory mutation, including
      persistence across a remount.
- [ ] Cover rename where source and destination are the same inode, all
      valid file/directory source-target combinations, empty and
      non-empty directory replacement, `.`/`..` rejection, and
      ancestor-cycle rejection.
- [ ] Exercise FIFO blocking and non-blocking I/O, UNIX-domain socket
      creation and removal, and safe character/block-device node
      metadata operations without opening arbitrary devices.
- [ ] Repeat rejected namespace operations enough times to expose vnode,
      buffer, or reference leaks, then require an ordinary non-forced
      unmount.

### 4. Parallel allocation and free stress

- [ ] Add a deterministic seeded multi-process workload which
      concurrently creates, writes, truncates, links, renames, unlinks,
      and recreates files in shared and disjoint directories.
- [ ] Run allocation/free races on both a mostly empty image and an
      image near block and inode exhaustion.
- [ ] Include competing directory growth, same-name creation, rename
      replacement, and open-unlinked-file workloads.
- [ ] Keep a userspace model of successful operations and verify
      namespace, lengths, link counts, and file contents after sync and
      remount.
- [ ] Bound every worker and the overall run with timeouts; record the
      seed on failure so every random schedule/workload can be
      reproduced.
- [ ] Finish every stress pass with a clean unmount, `e2fsck -fn`, a
      remount verification pass, and another clean unmount.

### 5. Format and boundary matrix

- [ ] Test valid unwritten extents for reads, partial conversion,
      truncate, hole punching where supported, and zero exposure at
      block boundaries.
- [ ] Test read-only access to depth-2-or-deeper extent trees and prove
      that unsupported mutations fail without changing the image;
      convert these to success tests when deep-tree mutation is
      implemented.
- [ ] Test indexed directories at one and multiple index levels; require
      unsupported mutations to be rejected without silently damaging the
      index.
- [ ] Exercise 32-byte and 64-byte group descriptors, `FLEX_BG`,
      `UNINIT_BG`, `metadata_csum`, and the explicitly supported feature
      combinations.
- [ ] Exercise one-group and multi-group filesystems, a short final
      block group, sparse large images, and logical/physical values
      around 2^31 and 2^32 without requiring fully allocated
      multi-terabyte storage.
- [ ] Run each applicable case with 1 KiB, 2 KiB, and 4 KiB filesystem
      block sizes and verify `HUGE_FILE` block accounting and timestamp
      boundaries.

### 6. Architecture coverage

- [ ] Build the kernel and both regression helpers with warnings treated
      as errors on amd64 and i386, fixing narrowing, signedness, shift,
      and format issues rather than suppressing them.
- [ ] Run the complete ordinary-operation, malformed-metadata, ENOSPC,
      namespace, and stress suites through the booted production kernel
      on amd64 and i386.
- [ ] Use identical deterministic seeds and fixture manifests on both
      architectures, and compare expected errno values, namespace
      results, file contents, and `e2fsck -fn` results.
- [ ] Record architecture, filesystem block size, feature set, random
      seed, and the exact failed stage in retained-fixture diagnostics.

## Phase 4: Convert metadata writers

Audit every metadata write in `sys/ufs/ext4fs` and route it through a
journal handle. This includes:

- [x] inode-table blocks and inode timestamps;
- [x] block bitmaps;
- [x] inode bitmaps;
- [x] block-group descriptors;
- [x] extent-tree roots, index blocks, and leaf blocks;
- [x] directory blocks and checksum tails;
- [x] orphan-list and orphan-file updates;
- [x] allocation and free counters;
- [x] the ext4 superblock.

The first Phase 4 path was validated on 2026-09-25 against the booted
amd64 production kernel.  Inode-table updates now use a runtime journal
handle, commit and checkpoint synchronously, retain dirty inode state on
failure, and have a regression which proves that `chmod` advances the
JBD2 sequence before checking the checkpointed inode and the clean
journal.  The complete journal mount and ordinary-operation suites pass
with this path active.

Regular-file extent allocation, single-block and range free,
truncate-to-zero, and non-zero shrink are now journaled for supported
depth-0 and depth-1 extent trees.  Allocation bitmap, group-descriptor,
counter, superblock, extent-node, and inode changes are committed
together; freed extent-node blocks are revoked, and a retained partial
EOF block is synchronously zeroed before the metadata commit.  The
implementation rejects deeper mutable trees and invalid or physically
aliased extent metadata before changing the filesystem.  On 2026-09-25,
the production-kernel `ext4fsops` matrix passed on 1 KiB, 2 KiB, and 4
KiB filesystems, including depth-1 full release, depth-1 non-zero
shrink, partial-EOF zeroing and regrowth, remount verification, and
`e2fsck -fn` after each mutation stage.  The remaining broad metadata
checklist entries stay open until every writer using those structures
has been converted.

Hard-link creation is the first journaled compound namespace operation.
Linear directory blocks are structurally validated and checksum-verified
before mutation, and in-block insertion or directory growth is committed
with the parent inode and target inode link count in one transaction.
Indexed directory mutation remains explicitly unsupported.  On
2026-09-25, the production-kernel `ext4fsops` matrix passed on 1 KiB, 2
KiB, and 4 KiB filesystems with both ordinary hard-link insertion and a
packed-directory fixture which forces the hard link itself to allocate a
second directory block.  Remount verification and `e2fsck -fn` accepted
every image, and the FLEX_BG/BLOCK_UNINIT allocation control continued
to pass.

Non-final unlink is now journaled.  When the target has more than one
name, removal validates and checksum-verifies the linear directory
block, then commits its edit with the parent inode and decremented
target link count in one transaction.  The regression covers both
merging a removed entry into its predecessor and zeroing the first entry
in a directory block.  On 2026-09-26, both cases passed against the
booted production kernel on 1 KiB, 2 KiB, and 4 KiB filesystems; remount
verification, `e2fsck -fn`, and the FLEX_BG/BLOCK_UNINIT control also
passed.

Supported final-link removal of regular files now uses either the
classic orphan list or the orphan file.  The namespace removal and
orphan insertion commit atomically; final close leaves that durable
reference in place while truncation commits, then removes the reference
in the same transaction which retires the inode bitmap, group
descriptor, counters, inode, and superblock. A synchronized runtime list
permits non-head orphans to close safely, and unmount refuses to mark
the journal clean while a runtime orphan remains. Orphan-file insertion
and removal authenticate the orphan-file inode and extent map, every
block-tail checksum, every occupied slot, and the referenced inode
checksum; slot and checksum changes are journaled with the
`ORPHAN_PRESENT` feature bit.  The regression verifies that an
open-unlinked inode is not freed early, is freed exactly once on final
close, is immediately reusable, and that two orphans can close out of
insertion order.  On 2026-09-26, the production-kernel matrix passed
classic-orphan operation on 1 KiB, 2 KiB, and 4 KiB filesystems and
orphan-file operation on 1 KiB, including remount, `e2fsck -fn`, and
FLEX_BG/BLOCK_UNINIT checks.  External extended attributes remain on the
legacy path.  Final-link removal of fast symlinks, extent-backed slow
symlinks, and FIFOs now uses the same orphan lifecycle, with exact inode
and data-block accounting added to the ordinary and orphan-file
regressions.  On 2026-09-26, the full ext4fs regression set passed
against the booted production kernel with that extension.  Device nodes,
sockets, and regular files with external extended attributes now use the
same orphan lifecycle as well.  External-xattr deletion verifies the
block bitmap checksum and allocation bit, xattr checksum, and exact live
reference count before namespace mutation; final retirement either
journals the shared-block refcount/checksum decrement or frees and
revokes the final xattr block with the inode and orphan record.  The
regression constructs both unique and shared external-xattr fixtures and
checks exact inode/block accounting, along with offline-created
character, block, and socket inodes. Unsupported or corrupt final-link
inode shapes now fail closed instead of falling back to direct metadata
writes.  On 2026-09-26, the booted production- kernel matrix passed the
complete unlink regression on 1 KiB, 2 KiB, and 4 KiB classic-orphan
filesystems and the 1 KiB orphan-file filesystem, including remount and
offline `e2fsck -fn` validation.  The FLEX_BG/BLOCK_UNINIT case also
passed, so the overall unlink checklist item is complete.

The `create` and `mknod` conversion is complete.  A handle-aware inode
allocator now validates initialized inode bitmaps and their free counts,
safely constructs uninitialized inode bitmaps, and journals the selected
bitmap, block-group descriptor, free-inode counters, and superblock.
The new inode-table entry, parent directory block and checksum, parent
inode, and any directory-growth allocation or extent metadata are
committed in the same transaction.  Unused inode-table slots are no
longer zeroed by an out-of-transaction `bwrite()` in `VFS_VGET`.
Character and block device numbers, FIFOs, and sockets are fully
initialized before the namespace entry becomes durable.  The
ordinary-operation regression now creates those special inodes through
the kernel, checks exact inode/block accounting and nonzero device
numbers, and retains offline debugfs only for the shared-xattr fixture.

The first production-kernel special-inode lookup exposed a null
dereference: `ext4fs_vget()` assigned through `v_rdev` before
`v_specinfo` existed.  Device vnodes now follow the native BSD
`checkalias()` initialization path and use ext4fs special-device
operations; FIFO vnodes use the corresponding FIFO operations and
reclaim wrapper.  A focused `run-regress-ext4fsops-special` target
exercises create, remount/lookup, type and device-number validation,
unlink, remount, and offline `e2fsck` without running the full
operations matrix.  On 2026-09-27, the fixed booted production kernel
passed the complete ordinary-operation matrix for 1 KiB, 2 KiB, and 4
KiB filesystems, the 1 KiB orphan-file case, and FLEX_BG/BLOCK_UNINIT
allocation.  This covers the remounted special-inode lookup which
previously faulted, so the runtime gate is complete.

The `mkdir` and `rmdir` conversion is complete.  Directory creation now
commits inode allocation, the initialized `.`/`..` block and checksum
tail, the child inode, the parent link count, and the parent namespace
entry in one transaction.  Empty-directory removal commits the namespace
deletion and both link-count changes with a classic-orphan or
orphan-file record; the existing restartable inactive path then
truncates and retires the directory. Empty-directory validation now
checks every linear directory block and checksum, requires exactly one
correctly linked `.` and `..`, and fails closed on malformed records.
The ordinary-operation regression checks exact inode, block, child-size,
mode, and parent-link accounting for creation and removal, then verifies
absence after remount and runs offline `e2fsck -fn`. On 2026-09-27, the
booted production-kernel matrix passed on 1 KiB, 2 KiB, and 4 KiB
classic-orphan filesystems, the 1 KiB orphan-file filesystem, and the
FLEX_BG/BLOCK_UNINIT allocation fixture.

The `rename` conversion is complete.  Linear source and destination
directories are fully checksum- and structure-checked before mutation,
and explicit entry offsets prevent a destination lookup from overwriting
the source location during a same-directory rename.  One journal
transaction now covers destination insertion or replacement, source
removal, source and target inode changes, parent link counts,
cross-parent `..`, and a classic-orphan or orphan-file record when
replacement removes the target's last name.  The ancestry walk validates
every `.`/`..` block and has a hard cycle bound.  The ordinary-operation
regression now checks exact inode/block and parent-link accounting for
file and directory moves, final and non-final replacement, same-inode
no-op semantics, non-empty-target rejection, and same- and cross-parent
directory replacement across remount.  On 2026-09-27, the rebuilt and
booted production kernel passed the complete ext4fs regression matrix
with this path active.

The symlink conversion is complete.  It shares the new-inode
transaction used by `create` and `mknod`.  Fast targets are stored in
the journaled inode before their name becomes visible.  Block-backed
targets allocate their data block and extent in the same transaction,
and write the target to its home block before the metadata commit
exposes it.  The ordinary-operation regression checks both inode forms,
exact size and block accounting, target contents after remount, and
offline `e2fsck -fn` acceptance.  On 2026-09-27, the complete ext4fs
regression matrix passed against the rebuilt and booted production
kernel with this path active.

The block-bitmap writer audit and focused production-kernel gate are
complete.  Runtime allocation and free on a journaled mount use
handle-owned bitmap buffers, verify initialized bitmap checksums and
final-group bounds, update descriptor and superblock counters in the
same transaction, and revoke freed blocks.  Direct bitmap writes are
confined to explicitly documented journal-less ext4 paths and
restartable mount-time orphan recovery.  The focused regression checks
allocation, truncate-free, reuse, and final retirement on all three
block sizes.  Every mutation advances the on-disk JBD2 sequence,
preserves exact block and inode accounting across remount, and passes
offline `e2fsck -fn`.  On 2026-09-27, this matrix passed against the
booted production kernel together with the complete ordinary-operation
and special-inode suites.

The inode-bitmap writer audit and focused production-kernel gate are
complete.  Journaled allocation and retirement use handle-owned inode
bitmap buffers, validate initialized bitmap checksums and free counts,
reconstruct uninitialized bitmaps with reserved and padding bits set,
and update descriptor and superblock counters in the same transaction.
Legacy direct allocation and free fail closed if reached on a
journal-bearing mount.  The focused regression checks empty-file inode
allocation, free, immediate reuse, and final retirement on all three
block sizes.  Each mutation advances the JBD2 sequence, preserves exact
block and inode accounting across remount, and passes offline
`e2fsck -fn`.  On 2026-09-27, this matrix passed against the booted
production kernel, including `INODE_UNINIT` reconstruction, together
with the ordinary, block-bitmap, and special-inode suites.

The block-group descriptor writer audit and production-kernel gate are
complete.  Mount now accepts both ext4 descriptor
formats: 32-byte descriptors without the `64bit` feature and 64-byte
descriptors with it.  Each on-disk descriptor is unpacked into the full
in-memory structure, and every writer copies only the selected on-disk
width.  One shared location helper is used by journaled, journal-less,
and restartable-recovery writes.  Runtime journal mounts use only the
handle-owned writer; the journal-less writer is named and guarded as a
direct path.  The focused `run-regress-ext4fsops-bgd` target covers
both descriptor formats with 1 KiB, 2 KiB, and 4 KiB blocks.  It checks
allocation and complete retirement, JBD2 sequence advancement,
per-group counter restoration, remount persistence, descriptor
checksums, and offline `e2fsck -fn`.  On 2026-09-27, all six descriptor
format and block-size combinations passed against the rebuilt booted
production kernel.  The ordinary, block-bitmap, inode-bitmap, and
special-inode suites passed in the same run, so this checklist item is
complete.

The extent-tree writer audit and production-kernel gate are complete
for supported depth-0 and depth-1 trees.  Inode roots and external leaf
blocks are updated through the same transaction as data-block and
extent-block allocation or release.  Direct extent helpers are named
and guarded as journal-less paths.  A preflight before allocation and a
second check under the serialized journal handle reject any insertion
which would require an external index block and depth-2 mutation.

The focused `run-regress-ext4fsops-extents` target starts with a full
external leaf and exercises a split, another leaf insertion, partial
truncate and zeroed regrowth, removal of an empty leaf, and complete
truncate.  Every mutation advances the JBD2 sequence, has exact inode
block accounting, survives a remount, preserves extent checksums, and
passes offline `e2fsck -fn`.  A four-leaf boundary fixture also proves
that unsupported depth-2 growth returns `EOPNOTSUPP` without allocating
a block, changing descriptor counters, or starting a transaction.  On
2026-09-27, these cases passed against the rebuilt booted production
kernel with 1 KiB, 2 KiB, and 4 KiB blocks.  The ordinary, bitmap,
descriptor, and special-inode targets passed in the same complete run.

The directory-block and checksum-tail writer audit is complete.  Every
linear directory block is authenticated before a namespace mutation,
including operations reached through the name cache.  Create, link,
unlink, mkdir, rmdir, rename, and symlink use handle-owned buffers on a
journaled mount.  The checksum tail is regenerated and the complete
block is revalidated before it joins the transaction.  Indexed-directory
mutation remains unsupported and fails closed.  Legacy direct helpers
are private, explicitly named, guarded against journal mounts, and
revalidate the completed block before their journal-less write.

The focused `run-regress-ext4fsops-directory` target exercises directory
creation, insertion, removal, replacement, same-parent rename, and
cross-parent directory movement with and without `metadata_csum` on
1 KiB, 2 KiB, and 4 KiB filesystems.  It checks journal-sequence
advancement, remount persistence, read-only non-mutation, and offline
`e2fsck -fn` acceptance.  A damaged checksum-tail fixture proves that
lookup and creation fail without changing the directory block or
starting a transaction.  On 2026-09-27, every case passed against the
rebuilt and booted production kernel.

The orphan-list and orphan-file writer audit is complete.  Runtime
insertion and removal use journal handles, while direct writes are
confined to guarded, restartable mount-time recovery.  Before final
unlink, replacement rename, or `rmdir` starts a transaction, a locked
preflight authenticates the complete classic chain or orphan file and
confirms that an orphan-file slot is available.  The transaction then
reauthenticates the selected orphan block through its journal-owned
buffer.

The focused `run-regress-ext4fsops-orphan` target covers classic and
orphan-file operation with 1 KiB, 2 KiB, and 4 KiB blocks.  It exercises
closed and open unlink, out-of-order final close, replacement rename,
empty-directory retirement, exact accounting, immediate inode reuse,
remount persistence, journal-sequence advancement, and offline
`e2fsck -fn`.  Its corrupt-orphan-file fixture proves that unlink,
rename, and `rmdir` fail without changing the namespace, accounting,
or journal sequence.

The first production run exposed a self-deadlock when a transaction
owned the parent inode's table block and an in-transaction orphan scan
tried to read a tracked inode from that same busy block.  The preflight
now passes its authenticated slot into the transaction, avoiding that
second `bread()`.  The regression deliberately places the parent and
first orphan in one inode-table block.  On 2026-09-27, the complete
orphan target passed against the rebuilt and booted production kernel,
including this shared-buffer case, so the checklist item is complete.

The allocation and free-counter writer audit is complete.  Shared
helpers now decode and encode free-block, free-inode, and used-directory
counters for both 32-byte and 64-byte group descriptors.  After journal
replay and restartable orphan cleanup, writable mounts reconcile every
bounded group count with the ext4 superblock totals before starting the
runtime journal.  Clean read-only mounts perform the same check.

Before a runtime block or inode allocation or free changes metadata, it
authenticates the selected bitmap and requires its exact free-bit count
to match the group descriptor.  The bitmap, descriptor, global count,
and ext4 superblock then enter one journal transaction.  VFS sync no
longer bypasses that ownership by directly rewriting the superblock on
a journaled mount.  Guarded journal-less paths and restartable recovery
use the same descriptor counter helpers.

The focused `run-regress-ext4fsops-counters` target exercises balanced
block and inode allocation and retirement with 1 KiB, 2 KiB, and 4 KiB
blocks.  It verifies journal advancement, exact restoration of group
and superblock counters, and offline `e2fsck -fn` acceptance.  Validly
checksummed inconsistent global and group counters fail mount without
changing the image.  Fixtures whose forged group and superblock totals
agree but disagree with the bitmap mount successfully, then reject the
block or inode allocation without changing accounting or advancing the
journal.  On 2026-09-27, the focused target and complete ext4fs suite
passed against the rebuilt and booted production kernel, so this
checklist item is complete.

The ext4 superblock writer audit is complete.  Runtime allocation,
free, and orphan updates attach the primary superblock block to their
journal handle.  A single preparation path encodes mutable fields and
regenerates the checksum.  Direct writes are guarded and limited to
journal-less operation or restartable recovery.  RECOVER and clean or
dirty mount-state transitions use a separate lifecycle path because
they must bracket the journal rather than enter it.

The focused `run-regress-ext4fsops-superblock` target exercises
transactional free-block and free-inode updates with 1 KiB, 2 KiB, and
4 KiB filesystem blocks.  It verifies journal advancement, exact
counter restoration, stable identity fields, clean journal state,
empty writable-mount behavior, read-only non-mutation, and offline
`e2fsck -fn` acceptance.  Checksummed corrupt and validly checksummed
dirty superblocks are rejected without modifying their images.  On
2026-09-27, the rebuilt production kernel and complete ext4fs tests
passed, completing Phase 4.

Direct `bwrite()`, `bdwrite()`, or `bawrite()` calls must remain only
for regular-file data, the journal's own I/O, recovery, checkpointing,
or another explicitly documented exception.

Wrap each compound namespace operation in one transaction:

- [x] create and mknod;
- [x] link;
- [x] unlink;
- [x] mkdir and rmdir;
- [x] rename;
- [x] symlink;
- [x] truncate and extent allocation/free for supported depth-0 and
      depth-1 regular-file extent trees.

## Phase 5: VFS semantics

- [x] Make `fsync()` commit the transaction containing the inode and
      wait for ordered data and the commit record to become durable.
- [x] Make synchronous mounts and `O_SYNC` writes force the required
      commit.
- [x] Make VFS sync commit and checkpoint outstanding journal work.
- [x] Ensure read-only mounts may replay recovery only when the device
      can be safely opened for writing; otherwise fail without modifying
      it.
- [x] Define remount read-only/read-write behavior.
- [ ] Implement consistent journal abort and ext4 error-policy handling.
- [x] Expose useful journal state and failure diagnostics without
      excessive normal-operation logging.

The `fsync()` durability path is complete.  A synchronous flush waits
for vnode data writes and checks their sticky error state before inode
metadata can enter the journal.  The inode-table block then joins the
running transaction.  Commit processing flushes earlier data, writes
and flushes the commit record, checkpoints the inode, and empties the
journal before returning.

The focused `run-regress-ext4fsops-fsync` target exercises initial and
in-place writes with 1 KiB, 2 KiB, and 4 KiB filesystem blocks.  It
requires dirty `fsync()` calls to advance the journal sequence, proves
that a clean `fsync()` creates no transaction, verifies remounted data,
and finishes with offline `e2fsck -fn`.  On 2026-09-28, the focused
target and complete ext4fs suite passed against the rebuilt and booted
production kernel.

Synchronous-mount and `O_SYNC` write semantics are complete.  The BSD
VFS layer supplies `IO_SYNC` for both cases, and ext4fs now propagates
synchronous data-buffer write failures instead of committing inode
metadata after a failed write.  A successful write commits and
checkpoints its inode transaction before returning.  `mount_ext4fs`
accepts the standard BSD `sync` option and passes `MNT_SYNCHRONOUS` to
the kernel.

The focused `run-regress-ext4fsops-sync` target exercises `O_SYNC`
writes and ordinary writes on a synchronous mount with 1 KiB, 2 KiB,
and 4 KiB filesystem blocks.  It observes the journal sequence while
the filesystem is still mounted, proving that the write returned only
after its commit and that unmount had no delayed transaction to finish.
It also verifies that an empty `O_SYNC` write creates no transaction,
checks remounted data, and runs offline `e2fsck -fn`.  On 2026-09-28,
the focused target and complete ext4fs suite passed against the rebuilt
and booted production kernel.

VFS sync semantics are complete.  Non-lazy sync first flushes dirty
vnodes, then commits and checkpoints any remaining journal work before
flushing the device.  `MNT_LAZY` skips the vnode walk but still drains
filesystem-owned journal state.  On a journal-less mount, the ext4
superblock is written before the final device flush.

The focused `run-regress-ext4fsops-vfs-sync` target performs a normal
buffered update followed by `sync()` with 1 KiB, 2 KiB, and 4 KiB
filesystem blocks.  While the filesystem remains mounted, it requires
the journal sequence to advance and `s_start` to be zero.  It proves
that unmount has no delayed transaction to finish and that a clean
`sync()` creates no transaction.  It also verifies remounted data,
read-only non-mutation, and offline `e2fsck -fn` acceptance.  On
2026-09-28, the focused target and complete ext4fs suite passed against
the rebuilt and booted production kernel.

Read-only recovery authorization is complete.  A read-only mount first
opens the device for reading.  If `RECOVER` requires replay, mountfs
must successfully reopen it with `FREAD | FWRITE` before calling the
replay engine.  A failed write open exits before any recovery write and
retains `RECOVER`.  Successful recovery restores a read-only device
open before publishing the mount.

The kernel journal regression stores valid recovery images inside a
read-only mounted ext4 filesystem and attaches those files as nested
read-only vnd devices.  It covers both a pending transaction and the
restartable `RECOVER` with `s_start == 0` state.  Both mounts must fail,
the image hashes must remain unchanged, and `RECOVER` must remain set.
The existing writable-device control proves that a requested read-only
mount still performs valid recovery when write access is available.
On 2026-09-28, the complete kernel journal suite passed against the
booted production kernel.

Read-only/read-write remount behavior is complete.  Transitions use an
ext4fs-private, non-blocking lifecycle lock, so overlapping updates fail
with `EBUSY` instead of concurrently destroying or rebuilding journal
state.  As in FFS and ext2fs, the existing block-device open is retained
across a transition.  A writable-to-read-only remount synchronizes dirty
vnodes, commits and checkpoints the journal, refuses active writable or
unlinked inodes, clears `RECOVER`, and marks the filesystem valid.  A
read-only-to-writable remount revalidates writable feature and
clean-state requirements, performs restartable orphan cleanup, checks
counters, rebuilds the runtime journal, sets `RECOVER`, and durably
marks the filesystem dirty.  Failed upgrades restore a usable read-only
runtime.

The focused `run-regress-ext4fsops-remount` target covers classic orphan
metadata with 1 KiB, 2 KiB, and 4 KiB blocks and orphan-file metadata
with 1 KiB blocks.  It verifies both transition directions, durable
journal and superblock state, read-only mutation rejection, and a failed
read-only transition while an unlinked inode remains open.  The failed
transition must retain `RECOVER`, leave the filesystem dirty and
writable, and succeed after the final close retires the orphan.  On
2026-09-28, the focused target and complete ext4fs suite passed against
the rebuilt and booted production kernel.

Journal abort and error-policy handling is implemented.  The first
journal error is sticky, marks the ext4 filesystem erroneous, publishes
the mount read-only, and wakes journal waiters.  Both `errors=continue`
and `errors=remount-ro` force read-only operation because metadata
updates cannot safely continue without the journal; `errors=panic`
panics.  Invalid on-disk policies are rejected during mount.  Aborted
teardown preserves `RECOVER` and any durable orphan roots for the next
recovery.

Only the first abort emits a diagnostic.  It names the mount, exact
caller and synchronized commit stage, error, transaction sequence, and
journal head, tail, and free-space state.  Normal commits do not log,
and the unsupported ext4fs VFS sysctl now returns `EOPNOTSUPP` silently.
The shared non-root journal-core regression verifies sticky errors and
every exact diagnostic stage name.  The kernel corruption suite covers
both non-panic policies at every filesystem block size.  On 2026-09-28,
the final production kernel built and booted after the commit-stage
locking refinement, and the complete ext4fs regression suite passed.
Only the isolated panic-policy test remains for abort-policy completion.

`errors=panic` must be tested only in a disposable vmd guest.  The guest
will boot the host's installed production `/bsd`, attach a copied IABSD
root disk and a panic-policy ext4 corruption fixture, and trigger the
abort over SSH.  A root-only VMM regression must capture the serial
console, require the exact ext4fs panic diagnostic, stop the guest, and
verify that the fixture retained `RECOVER`.  It must have a watchdog and
must never run as part of an ordinary non-root regression invocation.

## Phase 6: Crash-consistency test matrix

Use filesystem images created by Linux tools and run IABSD in a VM.
Inject an abrupt power loss after each commit phase and at journal
wraparound boundaries.

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
- IABSD's existing ext4fs implementation and locally generated
  filesystem images. Source code with an incompatible licence is
  intentionally not used as implementation input.
