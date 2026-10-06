# Read-strategy shootout

`sub0mempage-read-shootout` measures how long one row takes to get from a file into a RAM buffer, by each
read strategy, under identical conditions. It is a kept diagnostic for future optimization work, not a
test of the library. Build it with `-DSUB0MEMPAGE_BUILD_DIAGNOSTICS=ON`.

The scenario is a cache miss as Sub0Llm's expert cache sees it:
- One row of `--row-bytes` (default 1,766,400: one MoE expert) at a row-aligned offset, read into a page-
  aligned, pinned slot.
- A gap between misses stands in for the compute between them (default: a 2.8 ms busy spin, decode's
  pace).
- Every strategy reads the same rows, in an order rotated between rounds. Each miss reads a distinct
  row, so a cold run stays cold.

**Correctness first.** Sampled rows (every `--verify-every`-th, default 4) are checksummed and compared
with an independent buffered read after the timing. A strategy that returns wrong bytes stops the run.

## Strategies

| Name | What it does | Where |
|---|---|---|
| `naive-fread` | stdio `fseek` + `fread` on the calling thread: the baseline | all |
| `inline-pread` | one positional read (`ReadFile` overlapped / `pread`) | all |
| `inline-unbuffered` | one aligned non-cached read (`FILE_FLAG_NO_BUFFERING` / `O_DIRECT` / `F_NOCACHE`) | all |
| `pool-pread` | the row split into `--chunk-kib` chunks over `--readers` parked threads | all |
| `pool-unbuffered` | the same, non-cached: DMA straight into the slot | all |
| `mempage` | Sub0MemPage `LocalFileBackend` + `TransferSet`, chunked, as shipped | all |
| `mempage-unbuffered` | the same with `FileAccess::uncached`: the library's own non-cached path | all |
| `ioring` / `ioring-unbuffered` | Windows 11 IoRing, every chunk in one submission | Windows |
| `iocp-unbuffered` | all aligned chunks issued before waiting; persistent OVERLAPPED storage and IOCP | Windows |
| `map-copy` | memory-map the file, `memcpy` the row | all |
| `map-prefetch-copy` | map, `PrefetchVirtualMemory` / `madvise(MADV_WILLNEED)`, then copy | all |
| `map-ntcopy` | map, copy with non-temporal stores | x86-64 |
| `map-touch` | map, touch one byte per page, no copy: what reactive mmap pays | all |

A strategy the host cannot run is reported `unavailable` with the reason; the run continues.

Planned next: Linux `io_uring`, large-page slots, and a whole-row (unchunked) mode for the pool and
MemPage arms.

## Running

```
sub0mempage-read-shootout                         # warm page cache, all strategies
sub0mempage-read-shootout --cold                  # evict and verify before each strategy
sub0mempage-read-shootout --load-threads 12       # with DRAM streaming load, like decode
sub0mempage-read-shootout --only mempage,ioring --chunk-kib 1024
sub0mempage-read-shootout --list
```

| Option | Default | Meaning |
|---|---|---|
| `--file PATH` | temp dir `sub0mempage-read-shootout.bin` | test file; generated (pseudo-random) if missing or too small |
| `--file-mib N` | 256 | size of a generated file |
| `--row-bytes N` | 1766400 | bytes per miss |
| `--chunk-kib N` | 256 | chunk size for the pool, MemPage and IoRing arms |
| `--readers N` | 8 | threads for the pool and MemPage arms |
| `--misses N` | 100 | misses per strategy per round (capped at the rows in the file) |
| `--rounds N` | 2 | rounds; strategy order rotates each round |
| `--gap none\|busy\|sleep` | busy | what happens between misses |
| `--gap-us N` | 2800 | gap length |
| `--cold` | off | evict the file before each strategy, and refuse if a probe finds it still cached |
| `--load-threads N` | 0 | threads streaming over 128 MiB buffers during the run |
| `--verify-every N` | 4 | checksum every Nth miss |
| `--only a,b` | all | run only these strategies |
| `--no-pin` | pinned | leave the slots pageable |
| `--direct-session` | off | cold, exclusively unbuffered arms: one setup canary, then no buffered reads between runs; independent whole-window direct-read checksum oracle |

Output is one `JSON {...}` line per strategy and round, then a summary table: the median over rounds,
fastest mean first, with throughput and speed relative to `naive-fread`.

