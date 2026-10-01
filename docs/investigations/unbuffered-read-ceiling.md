# Investigation: unbuffered reads plateau at ~3 GB/s on the development host

Status: **OPEN**, opened 2026-10-01. Self-contained. Nothing from the session that found it is needed
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
