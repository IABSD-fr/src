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
- Most metadata operations still call `ext4fs_journal_commit()` with
  the `ORDINARY` reason before returning, so independent operations
  cannot yet share a transaction.
- Before Phase 3, a non-empty transaction reached six device durability
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
run.  The first production kernel capped a run at `MAXBSIZE`, kept the
run within one block group, inserted it as one extent, wrote every
ordered data block, and committed the metadata once.  Partial and
sparse writes remain on the single-block path.

The first `allocation-run` regression performed one `MAXBSIZE`
`pwrite`, required exactly one journal sequence advance, checked inode
allocation, unmounted, ran the offline checker, remounted read-only,
and verified all data.  The booted production kernel passed this
regression with 1024, 2048, and 4096-byte filesystem blocks.

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
change builds in the production kernel.  After reboot, the focused
allocation-run regression passed with 1024, 2048, and 4096-byte
filesystem blocks.  The checklist item is complete.

The subsequent live rsync run showed no obvious performance gain from
ordered-data queuing alone.  This means serial data-buffer completion
was not the dominant remaining cost for that workload.  Transaction
flush latency and the still-synchronous journal and checkpoint writes
remain the leading candidates.  The result is retained even though the
change improves I/O concurrency and gives every queued buffer explicit
completion and error ownership.

The follow-up Phase 2 implementation queues journal descriptors,
metadata payloads, revoke records, and the cleared future commit slot
as one completion batch.  It drains that batch before the unchanged
precommit durability flush.  Checkpoint home-block writes use a second
batch and drain before their unchanged durability flush.  Each journal
buffer receives its own copied contents before submission, delayed
errors are collected before journal state advances, and the device
`bufq` bounds outstanding writes.  Journal-superblock and commit-block
writes remain separately ordered and synchronous.  Production kernel
build, reboot, error injection, and regression results are pending.

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
      size, bounded byte cap, and the 32-bit logical-block limit.
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
- [x] Writes immediately below, at, and above the run boundary.
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

- [x] Queue newly allocated ordered-data buffers and wait for all of
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

The first implementation reduces six barriers to four:

1. Write ordered file data, journal records, and the cleared old commit
   slot, then issue one precommit durability flush.
2. Write the journal superblock that exposes the transaction and its
   commit block, then issue one commit durability flush.
3. Write home metadata, then issue one checkpoint durability flush.
4. Advance or clear the journal tail, then issue one clearing flush.

The first flush follows completion of every ordered-data write, every
journal record, and the zeroing of the future commit slot.  A commit
record is not written unless this flush succeeds, so a durable commit
cannot refer to non-durable ordered data.  The durably cleared slot and
the sequence-bound commit checksum prevent an older commit from
validating as the current transaction.

Journal exposure and the commit record may reach storage in either
order before the second flush.  Exposure without a complete matching
commit is an incomplete tail, which replay ignores without performing
a home-block write.  A commit without durable exposure remains
unreachable.  If both are durable, replay applies the complete
transaction.  Checkpoint buffers are drained and flushed before the
clean journal superblock is written, so clearing cannot overtake home
metadata durability.

The kernel and VMM matrix now implement these four boundaries.  The
flush-cut expectations are old state after boundary one, committed new
state after boundaries two and three, and clean new state after boundary
four.

The production kernel built and booted with the four-barrier path.  On
2026-10-03, the focused rename VMM matrix passed all four durability
boundaries with 1024-byte filesystem blocks.  The remaining Phase 3
gates are the other transaction shapes and block sizes, individual-write
cuts, injected write and flush failures, and performance measurement.

The focused 1024-byte error matrix also passed its device-read failure,
journal-write failure, and all four journal-flush failure cases.  Each
flush failure remained fatal to the transaction and recovered to an
accepted atomic state.

The rename flush-cut matrix then passed boundaries one through four
with 2048 and 4096-byte filesystem blocks.  Together with the earlier
1024-byte run, every supported block size now passes the complete
four-boundary matrix.

Required proof and tests:

- [x] Show that a durable commit can never reference non-durable ordered
      file data.
- [x] Show that a stale commit block cannot validate as the current
      sequence.
- [x] Show that every exposed but incomplete transaction is rejected by
      replay without writing home metadata.
- [x] Show that journal clearing can never become durable before all
      corresponding home metadata.
- [ ] Inject cuts and failures after every individual write as well as
      after every flush.
- [ ] Test devices that reorder writes and devices that reject cache
      synchronization.
- [x] Keep a cache-flush error fatal to the current transaction even if
      all individual writes appeared successful.

Do not combine checkpoint durability with journal-tail advancement in
the present immediate-reuse design.

## Phase 4: Defer Checkpointing

Separate transaction commit from checkpoint completion.  A transaction
may become durably committed while its metadata remains only in the
journal, but its journal space must not yet be reused.

The first Phase 4 slice gives every transaction an explicit state and
validates the only legal transition chain: new, running, committing,
committed, checkpointing, then done.  The committed transition occurs
only after the commit durability flush.  Checkpointing begins before
the first home write, and done occurs only after the durable journal
clear.  This slice does not defer checkpointing yet; it makes skipped
or reordered lifecycle boundaries fail before ownership is split.

The first concurrent run exposed a handoff race in the serialized
writer.  A waiter could detach another caller's transaction while the
original caller still held its ordered vnode.  The original caller then
waited for the commit while the committing waiter waited for that
vnode.  Transaction end now reserves the commit handoff for its caller,
and new handles wait for the committing transaction to finish.  The
rebuilt production kernel passed the complete ext4fs regression suite,
including the transaction-handoff stress case.

The second slice adds one transaction-owned, block-sized shadow image
for every dirty metadata buffer.  The image is captured after commit
validation and before the first journal record is constructed.  Both
the journal payload and the home-block checkpoint now consume that
same frozen image.  Failed commits free their shadows while discarding
their busy live buffers.  This slice deliberately keeps live buffers
busy and retains immediate checkpointing until the shadow path builds
and passes the existing operation and crash matrices.

The production kernel built and the operation and VMM crash suites
passed with the frozen-image path.  The exact-image requirement is
therefore complete.

The proposed third slice checkpoints each frozen image through an
anonymous BSD raw buffer without reacquiring the live cached block.
Once the commit flush is durable, its live metadata buffers are
released.  A new transaction may then run while the older transaction
checkpoints its immutable images.  Commit and journal-space reuse stay
serialized until the committed-transaction queue is implemented.

With this kernel, the representative live rsync workload reached
approximately 20 MB/s, compared with approximately 11 MB/s before the
checkpoint-overlap change.  This is a field observation rather than a
controlled benchmark, but it shows that synchronous checkpoint stalls
were a material part of the remaining workload latency.

The proposed fourth slice retains as many as eight durable committed
transactions or 16 MiB of journal blocks before checkpointing them as
one bounded batch.  The journal head advances and free space decreases
after each commit, while the tail remains at the oldest retained
transaction.  If a transaction does not fit, the retained batch is
checkpointed before the commit is retried.  Home writes remain ordered
by transaction, one checkpoint flush covers the batch, and a durable
clean-superblock update releases all of its log space.  Journals without
revoke support and filesystems mounted `sync` keep the earlier immediate
checkpoint behavior.  Explicit filesystem synchronization, remount,
and unmount drain any retained batch.

The VMM boundary helper now starts from an empty journal and performs an
explicit directory `fsync` after the tested operation.  This preserves
the four named crash boundaries while ordinary operations are allowed
to return after the second, commit-durability boundary.

The committed-queue kernel passed the transaction-handoff and
synchronous-operation regressions with 1024, 2048, and 4096-byte
filesystem blocks.  It also passed all four VMM durability cuts and all
injected device-read, journal-write, and journal-flush failures at each
of those block sizes.  The focused retained-queue VMM regression passed
at all three block sizes as well: recovery scanned multiple committed
transactions across physical log wraparound and honored a later revoke
against an older extent-leaf image.

