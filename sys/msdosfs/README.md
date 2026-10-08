# IABSD 0.1

## MS-DOS filesystem driver hardening plan

### Legalize

Copyright (c) 2025,2026 kmx.io <contact@kmx.io>

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the
above copyright notice and this permission notice appear in all
copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

### Scope and assumptions

This plan covers the in-kernel MS-DOS/FAT filesystem implementation in
`sys/msdosfs`.  It records the issues found during the source audit and the
regression coverage needed for each fix.

### Progress

Checked sub-items are implemented in the working tree.  A finding remains
open until its implementation, regression coverage, and kernel verification
are all complete.

- [ ] Finding 1: validate file handles and directory-entry locations
  - [x] Validate file-handle length and initialize handle padding.
  - [x] Validate cluster range, entry alignment, root bounds, and buffer
    extent before internalizing a directory entry.
  - [x] Share the directory-entry location checks with ordinary lookup I/O.
  - [x] Add stale-handle identity and generation checking.
  - [x] Add malformed, stale, and reused-slot file-handle regressions using
    `getfh(2)` and `fhstat(2)`.
- [ ] Finding 2: give `deget()` a complete error unwind
  - [x] Clear the output pointer before doing any work.
  - [x] Route post-hash failures through one cleanup path.
  - [x] Balance the device-vnode reference on success and failure.
  - [x] Add repeated corrupt-`pcbmap()` failure and recovery coverage.
  - [ ] Add a device-level injected `bread()` failure regression.
- [ ] Finding 3: correct FAT12/16 fixed-root sizing
  - [x] Use `DEV_BSIZE` units for the root denode size and entry bounds.
  - [x] Add a full-root FAT16 test with 2048-byte logical sectors.
- [ ] Finding 4: bound FAT-chain and parent-directory traversal
  - [x] Validate every FAT link before using it as a cluster.
  - [x] Bound FAT walks and detect cycles without unbounded memory.
  - [x] Validate the FAT32 root chain before publishing the mount.
  - [x] Propagate FAT mapping failures through buffered file reads.
  - [x] Bound `..` walks and detect self- and two-directory cycles.
  - [x] Add self-cycle, multi-cluster cycle, out-of-range link, root-cycle,
    and parent-cycle regressions.
- [ ] Finding 5: validate BPB layout with checked-width arithmetic
  - [x] Compute and narrow the layout through checked 64-bit intermediates.
  - [x] Validate FAT count and capacity, active FAT, root cluster, fixed-root
    alignment, boot signature, and region ordering.
  - [x] Reject a declared volume larger than the backing partition.
  - [x] Add malformed BPB and truncated-declaration regressions.
- [ ] Finding 6: bound FAT32 FSInfo I/O
  - [x] Limit each FSInfo buffer to one validated logical sector.
  - [x] Validate standard signatures without reading the legacy second-sector
    signature field.
  - [x] Add invalid-location, invalid-signature, 16 KiB, and 32 KiB sector
    regressions.
- [ ] Finding 7: validate DOS date fields
  - [x] Validate month and day, including the FAT leap-year rule.
  - [x] Map invalid dates deterministically to the Unix epoch.
  - [x] Add invalid month/day and valid/invalid leap-day regressions.
- [ ] Finding 8: make allocation accounting transactional
  - [x] Roll back uncommitted reservations after FAT-linking failure.
  - [x] Keep any unverified rollback suffix conservatively allocated and mark
    the allocator inconsistent.
  - [x] Reject repeated frees, bound free-chain traversal, and propagate
    errors through truncation.
  - [x] Add self- and two-cluster free-cycle accounting regressions.
  - [ ] Add a device-level injected FAT read failure regression.
- [x] Build every changed driver source file with the kernel's `-Werror` and
  `DIAGNOSTIC` options.
- [x] Build `GENERIC` and `GENERIC.MP` kernels containing every changed driver
  source file.
- [x] Correct automatic `newfs_msdos` FAT sizing and test that every data
  cluster has a representable FAT entry.
- [ ] Pass normal FAT12, FAT16, and FAT32 regression suites.
- [ ] Pass the malformed-image suite in a disposable VM.

