# Sub0Llm consumer trace

This document traces Sub0MemPage's design against Sub0Llm's real, already-merged code — the same
discipline [Sub0TieredCache/docs/reference-consumer-sub0llm.md](https://github.com/CraigHutchinson/Sub0TieredCache/blob/main/docs/reference-consumer-sub0llm.md)
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

`Store::raw(const Desc&)` returns encoded bytes from the CPU mapping. Explicit fills would instead
populate a distinct raw staging pool; dequantization then writes decoded output elsewhere. This changes
memory budgeting and the adapter, not just the mapping's hint policy.

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
job (schedule raw encoded bytes into bounded caller-owned storage) are genuinely different layers, and nothing
about adopting Sub0MemPage would remove `ExpertCache` — it would provide a bounded raw-input path beneath the decoded cache.

## 2. Corrected integration trace (2026-09-22)

`ExpertCache::pool_` holds decoded float planes. The file region holds encoded quantized bytes.
They cannot be the same registered destination: a raw fill would overwrite cached floats, and a decode
could read overlapping source/output. The earlier trace claiming unchanged storage with no additional
allocation was incorrect. `dequantize_expert` reads encoded bytes and writes decoded floats after a
scratch decode/transpose; newer consumer paths also need auditing before integration.

1. Keep Sub0Llm's decoded cache and allocations under Sub0Llm ownership. Allocate a separate bounded
   raw-byte pool at startup if explicit file fills are selected; count it in deployment memory planning.
   Alternatively, keep the mapped CPU source and accept its opportunistic residency semantics.
2. Register that raw pool only. Once the router has selected experts, submit their encoded ranges in a
   batch. `resolve`/`try_resolve` return leases over completed raw chunks, never decoded float entries.
3. Decode into the existing output storage while retaining every input lease. An adapter must handle
   multi-chunk segmented input or choose a separately justified contiguous staging strategy; this is
   not promised to be a one-line change to a contiguous-input decoder.
4. Release input leases after CPU consumption, or after the last GPU event using them has completed.
   A prefetch ticket and `wait` alone do not pin the input. Cache decoded output using Sub0Llm's policy.
5. Pipeline reads against computation within the known routed batch. Do not assume next-layer routing
   is available before computing the current layer. Preserve the current engine fallback until parity
   and measurement against its existing pipelined-I/O implementation pass.

This is a proposed adapter, not an integration already made. Inspect the current consumer at M4; the
B21 numbers below are historical motivation, not a claim about today's code or throughput.

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
