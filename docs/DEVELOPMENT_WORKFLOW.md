# Development workflow

**Status: PROCESS.** This document is binding for changes to the library. It makes correctness, sanitizer, mutation and
performance evidence cheap enough to collect on every change, so iteration time goes to the real work
instead of hand-run checks. It follows Sub0Llm's `docs/OPTIMIZATION_PROCESS.md`, whose rules were learned
from real measurement mistakes. Read that document for the reasoning; this one covers how the rules apply here.

## One command per question

| Question | Command | Gates (`docs/perf/kpi_gates.json`) |
|---|---|---|
| Does it build and pass, Windows and Linux? | `python scripts/dev.py test` | G-TESTS, G-SUITE |
| Is it memory- and thread-safe? | `python scripts/dev.py sanitize` | G-SANITIZE (ASan+UBSan, TSan, tests repeated x3) |
| Would the tests notice a real defect? | `python scripts/dev.py mutate` | G-MUTANTS |
| Does it build and pass correctly on ARM? | `python scripts/dev.py arm` | (correctness only; no gate yet) |
| All of the above, before every commit | `python scripts/dev.py check` | all of the above |
| Did my change cost or gain time? | `python scripts/dev.py bench --baseline main` | G-ALLOC, G-PERF |
| How noisy is this host? | `python scripts/dev.py bench --aa` | (records the noise floor) |

On a Windows host the Linux stages re-invoke the script inside WSL (`SUB0MEMPAGE_WSL_DISTRO`, default
`Ubuntu-24.04`). WSL builds live on ext4 (`~/.cache/sub0mempage-dev/<checkout hash>`), not the slower Windows mount.
Windows builds live in `build/dev/`. A platform that cannot run reports **SKIP**, never PASS. The
summary lists every configuration; a skipped platform has not been verified.

## Rules

- **Check counts are exact (G-SUITE).** Each test prints `N checks, M failures`, and the recorded count
  must match. If you add checks, re-record them with `dev.py test --accept-counts` in the same commit.
  A count that changes for a reason nobody can explain is an unexplained behaviour change, the same as
  Sub0Llm's G-SUITE rule.
- **Every new invariant gets a mutant.** Add an entry to `scripts/mutants.json` naming the one-line
  defect that breaks the invariant and the test that must catch it. A surviving mutant means the gate
  is vacuous. The first run of this stage found one (`speculative-ages-residents`), and the fix was a
  missing check in the admission test. A hang, killed by the 30 s timeout, counts as detection. CTest
  also carries a 60 s timeout, so a hung state machine fails CI instead of stalling it.
- **Hot paths never allocate (G-ALLOC).** Tests and benchmarks replace global `operator new` with a
  counter (`tests/allocation_counter.cpp`). Every benchmark's timed region must count zero. This is a
  measurement, not a code-review claim.
- **Benchmarks measure what this repository owns.** `bench/bench_main.cpp` times the bookkeeping
  against a backend that moves no bytes. I/O cost belongs to the M3+ backends and gets its own
  real-file measurements; those must not be mixed into these numbers.
- **The perf protocol (inherited from Sub0Llm):**
  - The bench refuses to run while build tools are active or sustained CPU load is above 5%.
    `--allow-contention` records an **ADVISORY** row, which is not evidence.
  - Arms are interleaved and their order rotates every round.
  - Every run is reported, not just the median.
  - `--baseline REF` compiles the *current* bench against REF's library headers, so an A/B isolates
    library changes. If the API changed, it fails loudly rather than comparing different programs.
  - Every run appends `docs/perf/perf_history.jsonl` and rewrites `docs/perf/perf_report.md`.
- **One negative measurement is not a verdict.** Take three passes before parking an optimization, and
  park it behind a default-off toggle rather than reverting it (Sub0Llm AGENTS.md sec 13).
- **Shared machine.** Before a bench run, check `docs/ACTIVE_WORK_LOG.md` here and Sub0Llm's
  equivalent. Another agent's workload invalidates timings even with no file overlap.

## ARM (`dev.py arm`, `cmake/toolchains/aarch64-linux-gnu.cmake`)

