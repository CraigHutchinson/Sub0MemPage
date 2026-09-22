# Implementation plan

Reviewed 2026-09-22. Implementation is authorized; the paging API remains a draft until its gates pass.

## Review findings and decisions

1. **Raw and decoded storage were conflated.** The consumer trace proposed filling encoded file bytes
   into `ExpertCache::pool_`, which holds decoded floats. Keep those allocations distinct; transformation
   and decoded-cache admission remain in Sub0Llm. Budget raw staging and decoded storage separately.
2. **Prepared copy is not a residency lease.** Intel's experimental prepare/release pair optimizes
   repeated explicit copies. It neither completes a copy nor grants kernel access to an arbitrary mmap
   pointer. Include the capability spike as an optional diagnostic first, without a SYCL core dependency.
3. **Completion and ownership were conflated.** A ticket records completion, not ownership. Only a
   move-only lease prevents reuse. `wait` must not hand out an unpinned readable pointer; follow with
   `try_resolve`/`resolve`, which can miss again after intervening eviction. GPU users retain the lease
   until their last event completes; `release` never waits for a GPU.
4. **The no-fault claim was too strong.** No explicit synchronous I/O on a hot-path call is enforceable;
   a portable promise of no OS page faults on pageable metadata or caller buffers is not. Logical slot
   pinning prevents reuse, not OS paging. Registration preallocates and touches metadata; physical locking
   needs a separate, capability-qualified extension and budget.
5. **Admission and overlap need precise failure rules.** Declared requests can exhaust slots or the
   bounded queue. Fail explicitly. Normalize ranges to fixed slot-sized source chunks (last chunk clipped
   at EOF); overlapping live requests share a fill per chunk. Multi-chunk results are segmented leases,
   never a falsely contiguous pointer. Reads repeated after eviction are allowed.
6. **Look-ahead was overstated.** Layer L+1 routing normally depends on completing layer L. Overlap
   within the already-known routed expert batch first. Future-layer guesses are speculative, not declared.
7. **Historical timing is motivation, not a current baseline.** Re-measure the actual consumer before
   integrating: Sub0Llm already has pipelined I/O and newer decoded-cache paths. Do not replace those
   blindly or claim the old B21 diagnosis still describes current performance.

## Delivery sequence and acceptance gates

| Package | Deliverable and consumer | Gate |
|---|---|---|
| M0 | Correct contracts, consumer trace and Intel scope in this repository | No raw/decoded aliasing; no physical-residency or GPU-access inference |
| M1 | Optional `tools/intel` capability executable; offline CLI tests used by that executable | Default build has no SYCL dependency; strict Intel Level Zero selection; six aspects, actual identity and explicit untested states; Windows/Linux portable tests |
| M2 | Bounded slot state machine, consumed by a deterministic fake-I/O scheduler fixture | Empty/filling/ready/failed states; generation-checked tickets; move-only leases; duplicate/overlap coalescing; rollback on batch failure; exhaustion and no hot-path allocations |
| M3 | Async local-file workers feeding M2: Windows overlapped I/O and Linux backend | Real files and short reads; errors and cancellation; concurrent overlap; complete draining before unregister/destruction; both OSes tested |
| M4 | Sub0Llm raw-byte adapter and Sub0Firn local-tier example | Encoded-byte parity with direct file reads; decoded outputs unchanged; explicit extra raw-pool budget; benchmark against current consumer |
| M5 | Optional caller-owned USM transfer adapter | Context/allocation validation; host/device ownership transitions; completion-before-reuse tests; async errors; independent Windows/Linux capability results |
| M6 | Policy and overlap tuning, then optional mmap hints | Measured hit/miss and unconsumed-hint counts; bounded memory/queues; uncontended interleaved baselines; no unsupported hard mmap residency claim |

M1 begins implementation without pretending the paging scheduler exists. It only inventories aspects;
allocation tests, prepared-copy execution, direct Level Zero extension inventory, mapping import and
performance are separate gates. No device work is registered with ordinary CTest.

## M2 contract decisions to implement and test before a public API is frozen

- Validate source extent, nonzero slot size/capacity, checked arithmetic and destination span size once.
  Sources are immutable for registration lifetime. Destination memory, backend and region outlive all
  tickets, leases and fills. Reject teardown while resources are outstanding, or explicitly drain off
  the hot path. Never hide draining in `release`.
- Reserve metadata, queue entries and result storage at registration; batch results use caller-provided
  storage. Define maximum batch/chunk count. `try_resolve` atomically pins the whole batch or pins none.
  `resolve` must report batch-too-large rather than deadlock waiting for its own pinned slots.
- A ticket keeps its completion generation, not a slot pin. Keep completion records until ticket
  destruction; discarded tickets relinquish only that record, not the in-flight operation. Queue or
  ticket-table exhaustion is visible. A deadline does not cancel or free a destination still in use.
- Backend completion publishes bytes before Ready. A slot cannot leave Filling while a worker can still
  write it. Errors never publish partial data as resident. Stale completions cannot overwrite reused state.
- Initial policy is deterministic and bounded; speculative admission tuning waits for measurements.
  Do not claim an unimplemented frequency sketch. Pinned and filling slots are never eviction candidates.
- Statistics distinguish admitted, declined, in-flight, ready, pinned, failed and consumed; count an
  unconsumed hint at eviction/discard, not immediately when submitted. Advisory callbacks must be queued
  outside locks, with bounded overflow accounting; they are not a cache-validity mechanism.
- Thread-safe does not mean wait-free. Document synchronization bounds and test concurrent access with
  race tooling where available. Never hold a state lock while blocking on backend I/O.

## Intel boundary

Read [intel-usm.md](intel-usm.md) before designing M5. Ordinary file mappings remain CPU sources.
Caller-owned host/shared/device USM are distinct allocation kinds; registration does not turn one into
another. Device-only slots cannot be CPU-filled by a file worker. A staging pool and explicit copy may
be necessary, and both allocations count in deployment memory planning. Prepared-copy registrations
must not overlap; keep them out of per-request leases and retire them only after all copies finish.

## Validation and rollout

Commit M0, then each working package separately. Keep experimental targets off by default. Run portable
unit tests with warnings enabled, Windows and Linux builds, and sanitizer checks where available. Record
unavailable platforms instead of calling them passed. The first backend is not production-ready until
M2/M3 budget, lifetime, concurrency, failure and real-file gates pass. No engine integration or performance
claim is part of M1. Coordinate hardware runs with Sub0Llm's active work log.