- [x] Represent running, committing, committed, and checkpointing
      transaction states explicitly.
- [x] Maintain the real journal head, oldest live tail, free-space
      count, and oldest committed sequence across multiple
      transactions.
- [x] Retain each committed transaction until all its home metadata is
      durable and the updated journal tail is durable.
- [x] Preserve the exact metadata image belonging to each transaction.
      A live buffer modified by a later transaction cannot serve as the
      older transaction's checkpoint image.
- [x] Choose and document a BSD buffer ownership design: transaction
      shadow copies, journal reads for checkpoint, or another design
      that prevents early home writes and buffer-cache deadlocks.
- [x] Release or unbusy live metadata only after the write-ahead rule is
      secured.
- [x] Let a later caller or a bounded worker checkpoint committed
      transactions in batches.
- [x] Force checkpoint progress under journal-space or memory pressure.
- [x] Drain committed transactions during `fsync`, synchronous mount
      operations where required, remount, and unmount.
- [x] Advance the journal tail only in transaction order.
- [x] Keep revoke records effective across every uncheckpointed
      transaction that could otherwise replay stale data.
- [x] Handle journal wraparound without treating all usable space as
      free after each commit.

VMM cuts must cover every state transition, multiple committed
transactions, wraparound, block reuse, revoke replay, and failure while
checkpointing or advancing the tail.

## Phase 5: Group Commit and Deferred Ordinary Commits

Do not remove unconditional operation-level commits until Phase 4 has
solved metadata buffer ownership and committed-transaction tracking.

The first Phase 5 slice replaces the unclassified force-commit entry
point with an explicit reason.  Ordinary operations, `fsync`,
`O_SYNC`, synchronous mounts, VFS sync, remount, unmount, credit
exhaustion, journal pressure, memory pressure, and transaction age now
have distinct values and a side-effect-free policy test.  Durability
call sites pass their actual reason.  `fsync`, synchronous mounts, VFS
sync, remount, unmount, journal pressure, and memory pressure drain the
checkpoint queue.  `O_SYNC`, credit exhaustion, and transaction age
require a durable commit but need not checkpoint unrelated committed
transactions.  Ordinary call sites still commit immediately in this
slice; allowing them to join is the next change.  Credit, memory, and
age triggers remain later Phase 5 work, but no further public API
change is needed to identify them.

On 2026-10-03, the production kernel passed the transaction-handoff
and synchronous-operation regressions with 1024, 2048, and 4096-byte
filesystem blocks.  The 1024-byte VMM rename test also passed all four
commit and checkpoint durability cuts.  The classified commit policy
is therefore accepted.

- [x] Classify commit reasons: `fsync`, `O_SYNC`, synchronous mount,
      VFS sync, unmount, remount, credit exhaustion, journal pressure,
      memory pressure, and maximum transaction age.
- [x] Allow ordinary asynchronous metadata operations to join the
      running transaction and return without forcing an immediate
      commit.
- [x] Bound transaction age so inactive filesystems still make durable
      progress.
- [x] Permit a new running transaction while an older one is committing
      or checkpointing.
- [x] Give every synchronous waiter a sequence fence for the transaction
      containing its own operation.
- [x] Group concurrent `fsync` requests behind one commit where their
      required sequences permit it.
- [x] Do not make one caller wait for unrelated operations that entered
      a later transaction.
- [ ] Propagate an abort to all handles and waiters whose guarantees can
      no longer be satisfied.
- [ ] Bound dirty metadata and ordered-data dependencies per running
      transaction.
- [ ] Stop and drain the commit machinery safely during unmount,
      remount, shutdown, and journal abort.

`ext4fs_journal_end_commit()` captures the sequence of the transaction
containing the completed handle.  Synchronous callers wait for that
transaction rather than committing whichever transaction is running
later.  An empty handle records an exclusive fence because its unused
sequence can be assigned to a successor.  Global VFS sync, remount, and
unmount retain their full-drain behavior.  On 2026-10-04, the booted
production kernel passed the handoff, grouping, `fsync`, and
synchronous-mount regressions at 1024, 2048, and 4096-byte filesystem
blocks.  The focused 1024-byte VMM flush regression also passed.

