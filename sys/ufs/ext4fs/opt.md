# ext4fs Journalling Optimization Plan

## Purpose

This plan improves ext4fs write performance without weakening ordered
journalling, recovery, or error handling.

The first target is newly allocated sequential file data.  Before
Phase 1, a large-file copy issued one filesystem block per transaction.
On a 4096-byte filesystem this produced roughly 4 KiB transfers and a
complete journal commit for every new data block.  This was consistent
with observed rsync throughput near 1 MiB/s.

The work must remain native BSD code.  It must not copy Linux source.
It must support ext4 only, preserve 32-bit system compatibility, and
work with 1024, 2048, and 4096-byte filesystem blocks.

## Correctness Rules

- [ ] Preserve metadata-only ordered journalling.
- [ ] Never make metadata reachable before its new data is durable.
- [ ] Never overwrite home metadata before its journal copy is durable.
- [ ] Never reuse journal space before the corresponding home metadata
      and updated journal tail are durable.
- [ ] Preserve revoke protection when freed blocks are reused.
- [ ] Preserve `fsync`, `O_SYNC`, synchronous-mount, unmount, and
      remount durability semantics.
- [ ] Propagate write and cache-flush failures to the journal abort
      path.
- [ ] Preserve the configured `errors=continue`, `remount-ro`, and
      `panic` policies.
- [ ] Keep all counters, offsets, credits, and byte calculations safe on
      32-bit systems.
- [ ] Retain the production kernel path.  Do not add a test-only kernel
      implementation or a separate optimization configuration.
- [ ] Pass the active journal handle through every in-transaction
      metadata lookup.  If the transaction already owns a block, use
      its tracked buffer instead of calling `bread()` or `getblk()` a
      second time and blocking on the same `B_BUSY` buffer.

## Initial Bottleneck Audit

The initial audit found these dominant costs:

- Newly allocated file growth handled one filesystem block in each
  transaction.  Allocation, extent insertion, ordered data write, inode
  update, commit, checkpoint, and journal clearing all occurred for that
  single block before Phase 1.
- Most metadata operations call `ext4fs_journal_force_commit()` before
  returning, so independent operations cannot share a transaction.
- A non-empty transaction currently reaches six device durability
  flushes: ordered data, precommit journal data, journal exposure,
  commit, checkpoint, and journal clearing.
- Journal descriptors, payload blocks, revoke blocks, commit blocks,
  and checkpoint blocks are written synchronously one block at a time.
- The committing caller also performs the complete checkpoint before it
  can return.  There is no queue of committed but uncheckpointed
  transactions.
- Transaction metadata buffers remain busy until commit and checkpoint.
  Removing forced commits without redesigning this ownership would risk
  deadlocks and premature home writes.
- Linear metadata and revoke searches and per-commit temporary
  allocations are secondary costs.  They should be addressed only
  after the I/O path is no longer the dominant limit.

## Status on 2026-10-02

Phase 1 now batches aligned append allocation into a bounded physical
run.  The production kernel caps a run at `MAXBSIZE`, keeps the run
within one block group, inserts it as one extent, writes every ordered
data block, and commits the metadata once.  Partial and sparse writes
remain on the single-block path.

The `allocation-run` regression performs one `MAXBSIZE` `pwrite`,
requires exactly one journal sequence advance, checks inode allocation,
unmounts, runs the offline checker, remounts read-only, and verifies all
data.  The booted production kernel passed this regression with 1024,
2048, and 4096-byte filesystem blocks.

The normal allocation-error VMM target injects failures at the first,
middle, and final data writes.  A separately named `-all-vmm` target
retains exhaustive failure injection at every write position.

The live whole-filesystem rsync initially transferred approximately
1--1.7 MiB/s.  With the Phase 1 kernel, rsync reached approximately
11 MiB/s while `systat` reported approximately 20 MiB/s of physical
write traffic on softraid RAID1.  These are field observations, not a
controlled benchmark.  The reported raw-device ceiling is 1200 Mbit/s,
or approximately 150 MB/s, so durability latency remains the likely
limit rather than media bandwidth.

On 2026-10-03, rsync reproduced a `getblk` wait after the queued-write
experiment had been reverted.  The follow-up audit found a separate
journal buffer ownership bug: a same-directory rename can grow a
depth-1 directory, dirty its external extent leaf, and then map the
source entry through a plain `bread()` of that same busy leaf.  The
pending fix establishes one rule for all such access: once a handle
has cached a metadata block, later access must ask that handle for the
tracked buffer and must not enter the buffer cache for the block again.
The handle is now passed through extent lookup and insertion preflight,
initialized block-bitmap construction, inode loading, and
extended-attribute allocation checks.  Orphan retirement performs its
complete file scan before opening the transaction and revalidates only
the selected blocks through the handle.  A focused rename-growth
regression forces this extent-leaf reuse on every supported filesystem
block size.  Kernel build, reboot, and production regression results
are still required.

