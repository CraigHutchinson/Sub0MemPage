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
| M2 | Bounded cached-slot and explicit-destination state machines; deterministic fake backend | Generation, admission, exact completion, cancellation and final-consumer lifetime gates in [transfer contract](transfer-contract.md) |
| M3 | Local-file host transport on Windows/Linux; portable worker baseline for macOS | Real-file parity, short reads, drain, bounded resources; record each platform independently |
| M4 | Optional staged CUDA transport, consumed by TieredCache T2 and Llm S2 | Host/device endpoint checks, bounded staging, delayed consumer events, errors and device parity |
| M5 | Optional Intel USM adapter | Allocation/context validation, explicit copy, completion/final-use ownership; per-platform capability qualification |
| M6 | Optional native-Linux NVIDIA cuFile/GDS | [NVIDIA gates](nvidia-gds.md), staged prerequisite, observed route, exact counts, failure and fallback parity |
| M7 | Policy and overlap tuning | Whole-stack measurements against existing consumers; no physical-residency inference |

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

## Checkpoint: M0/M1 complete

The reviewed plan and optional inventory are implemented. [Validation](validation/2026-09-22/README.md)
records offline Windows/Linux tests, sanitizer coverage, the actual Intel report and remaining limits.
M2 is next; M3–M7 remain unimplemented. The [shared plan](../../Sub0Llm/docs/STORAGE_STACK_PLAN.md) defines upper-layer acceptance and feedback.

## Checkpoint: M2 draft (2026-09-25), handover for the next session

**Done.** Both state machines are implemented over one backend seam, header-only, with no hot-path
allocation: `slot_pool.hpp` (cached slots, CLOCK admission, leases, tickets) and `transfer_set.hpp`
(explicit-destination claims). They are proven against a deterministic fake backend on MSVC and GCC,
under ASan+UBSan and TSan, with 11/11 mutants killed. Evidence and the defects caught are in
[validation/2026-09-25](validation/2026-09-25/README.md). The development loop is codified in
[DEVELOPMENT_WORKFLOW.md](DEVELOPMENT_WORKFLOW.md) (`scripts/dev.py`, `docs/perf/kpi_gates.json`).

**Not done, in suggested order for a session on a dedicated (uncontended) machine:**

1. Run `dev.py bench --aa` to measure the noise floor. Replace the provisional G-PERF 1.05 threshold
   with a measured one and commit the first `perf_history.jsonl` rows. `dev.py bench` has not yet run
   end to end: its contention gate, A/B path and report writer are untested.
2. Re-measure the contended `try_resolve` lead properly (one pool mutex serialises hit-path readers).
   Decide whether hit-path scaling matters before M3's consumers exist. Sub0Llm decode is the
   motivating case, with up to 10 resolving threads.
3. Add ARM coverage: a QEMU user-mode stage in `dev.py` (`g++-aarch64-linux-gnu` + `qemu-user` in WSL;
   run `test` and `sanitize`, since ASan works under qemu-user with caveats and TSan usually does not).
   It checks correctness only, never perf. Record which sanitizers qualify under emulation.
4. Wire MSVC `/fsanitize=address`, or record why not (the ASan runtime DLL must be on the test PATH).
5. Add a CI TSan job alongside the existing ASan job.
6. Start M3: the local-file host transport (Windows overlapped I/O / IOCP, Linux io_uring or a
   worker pool), real-file parity against a synchronous read oracle, and the canary/EOF/unaligned cases
   in transfer-contract.md.
7. Decide `feature/usm-plan-groundwork` (Codex). It forked before main's Intel rework, and its
   `tools/intel_usm` staged-copy probe was never merged; main carries `tools/intel` instead. It is
   pushed as-is for its owner to reconcile. Merging it wholesale would regress main.

Open design questions carried forward:
- R18's cross-instance guard (one allocation in both modes) is unenforced.
- Slot storage alignment is not validated; M3 unbuffered I/O needs sector alignment.
- `wait(ticket)` and `Claim::wait()` are asymmetric shapes.


## Checkpoint: dev loop + M3 slice 1 (2026-09-25, cloud session)

Worked in a shared Linux container (GCC 13, clang 18 + libc++, qemu-user 8.2), not the dedicated machine.

| M2 handover item | State |
|---|---|
| 1. `bench --aa` noise floor | Script path now runs end to end (contention gate, `--aa`, `--baseline`, report). Only ADVISORY rows were taken here, and none were committed. The G-PERF 1.05 threshold stays provisional until a dedicated-machine run. |
| 2. Contended `try_resolve` lead | Not re-measured; container timings are not evidence. Still open. |
| 3. ARM coverage | `dev.py arm` and the `arm-qemu` CI job: plain build and tests pass under qemu-aarch64. ASan (interceptor segfault) and TSan (unsupported VMA range) do not run under qemu-user and are recorded as SKIP. |
| 4. MSVC ASan | `ci-msvc-asan` preset and `msvc-asan` CI job. Verified only by CI (PR #1). |
| 5. CI TSan | `tsan` job + `ci-tsan` preset. The clang job now builds against libc++; it had been red since M2. |
| 6. M3 | Slice 1 landed: `local_file_backend.hpp`, a portable worker-pool backend (POSIX `pread`; Windows positional overlapped `ReadFile` with a per-worker event). Real-file parity against an `ifstream` oracle, canaries, EOF clipping, truncated-after-registration, queue-full and shutdown cases: 76 checks, plus 2 mutants (13/13 killed). io_uring/IOCP remain later optimizations behind the same seam. |
| 7. `feature/usm-plan-groundwork` | Untouched; still for its owner. |

Next for M3: the native async backends (io_uring, IOCP) and their measurements on the dedicated machine; a macOS
CI run of the worker backend (covered by `build-macos`); an exported `sub0mempage::testing` target for
the fake backend (Sub0TieredCache T0 feedback).
