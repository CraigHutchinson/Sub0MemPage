# Sub0Llm consumer trace

This document traces Sub0MemPage's design against Sub0Llm's real, already-merged code — the same
discipline [Sub0Firn/docs/reference-consumer-sub0llm.md](https://github.com/CraigHutchinson/Sub0Firn/blob/main/docs/reference-consumer-sub0llm.md)
uses one layer up, applied here to Sub0MemPage's own first real, concrete consumer: Sub0Llm's MoE-expert
sidecar cache. Nothing in this document is hypothetical — every code reference is to files that exist and
are merged in `D:\Craig\GitHub\Sub0Llm` today, and every number is from a real measurement (either the
already-merged B20 work or the B21 follow-up research), not an estimate.

## 1. The "before" state — `moeq::Store` and `ExpertCache`, quoted

`include/sub0/moe_quant.hpp` implements the `S0Q1` sidecar format: a ~37 GiB read-only file, memory-mapped
whole, holding every routed expert's `gate`/`up`/`down` tensors in their native quantized GGUF encoding.
Its own header comment already states the memory-pressure reasoning that makes a mapping (not an eager
read) the only viable shape at the real 48-layer scale — quoted verbatim because it is the load-bearing
context for why Sub0MemPage's own scope (residency management over an *existing* mapping, never a second
copy) fits this consumer exactly:

> "The payload used to be read into an owned buffer, which at the 4-layer sub-stack is 3.17 GiB and
> invisible. At the real 48 layers it is ~38 GiB, and an eager read of that plus the 18.3 GiB f32 backbone
> plus a 14 GiB activation arena exceeds this machine's free RAM before a single token is embedded. A
> mapping changes the arithmetic rather than shaving it... the pages that ARE touched are file-backed and
> evictable, so they count against the working set the OS can reclaim under pressure, not against
> committed private bytes that it cannot."

`Store::raw(const Desc&)` hands back a `std::span<const std::uint8_t>` directly into the mapping — today,
this IS the raw-byte access `resolve`'s fill would replace, per §2 below. Under Sub0MemPage's ownership
resolution (design.md §8), the *destination* those bytes ultimately land in was never going to be
Sub0Llm's real problem — `dequantize_expert` already reads them once and writes typed float output
elsewhere — so replacing this mapping-fault read with an explicit fill into `ExpertCache`'s own registered
slot changes only how the source bytes arrive, not what happens to them afterward. What today's code does
not need to worry about, because nothing evicts anything (the mapping is simply left to the OS's own
default page-cache behavior, unmanaged), is exactly the gap `resolve`'s lease (R8, D2 in
`sub0firn-reconciliation.md`) exists to close once residency becomes actively managed.

`ExpertCache<Slots, SlotFloats>::resolve()` is the actual "cache" today, and its own comment states plainly
what kind of cache it is:

> "The cache earns its slots on real routing, not in principle... It is a plain round-robin with no
> invalidation: the weights are immutable for the life of a forward-only run... so a cached expert can
> never be stale."

This is a **dequantized-plane cache** (holding already-decoded f32, keyed by `(layer, expert)`, evicted
round-robin) sitting *on top of* the mapping — it has no concept of the mapping's own residency at all. The
mapping itself has no prefetching, no budget, and no eviction policy whatsoever; it is handed entirely to
the OS's default behavior. **This is exactly the gap Sub0MemPage exists to fill**, one layer below
`ExpertCache`, not a replacement for it: `ExpertCache`'s job (cache decoded f32 planes) and Sub0MemPage's
job (manage which raw encoded bytes are physically resident) are genuinely different layers, and nothing
about adopting Sub0MemPage would remove `ExpertCache` — it would give the mapping underneath it the
proactive, budget-managed residency it currently lacks.

## 2. How `decode.cpp`'s `ParallelExperts` would look calling Sub0MemPage — sketched in prose

Today (per the B21 backlog entry, `Sub0Llm/docs/INDEPENDENT_REVIEW_BACKLOG.md`), each of
`MOE_DECODE_THREADS` (10, at the real generated config) OpenMP threads gets its own heap-allocated
`MoeDecodeThread` holding a private single-slot `ExpertCache`, and independently calls `resolve()` for its
own assigned expert — which, underneath, calls `dequantize_expert()` on bytes read straight out of the
shared `Store`'s mapping via `store.raw(desc)`, faulting them in reactively, one thread at a time, with no
coordination at all between the ten threads about what any of them will need next.

**Sub0MemPage's ownership resolution (`docs/design.md` §8) makes this trace far more concrete than the
mapping-based sketch this document originally had**: `ExpertCache<Slots, SlotFloats>` is not merely
*analogous* to Sub0MemPage's own caller-owned slot pool — it already IS one, hand-written, generalized
here rather than replaced. The trace below is therefore mostly about which of `ExpertCache`'s own two
conflated jobs (bookkeeping vs. content) moves underneath it, not about introducing a new structure.