Concurrent `fsync` grouping is accepted.  Once `fsync` has flushed its
vnode data, a metadata-only transaction may release the commit handoff
and yield once to already-runnable peer `fsync` callers.  Those callers
join the same running transaction and retain the same sequence fence.
A transaction with an ordered-data dependency keeps the handoff, so
another caller cannot detach it while its owner holds a required vnode
lock.  The focused grouping regression starts eight simultaneous
dirty-file `fsync` calls and requires fewer than eight sequence
advances.  On 2026-10-04, the production kernel passed that test and
the `fsync`, handoff, synchronous-operation, and focused VMM flush
regressions.

The exact-fence cutoff is accepted.  A waiter locates its sequence in
the running, committing, or committed state before considering an
active handle.  Once the required transaction is complete, a successor
handle or commit cannot extend that wait.  An exact checkpoint advances
only through the required committed transaction, preserves the next
transaction as the on-disk journal tail, and releases only the selected
prefix's journal space.  Exclusive-before fences use wrap-aware
sequence ordering.  Global VFS sync, remount, unmount, and pressure
reasons still drain the whole queue.  The non-root journal-state
regression covers sequence wraparound and partial-then-final checkpoint
accounting.  On 2026-10-04, the production kernel passed grouping,
`fsync`, synchronous-operation, VFS-sync, handoff, and remount tests at
1024, 2048, and 4096-byte filesystem blocks.  The focused VMM queue and
flush tests also passed.

The batching experiment raised the maximum ordinary transaction age
from 10 ms to 100 ms.  Synchronous reasons retained their existing
immediate fences, and credit exhaustion still rotated a transaction
early.  Runtime credit admission also capped dirty metadata at 4 MiB
per running transaction, further restricted by journal geometry and
the journal superblock's advertised transaction maximum.  The cap is
computed without multiplication so it remains safe on 32-bit systems.
Transactions carrying ordered-vnode dependencies retain their existing
ordinary-completion commit rule.  On 2026-10-04, the production kernel
passed the grouping, allocation-run, `fsync`, synchronous-operation,
and handoff regressions at 1024, 2048, and 4096-byte filesystem blocks.

A follow-up experiment reduced the maximum age to 10 ms.  The
production kernel passed the grouping, handoff, and
synchronous-operation regressions, but a live rsync transfer fell to
approximately 485 KiB/s.  Newly allocated data belongs to the running
transaction and does not force an ordinary-operation commit, so the
10 ms timer rotated an active large-file stream and greatly increased
journal commit and flush traffic.  The default is therefore restored
to 100 ms.  Synchronous fences remain immediate, and the 100 ms value
still bounds inactive metadata transactions.

The first live rsync run exposed a transaction-owned-buffer wait in
large depth-1 extent trees.  The append-allocation path checked extent
insertion and selected an allocation goal before opening its journal
handle.  Both lookups could enter `bread()` on the previous run's busy
extent leaf.  With a 100 ms transaction age and a 64 KiB allocation
run, this imposed a ceiling near 640 KiB/s; rsync measured 566.69 KiB/s.
Both lookups now occur after `ext4fs_journal_begin()` and pass the
active handle, so they reuse the transaction-owned extent buffer.  A
focused regression performs repeated small sequential appends through
an existing depth-1 extent leaf and requires fewer than sixteen journal
sequence advances.  On 2026-10-04, that regression rejected the booted
pre-fix kernel as intended, then passed with the rebuilt production
kernel.  The corresponding live rsync run sustained about 24 MiB/s on
an 816 MiB transfer instead of collapsing near 640 KiB/s.

