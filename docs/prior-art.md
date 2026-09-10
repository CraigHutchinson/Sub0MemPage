# Prior art

Real systems, papers, and OS documentation researched before designing Sub0MemPage, fetched and cited
directly rather than recalled from training data — the same discipline
[Sub0Firn/docs/prior-art.md](https://github.com/CraigHutchinson/Sub0Firn/blob/main/docs/prior-art.md)
uses one layer up: verify against the actual reference source before designing against it, quote the
source, don't paraphrase from memory. This document consolidates three separate research passes
(OS/mmap mechanics, library-level API shapes, and on-this-machine empirical measurement) rather than
re-deriving any of them.

**This document does not repeat Sub0Firn's own prior-art table** (HugeCTR HPS, Bandana, TT-Rec,
MoE-Infinity, DLRM hot/cold splitting, LMDB vs. RocksDB) — that table is about *what to cache and why* at
the row/table layer; read it directly at the link above. This document is entirely about *how the bytes
actually move* and *how a library exposes that mechanism* — the layer below it.

**Confidence tags**, used consistently across all three source streams:
- **High** — primary source fetched and quoted verbatim (a Microsoft Learn page, a Linux man page, a
  paper's own PDF text extracted locally, a raw-text-returned web page).
- **Medium** — fetched, but rendered/paraphrased by an automated summarizer rather than independently
  re-checked as raw text; or a well-corroborated general finding across several non-primary sources.
- **Title-only / search-confirmed** — a real, citable source located via search but not independently
  fetched. Any number attached to a title-only row is unverified.

---

## 1. Windows memory-mapped file and async-I/O mechanics

| Finding | Evidence | Confidence |
|---|---|---|
| **A hard page fault on a mapped view is, by definition, a synchronous blocking read of the backing file, and Windows provides no asynchronous page-fault mechanism at all.** This is the single most load-bearing Windows citation in this project. | `learn.microsoft.com/.../memory/working-set`: *"A **hard page fault** must be resolved by reading page contents from the page's *backing store*... A **soft page fault** can be resolved without accessing the backing store."* KB156932 (archived, NT-era, but its architectural claim is the load-bearing part, not its stale numbers), verbatim: *"The memory manager in Windows doesn't provide an asynchronous page fault mechanism to manage the file mappings used by the cache manager... If you issue numerous I/O operations for data that is not in the cache, the cache manager and memory manager become saturated and your requests are made synchronous."* | **High** (architectural claim); **Low** (KB156932's specific "three worker threads" figure — an NT-era number, not evidence about Windows 11) |
| **`PrefetchVirtualMemory` turns many faults into few large concurrent I/Os, takes a *scattered* range list in one call, does not add pages to the working set, and is a hint that can partially fail.** | `learn.microsoft.com/.../nf-memoryapi-prefetchvirtualmemory`, verbatim: *"the API will efficiently bring in those address ranges from disk using large, concurrent I/O requests where possible."* … *"The prefetched memory is not added to the target process' working set; it is cached in physical memory."* … *"it is treated as a **strong hint** by the system and is subject to usual physical memory constraints where it **can completely or partially fail under low-memory conditions**."* Available since Windows 8; signature is `NumberOfEntries` + an array of `WIN32_MEMORY_RANGE_ENTRY`. | **High** |
| **Whether `PrefetchVirtualMemory` blocks before returning is undocumented — must be measured, not assumed.** | Absence of a stating source, noted as such. | **None — open, see §6/OQ** |
| **Evicting a mapped file page does not throw the data away — it becomes a transition page and re-faulting it is a *soft* fault, cheap.** Read-only mapped pages are the cheapest memory on the machine to reclaim (clean, no write-back). | `learn.microsoft.com/.../memory/working-set`, verbatim: *"Transition pages remain cached in RAM until the page is either referenced again by some process or repurposed."* Read-only-cheap-to-evict claim corroborated across several Windows-Internals-derived secondary sources (PFN database standby-list behaviour), primary text not independently fetched. | **High** (transition-page quote); **Medium** (cheap-eviction corroboration) |
| **No documented Windows counterpart to `MADV_DONTNEED`/`MADV_PAGEOUT` for a read-only file mapping was found.** `SetProcessWorkingSetSize` is advisory only and does not pin or demote anything reliably (*"does not guarantee that the requested memory will be reserved, or that it will remain resident at all times"*), and its own docs warn working-set sizes are allocated system-wide first-come-first-served. | Absence of a stating source for demotion. `SetProcessWorkingSetSize` quote: **High**. | **None for demotion — the single largest open platform gap this project carries, see REQUIREMENTS.md OQ1** |
| **Overlapped I/O (`FILE_FLAG_OVERLAPPED`) is genuinely per-request async, but "asynchronous" is not guaranteed** — NTFS compression/encryption, file-extending writes, and cache-hit/cache-miss-saturation all silently degrade it to synchronous. `FILE_FLAG_NO_BUFFERING` is Microsoft's own stated remedy for guaranteed async, at the cost of losing all page-cache reuse and requiring sector-aligned I/O. | `learn.microsoft.com/.../synchronous-and-asynchronous-i-o`; KB156932. Both quoted verbatim in the source research. | **High** |
| **IOCP's concurrency value should be set to the CPU count; new code should prefer the thread-pool I/O API (`CreateThreadpoolIo`) over raw IOCP for "tens of async operations" — exactly this project's real scale (~10 experts/layer).** | `learn.microsoft.com/.../i-o-completion-ports`, verbatim: *"The best overall maximum value to pick for the concurrency value is the number of CPUs on the computer."* … *"For new server applications, consider the thread pool API first."* | **High** |
| **DirectStorage is Windows' purpose-built answer to many small scattered NVMe reads at low CPU cost, and its design notes are the closest thing to a spec for this exact workload shape** — but it is a console/DX12-oriented API, not a general-purpose library, and its console-specific guarantees do not transfer to desktop. Read-performance guidance: *"a large jump starting with 32 KiB reads... tops out with 64 KiB"* — i.e. a multi-MiB read (this project's real expert-plane size) is already well past the point of diminishing returns as a single request; the win is in issuing many of them concurrently, not in splitting them further. Anti-throttling guidance: *"enqueue requests as soon as they're created"* rather than buffering client-side. | `learn.microsoft.com/.../directstorage-overview` (GDK/Xbox page, quoted verbatim) | **High** (quotes); explicitly caveated as console-scoped, **Medium** for any desktop-specific claim |

## 2. Linux mmap/async-I/O mechanics

| Finding | Evidence | Confidence |
|---|---|---|
| **`posix_fadvise(POSIX_FADV_WILLNEED)` is explicitly documented as non-blocking** — the cleanest documented async-prefetch statement on either platform. | `man7.org/.../posix_fadvise.2`, verbatim: *"POSIX_FADV_WILLNEED initiates a nonblocking read of the specified region into the page cache."* Advice is non-binding: *"merely constitutes an expectation on behalf of the application."* | **High** |
| **`madvise(MADV_WILLNEED)`'s blocking behaviour is NOT documented, and historically it did block** — two independent fetch passes (one in each source research stream) confirm the man page states no synchrony guarantee at all, only *"Expect access in the near future. (Hence, it might be a good idea to read some pages ahead.)"* Kernel-history evidence (title-only, not independently verified) reports a case going from 2.48s to 61µs when moved off the synchronous `MADV_WILLNEED` path. **Do not assume `MADV_WILLNEED` returns immediately** — this must be re-verified against kernel source before being relied upon. | `man7.org/.../madvise.2`. | **High** (man-page absence of a synchrony statement, confirmed independently twice); **Title-only** (the specific blocking-history numbers) |
| **`MADV_POPULATE_READ` is the one honestly-documented "fault these in now" primitive** — it does not pretend to be async. Right call from a helper thread; wrong one inline. | `man7.org/.../madvise.2`, verbatim: *"faulting in all pages in the range just as if manually reading from each page."* | **High** |
| **The default readahead wastes 128 KB per 4 KB fault under `MADV_NORMAL`** — helpful for this project's multi-MiB reads, pure waste for Sub0Firn's own 320-byte rows (an asymmetry worth being explicit about, since Sub0MemPage serves both shapes of caller). | CIDR 2022 paper, quoted in §4 below. | **High** |
| **`io_uring` is the real async answer**: batched submission (many SQEs, one `io_uring_enter` call), completions gathered out of order, `IORING_SETUP_SQPOLL` for a kernel-side polling thread that eliminates the enter syscall entirely. | `man7.org/.../io_uring.7`, quoted verbatim in the source research. | **High** |
| **Registered buffers cut per-I/O overhead but cannot register the mapping itself** — they must be anonymous, non-file-backed memory (`malloc`/`MAP_ANONYMOUS`), max 1 GiB each. This is architecturally load-bearing: an io_uring-backed Sub0MemPage implementation needs its own scratch arena as the read destination, it cannot register the region being managed. | `man7.org/.../io_uring_register.2`, quoted verbatim. | **High** |
| **io_uring's exact maximum queue depth was not confirmed from a primary source** (commonly-cited 32768 SQEs is unverified) — irrelevant here since this project's real concurrency (10–500 in flight) is orders of magnitude below any plausible cap. | Absence of source, stated as such. | **None for the number; High for "the docs don't state it"** |
| **Interaction between an `O_DIRECT`/unbuffered prefetch and a coexisting `mmap` of the same file is NOT guaranteed to share the page cache** — direct I/O bypasses the page cache entirely and produces a private second copy rather than warming the mapping. Buffered reads and `mmap` do share the page cache (general Linux behaviour, well-established but not independently re-fetched this pass). | Follows directly from `FILE_FLAG_NO_BUFFERING`/`O_DIRECT`'s own documented bypass semantics. | **High** (the O_DIRECT non-interaction); **Medium** (the buffered/mmap coherence claim) |

## 3. Off-the-shelf transport libraries — the honest answer

There is **no off-the-shelf "async prefetch scheduler with a working-set budget."** There are good
*transport* libraries; the *policy* layer (what to warm, how much stays resident, what gets dropped) does
not exist as a reusable component anywhere located in this research — every system surveyed that has one
built it itself, mirroring Sub0Firn's own honest finding about HugeCTR HPS (the shape is standard, a
general reusable component is not).

