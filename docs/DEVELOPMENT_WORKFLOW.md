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
| All three, before every commit | `python scripts/dev.py check` | all of the above |
| Did my change cost or gain time? | `python scripts/dev.py bench --baseline main` | G-ALLOC, G-PERF |
| How noisy is this host? | `python scripts/dev.py bench --aa` | (records the noise floor) |

On a Windows host the Linux stages re-invoke the script inside WSL (`SUB0MEMPAGE_WSL_DISTRO`, default
`Ubuntu-24.04`). WSL builds live on ext4 (`~/.cache/sub0mempage-dev`), not the slower Windows mount.
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

## Not yet covered (see the M2 checkpoint in implementation-plan.md)

- MSVC `/fsanitize=address` is not wired; sanitizers run on Linux only.
- No ARM coverage. The planned route is a QEMU user-mode stage (`aarch64-linux-gnu-g++` + `qemu-aarch64`,
  both in WSL) for tests and sanitizers. Perf numbers are never taken under emulation.
- The G-PERF 5% threshold is provisional until a `bench --aa` run on the dedicated machine measures
  the noise floor.
- `dev.py bench` has been exercised only as a standalone binary (`sub0mempage-bench`), not end to end
  through the script's contention gate and A/B path.