The next Phase 2 change queues every physical data buffer in a bounded
allocation run before waiting.  A completion callback retains each
buffer until its result is collected, so the code neither loses buffer
ownership through `bawrite()` nor waits for unrelated device-vnode I/O.
All completions are checked before the inode or journal metadata is
logged.  The existing ordered-data durability flush is unchanged.  This
change builds in the production kernel.  A reboot and production
regression run are still required before its checklist item can be
marked complete.

## Phase 0: Establish a Reproducible Baseline

- [ ] Use disposable ext4 images for every benchmark.  Do not benchmark
      destructive changes on the live backup filesystem.
- [ ] Test 1024, 2048, and 4096-byte filesystem blocks.
- [ ] Test 32 and 64-byte group descriptors and the supported ext4
      feature combinations.
- [ ] Run each storage workload on a plain virtual disk and on softraid
      RAID1 when suitable hardware is available.
- [ ] Record the exact kernel build, source revision, image options,
      device layout, and mount options.
- [ ] Run each measurement at least three times and compare medians.
- [ ] Record wall time, user time, system time, throughput, transfer
      size, transaction count, metadata blocks per transaction, flush
      count, and time spent waiting for I/O.
- [ ] Prefer existing BSD tools such as `time`, `iostat`, `systat`,
      `vmstat`, and `ps` before adding kernel instrumentation.
- [ ] If counters are needed, keep them production-safe and readable
      without changing the active journalling algorithm.

The baseline workloads are:

- [ ] Create one large sequential file on an empty filesystem.
- [ ] Overwrite an existing large file without allocating blocks.
- [ ] Copy a directory tree with many small files and long names.
- [ ] Create and remove many files in one directory.
- [ ] Exercise `mkdir`, `link`, `rename`, `unlink`, and `rmdir` mixes.
- [ ] Repeat with periodic `fsync`, with `O_SYNC`, and on a synchronous
      mount.
- [ ] Run the representative whole-filesystem rsync workload.

After every benchmark image:

- [ ] Unmount cleanly.
- [ ] Run the available ext4 consistency checker read-only.
- [ ] Compare file contents, sparse ranges, links, modes, owners, times,
      extended inode state, and directory topology with the source.
- [ ] Run the existing ext4fs operation, corruption, journal-recovery,
      and VMM crash suites before accepting baseline measurements.

## Phase 1: Allocate and Commit Contiguous Runs

This is the highest-priority optimization.  It directly removes the
one-transaction-per-block behavior seen during rsync.

- [x] Replace the single-block journaled allocation path with a bounded
      contiguous-run path for aligned full-block writes.
- [x] Determine the requested run from the `uio`, filesystem block
      size, `MAXBSIZE`, and the 32-bit logical-block limit.
- [ ] Derive future larger run limits from extent capacity and available
      journal credits rather than a fixed conservative bound.
- [x] Begin with a conservative maximum run.  Raise it only from
      measurements and crash-test results.
- [ ] Reserve enough credits before changing anything for every bitmap,
      group descriptor, extent node, inode, and superblock that the run
      may dirty.
- [x] Stop or split a run at a block-group boundary unless all affected
      group metadata has been credited and validated.
- [x] Allocate a contiguous physical run where possible and insert one
      extent, or a bounded number of extents when fragmentation requires
      it.
- [x] Write and complete all ordered data for the run before exposing
      its metadata transaction as committed.
- [x] Keep partial first and last blocks on the existing safe path until
      their read-modify-write behavior is covered explicitly.
- [x] On a short write or data-write failure, expose only the fully
      initialized prefix or roll back the complete allocation.
- [x] Never expose an allocated block containing stale device data.
- [x] Preserve file size, `i_blocks`, timestamps, and extent checks when
      only part of the requested run succeeds.
- [x] Leave overwrites of already allocated blocks out of the allocation
      transaction unless they also change metadata.

Regression coverage must include:

- [x] Runs of one block and of the configured maximum length.
- [ ] Writes immediately below, at, and above the run boundary.
- [ ] Extent merging on the left, right, and both sides.
- [ ] Extent-root promotion and leaf splitting during a run.
- [ ] Block-group and flex-group boundaries.
- [ ] Fragmented allocation that returns a shorter run.
- [ ] ENOSPC after a partial allocation.
- [ ] Data-write failure at each block in a run.
- [ ] Power cuts before data durability, before commit, during
      checkpoint, and after journal clearing.
