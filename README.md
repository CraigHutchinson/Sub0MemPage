# Sub0MemPage — asynchronous, proactive paging orchestration over memory-mapped regions

Status: **SPEC / REQUIREMENTS DRAFT.** No implementation exists yet — `include/sub0mempage/sub0mempage.hpp`
is a skeleton. This document is the pitch and the concrete API surface; [REQUIREMENTS.md](REQUIREMENTS.md)
is the normative contract an implementation is checked against. Mirrors exactly how
[Sub0Firn](https://github.com/CraigHutchinson/Sub0Firn) started (README + REQUIREMENTS + prior-art +
design doc + a CMake skeleton, no working implementation yet) — this is the same discipline one layer
further down the stack.

**Documentation map**:
- [REQUIREMENTS.md](REQUIREMENTS.md) — the normative contract (R1–Rn), each a testable sentence.
- [AGENTS.md](AGENTS.md) — pre-flight checklist for anyone (human or agent) implementing against this spec.
- [STYLE_GUIDE.md](STYLE_GUIDE.md) — naming and code-style conventions.
- [docs/](docs/) — reference material: [design.md](docs/design.md) (the full design rationale — read
  this for the "why," this README is the "what"), [prior-art.md](docs/prior-art.md) (real cited
  systems/papers/OS mechanics), [sub0firn-reconciliation.md](docs/sub0firn-reconciliation.md) (the
  point-by-point divergence analysis against Sub0Firn's own API), and
  [sub0llm-consumer-trace.md](docs/sub0llm-consumer-trace.md) (the design traced against Sub0Llm's real,
  already-merged MoE-expert sidecar code).

## Naming

**Sub0MemPage** = "memory-mapped **page**" orchestration — a sibling of `Sub0Firn` (the glaciology-metaphor
tiered-cache library) and `Sub0Pipeline`/`Sub0Log` in this project family's naming convention, but named
plainly rather than metaphorically because this layer is genuinely mechanical: it owns *pages*, not a
metaphor for compacted snow. Keeps the `Sub0` lineage (`sub0::` is Sub0Llm's own C++ namespace; `sub0firn::`
and `sub0mempage::` are unnested siblings of it, not nested components).

- **Repository**: `Sub0MemPage`.
- **C++ namespace**: `sub0mempage::` (lowercase, unnested — matching `sub0firn::`'s own precedent).
- **Library target name**: `Sub0MemPage` (CMake target), conventionally installed as `libsub0mempage`.

## 1. Scope

**Sub0MemPage owns residency, not addressing, and not content.** It is a general-purpose, lower-level C++
library for *asynchronous, proactive paging orchestration over memory-mapped regions* — prefetch
scheduling, working-set budget enforcement, and the OS mmap/async-I/O mechanics (Windows
`PrefetchVirtualMemory`/overlapped I/O/IOCP, Linux `io_uring`/`madvise`/`posix_fadvise`) — sitting
**beneath** higher-level table-serving abstractions like Sub0Firn.

- A **region** is a contiguous virtual address range backed by a file (or a caller-supplied mapping). The
  region's *virtual* mapping exists for the region's whole lifetime and is never partially unmapped; what
  Sub0MemPage manages is which of its pages are *physically resident*, and when.
- The unit of every call is a **byte range within a region**, page-granular. Sub0MemPage has no concept of
  a row, a table, an index, a dtype, or a version. Translating `(table_id, row_index)` or `(layer, expert)`
  into a byte range is entirely the consumer's job — precisely the boundary Sub0Firn already draws in its
  R7 offset-resolver callback, viewed from the other side.
- **Every range is always legally readable.** A caller may dereference any address in a registered region
  at any time without calling into Sub0MemPage at all. The worst case is the page fault it would have
  taken anyway. **Sub0MemPage never gates correctness — only latency.** This is the property that makes it
  safe to adopt incrementally, and it is what lets a caller's existing mapping-based code keep working
  unchanged while hints are added around it.

### 1a. In scope

- Registering a memory-mapped (or caller-supplied) region under a hard working-set budget.
- A batch-shaped, non-blocking prefetch hint call, distinguishing *declared* access (the caller has
  computed it will read this) from *speculative* access (a predictor guesses so).
- A synchronous resolve/pin call for the "no useful look-ahead, or the hint didn't land in time" case,
  returning a lease that exempts pinned bytes from eviction until released.
- Hard budget enforcement with an honest failure mode (never a silent overshoot) — the DPDK
  `rte_mempool` precedent, not Redis's soft `maxmemory` ceiling (see `docs/prior-art.md` §4).
- Advisory demotion (`wont_need`) and advisory, non-vetoing eviction notification (`on_evict`).
- Observability: resident/pinned/budget/headroom bytes, hint efficacy counters (issued/honored/
  declined/**unconsumed**), resolve hit/miss counts, eviction counts, bytes read.
- Portability across Windows and Linux as first-class targets, with an honestly-stated platform gap where
  one exists (see REQUIREMENTS.md and `docs/design.md`'s OQ1) rather than a claim smoothed over.

### 1b. Explicitly out of scope (non-goals)

- **Not a row/table cache.** No concept of `(table_id, row_index)`, no dtype conversion, no
  content-versioning/`invalidate` semantics — that is entirely Sub0Firn's job, one layer up. See
  `docs/sub0firn-reconciliation.md` D6.
- **Not a source of truth for what to warm.** Sub0MemPage never derives byte ranges itself; the caller
  (a MoE router's top-k output, Sub0Firn's own offset-resolver, or anything else) always supplies them.
- **Not a remote-data client.** No HTTP Range requests, no pluggable remote sources — a region must be
  memory-mappable, so this stays entirely Sub0Firn's concern for its own remote/HTTP tier. See
  `docs/sub0firn-reconciliation.md` D7.
- **Not a general-purpose caching library or allocator.** It manages residency of pages already backed by
  a mapping — it does not allocate, does not serve arbitrary key-value pairs, and is not a `malloc`
  replacement.

## 2. How this relates to Sub0Firn

Sub0Firn and Sub0MemPage are **two separate projects at two separate layers**, not the same thing under two
names. Quoting the reconciliation's own headline conclusion (full point-by-point analysis in
[docs/sub0firn-reconciliation.md](docs/sub0firn-reconciliation.md)):

> Adopting Sub0MemPage beneath Sub0Firn would require **no change to any of Sub0Firn's R1–R10**, would
> leave `prefetch`/`wait`/`try_get`/`resolve_into`/`stats` implementable with their published contracts
> intact, would **close** an existing undocumented `try_get` view-lifetime gap, and would leave the
> HTTP/remote tier and all content semantics exactly where they already are.

- **Kept identical, on purpose**: `prefetch`'s name and non-blocking contract, `wait`'s "blocks the calling
  thread only" guarantee, the "only `resolve`/`wait` may block on I/O" rule, coalescing of concurrent
  requests for overlapping ranges, `stats`' "observability only" framing.
- **The one substantive divergence Sub0Firn's maintainer has to think about**: Sub0MemPage hands back
  pointers into a live mapping under active eviction pressure, so it introduces explicit **pinning via
  leases** — a concept Sub0Firn's own `resolve_into`/`try_get` contract can currently get away without,
  because `resolve_into` copies and `try_get`'s zero-copy view has no documented lifetime rule at all (a
  gap Sub0MemPage would close, not open, if Sub0Firn adopted it).
- **Additive, lower layer only**: a hard `budget_bytes` with an honest exhaustion failure mode; class-tagged
  (`DECLARED`/`SPECULATIVE`) prefetch; `wont_need`/`on_evict`; an optional inverted-control
  `open_stream`/`next` shape for a caller that can generate its own future access.
- **Deliberately NOT in scope for Sub0MemPage**: `invalidate`, `version_tag`, dtype conversion — those stay
  entirely Sub0Firn's job (D6); pluggable remote sources — HTTP Range stays entirely Sub0Firn's (D7).

Craig Hutchinson's own framing, quoted verbatim as the reason this is a separate project rather than a
Sub0Firn-internal detail: *"Its actually possible this could be a tool that Sub0Firn could/should depend
on using as there is some form of overlap here — Sub0Firn could be another use-case for the Sub0MemPage
library."* Sub0Llm's own MoE-expert sidecar cache (`sub0::moeq::Store`/`ExpertCache`,
`include/sub0/moe_quant.hpp`) is the other concrete, already-real use case — see
[docs/sub0llm-consumer-trace.md](docs/sub0llm-consumer-trace.md).

## 3. API surface

Given as an engine-agnostic contract, not C++ syntax — the style `Sub0Firn/README.md` §3 uses, and for
the same reason: an implementation should render this faithfully into whatever binding surface it
exposes, but the contract itself is language-neutral. The full design rationale for every call below —
including why each shape was chosen over the alternative precedents researched — lives in
[docs/design.md](docs/design.md) §2; this section states the calls themselves.

```
register_region(backing, budget_bytes, policy_hints) -> region_handle
    // Establishes the virtual mapping (or adopts a caller-supplied one) under a HARD working-set
    // budget. Does not fault in anything. Blocking: opens/maps the file, so yes, but once, at startup.
    // `policy_hints` is a STANDING policy set once (access shape, read-mostly, replacement family),
    // never re-issued on a hot path -- the CUDA cudaMemAdvise precedent (docs/design.md sec 2).

prefetch(region, ranges[], class) -> ticket
    // THE hint call. Batch-shaped from the start (Win32 PrefetchVirtualMemory takes an array of
    // discontiguous ranges in one call). NEVER BLOCKS. Never allocates on the caller's thread. Never
    // faults inline. Issues asynchronous I/O and returns.
    // `class` is DECLARED ("I have computed I will read this") or SPECULATIVE ("a predictor thinks
    // so") -- governs ADMISSION, not priority (docs/design.md sec 3).
    // The returned `ticket` is OPTIONAL to use -- a caller that will never wait may discard it and
    // pays nothing.

wait(ticket, deadline?) -> outcome
    // Blocks the CALLING THREAD ONLY until every range named by that ticket's prefetch is resident,
    // or the deadline passes. `outcome` reports resident / partially-resident / declined per range --
    // "partially fail" is a documented platform outcome (Windows PrefetchVirtualMemory), not an
    // exception here either.

resolve(region, ranges[], class) -> lease
    // Synchronous. Blocks until every named range is resident, then PINS them and returns a lease.
    // The recovery path for "the hint wasn't given in time," and equally correct for a caller with no
    // useful look-ahead at all. If the budget cannot accommodate the request, this FAILS with
    // budget-exhausted rather than overshooting the budget -- never a silent overshoot.

release(lease)
    // Unpins. Never blocks. Released ranges become eviction candidates, ordered by policy -- not
    // evicted eagerly. RAII-shaped in the C++ binding; the lease is the resource.

try_resolve(region, ranges[], class) -> optional<lease>
    // Never blocks, never faults, never starts I/O. Returns a pinned lease iff every named range is
    // ALREADY resident, otherwise nothing. The one call safe to place inside a tighter loop than the
    // resolve-pass the rest of the API is built around.

wont_need(region, ranges[])
    // The demotion hint (POSIX MADV_COLD analog). Never blocks. Marks ranges as no longer expected,
    // moving them toward the front of the eviction queue WITHOUT forcing them out.
    // Advisory in the strongest sense -- may be a complete no-op on a platform lacking a demotion
    // primitive (see REQUIREMENTS.md's honest platform-gap note, OQ1).

set_budget(region, budget_bytes)
    // Raises or lowers the hard cap at runtime. Lowering may trigger immediate eviction of unpinned
    // ranges; it can never evict pinned ones, so lowering below the currently-pinned total is a
    // reported failure, not silently ignored.

stats(region) -> { resident_bytes, pinned_bytes, budget_bytes, headroom_bytes, high_water_bytes,
                   hint_issued, hint_honored, hint_declined, hint_unconsumed,
                   resolve_hits, resolve_misses, evictions, bytes_read }
    // Observability only; nothing's behaviour depends on reading it. `hint_unconsumed` is the direct
    // measure of whether the caller's own predictor is any good.

on_evict(region, callback)          // optional
    // Advisory notification, invoked OFF the caller's threads, AFTER the fact. Cannot veto and cannot
    // block eviction (docs/design.md sec 4 explains why a veto shape was rejected).
```

**Optional second shape**, for callers that can generate their own future (PostgreSQL Read Stream
precedent — see `docs/prior-art.md` §3b), offered as an *addition*, not a replacement:

```
open_stream(region, next_range_callback, private_data) -> stream_handle
next(stream_handle) -> lease
    // The library pulls from `next_range_callback` as far ahead as its own budget/I/O concurrency
    // allow. Look-ahead depth becomes a deployment-level tunable the library owns, not a number every
    // call site guesses. The callback may be invoked on a library thread, must be cheap, must not
    // block, and must not re-enter Sub0MemPage.
```

## 4. What Sub0MemPage stands on — prior art

The full research — direct quotes, confidence tags, and the reasoning for why each one matters — lives in
[docs/prior-art.md](docs/prior-art.md); restated here in compressed form:

- **Windows `PrefetchVirtualMemory`/overlapped I/O/IOCP and Linux `madvise`/`posix_fadvise`/`io_uring`** —
  the OS mechanics layer this project sits directly on top of. The single most load-bearing finding: on
  Windows, *there is no asynchronous page-fault mechanism at all* (Microsoft's own KB156932), so a design
  that relies on many threads independently faulting a mapping is not actually building queue depth —
  it must issue explicit batched I/O instead.
- **CIDR 2022 (Crotty/Leis/Pavlo), "Are You Sure You Want to Use MMAP in Your DBMS?"** — measured,
  page-fetched evidence that `mmap`-based file I/O hits three scaling bottlenecks (page-table contention,
  single-threaded eviction, TLB shootdowns) under heavy concurrent random access, and that "spawn threads
  to prefetch by touching pages" — the exact workaround this project's motivating defect (B21, below)
  independently reached for — is explicitly named and rejected by the paper's own authors as adding
  complexity without solving the underlying problem.
- **CUDA Unified Memory (`cudaMemAdvise`/`cudaMemPrefetchAsync`)** — the cleanest existing precedent for
  separating a *standing* policy call from a *per-invocation* hint call against one underlying manager,
  and for "hints never gate correctness."
- **RocksDB's async I/O layering and PostgreSQL's Read Stream** — library-level API shape precedent: a
  submission call that reports whether it actually went async, and an inverted-control shape where the
  caller hands over a generator of its own future access.
- **DPDK `rte_mempool`** — the budget-as-a-hard-creation-time-constant precedent, with exhaustion as a
  reported failure rather than a soft, silently-breached ceiling (Redis's `maxmemory`, cited as the
  negative precedent).
- **Redis, Caffeine/W-TinyLFU, Linux MGLRU** — three independent, real systems that all abandoned an
  intrusive exact-ordering eviction list in favor of approximate ordering plus deferred, batched
  maintenance — and MGLRU specifically establishes that a policy can run entirely on OS-provided coarse
  signals with **zero per-access caller cooperation**, which is the shape Sub0MemPage's own primary
  consumer (a hot loop that must not call back into the library per access) requires.

## 5. The motivating real-world defect

This project exists because of a measured, not hypothetical, production defect in Sub0Llm's decode engine
— **B21** (`Sub0Llm/docs/INDEPENDENT_REVIEW_BACKLOG.md`): `ParallelExperts`' 10 decode threads, each
independently faulting its own disjoint expert planes from a shared 37 GiB read-only mapping, do **not**
achieve concurrent disk I/O in production — measured `\PhysicalDisk\Avg. Disk Queue Length` sits at
0.04–0.09 throughout steady-state decode, indistinguishable from the harness's own *single-thread* figure
(0.12–0.20), nowhere near its ten-thread figure (0.98–1.21) — while an isolated harness running the
identical resolve code scales 4.2–5.2x under the identical OS/mapping/file. The full trace, with the real
measured numbers, is in [docs/design.md](docs/design.md) §3 and [docs/sub0llm-consumer-trace.md](docs/sub0llm-consumer-trace.md).

## 6. License

MIT — see [LICENSE.md](LICENSE.md), matching Sub0Firn's own choice.