| Candidate | Verdict | Confidence |
|---|---|---|
| **liburing** (Linux io_uring userspace library) | Use it as the Linux transport. Provides nothing on Windows — Linux-only by construction. | **High** for the underlying syscalls' man pages; the library itself is title-only |
| **Boost.Asio** | The strongest single off-the-shelf candidate — genuinely portable, and maps onto the right two backends. Verbatim: *"This feature requires I/O completion ports on Windows, and io_uring on Linux (define BOOST_ASIO_HAS_IO_URING to enable)."* `stream_file`/`random_access_file` with `async_read_some_at()` is exactly the scattered-positional-async-read operation this project needs. Caveats: io_uring support is opt-in behind a build macro, brings an executor/coroutine model to reconcile with a no-allocation-hot-path rule, and does not address the mapping at all — it is an alternative *to* mapping, not an accelerator *of* it. | **High** (quoted) |
| **Meta's folly** (`AsyncIO`/`IoUringBackend`) | Reject. No evidence of a Windows file-I/O backend; pulling folly in for a Linux-only half-answer is a large dependency for little gain. Windows-unsupported claim rests on absence of evidence, stated honestly as such. | **Medium** |
| **libuv** | Reject for the fast path. Its own docs: *"The thread pool is internally used to run all file system operations"* — i.e. its "async" file I/O is blocking reads on worker threads, structurally the same shape as the production defect this project exists to fix (§4 below), just with someone else's threads. Notably added then reverted io_uring support (v1.49.0). | **Medium** |
| **Microsoft DirectStorage (desktop)** | Interesting, not adoptable — Windows-only, DX12/GPU-centric, separate redistributable. Its design guidance (§1) is more valuable than its API. | **Medium** |

