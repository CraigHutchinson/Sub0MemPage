# Investigation: unbuffered reads plateau at ~3 GB/s on the development host

Status: **cached-reader trigger reproduced; direct-session overlap fixed in diagnostics**, 2026-10-01.
LocalFileBackend integration remains deliberately unchanged pending consumer lifetime qualification.
Self-contained. Nothing from the session that found it is needed
to pick it up.

## The problem

On the development host, `FILE_FLAG_NO_BUFFERING` reads into caller-owned buffers reach only about half
the throughput of cold buffered reads of the same file. The unbuffered path appears to have a fixed cost
of several milliseconds per request.

| Random 1.78 MB reads, depth 16 | Throughput |
|---|---:|
| Buffered, cache evicted (verified cold) | 5.1-5.6 GB/s |
| Unbuffered | 2.9-3.0 GB/s |

Unbuffered is faster at depth 1 (2.1-2.4 GB/s against 1.2-1.5 GB/s), then stops scaling from depth 4.

## Why it matters

MemPage's primary mode is caller-owned slots filled by explicit reads, and an owned cache only pays off
when it is the only copy. Buffered fills also leave a copy in the OS standby cache, so the same bytes are
cached twice and RAM goes to the OS's generic LRU instead of the workload-aware pool. Unbuffered fills
avoid this, but today they cost about half the miss bandwidth.

The motivating consumers are Sub0Llm's routed-MoE expert cache, with expert blobs of about 1.5-1.8 MB,
and its n-gram embedding table, which is 102 GB of 320-byte rows (Sub0Llm
`docs/STORAGE_STACK_PLAN.md`, "Core use case"). Until this is resolved, fills default to buffered, and
unbuffered stays an explicit, measured mode.

### Update 2026-10-01: the copy is the miss cost under decode load

Sub0Llm's owned expert cache now beats reactive mmap at matched memory (+3%), so the miss is the
remaining cost: about 51k blocking misses per 2,000 tokens, averaging ~500 us each, about 8% of decode.
`tools/miss_probe/` breaks that time down (warm page cache, one 1,766,400-byte expert per miss, 10
readers, 256 KiB chunks, a 2.8 ms busy gap between misses as in decode):

| Measurement | p50 per expert |
|---|---:|
| MemPage pool (TransferSet + LocalFileBackend), quiet host | 273 us |
| Same, a 28 KB read (fixed hand-off cost only) | 42 us |
| Ideal pool: spinning threads, per-thread handles (`raw_copy_probe`) | 163-185 us (~10 GB/s ceiling) |
| One synchronous read on the calling thread | 264 us (~7 GB/s) |
| MemPage pool, DRAM saturated by `bw_hog` (8-14 threads, ~90 GB/s) | 367-425 us |
| One synchronous read, DRAM saturated | 510-718 us |
| Engine, sidecar warm, default vs `KMP_BLOCKTIME=0` (mean) | 520 vs 486 us |

Ruled out:
- **Hand-offs:** about 40 us per miss.
- **The shared file handle:** per-thread handles are no faster.
- **Waiter wake-up:** spinning 500 us before parking changed nothing.
- **OpenMP spin-waiting:** about 7%.

The buffered copy itself is the cost:
- It peaks at ~10 GB/s however it is parallelised.
- Under decode's DRAM load it slows to the engine's ~500 us.

The only large lever left is not copying: DMA straight into the pinned pool with unbuffered reads.
That makes this investigation the critical path for a cheaper miss, not only for avoiding double
caching.

```
clang++ -std=c++23 -O3 -I include tools/miss_probe/miss_probe.cpp -o miss_probe.exe
clang++ -std=c++23 -O3 tools/miss_probe/raw_copy_probe.cpp -o raw_copy_probe.exe
clang++ -std=c++23 -O2 -mavx2 tools/miss_probe/bw_hog.cpp -o bw_hog.exe
miss_probe <file> 10 1500 1766400 262144 1          # pool vs inline, gap none/busy/sleep
raw_copy_probe <file> 10 1500 262144                # ideal pool, shared vs per-thread handles
bw_hog 14 60 & miss_probe <file> 10 1000 1766400 262144 1   # under DRAM load
```