- [ ] Truncate and block reuse after a batched allocation.

The phase is successful when a large aligned write no longer produces
one transaction per filesystem block and measured transfer sizes and
throughput rise without a correctness regression.

## Phase 2: Queue Writes Within Existing Barriers

This phase changes I/O submission, not the durability graph.

An initial file-vnode `bawrite` implementation was reverted after a
live rsync child waited indefinitely in `getblk` with no device
activity.  A replacement must own each queued buffer and completion
explicitly; vnode-wide waiting is unsafe for this transaction path.

- [ ] Queue newly allocated ordered-data buffers and wait for all of
      them before beginning metadata logging.
- [ ] Queue descriptor, metadata payload, and revoke writes instead of
      waiting for every journal block separately.
- [ ] Wait for all queued precommit I/O at the existing precommit
      durability barrier.
- [ ] Queue independent checkpoint writes and wait for them at the
      existing checkpoint barrier.
- [ ] Retain separate ordering for the journal superblock and commit
      block until Phase 3 proves that a barrier may be shared.
- [ ] Ensure every queued buffer owns stable data until its I/O has
      completed.  Do not reuse a shared scratch block prematurely.
- [ ] Collect delayed buffer errors and convert any one of them into a
      journal abort before advancing the journal state.
- [ ] Verify that `VOP_FSYNC` and `DIOCCACHESYNC` report failures from
      all queued writes on plain disks, virtual disks, and softraid.
- [ ] Bound outstanding I/O so a large transaction cannot exhaust the
      buffer cache or starve unrelated filesystems.

Regression coverage must inject a failure into every queued write
position and vary completion order.  The existing rule that validation
precedes every home write remains mandatory.

## Phase 3: Reduce Durability Barriers

First document the exact write-order graph in source comments and in
the crash model.  Remove a barrier only after every earlier write may be
arbitrarily reordered within the proposed combined interval.

The first candidate is a reduction from six barriers to four:

1. Write ordered file data, journal records, and the cleared old commit
   slot, then issue one precommit durability flush.
2. Write the journal superblock that exposes the transaction and its
   commit block, then issue one commit durability flush.
3. Write home metadata, then issue one checkpoint durability flush.
4. Advance or clear the journal tail, then issue one clearing flush.

Required proof and tests:

- [ ] Show that a durable commit can never reference non-durable ordered
      file data.
- [ ] Show that a stale commit block cannot validate as the current
      sequence.
- [ ] Show that every exposed but incomplete transaction is rejected by
      replay without writing home metadata.
- [ ] Show that journal clearing can never become durable before all
      corresponding home metadata.
- [ ] Inject cuts and failures after every individual write as well as
      after every flush.
- [ ] Test devices that reorder writes and devices that reject cache
      synchronization.
- [ ] Keep a cache-flush error fatal to the current transaction even if
      all individual writes appeared successful.

Do not combine checkpoint durability with journal-tail advancement in
the present immediate-reuse design.

## Phase 4: Defer Checkpointing

Separate transaction commit from checkpoint completion.  A transaction
may become durably committed while its metadata remains only in the
journal, but its journal space must not yet be reused.

- [ ] Represent running, committing, committed, and checkpointing
      transaction states explicitly.
- [ ] Maintain the real journal head, oldest live tail, free-space
      count, and oldest committed sequence across multiple
      transactions.
- [ ] Retain each committed transaction until all its home metadata is
      durable and the updated journal tail is durable.
- [ ] Preserve the exact metadata image belonging to each transaction.
      A live buffer modified by a later transaction cannot serve as the
      older transaction's checkpoint image.
- [ ] Choose and document a BSD buffer ownership design: transaction
      shadow copies, journal reads for checkpoint, or another design
      that prevents early home writes and buffer-cache deadlocks.
- [ ] Release or unbusy live metadata only after the write-ahead rule is
      secured.
- [ ] Let a later caller or a bounded worker checkpoint committed
      transactions in batches.
- [ ] Force checkpoint progress under journal-space or memory pressure.
- [ ] Drain committed transactions during `fsync`, synchronous mount
      operations where required, remount, and unmount.
- [ ] Advance the journal tail only in transaction order.
- [ ] Keep revoke records effective across every uncheckpointed
      transaction that could otherwise replay stale data.
- [ ] Handle journal wraparound without treating all usable space as
      free after each commit.

VMM cuts must cover every state transition, multiple committed
transactions, wraparound, block reuse, revoke replay, and failure while
checkpointing or advancing the tail.