The next allocation experiment raises the per-run byte cap from
`MAXBSIZE` to 1 MiB.  The requested block count is also bounded by the
current `uio`, block-group size, initialized-extent length, and the
32-bit logical-block limit; fragmented allocation may return a shorter
run.  Each filled data buffer is submitted immediately into one bounded
completion batch.  This avoids retaining a 1 MiB set of unsubmitted
busy buffers on a small buffer cache, while one final wait still checks
every completion before metadata is committed.  The synchronous-mount
regression performs writes immediately below, at, and above 1 MiB.  It
requires four sequence advances: one for each of the first two writes
and two for the final split write.  A kernel retaining the 64 KiB cap
requires roughly 48 transactions and fails this test.  The fixture first
consumes the short free tail in the initial 1 KiB block group with
non-sparse padding.  Otherwise that valid geometry adds one
fragmentation split and makes the result depend on formatter layout.
On 2026-10-04, the rebuilt production kernel passed this regression
with 1024, 2048, and 4096-byte filesystem blocks.  The focused
extent-tree and synchronous-operation regressions also passed.

The deferred-ordinary and transaction-age items land together.  Running
transactions retain modified metadata buffers as `B_BUSY`.  A pathname
lookup can therefore reach `bread()` before it opens a new journal
handle and cannot itself join the transaction.  Deferral without an
independent bounded-age commit path would turn that wait into a
deadlock.

The implementation gives each mount a native BSD timeout and private
task queue.  A new transaction arms a 100 ms maximum-age timeout.  The
timeout queues a process-context commit task carrying a transaction
sequence fence, so a delayed callback cannot commit a newer
transaction.  Ordinary completion releases the vnode-safe handoff
without committing; a following handle reserves credits and joins the
running transaction.  Credit exhaustion rotates it without draining
unrelated committed transactions.  Unmount cancels the timeout and
places a barrier behind the commit task before freeing journal state.
Every transaction detach also crosses a timeout barrier before a new
transaction may start.  This prevents an already-running callback for
the old transaction from observing and prematurely aging its successor.

An ordinary transaction with ordered regular-file data still commits
at operation completion.  Otherwise a following read could hold that
vnode while waiting for a busy extent block, while the age task waited
for the same vnode to flush ordered data.  Metadata-only transactions
are the safe first grouping scope.  Deferring ordered-data transactions
requires a later ownership design that removes this lock cycle.

The focused `group` regression requires four ordinary inode updates
and their final `fsync` to advance exactly one sequence.  It then
requires an otherwise inactive update to become durably committed by
age.  The accepted state is either logged but uncheckpointed, or clean
and one sequence ahead when journal policy requires an immediate
checkpoint.  It also checks that a pathname reader makes progress
through the age commit and that an ordered-data operation still commits
before a following read.  Kernel build and production regression
results passed with 1024, 2048, and 4096-byte filesystem blocks on
2026-10-04.

The live whole-filesystem rsync target was later identified as SATA,
not NVMe.  Its throughput briefly reached approximately 30 MB/s and
later fell to approximately 3 MB/s.  The 30 MB/s result is expected for
that setup and cannot measure NVMe scalability.  The run still does not
establish an optimization gain because it lacks a controlled baseline
on the same device; use the Phase 0 method before drawing one.

A separate NVMe run also started quickly and then entered intervals near
3 MB/s as caches filled.  This rules out the SATA transfer ceiling as
the sole explanation.  The burst-and-valley shape is consistent with
front-end cache absorption followed by synchronous journal durability
or checkpoint stalls.  Correlate the valleys with device utilization,
flush latency, and journal features before changing another barrier.

The mounted 4096-byte NVMe filesystem subsequently reported `Journal
features: (none)`.  The current safety policy therefore checkpoints
every transaction because it cannot persist revoke records.  Such a
transaction pays the precommit, commit, checkpoint, and journal-clear
durability flushes serially.  This directly explains why cached bursts
can collapse into low-throughput intervals even when the media is fast.