The malformed-image cases assume that an attacker can cause a crafted FAT
filesystem to be mounted, normally through an administrator or an automounter.
The file-handle case is also reachable by an authorized NFS client when the
filesystem is exported.  Tests that exercise corrupt allocation chains should
run in a disposable VM with strict timeouts; a buggy kernel may otherwise hang
or panic.  Kernel address and undefined-behavior sanitizers should be enabled
when available.

### Findings

#### 1. Validate file-handle directory locations before reading a direntry

Severity: high for an NFS-exported filesystem.

`msdosfs_fhtovp()` in `sys/msdosfs/msdosfs_vfsops.c` passes the directory
cluster and offset from a file handle to `deget()`.  `readep()` in
`sys/msdosfs/msdosfs_lookup.c` does not establish that the offset is aligned or
that the complete `struct direntry` lies within the returned buffer.  A forged
file handle can therefore make `DE_INTERNALIZE` read beyond the buffer, and a
later metadata update can make `DE_EXTERNALIZE` write beyond it.  File-handle
generation checking is currently disabled in `sys/msdosfs/denode.h`.

Required changes:

- Validate the file-handle length before consuming any fields.
- Reject directory clusters outside the mounted volume's valid cluster range.
- Require a directory-entry-aligned offset.
- Check `offset + sizeof(struct direntry)` against the actual buffer extent,
  including the fixed FAT12/16 root-directory bounds.
- Add an identity or generation check so a stale handle cannot silently name a
  reused directory slot.
- Use one checked helper for file-handle lookup and ordinary directory-entry
  lookup so the bounds rules cannot diverge.

Regression tests:

- Send short and oversized file handles through the NFS file-handle path.
- Exercise unaligned offsets, end-of-buffer offsets, offsets beyond the fixed
  root directory, invalid clusters, and stale generations.
- Repeat against read-only and read-write exports under a kernel memory
  sanitizer.
- Require a clean stale-handle or invalid-argument error and no kernel memory
  access outside the directory buffer.

#### 2. Give `deget()` a complete error unwind

Severity: high.

`deget()` in `sys/msdosfs/msdosfs_denode.c` inserts a new denode in the hash
before all initialization can succeed.  Errors from `readep()` and `pcbmap()`
then return directly, leaving a locked, referenced, hashed, partially
initialized vnode.  Some callers, including `doscheckpath()` in
`sys/msdosfs/msdosfs_lookup.c`, also assume that a failed `deget()` clears its
output pointer and can release the wrong denode when that contract is broken.

Required changes:

- Set `*depp = NULL` on entry and leave it null on every failure.
- Replace post-hash direct returns with a single unwind path.
- On failure, remove the denode from its hash chain, undo only references that
  were acquired, unlock it, and dispose of the vnode safely.
- Account for the fact that `de_devvp` is not referenced until successful
  initialization; cleanup must not release an unowned reference.
- Document the output-pointer and lock-state contract for callers.

Regression tests:

- Inject failures into the directory `bread()` and final `pcbmap()` operations.
- Repeat the failing lookup enough times to detect a retained lock, hash entry,
  vnode, or reference.
- Verify that a later valid lookup succeeds and that vnode and denode counts
  return to their baseline.

#### 3. Correct the FAT12/16 fixed-root size calculation

Severity: high because it can turn a full root directory into file-data
corruption.

`pm_rootdirsize` is stored in `DEV_BSIZE` blocks by the mount code, while
`deget()` multiplies it by `pm_BytesPerSec`.  When the logical sector is larger
than 512 bytes, the fixed root directory is reported too large.  For example,
a 16 KiB root on a filesystem with 2048-byte sectors is exposed as 64 KiB.
Lookup or creation at the end of a full root can then continue into the data
area instead of returning `ENOSPC`.

Required changes:

- Compute the root denode size as `pm_rootdirsize * DEV_BSIZE`, or replace the
  mixed-unit field with an explicitly byte-sized value.
- Audit all other `pm_rootdirsize` users and name stored units in comments or
  field names.

Regression tests:

- Create a FAT16 image with 2048-byte logical sectors and a completely full
  fixed root directory.
- Confirm that one more create returns `ENOSPC`.
- Hash the first data cluster before and after the failed create and require it
  to remain unchanged.