### Update 2026-10-01: a one-command reproduction, and the cold-miss consequence

`tools/read_shootout` reproduces the symptom on a single miss, using a 256 MiB generated file and no
model file, and races the alternatives (README there). Cold, the number of non-cached requests per row
sets that row's latency, whether the requests run on parallel threads or in one IoRing submission:
28 x 64 KiB takes ~3.5 ms, 7 x 256 KiB ~1.8 ms, and 1 x 2 MiB ~0.7 ms.

So the plateau is a per-request cost, with no overlap between concurrent requests. One unbuffered
request per row is the fastest cold strategy measured: 574-750 us, against 818-1,019 us for chunked
buffered fills. Re-run the shootout after any test below changes the setup.

### Update 2026-10-01: DiskSpd rules out the drive and the stack

DiskSpd 2.3.0 reads the same file uncached at ~5 GB/s at depth (4.55 GB/s with 256 KiB blocks, 5.27 GB/s
with 2 MiB). It holds that with 8 threads x 1 (3.1 GB/s), with adjacent blocks (4.2 GB/s), and with
bursts of 7 plus think time (p90 0.92 ms per I/O). Hypotheses 1-3 (filter driver, device ceiling,
BitLocker) therefore cannot be the cause: they would bound DiskSpd too. The cause is in how this repo's
probes issue uncached I/O. They serialize, bimodally, where DiskSpd does not; pinned versus pageable
slots makes no difference. Reproduce with `tools/read_shootout/diskspd_reference.ps1` and the shootout.
DiskSpd's I/O path (https://github.com/microsoft/diskspd) is the reference to diff against.

An earlier, unexplained record of the same symptom is in `docs/design.md` sec 5 and `docs/prior-art.md`
sec 7: "3.7x slower at depth 16 ... did not scale with queue depth". It was ~1.44 GB/s against an
overlapped buffered baseline that may not have been cold. This investigation supersedes that record.

## Environment

- Intel Core Ultra 9 275HX (8P+16E, no SMT), 63.4 GB RAM.
- Windows 11 build 26220.
- NTFS on a consumer NVMe SSD: D: is disk 1, a Predator SSD GM7 M.2 1 TB (PCIe 4, DRAM-less with host
  memory buffer). C: is a separate Samsung MZAL81T0HFLB.
- BitLocker is on for D: and C: (shell property `System.Volume.BitLockerProtection` = 1, readable
  without elevation). Whether it is software or hardware encryption needs `manage-bde -status D:`, which
  needs elevation.
- Hypotheses 1-3 need an elevated shell (`fltmc`, a Defender exclusion, `manage-bde`) or an install
  (`diskspd`). Checked 2026-10-01 from an unelevated session: all are refused.
- Test file: the Sub0Llm Qwen4 S0Q1 sidecar `D:\ModelWeights\Sub0Llm-Qwen4-full48-bf16\qwen4_full48_q_bf16.bin.moeq`, 39,848,247,352 bytes.
- `FILE_STORAGE_INFO` reports a logical sector of 512 bytes, physical sectors (atomicity and performance) of 4096, and an fs-effective size of 4096.
- File attributes are `0x20`: not compressed, not sparse, not encrypted.
- All offsets, lengths and buffers are 4 KiB-aligned (`VirtualAlloc`).

## Reproduce

```
clang++ -std=c++23 -O2 tools/unbuffered_probe/unbuffered_probe.cpp -o unbuffered_probe.exe
python tools/unbuffered_probe/drive.py unbuffered_probe.exe <file> D:/Craig/GitHub/Sub0Llm/scripts --sweep depth
python tools/unbuffered_probe/drive.py unbuffered_probe.exe <file> D:/Craig/GitHub/Sub0Llm/scripts --sweep handles
python tools/unbuffered_probe/drive.py unbuffered_probe.exe <file> D:/Craig/GitHub/Sub0Llm/scripts --sweep block
```