**Recommended shape given the above**: a thin platform seam with exactly two implementations —
`PrefetchVirtualMemory` (+ optionally overlapped I/O/IOCP into an owned arena) on Windows, `io_uring`/
`posix_fadvise` on Linux — with the policy (what to warm, budget, eviction) written once above that seam.
Boost.Asio is the only credible way to avoid writing the two transports by hand, at the cost of a real
dependency and an executor model — a design decision for an implementation to make, not settled here.

## 4. The CIDR 2022 paper — highlighted, given how load-bearing it is

**Crotty, Leis, Pavlo — *"Are You Sure You Want to Use MMAP in Your Database Management System?"*, CIDR
2022.** Fetched and text-extracted locally from the paper's own PDF (`db.cs.cmu.edu/papers/2022/
cidr2022-p13-crotty.pdf`) after the standard fetch tool returned undecoded binary — these are the paper's
own words. **Confidence: High.**

**The three named bottlenecks**, all properties of the mapping mechanism, none of the storage device:

> "we have identified three key bottlenecks that plague mmap-based file I/O: **(1) page table contention,
> (2) single-threaded page eviction, and (3) TLB shootdowns.**"

**No async faults, and the exact workaround this project's own motivating defect independently reached
for is named and rejected by the paper's own authors:**

> "mmap does not support asynchronous reads... Yet another possibility is to **spawn additional threads to
> prefetch (i.e., attempt to access) pages so that they will block in the event of a page fault rather
> than the main thread**. However, although these solutions may (partially) resolve some issues, they all
> introduce significant additional complexity, which defeats the purpose of using mmap in the first
> place."

**The measurement, 100 threads, random reads:**

> "mmap was initially similar to fio during the first 27 seconds, then dropped to nearly zero for about
> five seconds, and finally recovered to approximately half of fio's performance. This sudden drop in
> performance occurred when the page cache filled up, forcing the OS to begin evicting pages from
> memory... In summary... **mmap is 2–20× worse than fio**."

**Real production reports the paper itself collected:**

> "the developers identified the source of the problem as contention on a shared mmap write lock. By
> switching to read system calls, the queries became fully CPU-bound."
>
> "The time series DBMS VictoriaMetrics identified problems with mmap's blocking I/O for page faults."

**Its own closing advice, verbatim**: *"When you should not use mmap in your DBMS: ... You want to handle
page faults without blocking on slow I/O or need explicit control over what data is in memory."*

**Honest scoping**: experiments are Linux v5.11, AMD EPYC 7713, 10× NVMe SSDs, **4 KB random reads**, 100
threads. This project's own real workload is Windows, ~10 threads, multi-MiB reads, one laptop NVMe. The
*numbers* do not transfer. What transfers is the *mechanism inventory* — page-table contention,
single-threaded eviction, TLB shootdowns, no async fault path — stated by the paper's own §3.2/§3.4 as
properties of `mmap` itself, not of that hardware. Whether Windows' fault-path lock granularity matches
Linux's is genuinely unknown (no Windows-side equivalent study was located) — see `docs/design.md`'s OQ3.

## 5. Library-level API-shape precedent

| System | The one thing it contributes | Confidence |
|---|---|---|
| **CUDA Unified Memory** (`cudaMemAdvise`/`cudaMemPrefetchAsync`) | Advice and movement are separate calls; a prefetch never gates correctness (*"Memory accesses to this range are always coherent and are allowed even when the data is actively being migrated"*); `SET_ACCESSED_BY` is the strongest existing precedent for a declared-access hint, but it is range-persistent standing policy, not per-invocation — a real, stated limit on how far this precedent transfers. | **Medium** (summarizer-rendered; signatures High) |
| **RocksDB async I/O** | Three-tier async surface kept side by side (a boolean on the sync call, a retry-status protocol, genuinely async callback/coroutine entry points); the backend contract's honesty is the reusable idea — `SubmitReadAsync` *"returns true for a non-blocking submission and false when it used the synchronous fallback"* — a much more honest contract than a `void` hint. | **Medium** |
| **PostgreSQL Read Stream** | The most novel shape found: inverted control — the caller hands over a callback that produces its own next byte range, and the library decides how far ahead to run. Directly reused as this project's optional `open_stream`/`next` shape. | **Medium** |
| **PostgreSQL 8.4 → 18's async-I/O history** | A genuinely important cautionary precedent: PostgreSQL spent ~15 years on "hint the kernel via `fadvise`, let the page cache do the work" and then deliberately moved off it in v18 to owning its own buffer pool with direct async reads, because *"this advisory mechanism only hinted to the kernel to load data into the OS page cache, not into Postgres' own shared buffers."* This project's situation differs in one specific, load-bearing respect: because its regions are `mmap`'d, a page-cache hint *is* a hint that lands in the consumer's own address space with no second copy — PostgreSQL's reason (a) for moving away does not apply here; its reason (b) (insufficient control over kernel heuristics) does. Reasoning, not a cited finding — see `docs/design.md` OQ3. | **Medium** |
| **`effective_io_concurrency` default = 16** | A real DBMS's own answer to "how many scattered reads should be in flight" — and it considers 1 (reactive, one-at-a-time) bad enough to have changed the default away from it. The number itself should not be transplanted (8 KB pages vs. multi-MiB reads is a three-orders-of-magnitude size mismatch); the *fact* that a mature system treats reactive fetch as a bug worth fixing should be. | **High** (docs) |
| **DPDK `rte_mempool`** | Budget as a hard, declared, creation-time constant; exhaustion is a *reported failure*, never a silent overshoot — the single most valuable idea taken from this source, and the deliberate alternative to Redis's soft `maxmemory`. Per-core caches with bulk sync to a shared structure, independently re-derived by Caffeine (below) from a completely different direction. Separate `avail_count`/`in_use_count` queries, the precedent for `stats`' headroom/pinned split. | **Medium** |
| **Redis eviction (approximated LRU / Morris-counter LFU)** | Sampling + a candidate pool replaces an exact global LRU list entirely — no pointer-chasing, no contention point. Decay is mandatory, not optional, or a frequency counter becomes a permanent, non-adapting record of the past. | **High** (raw page text) |
| **Caffeine / W-TinyLFU** | Record-then-replay concurrency shape: a hot-path access appends to a striped, thread-local ring buffer; one thread later drains and applies reordering under a single lock — *"batches the work and spreads the cost across many threads"* rather than locking on every access. Independently converges with DPDK's per-core cache from a completely different domain. | **Medium** |
| **Linux MGLRU** | The single most load-bearing eviction-bookkeeping citation for this project's own API shape: the kernel does **not** ask anyone to report accesses — it reads hardware accessed bits in bulk, on its own schedule. Directly informs REQUIREMENTS.md R10 ("no per-access caller cooperation required"), because this project's real consumer is a hot loop that must call into the library zero times per access. | **Medium** |
| **MoE-Infinity (arXiv:2401.14361)** | A genuinely useful **negative** finding, not smoothed over: its abstract describes *tracing* (learning) expert-activation sparsity statistically, not consuming a router's own declared foreknowledge. Even a system with full model access chose the inferred/reactive path over a declared one — worth knowing before assuming the declared/speculative split (R5) is an obviously-superior established pattern. | **Medium** |

## 6. Where the evidence is thin, or disagrees — stated honestly, not smoothed over

1. **Whether concurrent hard faults on *different* pages of the *same* Windows section object serialize
   on a section, VAD, or working-set lock is not established by any Microsoft source located.** Every
   Windows-side lock-contention argument in this project's design is inference from the Linux-side
   literature (`mmap_lock`, LWN), not evidence about Windows. The single largest gap in this document.
2. **Whether `PrefetchVirtualMemory` blocks before returning is undocumented.** Cheap to measure; must be
   measured before being designed around as a latency-hiding primitive.
3. **`madvise(MADV_WILLNEED)` and `posix_fadvise(POSIX_FADV_WILLNEED)` disagree in their own
   documentation** on whether they are non-blocking. Treating them as equivalent is a real trap.
4. **The CIDR paper is Linux and 4 KB reads; this project's real reads are multi-MiB.** Per-page costs
   (page-table contention, TLB shootdown pressure) are amortised roughly 1000x lower per byte at this
   project's real read size — a real reason to expect the CIDR mechanisms are *not* the dominant effect
   here, corroborated by this project's own empirical study (§7 below), which found none of page-table
   contention, TLB pressure, or page-cache exhaustion to be the actual bottleneck on this machine.
5. **No Windows-side equivalent of the CIDR measurement was found at all** — no published study of
   `MapViewOfFile` fault throughput vs. overlapped `ReadFile` under thread scaling on NVMe was located.
6. **io_uring's exact maximum queue depth and the specific percentage gains reported for registered
   buffers/NVMe passthrough (11%, 20%) are title-only** and should be treated as unverified; neither
   affects this document's conclusions.
7. **PostgreSQL's `effective_io_concurrency = 16` is simultaneously the strongest quantitative read-across
   and the weakest analogy** in this document — an 8 KB-page DBMS default transplanted onto a multi-MiB
   read workload three orders of magnitude larger per unit.
8. **§3c/§5's PostgreSQL history and this project's own `mmap`-is-the-right-substrate framing point in
   mildly opposite directions** on the central architectural question (hint the page cache vs. own a
   private buffer pool) — the resolution offered (`docs/design.md` OQ3) is reasoning, not a cited finding.

## 7. This machine's own empirical measurement — where the real bottleneck actually is

Full raw numbers, counters, and exact reproduction commands live in the empirical-concurrency research
session this document consolidates (session scratchpad, not part of this repo — see
`docs/design.md` §3 for the load-bearing subset reproduced with commentary). Summary of what was
directly measured, not estimated, on the real 37.11 GiB Sub0Llm MoE-expert sidecar, on this exact machine
(Core Ultra 9 275HX, NVMe `D:`), 2026-09-09:

- **Windows mmap DOES scale under concurrent faults from disjoint threads** — 5.1–5.6x at 10 threads,
  6.7–7.3x at 16 threads, refuting the a-priori hypothesis that Windows serializes hard faults on one
  section object for disjoint pages.
- **The NVMe device is nowhere near saturated by the real decode workload** — it sustains 6.34 GB/s to a
  16-deep overlapped queue on the exact same file, while the real production decode workload requests
  only 55–108 MB/s at an average disk queue depth of 0.04–0.20.
- **Explicit overlapped I/O beats `mmap` by 1.5–2.9x at matched concurrency and ~1.47x on ceiling** — a
  real, reproducible, measured effect, but **not** the order-of-magnitude effect that would explain a
  production 4–5x scaling shortfall.
- **`FILE_FLAG_NO_BUFFERING` measured WORSE, not better**, at matched depth on this hardware — 3.7x
  slower than buffered overlapped I/O at depth 16, and did not scale with queue depth at all. Reported as
  a genuine, unexplained measurement rather than reconciled away.
- **Prefetch depth, not faster individual faults, is the single largest lever by an order of magnitude**:
  93% of this device's measured ceiling throughput is reached by just 8 outstanding reads.

This empirical study is also where B21 — the concrete, measured, currently-unresolved production defect
that motivates this entire project — was confirmed by direct instrumentation rather than inferred from
throughput alone. See `docs/design.md` §3 and `docs/sub0llm-consumer-trace.md` for the full account.
