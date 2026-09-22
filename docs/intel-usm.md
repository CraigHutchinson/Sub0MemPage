# Intel USM: inclusion decision and evidence boundary

Decision, 2026-09-22: include a standalone, optional capability inventory in this project (M1).
Keep actual transfer scheduling behind M5's lifecycle and correctness gates. No SYCL or Level Zero
headers enter the portable library. The existing Sub0Llm spike stays intact, including its experimental
prepared-copy and direct-Level-Zero branches; this is a narrower diagnostic, not a wholesale migration.

## Historical evidence, not a fresh machine qualification

Sub0Llm commit `6cefe894e7c74b9bf8a894e7cbc60f72857d9dce` recorded on 2026-09-09:
Windows 10.0.26220, DPC++ 2025.3.3, Intel Graphics PCI `8086:7d67`, Level Zero backend,
SYCL driver string `1.15.39183+3`. Host/shared/device allocation aspects were true; system allocation,
atomic-host and atomic-shared aspects were false. Direct ZE inventory was not built because development
headers and the loader import library were absent. A separate 4 KiB prepared-copy prepare/release pair
passed, which establishes acceptance only, not pinning, kernel accessibility or useful transfer speed.

Evidence: [spike](https://github.com/CraigHutchinson/Sub0Llm/blob/7dbc6341cfbd4b9f92b272b89534078137b59ff0/docs/INTEL_IGPU_USM_CAPABILITY_SPIKE.md),
[raw output](https://github.com/CraigHutchinson/Sub0Llm/blob/7dbc6341cfbd4b9f92b272b89534078137b59ff0/docs/intel-groundwork/2026-09-09/runtime/usm-capabilities-probe.txt),
and [manifest](https://github.com/CraigHutchinson/Sub0Llm/blob/7dbc6341cfbd4b9f92b272b89534078137b59ff0/docs/intel-groundwork/2026-09-09/runtime/usm-capabilities-manifest.json).
The artifact links pin the later evidence commit; the manifest records the probe source commit above.
Do not interpret the SYCL driver string as the Level Zero API version.

## Contracts verified against primary sources

- [SYCL 2020 USM](https://registry.khronos.org/SYCL/specs/sycl-2020/html/sycl-2020.html#sec:usm):
  allocation provenance and context determine legal device access. The six allocation/atomic aspects
  are independent reported capabilities, not measurements of successful allocations or concurrency.
  With system USM false, there is no standard-SYCL basis for treating an ordinary mapped file pointer
  as device-dereferenceable. CPU mapping validity says nothing about GPU validity.
- [Intel copy optimization extension](https://github.com/intel/llvm/blob/sycl/sycl/doc/extensions/experimental/sycl_ext_oneapi_copy_optimize.asciidoc):
  "may be used as either the source or destination of an explicit copy operation". It is experimental;
  prepare/release is a repeated-copy optimization, not ownership or completion. Overlapping preparations
  are undefined even across contexts; release must match the original pointer/context. Header declaration
  is recorded separately from runtime execution. Core leases must not be implemented by these calls.

Confidence: primary source text checked 2026-09-22; applicability is conditional on the installed runtime.
Inference for this project: keep CPU file fills, explicit copies, logical slot pins and hardware residency
as distinct concepts. No physical zero-copy, import support, coherence or performance follows from M1.

## Gates for a future transfer adapter

1. Select an exact runtime device/context and qualify the caller's allocation kind. Never fall back to
   another device/backend when the requested one is absent or ambiguous.
2. Verify deterministic staged-copy contents, asynchronous errors, last-event lifetime and rollback.
   CPU and GPU access are explicitly ordered; never infer safe concurrent access from shared DRAM.
3. For prepared-copy experiments, use one persistent non-overlapping registration per caller range,
   retain the context, drain copies explicitly, then release registration before freeing host storage.
4. Direct mapped import is separate: matching ZE development files, inventory on the selected device's
   driver, import acceptance, bounds and read-only correctness, then writable ordering if needed.
5. Only after correctness, measure capacity, repeated-copy amortization and overlap in a reserved hardware
   window. The September spike's exploratory timings are not a portable backend-selection rule.