Cross-builds with Ubuntu's `g++-aarch64-linux-gnu` and runs the suite under `qemu-aarch64` user-mode
emulation (`apt-get install -y g++-aarch64-linux-gnu qemu-user`). Correctness only -- an emulated timing
is never a measurement of the target, so `dev.py arm` never records perf numbers, and `dev.py bench` has
no ARM arm. `dev.py check` runs it as its 4th stage; a missing cross toolchain or `qemu-aarch64` is one
SKIP row, never a silent PASS (`arm_tools_available()` in `scripts/dev.py`).

Verified 2026-09-25 on x86_64 Linux (container, not the dedicated machine; GCC 13, qemu-user 8.2.2):
- **Plain build + CTest (`arm-release`)**: PASS under `qemu-aarch64 -L /usr/aarch64-linux-gnu`, same
  28/82/39 check counts as the native build.
- **ASan+UBSan (`arm-asan`)**: does not run under this emulator. The process segfaults inside ASan's own
  interceptor-registration phase, before any test code runs (`qemu: uncaught target signal 11
  (Segmentation fault) - core dumped`, immediately after the last `AddressSanitizer: failed to
  intercept '...'` line ASan prints while resolving libc symbols it doesn't find under qemu-user).
  Recorded as SKIP with the qemu output attached, not as a library defect.
- **TSan (`arm-tsan`)**: fails immediately and cleanly with `FATAL: ThreadSanitizer: unsupported VMA
  range` / `FATAL: Found 47 - Supported 39, 42 and 48` -- qemu-user's aarch64 target reports a 47-bit
  VMA to a runtime built expecting 39, 42 or 48. Also recorded as SKIP, not a defect.

Windows: on an `IS_WINDOWS` host the `arm` stage delegates to WSL through the same generic
`delegate_to_wsl()` path `sanitize`/`mutate` already use (untested here -- no Windows host available --
but it takes no ARM-specific code path, so it should work the same way once qemu-user and the cross
toolchain are installed inside the WSL distro).

The `arm-qemu` CI job (`.github/workflows/ci.yml`) runs the same
build+ctest-under-qemu on `ubuntu-latest`; verified locally with the exact commands the job runs.

## MSVC ASan (`ci-msvc-asan`, `CMakePresets.json`)

**Not verified locally -- no Windows host in this environment.** `/fsanitize=address` is added via
`CMAKE_CXX_FLAGS` (which replaces CMake's defaults, so the preset restates `/EHsc` etc.), and the preset builds `Release` (not `Debug`) specifically because CMake's default
`CMAKE_CXX_FLAGS_DEBUG` carries `/RTC1`, which MSVC's ASan refuses to combine with. The CI job
(`.github/workflows/ci.yml`, `msvc-asan`) uses `ilammy/msvc-dev-cmd@v1` so the ASan runtime DLL
(`clang_rt.asan*.dll`, shipped next to `cl.exe`) is on `PATH` for `ctest`; an alternative if that proves
insufficient is a CTest `ENVIRONMENT` entry pointing `PATH` at the VC tools directory directly. Treat
this job as unverified until it has actually run in CI at least once.

## Not yet covered (see the M2 checkpoint in implementation-plan.md)

- Consumers link `Sub0MemPage::testing` for the deterministic fake backend (`testing/include/sub0mempage/testing/fake_backend.hpp`), a transport double only.
- The G-PERF 5% threshold is provisional until a `bench --aa` run on the dedicated machine measures
  the noise floor. A `bench --aa --allow-contention` run in this shared, non-dedicated container
  (2026-09-25) triggered a G-PERF FAIL on one microbenchmark (`try_resolve_hit_64slots`, ratio 1.131)
  purely from host noise -- exactly the scenario `--allow-contention`'s ADVISORY label exists for; that
  run's `perf_history.jsonl`/`perf_report.md` rows were not committed.
- `dev.py bench`'s contention gate, `--aa` path, `--baseline REF` A/B path and report writer have now
  been exercised end to end (`--allow-contention --aa` and `--allow-contention --baseline origin/main`,
  both in this same container) and produced sane output (G-ALLOC 0, G-PERF ratios near 1.0 for an
  identical-code A/B), but still only ever as ADVISORY runs -- never yet on the dedicated machine.
- ARM sanitizer coverage is limited to what qemu-user actually supports, i.e. currently just the plain
  build (see above); ASan/TSan on ARM need either real hardware or a qemu-user/runtime combination this
  session did not find.