Cold eviction is unelevated. On Windows, a non-cached open purges a file that nothing maps; this is
undocumented, which is why the run probes 4 KiB reads and refuses when the file still looks cached. On
Linux it uses `posix_fadvise(DONTNEED)`. macOS has no equivalent, so `--cold` is refused there. Run on a
quiet host: other load moves every number.

## First results (2026-10-01, development host, 256 MiB file, 100 misses x 2 rounds)

Historical mixed-session results below are superseded for uncached overlap by the controlled
direct-session investigation at the end of this README. They remain the negative control.

Intel Core Ultra 9 275HX, Windows 11 26220, the file on D: (Predator GM7 NVMe, BitLocker on). p50 per
1,766,400-byte row:

| Strategy | Warm | Cold | Warm + DRAM load (88 GB/s) |
|---|---:|---:|---:|
| `naive-fread` | 283 | 1,369 | 718 |
| `inline-pread` | 309 | 1,440 | 631 |
| `pool-pread` | 279 | 967 | 414 |
| `mempage` | 259 | 1,019 | **394** |
| `ioring` | **257** | 818 | 708 |
| `inline-unbuffered` | 736 | **574** | 924 |
| `pool-unbuffered` | 1,889 | 1,927 | 2,595 |
| `ioring-unbuffered` | 1,762 | 1,734 | 822 |
| `map-copy` | 723 | 3,879 | 1,127 |
| `map-prefetch-copy` | 736 | 1,812 | 1,203 |
| `map-ntcopy` | 732 | 3,522 | 1,090 |
| `map-touch` | 491 | 3,310 | 693 |

Readings:
- **Warm:** the chunked buffered arms lead, at ~10 GB/s copy ceilings. Mapping loses: soft faults
  cost ~490 us per row before any copy.
- **Under DRAM load:** MemPage's chunked buffered fill is the best arm, 1.8x the baseline.
- **Cold:** one unbuffered request per row wins. Unbuffered requests do not overlap on this host;
  cold, a row's time grows with its request count, whether the requests go to threads or to one IoRing
  submission (p50):

  | Chunk | `pool-unbuffered` | `ioring-unbuffered` |
  |---|---:|---:|
  | 64 KiB (28 requests) | 3,752 | 3,449 |
  | 256 KiB (7 requests) | 1,850 | 1,733 |
  | 1 MiB (2 requests) | 786 | 1,032 |
  | 2 MiB (1 request) | 801 | 691 |

  This is the fixed per-request cost of `docs/investigations/unbuffered-read-ceiling.md`, seen on a single
  miss. Until that is explained, an unbuffered fill should use one request per row, not chunks.

Linux (WSL2, GCC 15, ext4 on a virtual disk, 64 MiB file, 30 misses, warm): mapping is cheap there.
`map-touch` takes 49 us p50 and `map-copy` 159 us, against `inline-pread` 211 us and `mempage` 225 us. So
the Windows soft-fault cost is not universal. WSL's unbuffered numbers measure the virtual disk, not the
hardware.

## DiskSpd reference

