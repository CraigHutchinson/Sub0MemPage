# Reconciliation with Sub0Firn's API

Sub0Firn's [`README.md` §3](https://github.com/CraigHutchinson/Sub0Firn/blob/main/README.md) gives:
`register_table`, `prefetch`/`wait`, `resolve_into`, `try_get`, `stats`, `invalidate`, under its R2 ("only
`resolve_into` and `wait` may block on I/O") and R5 (coalescing) requirements. This document is the
point-by-point divergence analysis: what Sub0MemPage keeps identical on purpose, what diverges and why
each divergence is either necessary or purely additive, and what is deliberately excluded from
Sub0MemPage's scope because it stays Sub0Firn's job.

**The point of this exercise**: adopting Sub0MemPage underneath Sub0Firn should be a *substitution*, not a
rewrite. Every divergence below is checked against that bar.

## Kept identical, on purpose

- **`prefetch` keeps its name and its contract.** Asynchronous, returns a ticket, never blocks. Renaming
  it `will_need` to echo `MADV_WILLNEED` would have been gratuitous incompatibility for no gain.
- **`wait(ticket)` keeps its name and its "blocks the calling thread only, never a global lock" guarantee**
  (Sub0Firn README §3b).
- **The "only two calls may block on I/O" rule is preserved exactly**: the only calls that may block on I/O
  are `resolve` (Sub0MemPage's synchronous, pinning analog of Sub0Firn's `resolve_into`) and `wait`. Every
  other call — `prefetch`, `try_resolve`, `wont_need`, `release`, `stats` — is non-blocking, and
  `try_resolve` inherits `try_get`'s never-blocks guarantee verbatim.
- **Coalescing is inherited unchanged** (Sub0Firn's R5): concurrent requests for overlapping ranges must
  produce one real I/O, not one per caller. At page granularity this is if anything more natural than at
  row granularity.
- **`stats` keeps its name and its "observability only, nothing depends on it" framing** (Sub0Firn's R10),
  extended with budget/headroom/high-water and the hint-efficacy counters Sub0Firn's own `stats` has no
  equivalent of today.

## D1 — Addressing unit: byte ranges, not `(table_id, row_index)`

**Necessary, and it is the layering line itself.** Sub0Firn's own R1 says it receives already-computed row
indices; Sub0MemPage sits one level below that and receives already-computed byte offsets. Sub0Firn's
existing `local_flat_file(path)` descriptor — "rows at `row_index * row_width_bytes`, `mmap`'d" — is
*already* a row→offset function, and its `local_sharded(shard_paths[], offset_resolver_callback)`
descriptor is already the general form. So a Sub0Firn built on Sub0MemPage keeps its R7 offset-resolver
contract exactly as written and simply calls `prefetch(region, resolved_ranges)` instead of implementing
paging itself.

**No Sub0Firn requirement changes.**

## D2 — Explicit pinning via leases; Sub0Firn has no such concept today

**This is the substantive divergence, and the one Sub0Firn's maintainer actually has to think about.**
Sub0Firn can currently omit pinning because both of its read paths sidestep the lifetime question:
`resolve_into` **copies** into a caller-owned buffer (after which residency is irrelevant to correctness),
and `try_get`'s zero-copy `row_view` has no documented lifetime rule at all — a gap that is invisible today
only because Sub0Firn's RAM tier is a private, never-evicted-mid-use structure. It becomes a real
dangling-view hazard the moment that tier is a budget-enforced region that can reclaim pages under
pressure — which is precisely what a Sub0MemPage-backed RAM tier would be.

Sub0MemPage hands back pointers *into a live mapping under active eviction pressure*, so residency
lifetime must be explicit, or it is a use-after-evict bug waiting to happen.

**Reconciliation, and it is clean**: `resolve_into` (copy semantics) is trivially implementable on top of
`resolve` + `memcpy` + `release`, so Sub0Firn's own contract can stay exactly as written; and if Sub0Firn
ever wants zero-copy `try_get` to be genuinely safe under a real budget, the lease is the mechanism that
makes it so. **Adopting Sub0MemPage would close an existing latent hole in Sub0Firn's own spec, not open
a new one.**

## D3 — A hard budget, exposed and enforced

Sub0Firn's README describes tiers a caller "configures per deployment" but names no budget parameter in
its own §3 contract and no exhaustion behavior. Sub0MemPage makes `budget_bytes` a `register_region`
parameter with a documented failure mode (REQUIREMENTS.md R7).

**Additive, and it maps straight onto a number a Sub0Firn deployment already has to pick** when sizing its
RAM tier — the natural suggestion is that Sub0Firn's own RAM-tier size becomes a Sub0MemPage region budget
verbatim, rather than a second, separately-tracked number.

## D4 — `wont_need` and `on_evict` have no Sub0Firn counterpart

**Additive, lower layer only.** `wont_need` is meaningful because Sub0MemPage's caller can know a range is
finished with — a decoded layer's experts, once that layer is past — in a way Sub0Firn's row-level
abstraction has no natural place to express. `on_evict` exists mainly *for* a higher layer like Sub0Firn —
see design.md §4: if Sub0Firn's RAM tier were backed by a Sub0MemPage region, Sub0Firn would need to know a
range went away so it can drop its own row-level index entries pointing into it, without Sub0MemPage's
enforcement path ever depending on Sub0Firn's callback completing correctly (the veto-rejection reasoning
in design.md §4 applies here directly).

## D5 — Class-tagged prefetch (`DECLARED` vs `SPECULATIVE`)

Sub0Firn's `prefetch` has no class parameter today. **Additive and defaultable**: absent an explicit
class, treat a Sub0Firn-originated prefetch as `DECLARED`, which reproduces Sub0Firn's current semantics
exactly — Sub0Firn's own row-index-based prefetch calls are, definitionally, the caller having already
decided it wants those exact rows, the same shape as a `DECLARED` hint. Sub0Firn's own design doc
(`tiered-storage-design.md` §1a) already names both access shapes ("precomputed working-set" and
"reactive") as first-class, so the concept is already present in Sub0Firn's own design — Sub0MemPage just
gives it an explicit parameter one layer down.

## D6 — No `invalidate`, no `version_tag`, no dtype

**Deliberate omission — this stays entirely Sub0Firn's job.** Sub0Firn's R4 (value immutability between
registration and `invalidate`), R6 (dtype conversion performed once, cached converted), and its
cross-process disk-tier staleness rules are all *semantic* guarantees about content. Sub0MemPage never
interprets bytes at all — REQUIREMENTS.md R1 takes Sub0Firn's own R9 ("row content is opaque") one step
further: Sub0MemPage does not even know a row's width, let alone its dtype. A file-content change under a
live mapping is a caller error at Sub0MemPage's layer; detecting it is entirely Sub0Firn's `version_tag`,
unchanged by anything here.

## D7 — No pluggable remote sources

**Deliberate, and it is the honest boundary of the layering — worth being blunt about, not glossing over.**
Sub0MemPage's regions must be memory-mappable, so Sub0Firn's `remote_http_range(base_url, ...)` source
descriptor **cannot** be a Sub0MemPage region. A Sub0Firn built on Sub0MemPage would use it for the local-
file tiers only (`local_flat_file`, `local_sharded`, and the `local_disk_cache_dir` sitting in front of the
HTTP source) and would keep its own HTTP Range client entirely to itself.

**Sub0MemPage is not a complete substrate for Sub0Firn — it is a substrate for Sub0Firn's local tiers.**
Any adoption proposal that claims otherwise is overselling it.

## D8 — Optional `open_stream`/`next`

No Sub0Firn counterpart. Strictly additive, and the design's most speculative element (design.md §2, §5's
OQ6). If it does not earn its keep in a real implementation, it can be dropped without touching anything
else in the contract.

## Net answer to the reconciliation question

Adopting Sub0MemPage beneath Sub0Firn would require **no change to any of Sub0Firn's R1–R10**, would leave
`prefetch`/`wait`/`try_get`/`resolve_into`/`stats` implementable with their published contracts intact,
would **close** the undocumented `try_get` view-lifetime gap (D2), and would leave the HTTP/remote tier and
all content semantics exactly where they already are (D6, D7). The only genuinely new concept Sub0Firn
would have to absorb internally, if it chose to adopt Sub0MemPage as its own local-tier substrate, is the
lease.

This is stated as a design analysis, not a commitment either project has made — Sub0Firn's own maintainer
is the one who decides whether or when to make this substitution, per Craig Hutchinson's own framing of
the relationship (quoted in [README.md](../README.md) §2): *"Sub0Firn could be another use-case for the
Sub0MemPage library"* — a plausible future consumer, not a forced dependency.
