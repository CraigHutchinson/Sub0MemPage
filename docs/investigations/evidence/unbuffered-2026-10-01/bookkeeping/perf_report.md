# Sub0MemPage performance report

Generated 2026-10-01T22:02:40Z -- label `uncached-diagnostics` -- git `e343df4491ba` (dirty) -- `msvc-release` on `CrogLegion`

Contention: named `[]`, background 4.57056434564215%

## Gate panel

| Gate | Detail | Value | Status |
|---|---|---:|---|
| `G-ALLOC` | timed-region allocations | 0 | PASS |
| `G-PERF` | slot_pool.try_resolve_hit_64slots: current / baseline | 1.036 | PASS |
| `G-PERF` | slot_pool.try_resolve_hit_4096slots: current / baseline | 1.01 | PASS |
| `G-PERF` | slot_pool.resolve_hit_batch8: current / baseline | 1.126 | FAIL |
| `G-PERF` | slot_pool.miss_cycle: current / baseline | 0.96 | PASS |
| `G-PERF` | slot_pool.try_resolve_hit_contended_4t: current / baseline | 1.068 | FAIL |
| `G-PERF` | transfer_set.submit_complete: current / baseline | 0.982 | PASS |

## Microbenchmarks (3 interleaved rounds x 3 samples; ns/op, median of per-run medians)

| Benchmark | baseline (spread) | current (spread) | Runs |
|---|---:|---:|---|
| slot_pool.try_resolve_hit_64slots | 35.66 (23.9%) | 36.94 (6.9%) | baseline: 35.66, 43.91, 35.37; current: 35.69, 36.94, 38.22 |
| slot_pool.try_resolve_hit_4096slots | 35.85 (2.5%) | 36.2 (4.2%) | baseline: 36.68, 35.77, 35.85; current: 37.71, 36.18, 36.2 |
| slot_pool.resolve_hit_batch8 | 192.14 (7.3%) | 216.34 (36.6%) | baseline: 191.46, 205.51, 192.14; current: 216.34, 192.46, 271.67 |
| slot_pool.miss_cycle | 153.7 (1.8%) | 147.62 (7.4%) | baseline: 153.7, 154.78, 152.03; current: 147.62, 157.26, 146.28 |
| slot_pool.try_resolve_hit_contended_4t | 41.45 (6.3%) | 44.27 (28.9%) | baseline: 41.45, 43.01, 40.38; current: 44.27, 48.01, 35.23 |
| transfer_set.submit_complete | 108.26 (14.2%) | 106.28 (2.2%) | baseline: 107.39, 122.75, 108.26; current: 103.96, 106.3, 106.28 |

History: `perf_history.jsonl`. Policy: `docs/DEVELOPMENT_WORKFLOW.md`.
