# Agent instructions for Sub0MemPage

This file is a pre-flight checklist, not a tutorial — the same role
[Sub0TieredCache's own `AGENTS.md`](https://github.com/CraigHutchinson/Sub0TieredCache/blob/main/AGENTS.md) plays one
layer up, and [Sub0Llm's own `AGENTS.md`](https://github.com/CraigHutchinson/Sub0Llm/blob/main/AGENTS.md)
plays for the project this whole lineage descends from. The paging scheduler has not shipped yet, so unlike Sub0Llm's own
document these rules aren't citing a bug that already happened in *this* repo — they're derived directly
from real decisions already made in `REQUIREMENTS.md`/`README.md`/`docs/`, stated here as constraints so
implementation starts from them rather than rediscovering them the hard way. Update this file the day a
rule below is actually tested by a real incident, and say so.

If you're about to implement, extend, or change anything here: read this first.

**More than one coding agent may work on this repo over time** (this project's own scaffold was produced
by an agent working from a detailed spec; future implementation work may involve Claude Code, GitHub
Copilot, Codex, or others, following the same multi-agent-shared-tree precedent Sub0Llm's own
`docs/ACTIVE_WORK_LOG.md` establishes). If this repo ever grows a shared active-work log the way Sub0Llm's
has, check it before touching a file another agent's in-flight work depends on.

## 1. `resolve`/`wait` are the only calls that may block on I/O (REQUIREMENTS.md R3)

This is the entire reason the project exists — a caller with its own no-heap-allocation or
bounded-latency hot-path rule (Sub0Llm's `ParallelExperts` decode loop is the real, concrete motivating
case — see `docs/sub0llm-consumer-trace.md`) needs a hard guarantee, not a "usually fast." Any
implementation change that makes `prefetch`/`try_resolve`/`wont_need`/`release`/`stats` occasionally
block, or that adds a new call implying it might do I/O without saying so explicitly in its name and
contract, breaks the property this project's entire design exists to provide — not a style nit, a
correctness-of-contract violation.

## 2. Mapped sources remain readable; caller slots require completion and a lease (R2, R8)

In the secondary mmap mode, CPU access to the caller's mapping stays valid without a hint.
In the primary caller-slot mode, a successful lease acquisition is required before reading filled bytes.
A completion ticket alone does not prevent reuse. Neither rule grants a GPU access to ordinary mapped
memory or physically locks a page in RAM. See docs/implementation-plan.md and docs/intel-usm.md.

## 3. Never bake a specific consumer's domain knowledge into the core (REQUIREMENTS.md R1)

The reuse boundary is the byte range, full stop — `register_region` never learns what "expert," "row," or
"n-gram" mean, and the core never parses any specific file format or interprets any byte's meaning. A
change that special-cases any one caller's domain inside the residency-management core is a scope
violation, not a convenience — exactly the failure mode README.md §1b argues against. If a need looks
domain-specific, it belongs in the caller's own adapter code (or in Sub0TieredCache, one layer up), not here.

## 4. Portability is checked on Windows and Linux both, not compiled on one (REQUIREMENTS.md R14)

See `STYLE_GUIDE.md`. A change that only builds/behaves correctly on the author's own OS is not done. This
is a harder rule for Sub0MemPage than for a typical library, because the two platform mechanics (Windows
`PrefetchVirtualMemory`/IOCP, Linux `io_uring`/`madvise`) are genuinely different primitives with
genuinely different documented guarantees (`docs/prior-art.md` §1–§2) — a careless implementer could
quietly let one platform's assumption leak into the portable contract on the theory that "that's what the
first real consumer's platform needs anyway." That reasoning is exactly backwards; the honest platform gap
that already exists (OQ1 — no documented Windows demotion primitive) must be stated explicitly wherever it
bites, never hidden behind a claim of parity the implementation does not have.

## 5. Verify replacement-policy and platform-primitive choices against real prior art before implementing

`docs/prior-art.md` exists so a policy choice (sampling vs. an intrusive LRU list, io_uring vs. a worker
thread pool, whether `madvise(MADV_WILLNEED)` blocks) is argued from a real, cited source with an honest
confidence tag — not recalled from training data or picked because it "seems standard." If the citation
that would justify a choice doesn't exist yet in `docs/prior-art.md`, fetch and quote the real source
before writing the code, and add it there. A plausible-sounding description is not the same thing as a
verified one — this project's own prior-art document already flags, honestly, which of its citations are
verbatim-quoted vs. tool-summarized vs. title-only; extend that same discipline to new sources, don't
relax it.

## 6. Before changing the public API, re-check `docs/sub0llm-consumer-trace.md` and `docs/sub0tieredcache-reconciliation.md`

Those two documents are this project's acceptance tests, not just examples — the API is checked against a
real caller's real needs (Sub0Llm's `ParallelExperts`) and against the layering contract a real sibling
project (Sub0TieredCache) would need to keep working if it ever adopted this library. If a proposed API change
would make any of the consumer trace's call patterns, or any of the reconciliation's "kept identical"
guarantees, stop being expressible, either the change is wrong or those documents need updating to show
why — never leave them silently inconsistent with the actual API.

## 7. A budget claim needs an actual enforcement test; a concurrency claim needs an actual concurrent test

REQUIREMENTS.md R7's "hard budget, never a silent overshoot" and R13's "overlapping-range coalescing" are
both testable, specific claims — the same "verify eviction/caching algorithm choices" discipline extends
to verifying the *code*, not just the *design*, actually enforces them under real concurrent load and real
budget pressure, not only single-threaded happy-path exercise.

## 8. Correctness before performance (mirrors Sub0TieredCache AGENTS.md §8)

A change is not "done" on a benchmark number alone. The paging scheduler has no correctness
infrastructure yet (the first tests cover capability-tool argument validation only), which makes this rule easy to skip past in the early stages —
resist that. A claimed throughput or hit-rate improvement needs the same rigor `docs/prior-art.md` already
models for citations: state what was actually measured, under what real (or realistically simulated)
access pattern, and whether it was checked against a correctness baseline first. The B21 motivating
evidence (design.md §6) is itself a cautionary tale about trusting a plausible-sounding architecture
without measuring it: `ParallelExperts` is architecturally identical to a shape that DID scale in
isolation, and does not scale in production, for a reason still not fully understood — a reminder that
"this should scale" is not the same claim as "this was measured to scale."

## 9. Observability ships with the feature, not after it (REQUIREMENTS.md R10's `stats` extension)

A new replacement policy or platform-primitive backend is not complete without corresponding `stats()`
coverage, including the `hint_unconsumed` counter specifically — it is the direct measure of whether a
declared/speculative admission policy is actually working, and without it "is prefetching helping" is a
guess (see OQ7 in design.md §7).

## 10. Implementation status must describe what actually works

Implementation began at the user's request on 2026-09-22. README.md distinguishes working experimental
diagnostics from the still-unimplemented paging scheduler. Update the plan and validation evidence as
each package lands; do not describe a capability query as a working residency backend.

## 11. Use the codified loop, not hand-run checks

`python scripts/dev.py check` (tests on Windows and Linux, ASan+UBSan, TSan, mutation) runs before every
commit. `dev.py bench --baseline main` runs before any performance claim. Gates live in
`docs/perf/kpi_gates.json`; the policy is in `docs/DEVELOPMENT_WORKFLOW.md`. The rule was established on
2026-09-25, when the mutation stage's first run found a vacuous admission test that every other gate
had passed.

