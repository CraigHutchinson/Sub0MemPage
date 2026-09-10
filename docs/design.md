# Sub0MemPage — design rationale

**Companion doc**: [README.md](../README.md) covers the "what" — the scope statement and the API surface
as a contract. This document covers the "why" — the full design synthesis behind that contract, the
measured production defect that motivates the project, and the open questions the design deliberately
does not paper over. Mirrors
[Sub0Firn/docs/tiered-storage-design.md](https://github.com/CraigHutchinson/Sub0Firn/blob/main/docs/tiered-storage-design.md)'s
own role one layer up the stack.

Status: **DESIGN ONLY. No engine code this pass**, following the same staging convention Sub0Firn itself
used (and that Sub0Llm's own architecture docs established before it).

---

## 1. Scope line, stated precisely

**Sub0MemPage owns *residency*, not *addressing*, not *content*, and never allocates bulk data storage
(R1, R9 — see §8 for the full ownership-model resolution this section is written to already assume).**

- A **region** names an addressable byte-range source — typically a file — not necessarily a live virtual
  mapping. A region MAY also carry a caller-supplied mapped view, enabling an explicitly **secondary,
  opportunistic** mmap-based access mode (§8); the **primary** mode below never requires one.
- The unit of every call is a **byte range within a region**, paired with a **caller-owned destination**
  the caller allocated via `register_slots` (§8). Sub0MemPage has no concept of a row, a table, an index,
  a dtype, or a version, and it never allocates the memory a range's bytes land in. Translating
  `(table_id, row_index)` or `(layer, expert)` into a byte range — and sizing/typing the destination that
  range's bytes fill — are both entirely the caller's job — the same boundary Sub0Firn already draws in
  its own R7 offset-resolver callback and its own `resolve_into`'s copy-into-caller-buffer contract,
  viewed from underneath it.
- **In the secondary mmap mode only, every range is always legally readable.** Following CUDA's
  *"accesses to this range are always coherent and are allowed even when the data is actively being
  migrated"* and Windows' *"prefetching is not necessary for accessing the target address ranges"*
  (`prior-art.md` §1, §5): a caller using that mode may dereference any address in a registered mapped
  view at any time without calling into Sub0MemPage at all — the worst case is the page fault it would
  have taken anyway. **This does not extend to the primary caller-slot mode** (§8): there, a destination
  is only valid once `resolve`/`wait`/`try_resolve` reports it resident, the same contract every real
  precedent researched for caller-owned destinations makes (REQUIREMENTS.md R2). In the mode where it
  applies, Sub0MemPage never gates correctness — only latency — the property that makes the secondary mode
  safe to adopt incrementally around existing mapping-based code, and it is what lets Sub0Llm's existing
  `moeq::Store`/`FileMap`-based code keep working unchanged today while a migration to the primary mode
  happens around it (see `sub0llm-consumer-trace.md`).

Grounding in the immediate consumer (`Sub0Llm/include/sub0/moe_quant.hpp`, `file_map.hpp`): the region is
the ~37 GiB S0Q1 sidecar `moeq::Store` maps whole and read-only today; the ranges are `moeq::ByteRange` per
`(layer, expert)` plane, already computed by existing code; the declared signal is the MoE router's top-k
output, known before any expert byte is touched; the caller-owned destination pool is `ExpertCache`'s own
already-allocated `pool_` array, registered via `register_slots` unchanged (§8, `sub0llm-consumer-trace.md`
§2). `ExpertCache`'s current round-robin slot policy is exactly the naive replacement a Sub0MemPage-backed
policy would supersede — and `file_map.hpp`'s own header comment already anticipates this, listing *"no
madvise/prefetch hints"* under a deliberately narrow scope with no consumer yet.

## 2. The calls, and why each shape was chosen

The full call-by-call contract is in [README.md](../README.md) §3; this section is the *reasoning* behind
each shape, not a restatement of the contract. Signatures below reflect the ownership-model resolution in
§8 (caller-owned slot pools) — read §8 first if the `pool`/`register_slots` shapes below look unmotivated;
this section explains each call's *own* reasoning assuming that resolution, not why the resolution itself
was made.

**`register_region` names an addressable source and a *standing* `policy_hints`, never re-issued on a hot
path — and carries no budget.** The standing-vs-per-invocation split is CUDA's own composition rule,
generalized: `cudaMemAdvise` sets a *standing* policy on a range once ("this is read-mostly", "this
processor will touch it"); `cudaMemPrefetchAsync` is a *one-shot* "move it now, in the background," issued
thousands of times. Collapsing the two into one call would force a per-region standing decision through the
hot path every time it needs restating. Budget lives on `register_slots` instead (§8) — once the caller
owns the destination memory, the budget is simply how much of it the caller allocated, not a separate
number Sub0MemPage must be told and then police against a pool it doesn't own.

**`register_slots` takes the caller's own pre-allocated array and a uniform slot size, and that allocation
size IS the hard budget.** Follows DPDK's `rte_mempool` precedent — a hard, declared, creation-time number,
not Redis's soft `maxmemory` ceiling — but goes one step further than the original draft: DPDK's pool is
still library-allocated internally; here, the *caller's own allocation* is the pool, and Sub0MemPage's role
narrows to bookkeeping over it (`prior-art.md` §5a, §8 below). A library whose entire reason to exist is
fitting a huge file into a real, finite RAM budget cannot afford "approximately respected" — and cannot
afford to own memory it has no business knowing the shape of either (R1).

**`prefetch` is batch-shaped from the start, never blocks, and returns an optional ticket.** Batch-shaped
because Windows' own `PrefetchVirtualMemory` takes an array of discontiguous ranges in one call for
exactly this reason — *"the API will efficiently bring in those address ranges from disk using large,
concurrent I/O requests where possible"* — and because the real consumer's own access shape is "these ~10
scattered expert planes for this token," one call, not ten (`prior-art.md` §1). Each miss claims a
caller-owned slot and fills it directly, in the DirectStorage sense — *"the hardware will write directly
into the buffer that's provided by the title"* (`prior-art.md` §5a). The ticket is optional because a
caller that will never wait may discard it and pay nothing — but it exists at all because Windows itself
returns only a bare `BOOL` and CUDA returns nothing at all for its advice calls, and a library that owns
its own budget bookkeeping can do better: it knows exactly which ranges it declined and can say so
(`prior-art.md` §5, RocksDB's `SubmitReadAsync` — *"returns true for a non-blocking submission and false
when it used the synchronous fallback"* — is the direct precedent for reporting honestly rather than
staying silent).

**`resolve` is the one call that may synchronously block, and it PINS a caller-owned slot, not a
library-owned one.** Two distinct real needs collapse into one call: the recovery path for "the hint
wasn't given in time," and the correct call for a caller with no useful look-ahead at all. Pinning exists
because Sub0MemPage hands back a lease naming a slot *the caller allocated*, but whose content Sub0MemPage
is actively managing (filling, potentially reusing for a different range once released) — a fundamentally
different hazard than Sub0Firn's own two read paths, which both sidestep the lifetime question
(`resolve_into` copies; `try_get`'s zero-copy view has no documented lifetime rule at all, invisible today
only because Sub0Firn's RAM tier is not yet budget-managed). Under this resolution, `resolve`'s shape is
now the *same* shape `resolve_into` already has — a call that fills a caller-owned destination and hands
back something the caller can safely use until it explicitly releases it — not a new concept Sub0Firn's
maintainer has to learn. See `sub0firn-reconciliation.md` D2 for the full argument that adopting Sub0MemPage
would *close*, not open, the gap `try_get`'s own undocumented lifetime rule leaves today.

**`try_resolve` is deliberately pure — it never starts I/O on a miss.** RocksDB's own `TryAgain()` retry
protocol starts the read on the failed probe (`prior-art.md` §5); this design deliberately does the
opposite. Keeping the probe pure means it costs a predictable handful of nanoseconds and is genuinely safe
to place inside a tighter loop than the resolve-pass shape the rest of the API is built around. A caller
that wants "probe, and start it if missing" writes `try_resolve` then `prefetch` — two obvious calls
rather than one call with a hidden side effect.

**`wont_need` is advisory demotion, may be a complete no-op on a platform lacking the primitive.** Modeled
on POSIX `MADV_COLD` (*"Deactivate a given range of pages... a more probable reclaim target... a
nondestructive operation"*, `prior-art.md` §2). Stated honestly in REQUIREMENTS.md R14/OQ1: no documented
Windows counterpart for a read-only file mapping was found, so this call's actual effect on Windows may be
limited to "make this range a lower-priority speculative-eviction candidate," not true demotion.

**`on_evict` is advisory-only and structurally cannot veto.** A veto-capable callback would place
arbitrary caller code on the critical path of budget enforcement, where it could deadlock (by touching the
region and faulting inside the veto itself) or stall every other thread sharing the region. Advisory-
after-the-fact keeps the enforcement path bounded while still giving a higher layer — Sub0Firn, in
particular — the signal it needs to drop its own row-level index entries pointing into a range that just
went away.

**The optional `open_stream`/`next` shape is inverted control, borrowed directly from PostgreSQL's Read
Stream.** The caller hands over a callback that produces its own next byte range; the library decides how
far ahead to run, turning look-ahead depth into a deployment-level tunable rather than a number every call
site has to guess. Offered as a strict *addition*, in the spirit of RocksDB keeping all three of its async
tiers side by side rather than replacing the simpler one (`prior-art.md` §5) — if it does not earn its
keep in a real implementation, it can be dropped without touching anything else in the contract.

## 3. Declared vs. speculative composition, and eviction exposure

Per `prior-art.md` §6, this is where the design is least backed by precedent, and REQUIREMENTS.md R5
already says so explicitly rather than presenting it as established practice. The proposed rule:

1. **Both classes go through `prefetch`; the class is a parameter, not a different function.** Two
   functions would imply two mechanisms; there is one queue, one budget, one policy structure — following
   CUDA's own composition rule that advice *"guides the migration policy when a fault occurs"* rather than
   inventing a parallel cache.
2. **Class governs admission, not priority.** A `DECLARED` range is admitted unconditionally, evicting
   speculative residents if necessary. A `SPECULATIVE` range is admitted only if the policy estimates it
   beats the current eviction candidate — Caffeine's TinyLFU admission filter, applied to prefetch rather
   than to insertion. Directly justified by Windows' own warning that over-eager prefetch *"can also
   create memory pressure... applications should only prefetch address ranges they will actually use."*
3. **Consumption, not hinting, feeds the frequency estimate** (REQUIREMENTS.md R6) — an inference from
   precedent, not a cited finding, but the failure mode it prevents is concrete: without it, a bad
   predictor's wrong guesses become self-reinforcing evidence that the guessed ranges are hot.
4. **No per-access reporting is ever required** (REQUIREMENTS.md R11) — Linux MGLRU's design is the
   precedent, and the real consumer's own requirement is the reason: a resolve-pass-then-hot-loop shape
   means the library provably cannot see individual accesses. The policy runs on the hint stream, the
   resolve stream, and whatever coarse residency information the OS will give it, and on nothing else.
5. **No intrusive LRU list** (REQUIREMENTS.md R12) — three independent real systems (Redis, Caffeine,
   MGLRU) all abandoned exact-order eviction structures for approximate-ordering-plus-batched-maintenance;
   treated as settled given the three-way convergence.

**Eviction/reuse exposure**: default silent. In the secondary mmap mode, an evicted unpinned range is not
an error — it's exactly the page fault the caller would pay anyway, per §1's mode-scoped "always legally
readable" rule. In the primary caller-slot mode (§8), there is no OS-level eviction at all — "eviction"
means the same slot's key/liveness bookkeeping is cleared and the slot becomes a *reuse* candidate for a
different range on its next fill, entirely Sub0MemPage's own decision over memory the caller already owns
(REQUIREMENTS.md R10, R13's updated note). Pinned slots are never reused — that is the entire reason
pinning exists. When the pool's capacity cannot be met, the order is: (a) evict/reuse unpinned slots by
policy; (b) if still short and the request is `DECLARED`, evict/reuse speculative residents; (c) if still
short because pinned slots alone fill the pool, **fail the call and say so** — never a silent overshoot
(the DPDK-over-Redis choice, §2 above).

## 4. Why not a veto-capable eviction callback

Named explicitly because it's the most obvious alternative design and worth ruling out on the record, not
just by omission. A veto callback (`on_evict` returning bool, "may I evict/reuse this?") would let a
higher layer (Sub0Firn, say) refuse an eviction/reuse Sub0MemPage's own capacity accounting has already
decided it needs — but the callback runs on Sub0MemPage's own enforcement path, inside whatever lock or
bookkeeping structure decided the eviction was necessary in the first place. If the callback body touches
the very slot under eviction (a realistic mistake, not a contrived one — a veto handler checking "is this
range still needed" plausibly wants to read something nearby), it can fault (secondary mode) or race the
next fill (primary mode), and either is a reentrancy hazard this design has no answer for.
Advisory-after-the-fact has no such hazard: by the time `on_evict` runs, the eviction/reuse has already
happened and the enforcement path is clear.

## 5. Why this, and not X — synthesizing the OS-mechanics hypothesis-testing

The OS-mechanics research stream (`prior-art.md` §1–§2, §6) worked through four hypotheses for why a
naive "just spawn N threads faulting a shared mapping" design underperforms, before this project's own
empirical study (§3 below) actually measured which one was real on this machine:

- **H1 (page-fault path has no async primitive, so N threads are N shallow serial streams, not one deep
  concurrent one)** — supported by Microsoft's own explicit statement that Windows provides no
  asynchronous page-fault mechanism, and independently corroborated by this project's own measurement
  (§3): under the real decode workload, ten threads produce the *same* disk queue depth profile as one.
- **H2 (a shared lock in the Windows fault path for one section object)** — Windows' lock granularity for
  *disjoint*-page concurrent faults on one section object is genuinely undocumented by any Microsoft
  source located; this project's own empirical mmap-scaling measurement (§7 of `prior-art.md`) directly
  refutes it as a bottleneck at this project's real scale — mmap scales 5.1–7.3x under concurrent disjoint
  faults, so no such lock is capping throughput here.
- **H3 (working-set trimming quietly undoing warm-up)** — a real, documented mechanism (read-only mapped
  pages are the cheapest memory on the machine to evict), but this project's own memory-pressure
  simulation measured it as a contributory ~9-12% effect, not the dominant one.
- **H4 (collided page faults — same-page serialization)** — ruled unlikely by construction: the real
  workload's threads touch disjoint byte ranges.

**Ranked recommendations that follow, by the size of the measured lever** (full numbers in
`prior-art.md` §7 and `sub0llm-consumer-trace.md`):

1. **Prefetch, not faster faults, is the top lever — by an order of magnitude.** The real decode workload
   runs the device at queue length 0.04–0.20 against a demonstrated 6.34 GB/s ceiling at queue length
   9-12. Every one of the real workload's per-token resolves is a *predictable* read (the router picks its
   experts for layer L before layer L's FFN runs), so a design that issues layer L+1's reads while layer L
   computes turns a blocking stall into overlapped work. Nothing else in this document is worth as much.
2. **Explicit overlapped I/O beats mmap by 1.5-2.9x at matched concurrency, and ~1.47x on ceiling** — a
   real, local, measured argument for async-I/O-over-mmap, stated honestly as 1.5-2.9x, not as "mmap is
   the bottleneck." It is not, on this machine, at this scale.
3. **Read whole planes, not pages.** The mapping's fault path was measured issuing ~87 KB per physical I/O
   and blocking the thread each time; a single explicit read of a whole multi-MiB plane is one request and
   reaches a deeper queue even at depth 1.
4. **Buffered, not `FILE_FLAG_NO_BUFFERING`.** Measured 3.7x *slower* at depth 16 on this hardware and did
   not scale with queue depth at all — reported as a genuine, unreconciled measurement (`prior-art.md`
   §7), not smoothed into a tidier story.
5. **Target queue depth ~8.** 93% of ceiling throughput was reached by 8 outstanding reads on this
   hardware; a design does not need dozens in flight.
6. **Do not size the design against the full cold-resolve cost alone.** Even a perfect I/O layer leaves
   real dequantize/transpose CPU cost on the table for this project's own real consumer — I/O is roughly
   43% of the real per-expert cold-resolve cost, not all of it (see `sub0llm-consumer-trace.md`).

## 6. The B21 motivating evidence — concrete, not hypothetical

This project exists because of a real, measured, currently-unresolved production defect, not a
hypothetical one. Filed as **B21** in `Sub0Llm/docs/INDEPENDENT_REVIEW_BACKLOG.md`. The headline numbers,
reproduced here because they are the single most concrete justification for this project's existence:

| Measurement | Value |
|---|---|
| Real production decode, `\PhysicalDisk\Avg. Disk Queue Length`, steady state | **0.04–0.09** |
| Isolated harness, identical resolve code, 1 thread | 0.12–0.20 (matches production almost exactly) |
| Isolated harness, identical resolve code, 10 threads | **0.98–1.21** (production does NOT reach this) |
| Harness 1-thread → 10-thread wall-clock scaling | 4.2–5.2x |
| Production's actual measured decode throughput | 5.53–5.85 s/token |
| Harness's 1-thread projected throughput | 5.31–5.67 s/token (matches production) |
| Harness's 10-thread projected throughput | 1.02–1.27 s/token (production does NOT reach this) |

**What this means, precisely**: Sub0Llm's `ParallelExperts` already runs 10 OpenMP decode threads, each
independently resolving its own disjoint cold expert planes from a shared read-only mapping — architecturally
the *exact* shape this project's own empirical study (§7 of `prior-art.md`) proved DOES scale 4.2–5.2x in
isolation on this same machine, this same file, this same OS. In the live production process, it does not
scale at all — the disk queue depth measured during real decode is indistinguishable from the harness's own
single-thread figure. Code review of the actual call site found no lock, no shared mutable state, and no
structural serialization bug (`Sub0Llm/docs/INDEPENDENT_REVIEW_BACKLOG.md`'s B21 entry, full detail). The
two most obvious candidate explanations — fork/join barrier tail cost across 48 short parallel regions, and
memory pressure from the engine's own ~25 GiB resident private footprint competing for page-cache room —
were both directly tested and measured too small (1.2–1.3x each) to explain the 4-5x gap. **The exact root
cause remains open** — thread-affinity/P-core-vs-E-core placement of the real OpenMP team, OpenMP wake-up
cost inside a much larger and busier process, or something upstream of the resolve call itself are all
still untested candidates.

**Why this does not weaken, and in fact strengthens, this project's own rationale**: point 5.1's headline
("prefetch, not faster faults, is the top lever") stands regardless of which exact mechanism is currently
capping `ParallelExperts` in production, because a proactive/async design that never depends on N
*reactive* blocking-fault threads racing each other to build queue depth sidesteps this entire class of bug
by construction — it does not need ten threads to cooperate correctly with an undocumented kernel
scheduling detail, because it never asks them to build queue depth via faults in the first place.

## 7. Open questions — a living list, not resolved

Carried in full from the source research, and deliberately not resolved by this design document:

- **OQ1 — Windows demotion.** No documented Windows counterpart to `MADV_DONTNEED`/`MADV_PAGEOUT` for a
  read-only file mapping was found. If none exists, budget *enforcement* on Windows — the first target
  platform — may reduce to "stop prefetching and let the OS reclaim," materially weaker than the DPDK-style
  hard cap §3 promises. Candidates to investigate: `VirtualUnlock`, `OfferVirtualMemory`,
  `SetProcessWorkingSetSizeEx`, or unmapping/remapping sub-views. **Must be resolved before the hard budget
  is claimed as a portable guarantee**, or REQUIREMENTS.md R14 is violated on day one.
- **OQ2 — `madvise` blocking semantics.** Not independently confirmed from a primary source. Re-verify
  before claiming `MADV_WILLNEED` is non-blocking on Linux.
- **OQ3 — RESOLVED, 2026-09-10.** *Was*: hint the OS page cache (cheap, portable, the whole point of an
  mmap design) versus manage a private buffer pool with direct async reads (PostgreSQL's own chosen
  direction after ~15 years of the former). *Now*: resolved in favor of the latter as the **primary,
  hot-path-safe mode** — see §8 below for the full reasoning and the fresh prior-art (`prior-art.md` §5a).
  The deciding evidence beyond the PostgreSQL precedent alone is this project's own B21 finding (§6): the
  real production consumer's reactive-fault-based design measurably does not achieve the concurrency an
  identical, architecturally-correct isolated test achieves on the same file, same OS, same machine — a
  first-party demonstration that "the OS page cache is the buffer pool" cannot be trusted to deliver
  controllable concurrency on this platform, not just PostgreSQL's own stated 15-year-old reasoning for
  moving away from it. The OS mmap fault path is kept as an explicitly secondary, opportunistic mode (§8),
  not removed — a caller that accepts its weaker guarantees may still use it.
- **OQ4 — DPDK hugepages** relative to mempool allocation: not retrieved. Relevant if Sub0MemPage ever
  wants large-page-backed regions.
- **OQ5 — Caffeine's frequency-sketch aging/reset step**: not retrieved. Redis independently establishes
  decay is necessary; Caffeine's specific mechanism is open.
- **OQ6 — Is the declared/speculative split (R5) even the right shape?** No system researched offers two
  first-class call shapes for this against one cache. Worth one more literature pass before committing to
  it in an implementation.
- **OQ7 — Sizing by measurement, not by guess.** An appropriate budget for any real deployment (the MoE
  sidecar, or any future consumer) is currently a guess. `stats`' `hint_unconsumed`/`resolve_misses`
  counters exist specifically so the first real run produces the trace that answers it, following
  Bandana's own "simulate dozens of small caches" technique (Sub0Firn's own prior-art, cited by reference).
- **Whether Windows' fault-path lock granularity for one section object under disjoint-page concurrent
  faults matches or differs from Linux's documented `mmap_lock` behaviour** is genuinely unknown — no
  Windows-side equivalent of the CIDR 2022 study was located, and this project's own empirical study
  measured throughput scaling (which refutes H2 as a bottleneck at this scale) without measuring the lock
  mechanism directly.

## 8. Ownership model — caller-owned destination buffers, resolved 2026-09-10

Two scope decisions, made together because they turned out to be the same decision seen from two angles:
**(a)** should the hot-path-safe mechanism be OS-reactive mmap paging, hinted, or self-managed pinned
scratch with explicit async fill (OQ3, above)? **(b)** who allocates that scratch — Sub0MemPage, or the
caller? Full prior-art trail in `prior-art.md` §5a; this section states the resolution and the reasoning
for *this* project specifically.

**(a) resolved: self-managed pinned scratch, explicitly filled ahead of the hot loop, is the primary
mode.** OS-reactive mmap paging — even hinted via `PrefetchVirtualMemory` — remains available as a
secondary, opportunistic mode, but is no longer the design's default assumption. Three independent
arguments converge here, not one:

1. **B21, directly.** Sub0Llm's real `ParallelExperts` already does the "many threads independently fault
   a shared mapping" thing this design originally leaned on, on this exact machine, this exact file, this
   exact OS — and it measurably does not build the concurrency an architecturally-identical isolated test
   achieves (§6, above). A design that depends on the reactive fault path delivering controllable
   concurrency is depending on something this project has now watched fail to deliver it, in production,
   more than once.
2. **PostgreSQL's own 15-year arc** (`prior-art.md` §3c/§5): hint the kernel via `fadvise`, discover the
   heuristics aren't controllable enough, move to owning async I/O directly. This project's original OQ3
   reasoning noted PostgreSQL's *other* stated reason (the page-cache-to-private-buffer copy) doesn't apply
   here because Sub0MemPage's regions are mapped — but that reasoning quietly assumed the destination would
   still be *inside* the mapping. Once ownership (b) is resolved caller-owned, the copy PostgreSQL was
   avoiding doesn't reappear either: a caller-owned scratch slot that was never inside the OS mapping in the
   first place has no "second copy" to avoid, because there was only ever going to be one.
3. **Windows offers no real pinning guarantee for a read-only file mapping at all.** OQ1 is not a footnote
   here: `PrefetchVirtualMemory` explicitly does not add pages to the working set and is a "strong hint"
   that "can completely or partially fail under low-memory conditions" — under this project's own real
   consumer's memory pressure (55+ GiB used of 63 GiB), a page warmed for layer L can be gone again before
   layer L+3 needs it. A caller-owned buffer, once filled, stays exactly as filled until the caller itself
   releases the slot — a guarantee the mmap+hint path structurally cannot make on this platform.

**(b) resolved: the caller owns and allocates all destination storage; Sub0MemPage never allocates bulk
data.** Full reasoning and the four fresh High-confidence citations (io_uring registered buffers, POSIX
`aio_read`, NVIDIA GPUDirect Storage `cuFile`, Microsoft DirectStorage — every storage/transport precedent
researched takes this shape) are in `prior-art.md` §5a. The short version: it keeps R1 ("not content")
airtight — a library that allocates its own typed destination has to know that destination's size and
shape, which is content knowledge this project's scope line says it must never have; a library that only
ever fills a caller-supplied `void*`/byte-range never needs to know what a "slot" means. It also matches
this whole project family's own standing no-runtime-allocation-on-hot-paths discipline, and it converges
with (a): once the primary mode is an explicit async fill rather than a reactive fault, the destination
being filled has to be a stable address *before* the read is issued — which a caller-owned buffer already
is, and a library-internal allocation would need its own separate lifetime story to provide.

**This is not new design — it is the general form of code the real consumer already wrote out of
necessity.** Sub0Llm's `moeq::ExpertCache<Slots, SlotFloats>` (`include/sub0/moe_quant.hpp`, unchanged,
already merged) already allocates and owns its own pool (`std::unique_ptr<float[]> pool_`, sized from its
own compile-time-known constants), and its `resolve()` method today conflates two genuinely separate jobs:
deciding which slot a `(layer, expert)` key belongs in and detecting a hit — pure bookkeeping, no content
knowledge required — and faulting the raw bytes in, then dequantizing them into that slot — entirely
content-aware, entirely Sub0Llm's own business. Sub0MemPage's job is exactly the first half, generalized to
any caller-owned destination shape, plus scheduling the I/O that fills wherever the caller points it —
never the second half, and never the allocation the caller already had a compile-time-sized answer for.

**What this changes in the call surface** (README.md §3, revised to match): a new `register_slots(region,
slot_bytes, num_slots, slots_ptr) -> pool_handle` call registers the caller's own pre-allocated array of
destination slots, and `prefetch`, `resolve`, and `try_resolve` operate over that `pool_handle` rather than
over `region` directly — `resolve(pool, ranges[], class) -> lease[]`, not `resolve(region, ranges[], class)
-> lease` returning a library-owned pointer. A pool of exactly one slot degenerates to the simpler
"one destination per call" shape every other precedent researched uses (`prior-art.md` §5a) — not a second
API family. `register_region`'s own role narrows to naming the addressable *source* (a file, or a caller-
supplied mapping used only for the OQ3-secondary opportunistic path); it carries no budget of its own —
the caller's `register_slots` allocation size IS the budget (R7). The `lease[]` returned by
`resolve`/`try_resolve` now
marks "this caller-owned destination is validly filled with exactly this byte range's content," not "here
is a pointer into memory the library owns" — a lease over a caller's own memory rather than over the
library's, but still the thing `release` un-pins and still the thing that is never evicted while held
(R8 is unaffected in substance, only in whose memory it protects).

**Consequence for §1's "every range is always legally readable" guarantee**: that guarantee now applies
specifically to the OQ3-secondary opportunistic mmap mode, where a live mapping genuinely exists and a
caller may dereference it without calling into Sub0MemPage at all. Under the primary caller-buffer mode,
there is no mapping for the caller to legally dereference ahead of a `resolve`/`wait` — the caller's
destination buffer is only valid after that call completes, exactly the same contract every precedent in
`prior-art.md` §5a already has (an `aiocb`'s `aio_buf`, a `DSTORAGE_REQUEST::Destination`, a `cuFile`-
registered pointer are all only valid once the operation they're tied to completes). This is not a
weakened guarantee relative to those precedents — it is the same one they all make — but REQUIREMENTS.md
R2 needs its wording narrowed to state which mode it covers, rather than reading as a universal claim.

**A pleasant, unplanned convergence with `Sub0Firn`**: `sub0firn-reconciliation.md` D2 already noted that
Sub0Firn's own `resolve_into` copies into a caller-owned buffer today, while `try_get`'s zero-copy path has
an undocumented lifetime rule. Under this resolution, Sub0MemPage's own primary `resolve` call has *the
same shape* `resolve_into` already has, one layer down — not a new concept Sub0Firn's maintainer has to
learn to bridge to when adopting Sub0MemPage, but the same contract, repeated at the layer beneath it.

Full sketch of what this looks like at the real consumer's actual call site: `sub0llm-consumer-trace.md`
§2, revised alongside this section.

## 9. Future scope — heterogeneous CPU/GPU/iGPU memory backends (proposed, not yet designed)

Raised 2026-09-10, alongside §8's ownership-model resolution — recorded here as a **deliberate,
documented future direction**, not a design commitment. No research pass has been done for this section
yet; everything below is a scoping argument for *why it belongs in this project's charter*, not a spec.

**The argument for including it, not spinning up a separate project**: §8's caller-owned-slot contract
(`register_region`/`register_slots`/`prefetch`/`resolve`/`release`, a hard capacity fixed by the caller's
own allocation, declared-vs-speculative admission) is not disk-specific in its shape — it is already
modeled in part on CUDA Unified Memory's `cudaMemAdvise`/`cudaMemPrefetchAsync` (`prior-art.md` §1, the
single most-cited precedent in this whole document), which solves the *identical* residency-hinting
problem for CPU/GPU memory placement rather than disk-backed files. A GPU or iGPU backend under the same
contract fills in a box this design already drew, rather than requiring a new one.

**This is not speculative for Sub0Llm specifically — it is already live, independent work reinventing
part of this vocabulary.** `Sub0Llm/docs/INTEL_IGPU_USM_CAPABILITY_SPIKE.md` is a real, in-progress
research spike into Intel iGPU Unified Shared Memory (SYCL USM allocation aspects, Level Zero extension
inventory), and its `prepare_for_device_copy`/`release_from_device_copy` pair (SYCL's own
`SYCL_EXT_ONEAPI_COPY_OPTIMIZE` extension) is structurally the same "pin before use, release after" cycle
as this project's own `resolve`/`release` — arrived at independently, for a different backend, by a
different piece of the same project. Left unaddressed, Sub0Llm accumulates a second, independently-
invented residency vocabulary rather than a second *backend* for one.

**What genuinely differs across the candidate backends, stated honestly rather than glossed over** — this
is NOT "one implementation, three flags":

- **Local disk (the backend this project is currently designed against)**: data genuinely moves across a
  storage bus; residency means "is it in DRAM at all."
- **Discrete GPU (CUDA Unified Memory)**: data genuinely moves across PCIe/NVLink between distinct physical
  memory pools; `cudaMemAdvise`'s `SET_PREFERRED_LOCATION`/`SET_ACCESSED_BY` are about *which* pool, not
  just *whether* resident.
- **Integrated GPU (Intel iGPU/USM, and analogous APUs)**: physical memory is frequently already shared
  between CPU and GPU; "residency" there is closer to pinning against a coherency domain and avoiding an
  unnecessary copy than to moving bytes at all — a materially different cost model from the other two.

Each would be a genuinely distinct backend implementation, the same way this project's own Windows/Linux
OS-mechanics split already is (§2, §8) — the value of including them under one project is the *shared
contract and vocabulary* (budget semantics, admission classes, lease/pin lifecycle, observability), not a
claim that one code path serves all three.

**Naming**: no rename is proposed. "Page" is not disk-specific — it is the literal common addressing unit
across CPU virtual memory, CUDA Unified Memory (which itself uses page-granular fault-driven migration),
and iGPU/USM pages alike; broadening scope this way makes the name more apt, not less.

**Explicitly not done by this section**: no API surface, no requirements, no prior-art research for GPU/
iGPU-specific primitives (Level Zero, SYCL USM, `cudaMemAdvise`'s full advice set beyond what `prior-art.md`
§1 already cites, AMD's HIP/ROCm equivalent) has been done yet. This section exists to record the scoping
decision and its reasoning; a dedicated research-and-design pass (mirroring how §8's own ownership model
was researched) is the next step if this direction is pursued.
