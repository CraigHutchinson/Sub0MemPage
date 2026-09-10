# Requirements

Normative contract for Sub0MemPage. Each requirement below is stated as a testable sentence first, then
explained — the sentence is what an implementation is checked against; the explanation is why it says
that and not something weaker. Sourced from the three research streams consolidated in
[docs/prior-art.md](docs/prior-art.md) and the design synthesis in [docs/design.md](docs/design.md) —
nothing here is asserted without a concrete cited source or a measured finding behind it. Follows
[Sub0Firn/REQUIREMENTS.md](https://github.com/CraigHutchinson/Sub0Firn/blob/main/REQUIREMENTS.md)'s own
style exactly, one layer down the stack.

## R1. Sub0MemPage owns residency, not addressing, and not content

"Sub0MemPage receives only a region handle and byte ranges within it; it does not derive, hash, or
interpret how a byte range was computed, and it never reads or validates the bytes it manages beyond
their extent."

Translating `(table_id, row_index)` or `(layer, expert)` into a byte range is entirely the caller's job —
exactly the boundary Sub0Firn's own R7 offset-resolver callback already draws, viewed from the layer
below it (`docs/design.md` §1). Baking any addressing or content semantics into Sub0MemPage would tie a
generic residency-management engine to one caller's domain, defeating the reason it exists as a separate,
lower project at all.

## R2. Every range is always legally readable, independent of any Sub0MemPage call

"A caller may dereference any address inside a registered region at any time, whether or not `prefetch`,
`resolve`, or any other call has ever been made for that range. The worst case is the page fault the
caller would have taken with no Sub0MemPage in the picture at all."

Follows CUDA's own documented guarantee — *"accesses to this range are always coherent and are allowed
even when the data is actively being migrated"* — and Windows' own framing of `PrefetchVirtualMemory` as
*"purely a performance optimization: prefetching is not necessary for accessing the target address
ranges"* (`docs/prior-art.md` §1, §2a). **Sub0MemPage never gates correctness — only latency.** This is
the property that makes the library safe to adopt incrementally around existing mapping-based code.

## R3. Only `resolve` and `wait` may block on I/O

"`resolve` and `wait` are the only calls in the API that may block the calling thread on I/O. `prefetch`,
`try_resolve`, `wont_need`, `release`, and `stats` must never block on I/O. No other call implicitly
triggers a synchronous fetch."

Mirrors Sub0Firn's own R2 exactly, one layer down — the same load-bearing guarantee a caller with a
no-heap-allocation/bounded-latency hot-path rule (Sub0Llm's `AGENTS.md` §1, the real motivating case) needs
a hard contract for, not a "usually fast." `try_resolve`'s never-blocks guarantee is what makes it the one
call safe to place inside a tighter loop than the rest of the API is built around.

## R4. `prefetch` is batch-shaped and asynchronous by construction

"`prefetch` accepts an array of possibly-discontiguous byte ranges in one call and returns without
blocking, without allocating on the caller's thread, and without issuing any blocking fault."

Directly modeled on Windows' own `PrefetchVirtualMemory`, whose signature takes `NumberOfEntries` +
an array of `WIN32_MEMORY_RANGE_ENTRY` for exactly this reason — *"the API will efficiently bring in
those address ranges from disk using large, concurrent I/O requests where possible"* — and matches the
real consumer's own access shape (a MoE router emitting ~10 scattered expert-plane ranges per token, one
call, not ten) (`docs/prior-art.md` §2a).

## R5. Declared and speculative prefetch are a distinct admission class, not a priority number

"`prefetch`'s `class` parameter distinguishes DECLARED (the caller has computed it will definitely read
this range) from SPECULATIVE (a predictor guesses it might). A DECLARED range is admitted against the
budget unconditionally, evicting speculative residents if necessary. A SPECULATIVE range is admitted only
if the replacement policy estimates it beats the current eviction candidate."

