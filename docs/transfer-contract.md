# Transfer and ownership contract

Design revision S1, 2026-09-25. This is the proposed contract for M2 onward, not an implemented API.
It refines the original host-slot design for CPU, CUDA and Intel USM without exposing vendor types in
portable headers. See [implementation-plan.md](implementation-plan.md) for delivery gates and
[the stack plan](../../Sub0Llm/docs/STORAGE_STACK_PLAN.md) for end-to-end acceptance.
Workspace links between projects assume sibling checkouts; build dependencies use pinned revisions.

## Responsibility and dependency direction

Sub0MemPage owns byte movement, bounded transfer scheduling, completion/error propagation and the
lifetime of registered memory while transfers use it. Its optional raw-byte cache owns byte-range
admission/reuse, not row identity or decoded representations. It depends on OS/vendor backends, never
Sub0TieredCache or Sub0Llm. No table IDs, model shapes, tokenizer logic, codecs, version-tag interpretation,
remote HTTP clients or weight-file parsers enter this core.

Sub0TieredCache is a caller: it supplies immutable source generations, byte extents, destination storage
and representation policy. A standalone lower-level caller can do the same without linking the cache.

## Two uses of one transfer scheduler

| Use | Who chooses reuse/admission? | Owner and proof of validity |
|---|---|---|
| Raw-byte cached pool (existing resolve/prefetch design) | Sub0MemPage chooses unpinned slots | Caller allocates; a lower-level lease pins a completed slot |
| Transfer into an explicitly reserved destination | Caller chooses destination and replacement; no second lower cache | Caller allocates/reserves; a transfer claim prevents writing/freeing/reusing the range until terminal completion |

The explicit-destination form is needed for Sub0TieredCache's transformed output pools and contiguous
rows. Both forms share registration, bounded requests and completion handling; do not build two I/O
engines. A destination registered for one mode cannot simultaneously be managed by the other.
An allocation can change mode only after all claims/leases drain. Registration is not a resident-data
claim. M2 proves this ownership distinction before fixing public C++ signatures.

## Endpoint and region registration

A descriptor records extent, allocation kind, actual device/context identity, legal access agents and
required alignment. Initial kinds: host pageable, host pinned, CUDA device, CUDA managed, SYCL host,
shared and device. Backend capability checks accept or reject each kind; the list is not a promise
that every backend implements every pairing. Never represent device bytes as a CPU-readable span.
Vendor stream/context wrappers live in optional backend headers, with explicit borrowed lifetimes.

Registration validates overflow, accessible size, immutable source identity, destination overlap and
backend compatibility. Caller-owned allocations, file handles and contexts outlive their registrations
and outstanding operations. Preallocate queues, completion records, events and caller-visible results;
pre-touch host metadata. SDK registration, initialization, file opening and teardown are administrative
operations and may block. The hot-path nonblocking rule applies after successful registration.

Select backend and fallback policy once per registration/session from measured capabilities. Compile out
unbuilt backends; runtime device/filesystem qualification is unavoidable and does not belong in every
inner loop. No fresh heap allocation, file opening or driver registration on submission/acquisition/release.
SDK calls without a qualified bounded host-submission contract execute on a worker, not the caller.

## Completion, byte validity and reuse

A transfer moves through Reserved -> Submitted -> Completed or Failed, then Retired. A cached slot adds
Ready and Leased states. A generation check rejects stale completion or lease handles. A submit success
means accepted, not ready. Ready requires the exact requested byte count and all needed transfer/event
completion; short I/O and async errors never expose partial bytes as a complete range.

A ticket owns completion bookkeeping, not cached-byte residency. Dropping it does not cancel in-flight
writes or permit destination reuse. A timeout leaves the transfer/claim live. Cancellation is a request;
only terminal completion proves that a writer stopped. Unregister/close fails busy or explicitly drains
on an administrative path. Ordinary lease release never synchronizes a device or deregisters a stream.

For GPU consumers, the backend adapter orders compute after successful input readiness and retains
input/output claims until the last consuming event completes. A CPU-visible successful result is not
permission to free a device allocation still read by a kernel. Initially the completion worker validates
I/O status/byte count before publishing readiness and allowing dependent work to submit; a GPU-gated
error-aware pipeline is a later optimization, not a blind same-stream kernel after a failed read.
Cross-stream dependencies must be explicit. Shared physical DRAM does not remove this obligation for USM.

A caller may retain a lease and poll its own compute event, or transfer ownership of that lease to a
bounded retirement queue. The latter rejects queue exhaustion without losing ownership. No detached
callback may release a naked pointer whose registration has already died.

## Chunking, coalescing and contiguity

The cached-byte facade normalizes requests to slot-sized source chunks; the last is clipped at EOF.
Concurrent requests share a live fill per source-generation/chunk and compatible destination domain.
Two different device destinations are two transfers even when source bytes match. Reading the same
source again after eviction/failure is allowed. A shared staging read may feed several destinations only
while a source lease covers all copies. Do not promise one DMA across distinct devices or contexts.

