# Sub0MemPage Code Style Guide

Sub0MemPage sits one layer below [Sub0Firn](https://github.com/CraigHutchinson/Sub0Firn) in this project
family, so it follows Sub0Firn's own naming conventions rather than a different sibling project's — there
is no single "Sub0 house style" across the family (`Sub0Log`, for instance, follows `Sub0Pipeline`'s own,
different convention) — so this document is self-contained rather than a deltas-only note, matching
Sub0Firn's own `STYLE_GUIDE.md` precedent.

## Naming

- **Namespace**: lowercase, unnested — `sub0mempage::` (a sibling of `sub0firn::` and `sub0::`, not nested
  inside either; see README.md's naming section for why).
- **Types**: `PascalCase` — e.g. `RegionHandle`, `PrefetchTicket`, `Lease`.
- **Free functions**: `snake_case` — e.g. `register_region`, `try_resolve`, `wont_need`. Matches every
  function name already fixed by README.md §3's API surface.
- **Constants / compile-time values**: `ALL_CAPS` for genuine compile-time constants (a fixed page size
  known at compile time in a specific build); ordinary `snake_case` for runtime configuration values (a
  `budget_bytes` field) — the distinction is whether the value can ever differ between two builds of the
  same binary, not just whether it happens to be `const`.

## Modern C++ first, C++23 baseline

Reach for a language feature before inventing a workaround, and prefer the newest form the project's
standard supports. C++23 is a hard requirement (`CMakeLists.txt`'s `target_compile_features(...
cxx_std_23)`), matching this whole project family's baseline (Sub0Llm, Sub0Firn, Sub0Log) — not a
Sub0Llm-specific constraint carried over by habit, a deliberate choice for this project too. Prefer
`std::optional`/`std::span`/`std::expected`-shaped return types over out-parameters and sentinel values —
REQUIREMENTS.md's `resolve`→lease, `prefetch`→ticket, and `try_resolve`→`optional<lease>` contracts are
naturally expressed that way.

## Compile-time over runtime, where the decision is known upfront

This is a standing preference across the whole `Sub0` project family (carried directly from Sub0Llm's own
`AGENTS.md` §2: "bake every decision that's known upfront as `constexpr`/`if constexpr` — never a runtime
branch"). For Sub0MemPage specifically: the platform seam (Windows `PrefetchVirtualMemory`/IOCP vs. Linux
`io_uring`/`madvise`) is known at compile time for any given build and should be selected via `#if`-guarded
platform dispatch, not a runtime branch re-checked on every call — the two implementations never coexist
in one running process. Anything a caller decides once per region (page size, whether the demotion
primitive exists on this platform) belongs in `policy_hints` at `register_region` time, not re-evaluated
per `prefetch` call.

## Portability is load-bearing, not aspirational (REQUIREMENTS.md R14)

No header may contain an unconditional `#include <windows.h>`, POSIX-only header, or platform-specific
syscall without an `#if`-guarded portable abstraction on every other platform. A change that only compiles
on the author's own platform is not done. Where a genuine platform gap exists (REQUIREMENTS.md's OQ1 —
Windows demotion), it must be stated explicitly in the implementation's own documentation, never silently
degraded and never smoothed over as if the guarantee were uniform.

## Comment and citation discipline

Every non-obvious design decision gets a comment that says *why*, not just *what* — the style already used
throughout this project's own docs (`docs/prior-art.md`'s confidence tags, `REQUIREMENTS.md`'s "sentence
first, explanation second" structure). Concretely:

- A comment justifying an algorithm/data-structure choice (a replacement policy, a platform primitive
  choice) cites the real source it came from — `docs/prior-art.md` if one already exists there, a freshly
  fetched and quoted source if not. "It seemed reasonable" is not a citation.
- A comment noting a deliberate limitation or deferred feature says so explicitly (`// deferred:` or
  equivalent), the same "explicitly deferred, not silently dropped" discipline `docs/design.md` §7 already
  uses at the design level.
- Where an implementation choice reconciles with a specific real consumer's need, cite the concrete case
  (`docs/sub0llm-consumer-trace.md`'s style: real numbers, real call sites, not a hypothetical).

## No third-party dependencies in the header-only core

Matches R14's portability spirit and `CMakeLists.txt`'s interface-library shape: the vendored header-only
client must build with nothing beyond the C++23 standard library and the OS's own platform headers behind
an `#if` guard. A future linked library, once one becomes genuinely necessary for real background-thread
I/O machinery, may take on a real dependency (Boost.Asio, liburing) — that boundary is exactly why this
project (like Sub0Firn) starts header-only and grows a compiled component only when the need is real, not
speculative.