## Phase 5: Group Commit and Deferred Ordinary Commits

Do not remove unconditional operation-level commits until Phase 4 has
solved metadata buffer ownership and committed-transaction tracking.

- [ ] Classify commit reasons: `fsync`, `O_SYNC`, synchronous mount,
      VFS sync, unmount, remount, credit exhaustion, journal pressure,
      memory pressure, and maximum transaction age.
- [ ] Allow ordinary asynchronous metadata operations to join the
      running transaction and return without forcing an immediate
      commit.
- [ ] Bound transaction age so inactive filesystems still make durable
      progress.
- [ ] Permit a new running transaction while an older one is committing
      or checkpointing.
- [ ] Give every synchronous waiter a sequence fence for the transaction
      containing its own operation.
- [ ] Group concurrent `fsync` requests behind one commit where their
      required sequences permit it.
- [ ] Do not make one caller wait for unrelated operations that entered
      a later transaction.
- [ ] Propagate an abort to all handles and waiters whose guarantees can
      no longer be satisfied.
- [ ] Bound dirty metadata and ordered-data dependencies per running
      transaction.
- [ ] Stop and drain the commit machinery safely during unmount,
      remount, shutdown, and journal abort.

Regression coverage must combine concurrent create, write, rename,
unlink, truncate, and `fsync` operations with VMM power cuts and
injected I/O failures.

## Phase 6: CPU and Memory Scaling

Perform this work only after measurements show that CPU or allocation
cost has become significant.

- [ ] Reuse bounded per-journal scratch storage rather than allocating
      it for every commit.
- [ ] Add lookup structures for transaction metadata and revokes while
      retaining lists for deterministic journal order.
- [ ] Deduplicate ordered-vnode dependencies without repeated linear
      scans.
- [ ] Avoid copying or checksumming an unchanged metadata snapshot more
      than once.
- [ ] Coalesce repeated inode, group-descriptor, and superblock changes
      within one transaction.
- [ ] Measure journal-lock and vnode-lock contention under concurrent
      workloads.
- [ ] Audit every new table size, multiplication, block number, and byte
      offset for 32-bit overflow.

## Verification Matrix

Every phase must pass:

- [ ] `regress/sbin/fsck_ext4fs` journal fixtures and fuzz cases.
- [ ] All `regress/sys/ext4fsops` modes.
- [ ] All `regress/sys/ext4fs_corrupt` cases.
- [ ] The `errors=panic` VMM case.
- [ ] All `regress/sys/ext4fs_crash` local models.
- [ ] VMM crash tests for write, rename, directory, extent, truncate,
      reuse, journal wraparound, orphan handling, and error injection.
- [ ] 1024, 2048, and 4096-byte filesystem blocks.
- [ ] Supported 32 and 64-byte group descriptors.
- [ ] A 32-bit compile, with runtime tests when suitable hardware or a
      VM is available.
- [ ] Plain virtual-disk and softraid RAID1 workloads.
- [ ] Read-only consistency checks and content comparison after every
      recovered image.

New crash tests must cut power at individual write completions, not only
at the six historical flush boundaries.  Otherwise an optimization that
moves writes between barriers could escape the existing matrix.

## Performance Acceptance

- [ ] Keep baseline and optimized results in the test notes with the
      exact kernel and source revision.
- [ ] Report median and range, not only the best run.
- [ ] Confirm that aligned large-file growth uses multi-block allocation
      transactions.
- [ ] Confirm that journal and checkpoint writes are queued in batches
      after Phase 2.
- [ ] Count and document the durability barriers remaining after
      Phase 3.
- [ ] Measure small synchronous-operation latency as well as bulk
      throughput; do not hide a large latency regression behind rsync
      improvement.
- [ ] Reject an optimization that only shifts waiting into unmount or
      recovery without improving the complete workload.
- [ ] Reject any speedup that changes the accepted crash states, leaves
      `RECOVER` incorrectly cleared, or causes a checker error.

## Implementation Order

The intended order is:

1. Baseline and observability.
2. Multi-block journaled allocation and extent insertion.
3. Queued journal and checkpoint I/O within existing barriers.
4. Proven barrier reduction.
5. Committed-transaction queue and deferred checkpointing.
6. Group commit and deferred ordinary commits.
7. CPU and memory improvements justified by measurement.

Each item should be a small, reviewable change with its regression
tests.  The next phase begins only after the preceding phase builds,
passes the full correctness matrix, and has measured results.  This
makes the first optimization useful by itself and keeps later
concurrency work from obscuring the original failure when a crash
invariant is violated.