A segmented result must be exposed as segments. Callers requiring a contiguous row/plane either reserve
contiguous destination space for a gathered transfer or use a bounded gather buffer. File alignment
expansion must stay inside a valid source extent and reserved destination/scratch space; padding and tail
copies are accounted. No reading past EOF or writing before/after a row to meet GDS alignment.
Coalescing is an optimization for explicit destinations, not a guarantee of aliasing them together.

## Budgets and observability

The hard cap is on admitted, library-managed slots/requests, not process RSS or OS cache residency.
Account separately for raw host/device pools, pinned host staging, caller destinations, completion/event
records and registration resources. Driver-internal bounce buffers/context overhead are separately
configured or measured with reserved headroom; unknown driver overhead prevents a total-memory hard-cap
claim. No fallback may silently allocate another unbounded pool. Report budget/queue/registration
exhaustion, unsupported allocation/context, short read, I/O/device failure, cancelled and timeout.

Report requested versus observed path: host_file, cuda_staged, cufile_direct_verified,
cufile_compatibility, intel_copy, or unknown. Include fallback reason, bytes by path, transfer count,
staging high-water bytes, in-flight count, latency and coalescing savings. A successful cuFile call alone
cannot set direct_verified. Unknown stays unknown and fails direct-only qualification.

## Portable test gates (M2/M3)

A deterministic fake backend drives the real state machine with delayed/out-of-order completions, short
reads, errors, cancellation races and duplicate completion. Assert no reuse while filling/leased, bounded
queue and pool use, all-or-nothing batch acquisition, stale-generation rejection and zero hot-path
allocations. Use one-slot/two-slot and multi-slot cases; batches larger than capacity must fail instead
of waiting on their own pins. Concurrent tests force overlapping requests and cross-domain separation.

Real-file tests on Windows and Linux compare exact bytes with an independent synchronous read oracle:
empty/out-of-range requests, overflow, tail EOF, unaligned intervals, rows crossing chunks, duplicate
requests and destination canaries. macOS uses a bounded worker-backed local-file implementation before
Sub0TieredCache claims its three-platform baseline; worker I/O can block, caller submission cannot.
GPU tests use deterministic bytes plus a readback/check kernel and injected delayed consumer events.
Mocks validate the protocol; only real hardware can qualify a vendor path. Never count skipped hardware
tests as passes. Allocation/registration failures must leave every surviving resource drainable.

## M3 slice 1: local-file backend

[`local_file_backend.hpp`](../include/sub0mempage/local_file_backend.hpp) is the first real
`FillBackendRef` implementation: a portable, bounded worker-pool reader over files registered ahead of
time (POSIX `pread`; Windows overlapped `ReadFile` used synchronously, written to compile under MSVC but
not yet built/run there -- see docs/implementation-plan.md's M3 row). Two decisions worth stating here
because they are not obvious from the backend contract alone:

- **Unknown `SourceId` at submit time.** Looked up in a bounded table under the same lock as the queue
  (no allocation, no I/O). A miss is never reported by returning `false` from `submit()`: that return
  value's meaning is fixed by the backend contract as "the bounded queue is full," and SlotPool/TransferSet
  translate it specifically into `Status::queue_exhausted`, so reusing it here would misreport a
  registration mistake as transient backpressure. The request is accepted instead (still exactly one
  terminal delivery) and a worker delivers `Status::invalid_argument` once dequeued, never inline in
  `submit()`.
- **Shutdown.** `shutdown()` refuses further submits, delivers `Status::cancelled` for every request still
  queued and not yet picked up by a worker (one delivery each), lets any request a worker has already
  started finish normally (its ordinary ok/short_read/io_error delivery), then joins every worker thread.
  The destructor calls it if the caller has not already, so a backend never outlives a worker still
  writing into a caller's destination -- but calling it explicitly first lets the caller choose exactly
  when in-flight writes stop, per "Unregister/close fails busy or explicitly drains" above.

Every read, short or failed, reports `status = Status::ok` with the actual byte count achieved (matching
`tests/fake_backend.hpp`'s own convention): `detail::classify_fill` is the single place, shared by
SlotPool and TransferSet, that turns "ok but fewer bytes than requested" into `Status::short_read`, so a
short read -- whether from a genuine EOF or a file truncated after registration -- is never published as
resident however it was discovered. This backend does ordinary buffered I/O (no `O_DIRECT`/unbuffered
reads), so it does not need the slot-storage alignment the "Checkpoint: M2 draft" open question raises;
that question still applies to any future unbuffered/GDS-style backend. `io_uring` (Linux) and
IOCP/`CreateThreadpoolIo` (Windows) are named, deferred optimizations behind this same seam, not a silent
limitation -- see docs/prior-art.md sec 1-2 and the backend's own file comment.
