# Active work log

Check this table before edits or hardware runs. Also check Sub0Llm's log on the shared machine.

| Date | Owner | Scope | Status | Hardware |
|---|---|---|---|---|
| 2026-09-25 | Claude | M2: cached-slot + explicit-destination state machines, deterministic fake backend, portable tests (new headers under include/sub0mempage/, tests/) | done (handover: implementation-plan.md M2 checkpoint) | None held |
| 2026-09-22 | Codex | M0 contract/Intel review, M1 optional capability diagnostic and offline tests; naming deferred by user | done | Small serial builds and inventory only; no performance measurements |

M0/M1 committed. M2 draft committed 2026-09-25; next steps are listed in implementation-plan.md's M2 checkpoint. No hardware reservation.

## 2026-09-25 storage stack design

Completed documentation coordination across MemPage, TieredCache and Llm. Scope: plans, requirements,
consumer audit and NVIDIA source research. No engine files, builds or hardware workloads held.
GPU transport and model integration remain unimplemented and require the recorded gates.