The probe mirrors `LocalFileBackend`. Each of N threads does a synchronous positional `ReadFile` on an
`FILE_FLAG_OVERLAPPED` handle, then waits with `GetOverlappedResult(..., TRUE)` on its own event.
Offsets are uniformly random and 4 KiB-aligned. Each run reads about 900 MB.

The driver applies Sub0Llm's contention gate before the run and its named-process check before every
sample. It evicts the file before every run with Sub0Llm's `page_cache.evict_verified`, which raises
rather than report a warm "cold" run. It also alternates the variant order between rounds.

## Measurements (2026-10-01, all rounds on a quiet host unless noted)

**Depth sweep** (shared handle, 1.78 MB). Round 1 overlapped a sibling benchmark; round 2 was quiet and
agreed within ~10%.

| Depth | Buffered, cold | Unbuffered |
|---:|---:|---:|
| 1 | 1.15-1.48 | 2.09-2.36 |
| 4 | 4.64-4.92 | 2.96-3.00 |
| 8 | 5.34-5.55 | 2.77-2.97 |
| 16 | 5.43-5.57 | 2.83-2.91 |

**Handle model** (unbuffered, 1.78 MB). A shared handle and one handle per worker are identical:

| | Depth 8 | Depth 16 |
|---|---|---|
| Shared handle | 2.93-2.98 | 2.95-3.05 |
| One handle per worker | 2.98-3.02 | 2.92-3.00 |

So this is not per-file-object serialization.

**Block size** (unbuffered, one handle per worker, depth 16):

| Block | GB/s | Requests/s |
|---|---:|---:|
| 256 KiB | 0.65-0.67 | ~2,600 |
| 512 KiB | 1.24-2.73 (noisy) | ~2,400-5,200 |
| 1 MiB | 2.02-2.13 | ~2,000 |
| 1.78 MB | 2.92-3.00 | ~1,700 |
| 4 MiB | 3.84-3.88 | ~950 |
| *buffered 1.78 MB, cold* | *5.11-5.39* | *~3,000* |

## Reading

- **Throughput rises with block size while the request rate stays roughly flat** at 1-2.6k requests/s.
  At depth 16 that is ~6-16 ms per in-flight request, far above an NVMe read of this size (~0.1-0.3 ms).
- **This is a fixed per-request cost on the non-cached user-read path,** not device bandwidth: the same
  file reaches 5.5 GB/s cold through the cache manager.
- **Cold buffered reads arrive as paging I/O issued by the cache manager,** a different path through the
  I/O stack.

## Hypotheses, cheapest test first

**Historical hypotheses:** 1-3 below were ruled out as general ceilings by the unelevated DiskSpd
reference. The controlled cached-reader finding below supersedes the old interpretation that the
uncached issue primitives themselves cannot overlap.

Sources come from the research report summarized in the session that opened this. Confidence tags:
H = documented by Microsoft, M = public measurements, L = speculation.

1. **A filesystem filter driver taxes user non-cached reads but not paging I/O** (L, strongest lead).
   Defender (`MsMpEng`) was actively scanning during the runs, and minifilters commonly treat paging I/O
   differently.
   - Test: `fltmc filters`; repeat with the file's folder temporarily excluded from Defender real-time
     scanning; repeat with OEM agents (Lenovo Vantage was observed at high CPU) stopped.
2. **A device or driver ceiling for this I/O shape** (M). A public report shows another consumer NVMe
   flat at 1.85 GB/s from QD 8 for 640 KiB-1 MiB unbuffered reads:
   https://github.com/Apolog1ze-Dev/QwFNfer/pull/2
   - Test: `diskspd -b2M -r -o16 -t1 -Sh -d10 <file>`, then the same against the raw volume (read-only),
     bypassing NTFS.
   - If diskspd reproduces ~3 GB/s while buffered reaches 5.5, the cache manager's I/O shape (request
     size or splitting) is what differs. Capture it with ETW (`wpr -start DiskIO -start FileIO`).