Windows' own documentation warns explicitly that over-eager prefetch *"can also create memory pressure...
applications should only prefetch address ranges they will actually use"* (`docs/prior-art.md` §2a) — a
speculative hint is a *proposal*, and proposals need a gate the way Caffeine's TinyLFU admission filter
gates cache insertion (`docs/prior-art.md` §5b), applied here to prefetch rather than to insertion. Stated
honestly as the design's least-precedented element (see OQ6 below) rather than presented as established
practice: no system researched offers exactly two first-class call shapes for declared-vs-inferred access
against one cache with a documented composition rule; CUDA's `cudaMemAdvise`/fault-path split comes
closest but its advice is range-persistent standing policy, not a per-invocation declaration.

## R6. A range that is hinted but never consumed must not be recorded as an access

"A range prefetched via `prefetch` and subsequently read via `resolve` or `try_resolve` records an
access for replacement-policy purposes. A range that is prefetched and never subsequently resolved
records nothing but a `hint_unconsumed` counter tick — it must never feed the frequency/recency
estimate the same way a genuine access does."

Without this, a bad predictor's wrong guesses become self-reinforcing evidence that the guessed ranges
are hot, poisoning the policy over time — the same failure mode Caffeine's admission filter exists to
prevent for one-hit wonders (`docs/prior-art.md` §6). Stated in the source research as inference from
precedent rather than a directly cited finding, and repeated as such here.

## R7. The working-set budget is a hard cap; exhaustion is a reported failure, never a silent overshoot

"`register_region`'s `budget_bytes` and `set_budget`'s updated value are hard caps on resident bytes
attributable to that region. When a `resolve` cannot be satisfied because the budget is already consumed
by pinned ranges, it fails with a reported budget-exhausted outcome rather than admitting the request and
exceeding the declared budget. `set_budget` lowering the cap below the currently-pinned total is a
reported failure, not a silently-ignored request."

The DPDK `rte_mempool` precedent, deliberately chosen over Redis's `maxmemory`, which the Redis docs
themselves admit *"might temporarily exceed the limit by a large amount"* (`docs/prior-art.md` §4a). A
library whose entire reason to exist is fitting a huge file into a real, finite RAM budget cannot afford
"approximately respected" — the failure mode of exceeding it is not degraded performance, it is the
OOM/thrash the budget existed to prevent.

## R8. No range is ever evicted while pinned; a lease is the only thing that prevents eviction

"A byte range returned by `resolve` or `try_resolve` inside a live `lease` is never evicted while that
lease is held, regardless of budget pressure. `release`d ranges become eviction candidates ordered by
policy, not evicted eagerly."

This is the mechanism that makes handing back a pointer into a live, budget-managed mapping safe at all —
without it, a caller reading a "resolved" pointer under memory pressure risks a use-after-evict bug. See
`docs/sub0firn-reconciliation.md` D2 for why this is the one concept Sub0Firn itself lacks today and the
concrete hazard adopting Sub0MemPage would close.

## R9. Eviction is silent by default; `on_evict` is advisory-only and cannot veto

"Eviction of an unpinned range produces no error and requires no caller action — it is exactly the page
fault the caller would pay if it touches that range again (R2). An optional `on_evict` callback, if
registered, is invoked off the caller's own threads, strictly after the eviction has already happened,
and cannot block or reverse it."

A veto-capable callback would place arbitrary caller code on the critical path of budget enforcement,
where it could deadlock (by touching the region and faulting inside the veto) or stall every other thread
sharing the region (`docs/design.md` §4). Advisory-after-the-fact keeps the enforcement path bounded while
still giving a higher layer (Sub0Firn, in particular — see `docs/sub0firn-reconciliation.md` D4) the
signal it needs to drop its own index entries pointing into an evicted range.

## R10. No replacement policy requires per-access caller cooperation

"The replacement/eviction policy must be maintainable using only the `prefetch`/`resolve`/`try_resolve`
call stream and whatever coarse OS-level residency signal is available on the platform — never a
per-access `touch()`-style call the caller must remember to make on every read."

Linux MGLRU's own design is the precedent: the kernel does not ask anyone to report accesses, it reads
hardware accessed bits in bulk on its own schedule (`docs/prior-art.md` §5c). The real consumer's whole
point is a hot loop that reads a resolved pointer with zero further library calls (mirrors Sub0Firn's own
R3) — a design that needs per-access feedback to stay accurate is incompatible with its own primary
consumer.

