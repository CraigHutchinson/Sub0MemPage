# M0/M1 validation — 2026-09-22

M0 plan commit: `ee0e565`. M1 source identity is captured by SHA-256 in
[usm-manifest.json](usm-manifest.json); [usm-capabilities.txt](usm-capabilities.txt) is the actual output.
The manifest records the pre-commit base plus exact hashes, rather than guessing a future commit ID.

| Gate | Result |
|---|---|
| Windows MSVC 19.51.36246, C++23 Release, warnings as errors | 1/1 CTest; 28 checks, 0 failures |
| Ubuntu 24.04 WSL2, GCC 15.2.0, C++23 Release, warnings as errors | 1/1 CTest; 28 checks, 0 failures |
| Linux GCC 15.2.0, Debug + ASan/UBSan | 1/1 CTest passed |
| Windows DPC++ 2025.3.3, optional SYCL target | Compiled/linked; offline CTest passed; help exit 0 |
| Real Intel `8086:7d67` inventory | Exit 0; host/shared/device=1, system/atomic-host/atomic-shared=0 |
| Invalid selector `invalid` | Usage exit 2, before device enumeration |
| Absent selector `0xffff` | Runtime selection exit 1; no fallback |
| Enable optional target under MSVC | Configure failed with explicit IntelLLVM requirement |

The default scaffold previously registered no tests. M1 introduces one offline executable; it does
not test the unimplemented scheduler. No allocations, copy correctness, GPU coherence or speed are
qualified by the capability query. Linux SYCL runtime, macOS, and ambiguous identical physical adapters
were not exercised. CI already includes Windows/Linux/macOS portable builds; no CI run is claimed here.

## C++ review

Reviewed the local M1 diff against `ee0e565` using cpp-review's L0–L3 checks before commit. This is a local
implementation checkpoint, not a published PR review. `ProbeOptions::parse` is consumed by `main`;
`select_device` and `print_report` are executable-local; the CMake option builds that executable. No
speculative backend classes or unused public core APIs were added. No SYCL include leaks into the core.
The parser uses the standard `from_chars` facility; startup enumeration is outside any paging hot path.
Failure results are checked, device ambiguity fails closed, and there is no shared mutable cache.
The parser is not constexpr: the tested toolchain/library combinations are only required to provide
runtime `from_chars`. The fixed aspect table is constexpr. No outstanding MUST findings.

The plan review corrected raw/decoded aliasing, wait/lease lifetime, physical-residency overclaims,
queue exhaustion, overlap granularity and next-layer routing assumptions. A further documentation
consistency pass propagated the wait/lease distinction to the normative R2 and overview text.