3. **Software BitLocker** (L, unverified secondary sources).
   - Test: `manage-bde -status D:`. It caps bandwidth rather than adding per-request latency, so it is
     unlikely to explain a flat request rate.
4. **Maximum transfer size splitting** (L).
   - Test: per-IRP sizes in the same ETW trace. If 1.78 MB requests are split into many small IRPs that
     are then serialized, that explains it.
5. **Not the cause:** `FILE_FLAG_RANDOM_ACCESS`/`SEQUENTIAL_SCAN` (H: no effect with NO_BUFFERING),
   `FILE_FLAG_WRITE_THROUGH` (H: writes only), IoRing (M: within +0-2.4% of overlapped reads in the PR
   above).

## Done when

The plateau is explained by a captured trace, not inference, and either:
- an unbuffered configuration reaches at least cold-buffered throughput for 1.78 MB reads at depth 8+
  (the fix is recorded here and in `docs/prior-art.md`), or
- it is shown to be a property of this host or drive, recorded as such, with buffered fills documented
  as the recommended default and the duplicate-caching cost measured in a real regime-2 workload.

## Controlled issue-path finding (2026-10-01)

The trigger is **cached-file state created by buffered reads**, not an inherent uncached device
ceiling and not a failure to put all requests in flight. The shootout recreated that state itself:
`probe_cache_us()` opened/read a buffered handle immediately before each uncached arm; the independent
buffered checksum oracle did the same between arms. Closing a cached handle does not make this effect
disappear immediately on this host. A noncached open/close and a cold data-page probe are not proof
that the cached-reader lifetime has ended.

The smallest controlled repro is `tools/read_shootout/uncached_burst.cpp`, built as
`sub0mempage-uncached-burst`. It issues seven adjacent 256 KiB `ReadFile` requests on one
`FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING` handle into adjacent ranges of one allocation, before
waiting for any. Request storage is preallocated; all 100 distinct rows are checksum-checked against
an independent whole-row direct read after timing. The only change between the main controls is a
buffered handle that reads **one 4 KiB page** and stays open. The page is at file offset zero; the
bursts are scattered across the real 37 GiB sidecar, so the effect is not just rereading cached bytes.
Both controls have the same untimed startup allowance and the same 2.8 ms busy gaps.

Observed timestamps show the distinction clearly: calls return in roughly 0.1 ms for all seven
requests in both modes. With the held cached reader, completions are staggered by roughly 0.3 ms.
Without it, completions bunch within one burst latency. Event waits observe completion in request
order and may hide earlier out-of-order finishes; IOCP observes dequeue order, and neither is a
hardware timestamp. The reproducer retains both completion methods to avoid attributing the finding
to IOCP alone. Keeping the cached reader alive remains slow even after a two-second startup allowance;
closing it and allowing cleanup restores overlap.

### Source comparison and limits of the attribution

