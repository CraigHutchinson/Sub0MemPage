# Active work log

Check this table before edits or hardware runs. Also check Sub0Llm's log on the shared machine.

| Date | Owner | Scope | Status | Hardware |
|---|---|---|---|---|
| 2026-09-22 | Codex | M0 contract/Intel review, M1 optional capability diagnostic and offline tests; naming deferred by user | done | Small serial builds and inventory only; no performance measurements |

M0/M1 validated and ready to commit; no remaining hardware reservation. M2 has not started.

## 2026-09-25 storage stack design

Completed documentation coordination across MemPage, TieredCache and Llm. Scope: plans, requirements,
consumer audit and NVIDIA source research. No engine files, builds or hardware workloads held.
GPU transport and model integration remain unimplemented and require the recorded gates.
