# Sub0Llm consumer trace

This document traces Sub0MemPage's design against Sub0Llm's real, already-merged code — the same
discipline [Sub0Firn/docs/reference-consumer-sub0llm.md](https://github.com/CraigHutchinson/Sub0Firn/blob/main/docs/reference-consumer-sub0llm.md)
uses one layer up, applied here to Sub0MemPage's own first real, concrete consumer: Sub0Llm's MoE-expert
sidecar cache. The before-state references files that exist and
are merged in `D:\Craig\GitHub\Sub0Llm` today. Section 2 is a proposed integration, not implemented behavior.

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
this is the raw-byte access the proposed staging path replaces. `ExpertCache::pool_` is the distinct
float destination. `dequantize_expert` uses `gguf::to_f32` and `transpose_out_in`; those transforms stay
in Sub0Llm, outside the byte-range library.

## 2. Proposed integration — encoded staging and decoded cache are separate

The caller allocates a bounded encoded-byte staging pool and registers it with Sub0MemPage. This is
additional memory and must be budgeted alongside `ExpertCache::pool_` (decoded/transposed floats),
`raw_scratch_`, the model, and OS/driver overhead. Reusing the decoded allocation for encoded input is
not a drop-in substitution: `dequantize_expert` reads encoded bytes while producing distinct float data.

After the router selects experts for the current layer, prefetch their encoded `ByteRange`s into
staging. Each worker resolves a staging lease, dequantizes its bytes into the existing decoded cache,
and releases the staging lease after dequantization finishes. The decoded cache keeps its own keys,
liveness and replacement policy; Sub0MemPage tracks only encoded slots. Size the staging pool for
simultaneously needed planes and bounded read-ahead. Exhaustion is reported, not hidden by allocating.

Current-layer router results do not predict layer L+1 routing. Overlap within a layer is possible;
next-layer prefetch must be explicitly speculative unless the consumer proves its keys are known.
The parallel loop and numerical transforms can remain, but plumbing the raw-byte input and staging
lifetime requires an adapter and output-parity validation; no one-line/no-allocation claim is made.

For a future GPU consumer, retain the encoded lease through the final device operation that reads it.
A decoded device cache remains caller-owned. File I/O cannot write to device-only USM through a generic
host pointer; an explicit copy stage is required. See `implementation-plan.md` I3/M4.

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