[DiskSpd's issue loop](https://github.com/microsoft/diskspd/blob/master/IORequestGenerator/IORequestGenerator.cpp)
uses ordinary overlapped `ReadFile`, preallocated request/buffer state, and an IOCP for its asynchronous
path. It queues before waiting; it also applies affinity by default. None of those mechanisms prevents
the controlled cached-reader trigger. Exploratory variants of `FILE_FLAG_WRITE_THROUGH`, IOCP vs
events, `FILE_SKIP_SET_EVENT_ON_HANDLE`, and CPU affinity failed to remove serialization consistently.
DiskSpd does not put a buffered cache-canary/checksum reader between each uncached burst.

Microsoft's [FastFat read sample](https://github.com/microsoft/Windows-driver-samples/blob/main/filesys/fastfat/read.c)
explicitly handles a noncached read when a data section exists: it acquires exclusive file/paging
resources and flushes the accessed range before proceeding. Its comment says "to avoid stale data
problems". [CcFlushCache](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ccflushcache)
has no nonblocking wait parameter. This is primary-source evidence that bypassing the data cache can
still encounter cache-coherency synchronization. **FastFat is not NTFS**: the exact NTFS internal lock
and IRP timeline have not been captured here. The cached-reader lifetime is demonstrated causally;
attributing the internal serialization specifically to the analogous NTFS coherency path is an
inference, not an observed kernel stack. No elevation, filter exclusion or raw-volume access was used.

### Diagnostic implementation

`iocp-unbuffered` issues the aligned window's chunks before waiting, retains OVERLAPPED storage sized
at open, and uses a persistent IOCP. Successful synchronous completions are still dequeued; accepted
requests are drained on a later issue failure before returning the slot. It reads directly into the
caller slot. It adds no intermediate data copy or per-miss allocation.

`--direct-session` keeps the diagnostic direct-only after a **single** primed/evicted/verified canary
and a two-second Windows cleanup allowance outside timing. Every verification uses an independent
whole aligned-window direct read. The allowance is a diagnostic control, **not** production
synchronization. Per-round `cache_probe_us` in this mode is the same setup canary, not a new buffered
probe. The default mixed-session mode remains available as the regression/negative control. The cold
canary now primes the exact deterministic sample before eviction, rather than mistaking unrelated
cold pages of a large file for proof that eviction worked.

### Recorded results and verification

Hardware/artifact: Core Ultra 9 275HX, Windows 11 26220, Predator GM7 on D:, BitLocker enabled,
`D:\ModelWeights\Sub0Llm-Qwen4-full48-bf16\qwen4_full48_q_bf16.bin.moeq` (38,002 MiB).
Clang 22.1.8, Release, Ninja. Slots were **not physically pinned** in these runs: VirtualLock was refused
in this process. Pinning is not claimed as the fix. Full raw outputs are kept in
[`evidence/unbuffered-2026-10-01/`](evidence/unbuffered-2026-10-01/).

The baseline reproduced the old failure on the real sidecar: pool p90 **2.758 / 2.893 / 2.954 ms**,
IoRing p90 **3.546 / 3.768 / 3.828 ms** across three rotated rounds; all sampled checksums passed.
The DiskSpd script was rerun unelevated (3 s measured / 2 s warmup per arm): depth-16, 256 KiB
**4,828 MiB/s**; depth-16, 2 MiB **5,256 MiB/s**; seven-request think-time p90 **0.925 ms**.
DiskSpd reports per-I/O percentiles; our burst percentile includes completion of the entire row.
GB/s below uses decimal units; DiskSpd's MiB/s must be converted before comparing.

**Exact seven full adjacent 256 KiB requests, 100 misses x 3 rotated rounds, 2.8 ms busy gaps,
every row verified**, direct session. The pre-run 10 s CPU-load gate measured **4.839%**, no named
competing storage/build tools. The primed eviction canary measured **116.4 us** after eviction.

| Arm | p90 ms, rounds 1 / 2 / 3 | Timed fill GB/s, rounds 1 / 2 / 3 |
|---|---|---|
| inline, one request | 0.791 / 0.632 / 0.644 | 2.472 / 3.135 / 3.163 |
| pool | 0.613 / 0.659 / 0.626 | 3.266 / 3.037 / 3.196 |
| **new IOCP** | **0.577 / 0.546 / 0.620** | **3.551 / 3.719 / 3.276** |
| IoRing | 0.567 / 0.554 / 0.603 | 3.625 / 3.606 / 3.452 |

The old arms recover without changing their issue loops. The session lifetime change, not IOCP alone,
is what removes serialization. For the original 1,766,400-byte (short final chunk, unaligned row
offset) shape, a preceding exploratory direct-session run also checked every row: IOCP p90
0.647 / 0.541 / 0.596 ms. The exact seven-full-chunk run above is the qualification evidence.

**Ceiling comparison:** no gaps, 1,000 misses x 3 rotated rounds. At 4 MiB (16 chunks), IOCP timed-fill
rates were **4.829 / 4.386 / 4.811 GB/s**; full-loop rates **4.575 / 4.189 / 4.584 GB/s**, with ten
whole rows checked per round. At 8 MiB (32 chunks), the pre-run 10 s load gate was **4.196%**, no
named competing tools; primed canary **125.3 us**. One complete 8 MiB row was checksum-checked per
1,000-row arm, after the fully checked seven-request qualification above.

| Depth-32 arm | Timed fill GB/s, rounds 1 / 2 / 3 | Full-loop GB/s, rounds 1 / 2 / 3 |
|---|---|---|
| **new IOCP** | **3.466 / 5.608 / 5.308** | **3.456 / 5.575 / 5.277** |
| IoRing | 5.601 / 5.156 / 5.589 | 5.569 / 5.130 / 5.556 |

Full-loop rates include issue/wait, clock reads, slot rotation and in-loop checksum work; they exclude
setup and the independent post-run oracle. The first deep IOCP arm was substantially slower (p90
6.194 ms) and its exact cause is **not resolved**; it is retained in the raw evidence and table.
The mechanism reaches the approximately 5.3 GB/s uncached bar in subsequent IOCP arms and all three
IoRing arms, but these results do not establish uniformly stable ceiling throughput in every run.
The OS eviction does not flush firmware caches, and setup allowances are not a guarantee of NTFS
cache-map teardown.

**Minimal causal control**, same gated measurement window, rotated `none / held / held / none /
none / held`, two-second startup in both modes, IOCP, all 600 rows independently checked:

| Buffered reader | p90 ms in observed order |
|---|---|
| none | 0.659 / 0.562 / 0.640 |
| held after a single 4 KiB read | 2.334 / 2.605 / 2.303 |

For example, a later held-reader burst returned from issuing all requests by **70.1 us**, with
completion observations **377.8, 619.2, 957.9, 1294.6, 1594.2, 1891.6, 2182.2 us**. A clean burst
issued by **129.4 us**, with completions between **432.8 and 504.1 us**. Startup bursts occasionally
take about 10 ms after the idle allowance; they are included in percentile calculations, not removed.

`python scripts/dev.py check` passed Windows/MSVC, Linux/GCC, ASan+UBSan and TSan (three repeats),
and killed all 13 mutants. Suite check counts stayed 28 / 82 / 39 / 78. The diagnostics' checksum
smoke passed on both platforms. ARM was explicitly skipped because its toolchain/qemu are absent.
`cpp-review` was applied to the working diff against `e343df4`; no outstanding MUST findings.
No library header, source or public API changed.

The required `dev.py bench --baseline main` also ran at 4.571% load with no named competitors.
G-ALLOC passed at zero. Two provisional G-PERF ratios failed (1.126 and 1.068), with current-arm
spreads 36.6% and 28.9%, even though current and baseline use identical library headers. This is
recorded as a failed bookkeeping gate, not silently reported green or used as I/O evidence; its
generated report and history are retained in the evidence directory's `bookkeeping/` subdirectory. Diagnostic builds remain
off by default. The storage conclusions above come from the independently gated real-file runs.

### Recommendation for LocalFileBackend

Keep LocalFileBackend unchanged in this change. For a qualified uncached backend, maintain a
direct-only data lifetime for each stream: read sidecar metadata/tokenizer/validation bytes via an
aligned direct window too, avoid concurrently retained buffered readers or data mappings, and do
not put a buffered canary/oracle in the fill path. Closing a buffered handle needs qualification;
hardcoding a two-second sleep into `prefetch` or each fill is unacceptable.

Use persistent asynchronous request state and IOCP on Windows (or the separately qualified Linux
primitive), issue all currently available chunks before waiting, and preserve completion/slot-lease
ownership on every error. A single aligned request remains a useful fallback when a consumer must
mix cached and uncached access. For the hardware bandwidth ceiling, keep enough **independent misses**
in flight to replenish approximately depth 16 or higher; a lone seven-chunk miss drains its queue and
has a different roof. Qualify the real engine's retained sidecar mapping and metadata-reader lifetime
before promoting an uncached default. Success here is transfer-path evidence, not decode-throughput
or mixed-access evidence, and Linux/WSL timings are not measurements of this physical NVMe.