The upgrade enables the published JBD2 `REVOKE` incompat bit
when a writable mount opens a validated clean journal.  It writes and
flushes the journal superblock before updating runtime feature state,
then sets filesystem `RECOVER`.  Read-only mounts do not change the
journal.  A failed feature write or flush fails the mount before
`RECOVER` is set.  A focused production regression starts with the bit
clear, proves a read-only mount leaves it clear, proves a writable mount
makes it durable, and requires offline `e2fsck -fn` acceptance.  On
2026-10-04, that regression passed against the booted production kernel
with 1024, 2048, and 4096-byte filesystem blocks.  The retained-revoke
VMM test also passed at all three block sizes, including journal
wraparound and recovery.

The overlap slice adds an explicit frozen transaction state.
The committing transaction reaches it only after its immutable metadata
snapshot exists and all ordered-data vnode work has completed.  A new
transaction may then modify independent metadata while the older journal
records are written.  Access to an aliased metadata buffer remains
blocked until the older commit is durable and releases that buffer.
Block reuse also waits for the complete older commit or checkpoint, so
an old home write cannot overwrite a newly reused block.  The existing
handoff stress and VMM flush cuts provide the production concurrency and
durability coverage.  The production kernel build and the journal-core,
handoff, grouping, synchronous-operation, and focused VMM flush tests
passed on 2026-10-04.

The next ordered-data slice removes the completion wait from each newly
allocated write.  Every submitted physical data buffer now belongs to
the running transaction's embedded I/O batch.  Ordinary writes may
return while those buffers are pending, but the commit path drains the
complete batch before writing or exposing the journal commit.  An I/O
error therefore aborts the transaction and is reported by a later
`fsync`, synchronous operation, remount, or unmount.

Two conservative conflict barriers preserve last-writer and reuse
ordering.  A write to an already mapped block waits while either the
running or committing transaction still has allocation data in flight.
Freeing a block drains the running transaction's data before clearing
the allocation bitmap, so the allocator cannot reuse a block while an
older write can still target it.  This also replaces the temporary
allocation-run batch with one transaction-owned batch.  The affected
GENERIC.MP objects build with the production kernel flags.  Production
regression, error-injection, crash, and performance results are pending.

Regression coverage must combine concurrent create, write, rename,
unlink, truncate, and `fsync` operations with VMM power cuts and
injected I/O failures.

## Phase 6: CPU and Memory Scaling

Perform this work only after measurements show that CPU or allocation
cost has become significant.

The many-small-file rsync workload now supplies that evidence.  The
block allocator counted every free bit in a group before every
allocation, then restarted its free-bit search at zero.  The inode
allocator and both free paths repeated the same complete validation
scan.  With hundreds of thousands of files, these scans dominate even
when journal I/O is grouped.

The first CPU-scaling change records exact bitmap validation on the
journal metadata record.  A block or inode bitmap is still checksummed
and its free count is still reconstructed on its first use in every
transaction.  Later operations may skip that repeated scan only while
they use the same transaction-owned busy buffer.  The record and its
validation flags disappear at the transaction boundary.

Per-group block and inode cursors remember where the previous search
ended.  They are hints only: allocation still checks the selected bit,
wraps at the valid group boundary, and returns an error when descriptor
counts promise space that the bitmap does not contain.  Freeing an item
moves the corresponding cursor to the newly available position.  Both
cursor tables use checked sizing and 32-bit entries.

On 2026-10-05, the production kernel passed the block-bitmap,
inode-bitmap, counter-corruption, directory-churn, and consistency
regressions with this implementation.

The follow-up comparison still measured FFSv2 at approximately three
times ext4fs throughput.  A second hot-path audit found that every
journal handle preallocated and then freed an unused transaction object
even when it joined the current running transaction.  Transaction
allocation is now lazy: the begin path drops the journal lock, allocates
a candidate only when no running transaction exists, then rechecks the
state.  A racing caller may safely join the transaction created while
the lock was dropped and release its unused candidate.

The production kernel passed the journal-core, grouping, handoff, and
directory-churn regressions with lazy transaction allocation on
2026-10-05.

