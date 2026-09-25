# M2 draft validation (2026-09-25)

Scope: `include/sub0mempage/{transfer,slot_pool,transfer_set}.hpp` against the deterministic fake
backend. No real I/O backend exists yet (M3), so nothing here qualifies file, GPU or USM behaviour.

| Gate | Result |
|---|---|
| Windows MSVC 19.51, C++23 Release, /W4 /WX | 3/3 CTest: probe-options 28, slot-pool 82, transfer-set 39 checks, 0 failures |
| Ubuntu 24.04 WSL2, GCC 15.2, C++23 Release, -Werror | 3/3 CTest, same counts |
| GCC 15.2 ASan+UBSan (`-fno-sanitize-recover=all`), tests x3 | clean |
| GCC 15.2 TSan, tests x3 | clean |
| Mutation (`scripts/mutants.json`, 11 mutants) | 11/11 killed after adding one missing admission check |
| Hot-path allocations (tests + bench timed regions) | 0 |
| clang, macOS | not run locally (GitHub CI covers clang/macOS builds) |
| MSVC ASan, ARM | not run; see DEVELOPMENT_WORKFLOW.md "Not yet covered" |

## Defects found while building M2, and what caught them

- **`FillBackendRef` copy hijack** (ASan, stack-use-after-return). The type satisfies its own backend
  constraint, so copying a non-const lvalue selected the converting constructor and wrapped a reference
  to the source ref. The constructor now excludes its own type.
- **Notify after unlock** (review). `on_complete` released the lock and then called `notify_all`. A
  waiter woken by an earlier notification could observe completion, return, and let the owner destroy
  the pool before that call. Both classes now notify while holding the lock.
- **Vacuous admission test** (mutation). The `speculative-ages-residents` mutant survived because a
  one-slot, single-sweep test could not see reference bits being cleared. A second speculative hint now
  pins the rule.

## Bookkeeping microbenchmarks (indicative only: no contention gate, single run)

`sub0mempage-bench`, MSVC Release, run directly rather than through `dev.py bench`:

| Case | ns/op |
|---|---:|
| slot_pool.try_resolve_hit_64slots | 34.9 |
| slot_pool.try_resolve_hit_4096slots | 34.1 |
| slot_pool.resolve_hit_batch8 | 198.9 |
| slot_pool.miss_cycle (prefetch, fill, wait, resolve) | 157.8 |
| slot_pool.try_resolve_hit_contended_4t (wall per op) | 43.1 |
| transfer_set.submit_complete | 109.1 |

The contended row is the first real perf lead. Four threads doing `try_resolve` hits get no scaling
(43 ns wall per op against 35 ns single-threaded), because every call takes the one pool mutex. R12's
record-then-replay precedent (Caffeine) and striped read paths are the candidate levers. Re-measure
properly with `dev.py bench` on an idle machine before acting on it.

## Code review

A `cpp-review` pass over the M2 diff found one MUST: the notify-after-unlock race above. Its SHOULDs
were also fixed:
- stale "skeleton" status prose in README, the umbrella header and CMake (AGENTS.md sec 10);
- duplicated test harness code, now `tests/test_support.hpp` + `tests/allocation_counter.cpp`;
- an unlocked `last_` read in the fake backend;
- the library not linking `Threads::Threads`;
- missing `noexcept`-terminate notes on `reset()`;
- deferred surface (`on_evict`, `open_stream`, the R18 cross-instance guard) now named in the header.

Left as-is (NICE): `SlotPool::wait(ticket)` versus `Claim::wait()` are asymmetric shapes; revisit when
M3 exercises both.
