# Requirements

Normative contract for Sub0MemPage. Each requirement below is stated as a testable sentence first, then
explained — the sentence is what an implementation is checked against; the explanation is why it says
that and not something weaker. Sourced from the three research streams consolidated in
[docs/prior-art.md](docs/prior-art.md) and the design synthesis in [docs/design.md](docs/design.md) —
nothing here is asserted without a concrete cited source or a measured finding behind it. Follows
[Sub0Firn/REQUIREMENTS.md](https://github.com/CraigHutchinson/Sub0Firn/blob/main/REQUIREMENTS.md)'s own
style exactly, one layer down the stack.

## R1. Sub0MemPage owns residency, not addressing, and not content — and never allocates bulk data storage

"Sub0MemPage receives only a region/pool handle and byte ranges within it; it does not derive, hash, or
interpret how a byte range was computed, never reads or validates the bytes it manages beyond their
extent, and never allocates the memory those bytes land in — that memory is always caller-allocated and
caller-owned (`register_slots`, README.md §3)."

Translating `(table_id, row_index)` or `(layer, expert)` into a byte range is entirely the caller's job —
exactly the boundary Sub0Firn's own R7 offset-resolver callback already draws, viewed from the layer
below it (`docs/design.md` §1). Baking any addressing or content semantics into Sub0MemPage would tie a
generic residency-management engine to one caller's domain, defeating the reason it exists as a separate,
lower project at all. The caller-owned-storage half of this requirement is the same boundary seen from the
allocation side, not a separate rule: a library that allocated its own typed destination storage would
necessarily need to know that storage's size and shape, which is exactly the content knowledge this
requirement forbids (`docs/design.md` §8, `docs/prior-art.md` §5a).

## R2. In the secondary mmap mode, every range is always legally readable, independent of any Sub0MemPage call

"For a region registered with a caller-supplied mapped view (the secondary, opportunistic mode —
`docs/design.md` §8), a caller may dereference any address inside that mapping at any time, whether or not
`prefetch`, `resolve`, or any other call has ever been made for that range. The worst case is the page
fault the caller would have taken with no Sub0MemPage in the picture at all. This guarantee does NOT
extend to the primary caller-slot mode: there, a slot's destination bytes are only valid once `resolve`,
`wait`, or `try_resolve` reports them resident — the same contract every real precedent researched for
caller-owned destinations makes (a POSIX `aiocb`'s `aio_buf`, a `DSTORAGE_REQUEST::Destination`, a
`cuFile`-registered pointer are all only valid once their own operation completes), not a weaker one."

Follows CUDA's own documented guarantee — *"accesses to this range are always coherent and are allowed
even when the data is actively being migrated"* — and Windows' own framing of `PrefetchVirtualMemory` as
*"purely a performance optimization: prefetching is not necessary for accessing the target address
ranges"* (`docs/prior-art.md` §1, §2a) — both of which are statements about an existing live mapping, and
so only transfer to Sub0MemPage's own secondary mmap mode, not to the primary caller-slot mode where no
such mapping need exist at all. **In the mode where it applies, Sub0MemPage never gates correctness — only
latency** — this is the property that makes the secondary mode safe to adopt incrementally around
existing mapping-based code (unchanged from the original intent); the primary mode makes a different,
equally standard promise instead (R3, R8).

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

## R7. The slot pool's capacity is a hard cap fixed by the caller's own allocation; exhaustion is a reported failure, never a silent overshoot

"`register_slots(region, slot_bytes, num_slots, slots_ptr)`'s `num_slots * slot_bytes` IS the hard cap on
concurrently-resident bytes for that pool — there is no separate `budget_bytes` parameter and no runtime
resize call. When a `resolve` cannot be satisfied because every slot is pinned and none can be reclaimed,
it fails with a reported pool-exhausted outcome rather than admitting the request and exceeding the
caller's own allocation (which, being caller-owned, Sub0MemPage cannot exceed even if it wanted to —
there is no slot beyond the ones the caller supplied)."

The DPDK `rte_mempool` precedent, deliberately chosen over Redis's `maxmemory`, which the Redis docs
themselves admit *"might temporarily exceed the limit by a large amount"* (`docs/prior-art.md` §4a) —
sharpened one step further than the original draft once ownership resolved caller-side (R1, `docs/design.md`
§8): the cap is no longer a number Sub0MemPage must be told and then police against memory it owns: it is
structurally enforced by the caller's own allocation size. A library whose entire reason to exist is
fitting a huge file into a real, finite RAM budget cannot afford "approximately respected" — the failure
mode of exceeding it is not degraded performance, it is the OOM/thrash the budget existed to prevent.

## R8. No slot is ever reused while pinned; a lease is the only thing that prevents reuse

"A slot named by a lease returned from `resolve` or `try_resolve` is never reused to hold a different
byte range while that lease is held, regardless of pool pressure. `release`d slots become reuse/eviction
candidates ordered by policy, not reused eagerly."