- **`ExpertCache::pool_` (the `std::unique_ptr<float[]>` backing storage) stays exactly where it is,
  allocated exactly as it is today** — Sub0Llm still owns and sizes it from its own compile-time constants
  (`SlotFloats`, `PerExpert`). What changes is that this same array is now also passed to
  `register_slots(region, slot_bytes, Slots, pool_.get())` once, at construction, registering it with
  Sub0MemPage as the destination pool Sub0MemPage will schedule fills into and track residency over.
  **No new allocation anywhere** — this is R14 in practice, not just in principle.
- **Before layer L's FFN region begins** (the router's top-k output for layer L is known at this point,
  strictly before any of that layer's expert bytes are touched — the same "declared signal known ahead of
  use" property REQUIREMENTS.md R5 is built around): one thread issues a single `prefetch(pool, ranges,
  DECLARED)` call carrying all ~10 experts' `ByteRange`s for that layer — replacing what is today ten
  independent, uncoordinated reactive fault sequences with one batched hint that fills directly into
  `ExpertCache`'s own registered slots, the same "many discontiguous ranges, one call" shape Windows' own
  `PrefetchVirtualMemory` is built for (design.md §2), but landing the bytes straight into memory
  `ExpertCache` already owns rather than into an OS-managed mapping a second copy would then have to leave.
- **Inside the parallel region**, each of the 10 `MoeDecodeThread`s calls `resolve(pool, its_own_range,
  DECLARED)` instead of `ExpertCache::resolve()`'s own current key-check-then-`store.raw(desc)` logic —
  Sub0MemPage now does the key-check-and-slot-selection bookkeeping (generalizing `ExpertCache`'s own
  `key_`/`live_` arrays, R14) and, on a miss, issues the fill directly into the chosen slot rather than
  through a mapping fault. Still synchronous, still correct if the prefetch hasn't finished yet (`resolve`
  blocks until resident, exactly like today's reactive fault would have), but now usually resolving
  against bytes the earlier `prefetch` call has already brought in. The returned lease is held for exactly
  as long as `dequantize_expert()` needs the raw encoded bytes, then released.
- **What `dequantize_expert()` reads from changes from `store.raw(desc)` (a span into the mapping) to the
  slot the lease names (a span into `ExpertCache`'s own `pool_`)** — a one-line change at the call site,
  not a restructuring, since the slot IS the same memory `store.raw` used to point application code at
  conceptually, just filled by an explicit async read instead of a page fault.
- **After layer L is fully consumed**, a `wont_need(pool, layer_L_ranges)` call marks those slots as no
  longer expected — cheap to add, advisory, and precisely expresses something `ExpertCache`'s own
  round-robin policy has no way to say today: "this layer is done, deprioritize these slots for reuse
  before the next one." Unlike the original mapping-based sketch, this needs no OS cooperation at all in
  the primary mode (REQUIREMENTS.md R13's own updated note) — "deprioritize for reuse" is pure Sub0MemPage
  bookkeeping over memory `ExpertCache` already owns.

**What does not change**: `dequantize_expert()`'s own two-step decode (`gguf::to_f32` then
`transplant::transpose_out_in`) is completely untouched — Sub0MemPage never interprets bytes
(`sub0firn-reconciliation.md` D6's point, one layer further down). The `#pragma omp parallel`/`#pragma omp
for schedule(static)` structure is completely untouched. `ExpertCache`'s own allocation, sizing, and
per-thread ownership are completely untouched. Only the path from "I need these raw encoded bytes" to
"here they are, in the slot I already own" changes, from an unmanaged reactive fault straight through the
OS to a managed, budget-aware, prefetch-able fill directly into existing memory.

## 3. B21 as the concrete, measured motivating evidence

The full account is in `docs/design.md` §6; the essential fact repeated here because it is specifically
*this* consumer's own defect: Sub0Llm's real production decode process, running `ParallelExperts`' 10
OpenMP threads against exactly the mapping described in §1 above, was directly measured (not inferred)
running at `\PhysicalDisk\Avg. Disk Queue Length` **0.04–0.09** throughout steady-state decode — the same
figure an isolated single-thread harness running the identical resolve code produces (0.12–0.20), and
nowhere near the 0.98–1.21 that same harness reaches at ten threads. Code review of the real call site
(`decode.cpp`'s `ParallelExperts`, this file's `Store`/`ExpertCache`) found **no lock, no shared mutable
state, and no structural serialization bug** — the ten threads are architecturally free to build real
concurrent disk queue depth, and measurably do not, for a root cause that remains open (§6 of design.md).

This is exactly the shape of defect a design that "never depends on N reactive blocking-fault threads
racing each other" sidesteps by construction (design.md §6's closing point) — not because it explains
B21's specific unresolved root cause, but because a batched, explicit `prefetch` call ahead of the parallel
region does not need ten independent threads to cooperate correctly with whatever undocumented kernel or
process-level scheduling detail is currently capping them; it builds queue depth from one thread, one call,
before the ten resolves even start.