- Cover other accepted non-512-byte sector sizes as well.

#### 4. Bound all FAT and parent-directory traversals

Severity: high.

`pcbmap()` in `sys/msdosfs/msdosfs_fat.c` follows FAT links without validating
each non-special cluster against the volume's maximum cluster and without a
volume-sized traversal limit.  A self-loop can make the `CLUST_END` scan from
`deget()` run for nearly `UINT32_MAX` iterations.  `doscheckpath()` similarly
follows `..` entries without a cycle bound, so a self-cycle or two-directory
cycle can wedge a cross-directory rename.

Required changes:

- Validate every ordinary FAT value before using it as a cluster number.
- Limit a chain walk to no more than the number of data clusters on the volume.
- Return `EIO` for an invalid link, a cycle, or a chain longer than the volume.
- Apply an equivalent volume-derived bound while following directory parents.
- Keep the checks in shared traversal primitives where possible so reads,
  truncation, allocation, lookup, and rename get the same protection.

Regression tests:

- Test a FAT32 root cluster that points to itself.
- Test self-cycles and multi-cluster cycles in subdirectories and regular
  files, plus links to cluster numbers outside the valid range.
- Construct self-referential and two-directory `..` cycles, then attempt a
  cross-directory rename.
- Put hard timeouts around every operation and require `EIO` or `EINVAL`, not a
  hang, livelock, panic, or traversal outside the image.

#### 5. Strengthen BPB validation and use checked-width layout arithmetic

Severity: medium.

Mount-time BPB validation in `sys/msdosfs/msdosfs_vfsops.c` does not fully
establish that all metadata regions fit within the declared volume and backing
device.  It also needs explicit checks for a nonzero FAT count, the selected
FAT index, FAT32 root cluster, and FSInfo location.  Several layout operations
use 32-bit multiplication, addition, and subtraction before the relationships
between regions have been proven, allowing wraparound or underflow.

Required changes:

- Decode untrusted BPB fields into 64-bit temporaries.
- Check every multiplication and addition before narrowing to an on-disk or
  in-memory field width.
- Require the reserved area, every FAT, FAT12/16 root directory, first data
  cluster, and declared total sector count to be monotonic and within the
  volume.
- Compare the declared volume length with the actual block-device or vnode
  length.
- Require at least one FAT and ensure the active FAT index is below the FAT
  count.
- For FAT32, validate the root cluster and require the FSInfo sector to be in
  the reserved region before reading it.
- Validate the BPB and boot-record signatures that the driver relies on.

Regression tests:

- Reject images whose first data cluster is at or beyond the declared volume.
- Reject zero FATs and an active FAT index at or above the FAT count.
- Reject an invalid FAT32 root cluster and an FSInfo sector outside the
  reserved region.
- Reject truncated backing devices and geometries designed to overflow each
  layout multiplication, addition, or subtraction.
- Verify that every failure leaves the device cleanly unmounted and releases
  all buffers and vnodes.

#### 6. Bound the FAT32 FSInfo I/O size

Severity: medium to low.

The FSInfo buffer size in `sys/msdosfs/msdosfsmount.h` grows as
`1024 << (pm_BlkPerSec >> 2)`.  With a valid 32 KiB logical sector,
`pm_BlkPerSec` is 64 and this requests a 64 MiB buffer for a small metadata
record.  Crafted images can therefore cause disproportionate kernel-memory
pressure or buffer-allocation failures during mount.

Required changes:

- Read no more than the validated logical-sector size needed to contain the
  standard FSInfo fields.
- Check field offsets against that buffer before decoding them.
- Treat invalid FSInfo signatures or values as unknown allocation hints, not
  as authority over the allocator.

Regression tests:

- Mount FAT32 images with accepted 16 KiB and 32 KiB logical sectors.
- Instrument or otherwise assert that the FSInfo read is bounded by one
  logical sector.
- Cover truncated and signature-invalid FSInfo sectors and verify clean
  fallback behavior.

#### 7. Validate DOS date fields before indexing the month table

Severity: medium to low.

`dos2unixtime()` in `sys/msdosfs/msdosfs_conv.c` special-cases month zero but
does not reject encoded months 13 through 15.  Those values index beyond the
12-element month table while servicing ordinary metadata operations such as
`stat()`.