`diskspd_reference.ps1` runs [DiskSpd](https://github.com/microsoft/diskspd) on the same file in matching
shapes. It shows what the drive and I/O stack can deliver, so an arm far below it is losing time in its
own code. D: sidecar, 2026-10-01:

| DiskSpd run | MiB/s | p50 ms per I/O |
|---|---:|---:|
| uncached 2 MiB, depth 1 / 16 | 2,545 / 5,268 | 0.77 / 6.04 |
| uncached 256 KiB, depth 1 / 16 | 743 / 4,552 | 0.35 / 0.82 |
| uncached 256 KiB, 8 threads x 1 | 3,125 | 0.56 |
| uncached 256 KiB, 8 threads, adjacent blocks | 4,151 | 0.41 |
| uncached 256 KiB, bursts of 7 + think time | (idle-bound) | 0.52 (p90 0.92) |
| cached 2 MiB, depth 16 | 6,452 | 0.54 |

Uncached I/O overlaps fine under DiskSpd: about 5 GB/s at depth, with adjacent blocks and with bursts.
The shootout's chunked uncached arms take 1.6-2.8 ms for 7 x 256 KiB in bursts, where DiskSpd's p90 is
0.92 ms, and are bimodal (sometimes overlapping, often serial). The ~3 GB/s "plateau" is therefore in
how this repo's code issues I/O, not in the drive, NTFS or BitLocker. Open:
`docs/investigations/unbuffered-read-ceiling.md`.

## Correct uncached session and minimal repro (2026-10-01)

The old cold setup opened a buffered canary immediately before every uncached run, then opened a
buffered checksum oracle afterwards. Those operations create cached-file state. On this Windows host,
even a **held buffered reader that has read only one 4 KiB page** makes seven otherwise identical
uncached requests complete about one request-latency apart. The user-space issue loop is already
asynchronous. IOCP, IoRing, more workers, write-through and affinity alone do not remove this condition.

`--direct-session` separates this lifetime effect from the transfer mechanism. It primes the exact
deterministic canary sample, evicts it and verifies it once, then allows two seconds for Windows'
deferred cached-reader cleanup **outside measurement**. It runs only uncached arms and verifies
every sampled row with an independent, whole aligned-window direct read. No buffered reader is opened
again during the session. Uncached reads bypass the OS data cache even when rounds revisit a row;
this does not flush the drive's own cache. The two-second setup allowance is an empirical diagnostic
control, not an API guarantee or a proposed per-fill sleep in the library. Other processes can still
open/map the file and invalidate the clean-session condition.

```powershell
$file = 'D:\ModelWeights\Sub0Llm-Qwen4-full48-bf16\qwen4_full48_q_bf16.bin.moeq'
$tools = 'build/unbuffered-investigation/tools/read_shootout'
& "$tools/sub0mempage-read-shootout.exe" --file $file --cold --direct-session `
    --only inline-unbuffered,pool-unbuffered,ioring-unbuffered,iocp-unbuffered `
    --row-bytes 1835008 --chunk-kib 256 --rounds 3 --verify-every 1
```

The separately built `sub0mempage-uncached-burst` is the small Windows reproducer: seven **full**
adjacent 256 KiB requests into one allocation, issued before any wait, 100 distinct shuffled rows,
2.8 ms busy gaps, all rows checked against an independent direct whole-row read. It prints issue-return
and observed-completion timestamps for the first three bursts. Choose `event|iocp`, a cached-reader
control (`none|held|closed`), and an untimed startup allowance in milliseconds. `held` reads one 4 KiB
page through a buffered handle and retains it; `closed` immediately closes it. Compare rotated pairs:

```powershell
& "$tools/sub0mempage-uncached-burst.exe" $file iocp none 2000
& "$tools/sub0mempage-uncached-burst.exe" $file iocp held 2000
& "$tools/sub0mempage-uncached-burst.exe" $file iocp held 2000
& "$tools/sub0mempage-uncached-burst.exe" $file iocp none 2000
```

Keep burst latency and the sustained throughput ceiling separate. One seven-request miss has only
seven outstanding operations and drains them all. DiskSpd's depth-16 ceiling continuously replenishes
sixteen requests. Use a 4 MiB row (16 chunks) or 8 MiB row (32 chunks) and `--gap none` for a deeper
batch comparison; the shootout's GB/s is bytes divided by **timed fill durations**, excluding gaps,
checksum work and setup, and must not be labelled sustained application throughput. Full measurements
and the LocalFileBackend recommendation are in the investigation document.

Qualification on the real D: sidecar, three rotated rounds, exact seven full 256 KiB requests:
`iocp-unbuffered` p90 **0.577 / 0.546 / 0.620 ms**, all 300 rows verified. At depth 32 with no gaps,
it reached **5.575 / 5.277 GB/s** including loop overhead in rounds 2/3; round 1 was **3.456 GB/s**
and is retained as an unresolved slower run. IoRing under the same session fix reached
**5.569 / 5.130 / 5.556 GB/s**. The minimal held-reader control was **2.30–2.60 ms p90** against
**0.56–0.66 ms** without it. These runs passed the pre-run CPU gate; physical slot pinning was refused.
See the investigation for all rounds, raw evidence, unit conversions, validation and limitations.

JSON also reports `direct_session`, `fill_gbps` (timed fill durations only) and `loop_gbps`
(the measured miss loop including gaps and in-loop checksum work, excluding setup/post-run oracle).