This is the mechanism that makes handing back a lease over a caller-owned, Sub0MemPage-managed slot safe
at all — without it, a caller reading a "resolved" slot under memory pressure risks another resolve
overwriting it out from under a still-live read (a use-after-evict bug, unchanged in substance from the
original mapping-based framing — only whose memory is being protected changed, per R1/`docs/design.md`
§8). See `docs/sub0firn-reconciliation.md` D2 for why this is the one concept Sub0Firn itself lacks today,
and note the pleasant convergence R1's own note already makes: this is now the *same* shape Sub0Firn's own
`resolve_into` already has, one layer down, not a new concept its maintainer has to learn.

## R14. Sub0MemPage never allocates the destination storage bytes land in

"Every call that fills or resolves byte-range content writes into memory the caller supplied at
registration time (`register_slots`'s `slots_ptr`); Sub0MemPage's own internal allocations, if any, are
limited to small per-slot bookkeeping metadata (a key and a liveness flag per slot, generalizing
`ExpertCache`'s own `key_`/`live_` arrays — `docs/prior-art.md` §5a), never the bulk data itself."

Every real storage/transport precedent researched for this specific question takes this shape — `io_uring`
registered buffers (*"the application always supplies the memory pointer"*), POSIX `aio_read`'s
caller-owned `aio_buf`, NVIDIA GPUDirect Storage's explicit *"cuFile relies on users to complete their own
allocation"*, and Microsoft DirectStorage's *"the hardware will write directly into the buffer that's
provided by the title"* plus its own best-practice warning against allocating a fresh buffer per request
(`docs/prior-art.md` §5a — every citation there is High confidence and freshly fetched for this
requirement specifically). It also keeps R1 airtight from the allocation side, and matches this whole
project family's own standing no-runtime-allocation-on-hot-paths discipline (Sub0Llm's own `AGENTS.md`
§1) — a library that allocated its own bulk destination storage would be reintroducing exactly the kind of
hot-path allocation its real, motivating consumer has spent its own engineering effort eliminating
everywhere else.

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
mapping was found in this project's own research** (`docs/prior-art.md` §2a). This gap is now scoped
narrower than the original draft: it affects only the **secondary mmap mode** (§1's opportunistic
zero-copy path), where actually removing a page from residency genuinely depends on an OS primitive that
may not exist on Windows. It does **not** affect R7/R9 for the **primary caller-slot mode** (R1, R14),
because "reuse" there is Sub0MemPage's own bookkeeping decision over memory the caller already owns — no
OS cooperation is needed to mark a caller-owned slot reusable, since nothing has to be evicted from
anywhere, only overwritten on the next fill. `wont_need` and the hard cap keep their full documented
strength in the primary mode on every platform; OQ1 is a real, unresolved gap only for a caller that
opts into the secondary mode.

## Open questions carried as requirements-with-caveats, not silently resolved

Full detail in [docs/design.md](docs/design.md) §5 (OQ1–OQ7); listed here because each one bears directly
on whether a requirement above can be claimed as fully met on a given platform:

- **OQ1** (feeds R13, R9) — narrowed, not closed: Windows demotion primitive for a read-only mapping is
  still unresolved for the **secondary mmap mode only**; it no longer affects R7/R9 in the primary
  caller-slot mode (R13's own text above explains why).
- **OQ2** (feeds R3, R4): whether Linux `madvise(MADV_WILLNEED)` genuinely never blocks was not confirmed
  from a primary source — only `posix_fadvise(POSIX_FADV_WILLNEED)`'s non-blocking guarantee is
  independently confirmed. Now relevant only to the secondary mmap mode, since the primary mode's async
  fill uses explicit `io_uring`/overlapped I/O rather than a page-cache hint. Re-verify before relying on
  `madvise` for the secondary mode's own guarantee on Linux.
- **OQ3 — RESOLVED, 2026-09-10.** Was: hint the OS page cache vs. own a private buffer pool with direct
  async reads. Resolved in favor of the latter as the primary mode, with the caller owning the buffer pool
  (R1, R14) — full reasoning in `docs/design.md` §8, decisive additional evidence beyond the original
  PostgreSQL precedent is this project's own B21 finding (`docs/design.md` §6).
- **OQ6** (feeds R5): whether the declared/speculative split is the right shape at all — no precedent
  found offers two first-class call shapes for this against one cache. Named as the design's most
  speculative element, not hidden as settled.
- **OQ7** (feeds R7, R10): the correct pool sizing (`num_slots`/`slot_bytes`) for any real deployment is
  currently a guess; `stats`' `hint_unconsumed`/`resolve_misses` counters (part of every implementation,
  per this contract) exist specifically so a real run produces the trace that answers it later, following
  Bandana's own "simulate dozens of small caches" sizing technique (`docs/prior-art.md`).
- **New, from R14**: whether a single uniform `slot_bytes` per pool (matching `ExpertCache`'s own
  same-shaped-slots precedent exactly) is general enough for a future consumer with genuinely
  variable-sized ranges, or whether a non-uniform-slot variant will eventually be needed, is untested —
  named here rather than assumed away, since the only real consumer characterized so far (Sub0Llm's MoE
  experts) happens to have uniform-sized planes.

## Non-requirements (explicitly out of scope)

Not repeated in full here — see README.md §1b for the full non-goals list. A change proposing to add any
of the following needs to argue why the project's scope should change, not just that the feature would be
convenient: row/table addressing, dtype conversion, content versioning/`invalidate`, remote/HTTP data
sources, general-purpose key-value caching, allocator replacement.