The next CPU-scaling change adds native BSD red-black-tree indexes for
transaction metadata by filesystem block and buffer identity, and for
revokes by filesystem block.  Repeated metadata acquisition, dirtying,
bitmap validation, duplicate detection, and revoke checks no longer
scan the complete transaction.  Existing tail queues remain the sole
descriptor, revoke, checkpoint, and teardown order.  Every insertion
and removal updates its index and queue together, and transaction
teardown asserts that all indexes are empty.

The retained-revoke VMM fixture now uses `O_SYNC` writes to create two
durable but uncheckpointed transactions.  It no longer depends on the
100 ms transaction-age timer firing between its growth and truncation
operations.  The 1024-byte journal-wrap case passed with indexed lookup
on 2026-10-05.

The bounded-scratch implementation allocates two block-sized workspaces
when a journal is initialized instead of allocating and freeing them for
every commit.  One workspace constructs descriptors, revoke records,
and the commit block; the other constructs escaped metadata payloads.
Queued journal buffers copy the complete workspace contents before I/O,
so reuse cannot modify an outstanding write.  The serialized committer
owns both workspaces explicitly, and journal teardown asserts that they
have been released before freeing them.  The production GENERIC.MP
object builds with this implementation.  Booted-kernel operation,
failure-injection, and crash tests passed on 2026-10-07.  Live rsync
testing then remained very slow on a many-small-file tree and showed
visible waits between update bursts.  Per-commit scratch allocation was
therefore not the dominant cost for that workload.

The follow-up source audit found the next structural wait.  Pathname
lookup and pre-transaction directory validation issued plain `bread()`
calls before a journal handle existed.  A preceding grouped operation
could own the same directory block or external extent node as a busy
running-transaction buffer.  The lookup then waited for the 100 ms age
commit even though the buffer was stable between handles.  The
ordered-vnode list currently has no callers, so deduplicating that list
cannot improve this workload.

The read-only transaction-aware path now checks the running transaction
under the journal lock.  It waits while an active handle may modify
metadata, then returns a non-cacheable anonymous copy of a stable busy
buffer.  The copy can be released with `brelse()` but can never become a
home-block write.  Allocation occurs outside the journal lock and the
metadata record is revalidated before copying.  A missing running record
falls back to `bread()`; committing records retain their existing wait
until their live buffers are released.

Extent traversal, directory lookup and validation, inode loading,
truncate preflight, and runtime orphan and extended-attribute validation
use this path before opening a write handle.  Direct journal-less
mutation and ordinary file-data reads remain unchanged.  The affected
production GENERIC.MP objects build successfully.  A focused regression
alternates creates and negative lookups in one dirty directory and
rejects the former one-age-wait-per-lookup behavior.  Booted-kernel and
full regression results are pending.

- [x] Reuse bounded per-journal scratch storage rather than allocating
      it for every commit.
- [x] Add lookup structures for transaction metadata and revokes while
      retaining lists for deterministic journal order.
- [ ] Deduplicate ordered-vnode dependencies without repeated linear
      scans.
- [ ] Avoid copying or checksumming an unchanged metadata snapshot more
      than once.
- [ ] Coalesce repeated inode, group-descriptor, and superblock changes
      within one transaction.
- [x] Validate each block and inode bitmap once per transaction-owned
      metadata record rather than once per allocation or free.
- [x] Continue block and inode searches from checked per-group cursors
      instead of restarting every search at bit zero.
- [x] Allocate candidate transaction state only at a transaction
      boundary, not once for every joining journal handle.
- [x] Let read-only pre-handle metadata access copy stable running
      transaction buffers instead of waiting for their age commit.
- [x] Build the production kernel and pass bitmap, inode-bitmap,
      counter-corruption, directory-churn, and full consistency tests.
- [x] Build and test the lazy transaction-allocation change with
      grouping, handoff, directory-churn, and journal-core regressions.
- [ ] Build and test indexed transaction lookup with grouping, handoff,
      bitmap, directory-churn, truncate, reuse, and revoke regressions.
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