Required changes:

- Validate the decoded month as 1 through 12 before indexing the table.
- Validate the day against the selected month's range, including leap years.
- Define one deterministic policy for invalid on-disk timestamps: return a
  safe sentinel timestamp or report an error where the call interface permits.

Regression tests:

- Exercise months 0 and 13 through 15, day zero, excessive days, and leap-day
  edge cases through `stat()` and directory reads.
- Require deterministic results with no out-of-bounds access under a kernel
  memory sanitizer.

#### 8. Make allocation accounting transactional

Severity: medium to low.

`chainalloc()` in `sys/msdosfs/msdosfs_fat.c` marks clusters allocated in the
in-memory bitmap before the FAT chain update is known to have succeeded and
does not roll that state back on failure.  `freeclusterchain()` does not reject
an already-free cluster, so a corrupt cyclic chain can free the same cluster
twice and inflate the free-cluster count.  `detrunc()` in
`sys/msdosfs/msdosfs_denode.c` currently discards errors from the free path.

Required changes:

- Roll back bitmap bits, free-cluster counts, and allocation hints if FAT
  linking fails.
- Before freeing, require the allocation bitmap to show that each cluster is
  allocated; an already-free cluster is filesystem corruption.
- Apply the same traversal bounds used for reads to allocation and free walks.
- Propagate free-chain failures to callers, including truncation, wherever the
  vnode operation can report them.
- Ensure partially completed on-disk changes leave accounting conservative and
  cause the filesystem to be marked inconsistent when recovery is required.

Regression tests:

- Inject a failure after reserving clusters but before or during FAT linking;
  verify complete in-memory rollback.
- Free a self-cycle and a multi-cluster cycle; verify that no cluster is
  counted twice and that an error is returned.
- Compare the allocation bitmap, free-cluster count, and next-free hint before
  and after each failed operation.
- Verify that truncation and removal do not report success after an allocation
  accounting failure.

### Implementation order

1. Add reusable cluster, directory-offset, and traversal-bound validation.
2. Repair `deget()` ownership and error unwinding before expanding error paths.
3. Apply bounded traversal to lookup, mapping, rename, truncation, allocation,
   and free operations.
4. Harden file-handle decoding and add stale-handle identity checking.
5. Convert mount geometry calculations to checked 64-bit arithmetic and
   enforce complete BPB region validation.
6. Fix the FAT12/16 root size, FSInfo read size, and DOS date validation.
7. Make allocation changes transactional and propagate their failures.
8. Add each regression with its corresponding fix; do not wait until the end
   to assemble one large test change.

## Test layout

The current `regress/sys/fileops/msdos16` and `msdos32` suites cover normal
file operations but not malformed images or failure injection.  Add a separate
corrupt-image suite so dangerous cases are unmistakable and easy to run only
inside a disposable VM.  Keep generated images small and deterministic, and
provide helpers that patch BPB, FAT, directory-entry, and timestamp fields
without requiring privileged host mounts.

Each test must:

- use a timeout for mount and every filesystem operation;
- unmount and detach its image on both success and failure;
- distinguish a rejected mount from a runtime `EIO` where that distinction is
  part of the expected behavior;
- verify backing-image bytes when the scenario could otherwise corrupt data;
- record vnode, buffer, and allocation-accounting baselines for fault-injection
  cases; and
- be runnable under kernel memory and undefined-behavior sanitizers.

### Completion criteria

- All malformed geometries are rejected before any derived offset is used.
- Invalid FAT input fails with `EINVAL` or `EIO`; it never faults or panics the
  kernel.
- All FAT and parent-chain walks terminate in time proportional to the number
  of data clusters.
- No directory entry can be internalized or externalized outside its buffer.
- Every failed `deget()` leaves no locked, referenced, or hashed partial vnode.
- FAT12/16 fixed-root capacity is correct for every supported sector size.
- FSInfo reads are bounded by the logical sector size.
- Invalid timestamps cannot index outside conversion tables.
- Failed allocation or free operations keep in-memory accounting consistent or
  explicitly mark the filesystem inconsistent.
- Normal FAT12, FAT16, and FAT32 regression suites continue to pass.