## R11. No intrusive exact-ordering eviction structure

"The replacement policy must not maintain a global intrusive linked list (or equivalent exact-order
structure) that every access reorders under a shared lock."

Three independent, real, battle-tested systems — Redis (sampling + a candidate pool), Caffeine/W-TinyLFU
(striped ring buffers replayed in batch), and Linux MGLRU (a small number of generation lists aged in bulk)
— all abandoned exact LRU lists for the same reason and converged on *approximate ordering plus deferred,
batched maintenance* (`docs/prior-art.md` §5d). That three-way convergence is treated as settled.

## R12. Concurrent requests for overlapping ranges coalesce into one real fetch

"Multiple threads calling `prefetch`/`resolve` concurrently for ranges that overlap must trigger exactly
one real I/O for the overlapping bytes, not one per caller."

Inherited unchanged from Sub0Firn's own R5, one layer down — at page granularity this is if anything more
natural to guarantee than at row granularity, and the real motivating consumer (Sub0Llm's `ParallelExperts`,
up to 10 concurrent decode threads) makes duplicate concurrent fetches for the same hot plane a real,
expected occurrence, not an edge case.

## R13. Public semantics do not differ by platform; a genuine platform gap is stated honestly, not hidden

"No public API behavior differs by platform for a caller that does not opt into a platform-specific
extension. Where a platform genuinely lacks a mechanism another platform has (see OQ1 below), the
requirement this affects is documented as weakened on that platform — never silently degraded and never
smoothed over as if the guarantee were uniform."

Mirrors Sub0Firn's own R8. The honest caveat that requirement inherits and sharpens here: **no documented
Windows counterpart to Linux's `MADV_DONTNEED`/`MADV_PAGEOUT` demotion primitives for a read-only file
mapping was found in this project's own research** (`docs/prior-art.md` §2a). Until this is resolved (see
OQ1), R9's `wont_need` and R7's hard-budget enforcement on Windows may in practice reduce to "stop
prefetching and let the OS reclaim" — a materially weaker guarantee than the DPDK-style hard cap promises,
and this requirement obligates stating that plainly in the Windows implementation's own documentation
rather than claiming parity it does not have.

## Open questions carried as requirements-with-caveats, not silently resolved

Full detail in [docs/design.md](docs/design.md) §5 (OQ1–OQ7); listed here because each one bears directly
on whether a requirement above can be claimed as fully met on a given platform:

- **OQ1** (feeds R13, R9): Windows demotion primitive for a read-only mapping — unresolved.
- **OQ2** (feeds R3, R4): whether Linux `madvise(MADV_WILLNEED)` genuinely never blocks was not confirmed
  from a primary source — only `posix_fadvise(POSIX_FADV_WILLNEED)`'s non-blocking guarantee is
  independently confirmed. An implementation must re-verify before relying on `madvise` for R3's
  non-blocking guarantee on Linux.
- **OQ3** (architectural, feeds the whole design): hint the OS page cache (the whole economy of an
  `mmap`-based design) versus own a private buffer pool with direct async reads (PostgreSQL's own chosen
  direction after ~15 years of the former). Reasoned about, not settled, in `docs/design.md` §5.
- **OQ6** (feeds R5): whether the declared/speculative split is the right shape at all — no precedent
  found offers two first-class call shapes for this against one cache. Named as the design's most
  speculative element, not hidden as settled.
- **OQ7** (feeds R7, R10): the correct budget size for any real deployment is currently a guess; `stats`'
  `hint_unconsumed`/`resolve_misses` counters (part of every implementation, per this contract) exist
  specifically so a real run produces the trace that answers it later, following Bandana's own
  "simulate dozens of small caches" sizing technique (`docs/prior-art.md`).

## Non-requirements (explicitly out of scope)

Not repeated in full here — see README.md §1b for the full non-goals list. A change proposing to add any
of the following needs to argue why the project's scope should change, not just that the feature would be
convenient: row/table addressing, dtype conversion, content versioning/`invalidate`, remote/HTTP data
sources, general-purpose key-value caching, allocator replacement.
