# Implementation plan and review — 2026-09-10

Decision: include Intel USM capability work as an opt-in experimental tool and later caller adapter.
Begin implementation now. Preserve the dependency-free C++23 core and caller-owned destinations.
Baseline reviewed: `02ea63b`; no paging implementation or tests existed at that commit.

## Review findings and resolutions

1. **MUST — design §9, “same pin before use, release after cycle”.** This conflates slot reuse,
   page locking, prepared-copy registration and device completion (R2/R8/R14). Keep four separate
   concepts. A GPU consumer releases its slot lease only after its last use completes. Preparation
   belongs to allocation lifetime, outside the hot path; it does not make arbitrary pointers USM.
2. **MUST — consumer trace §2, reuse `ExpertCache::pool_` unchanged.** The actual pool holds
   dequantized/transposed floats (`moe_quant.hpp` resolve/plane), while file reads return encoded bytes.
   Direct filling is not a drop-in replacement (R1). Use separately budgeted encoded staging slots;
   retain the decoded cache and caller-owned transform scratch. Remove the “no new allocation” claim.
3. **MUST — design §8, filled buffers stay resident.** Stable contents do not imply physical
   residency. R7 bounds slot capacity, not process RSS, page cache, driver allocations or page faults.
   An OS locking requirement needs separate qualification and failure handling.
4. **MUST — R5 unconditional admission versus R7 exhaustion.** Declared requests bypass the
   speculative filter only when unpinned capacity and bounded queue capacity exist. Return explicit
   exhaustion/backpressure; never overwrite a lease or wait inside `prefetch`.
5. **SHOULD — fixed contiguous slots versus R13 arbitrary overlap.** Equal/contained requests can
   share a fetch/lease. Partial overlaps spanning slots require a scatter result or copying completed
   fragments; the current `lease[]` sketch does not settle that. Resolve before core API publication.
6. **SHOULD — lifecycle and batch error semantics are unspecified.** Define region/pool ownership,
   deregistration, draining, ticket generations, deadlines, short reads, partial admission, output
   capacity and failure rollback before publishing signatures. Do not allocate returned vectors on
   the hot path. `wait(ticket)` completion alone must not authorize reading an unleased reused slot.

## Sequence and named consumers

| ID | Deliverable / consumer | Acceptance gate |
|---|---|---|
| M0 | This plan; corrected requirements, design and consumer trace | Contradictions above resolved or explicitly gated |
| I1 | `tools/intel_usm` capability executable, CMake opt-in | Build/run separate; actual identity and all six aspects; no silent device fallback; default core has no SYCL dependency |
| I2 | Same executable's explicit staged-copy check | Host-USM -> device-USM -> kernel -> host; exact full-buffer reference at 4 KiB and 4 MiB, changed data on second pass; explicit dependencies and drain-before-free |
| M1 | Core API and deterministic slot-state implementation; CPU byte-range example consumes it | Bounds/overflow; move-only leases; stale tickets; partial failures; no per-call allocation; exhausted/pinned budget and actual concurrent tests |
| M2 | File transport used by M1 scheduler; Windows overlapped I/O and Linux explicit async I/O | Real file bytes, short-read/error injection, bounded submission, nonblocking probes, coalescing including partial overlap; Windows + Linux pass |
| M3 | Admission/replacement plus stats, wired into M2 | Declared under pressure, speculation rejection, consumed-only frequency, `hint_unconsumed`, concurrent budget tests; prior-art-backed policy |
| I3 | Intel caller adapter over M2 slots; example consumes both | File fill -> device use -> completion -> lease release; reuse-under-load, allocation failure, exact-context lifetime tests; host/shared choices qualified independently |
| M4 | Sub0Llm/Sub0Firn caller adapters | Separate encoded staging/decoded-cache accounting; real sidecar, output parity, thread/lease review |
| I4 | Prepared-copy and external-memory investigations | Separate opt-ins; SDK/extension evidence first; balanced nonoverlapping registration; no production adoption without useful measured benefit |
| M5 | Performance and secondary mmap mode | Correctness first; counterbalanced independent processes, multiple sizes, capacity/pressure metrics; platform limits stated |

M0 then I1/I2 are this initial implementation slice. M1-M5 and I3/I4 remain follow-up work;
I2 is a transport qualification executable, not a working MemPage backend. No speculative public
GPU enums or allocator interfaces are added. New diagnostic helpers are private and called by `main`.

## Core decisions required at M1

Registration is setup work: callers own immutable source lifetime, destination storage and allocation
kind; metadata and submission capacity are sized once. Pools cannot move or die while leases or I/O
reference them. Teardown is an explicit draining control-plane operation, outside R3's hot-path API.
Only host-accessible storage may receive CPU file reads. Generic byte pointers cannot certify GPU access.

Choose and document all-or-nothing versus per-range batch admission with a bounded caller-supplied
result span. Reject zero length, overflow and ranges larger than one slot unless segmented results are
implemented. Coalescing must define exact, contained and crossing requests, including late arrivals
and cancelled waiters. Deadline expiry stops waiting, not ownership of an in-flight write. A completed
ticket reports completion; acquisition of a lease must atomically protect the bytes from reuse.

Hard capacity counts reserved/filling/ready/pinned slots, with metadata and driver costs reported
separately. No capability or `stats` bit may call ordinary pageable memory “physically pinned”.
Keep per-pool synchronization; `try_resolve` cannot wait behind I/O or an unbounded lock holder.

## Intel evidence and boundaries

Historical Sub0Llm record: `docs/intel-groundwork/2026-09-09/runtime/usm-capabilities-probe.txt`:
Intel 8086:7D67, Windows, DPC++ 2025.3.3, driver `1.15.39183+3`, Level Zero backend.
Host/shared/device allocations reported true; system and atomic host/shared reported false.
The prepared-copy arm accepted a balanced 4 KiB call pair only. Direct Level Zero extension inventory
was not built because matching development headers/import library were unavailable. Re-run qualification
for a changed compiler/driver/device tuple; historical results are not universal support claims.

I1 selects the measured PCI ID explicitly. I2 uses no prepared-copy extension, mapped-pointer kernel
access or concurrent host/device mutation. Allocation happens once per size, reused across passes.
No timing ranking is emitted. Default CTest never starts GPU work; execution is an explicit command.

## Verification record

Initial implementation results are recorded in `intel-usm.md` as gates run. Default builds must remain
independent of the optional Intel toolchain. Windows and Linux qualification are separate gates; a
Windows pass alone never closes portability. No performance claim is made by this slice.
