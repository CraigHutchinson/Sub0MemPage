# Intel USM groundwork

This optional tool implements I1/I2 in [implementation-plan.md](implementation-plan.md). It reports
capabilities and can check host-USM -> device-USM -> kernel -> host data movement. It does not implement
MemPage registration, file I/O, caching, leases, physical pinning or zero-copy. Ordinary inventory
submits no kernel. `--copy-check` explicitly runs two sizes and changed-data passes, checking every
uint32 against a CPU reference with dependencies between upload, kernel and readback.

## Build and run

Use a separate CMake build with an installed IntelLLVM SYCL compiler and its runtime environment.
On Linux use `icpx`; on Windows use `icx` from a shell with the MSVC and oneAPI environments loaded.
No download or SDK installation is performed by CMake.

```text
cmake -S . -B build/intel-usm -G Ninja -DCMAKE_CXX_COMPILER=icx -DSUB0MEMPAGE_BUILD_INTEL_USM_SPIKE=ON
cmake --build build/intel-usm
ctest --test-dir build/intel-usm --output-on-failure
build/intel-usm/tools/intel_usm/sub0mempage-usm-probe --copy-check
```

Windows adds `.exe`. Without `--copy-check`, the executable only inventories capabilities. PCI ID
selection defaults to the historical Intel `7d67` device; `--device-id=HEX` permits explicit selection
of another Intel Level Zero GPU. Absence fails instead of selecting a different device/backend.
CTest checks only help/argument rejection and never intentionally enumerates or starts GPU work.
Exit 0 means the requested checks passed, 1 a runtime/qualification failure, 2 invalid arguments.

Record OS, compiler, backend, actual device/driver, source commit and output when qualifying a tuple.
The six USM aspects are capability reports, not a concurrent-access or performance result. Prepared
copy is reported as header availability only; direct Level Zero extension inventory is unbuilt.

## Provenance and limits

Adapted selection and diagnostic boundaries from Sub0Llm `tools/intel_probe/usm_capabilities.cpp` and
`runtime.hpp`, reviewed at Sub0Llm commit `2b25b1d86ccb0fa3b30f469183efde261b2af33b`. The original historical probe
and prepared-copy experiment stay there; this tool deliberately needs no sibling checkout. Primary
contracts are linked in [prior-art.md §9](prior-art.md#9-intel-usm-boundary).

Allocation owners drain submitted work before freeing USM, including exceptional paths. Allocations
are reused across changed-data passes. This diagnostic itself allocates and blocks; it is not a hot-path
API. Shared-USM execution, prepared-copy behavior, file mapping import, concurrency and performance are
separate unqualified paths. No core public API or provider hierarchy is introduced for an experiment.

## Validation

2026-09-10, Windows 10.0.26220, IntelLLVM 2025.3.3 with the installed MSVC environment:

- Default MSVC 19.51 configure/build: pass, no Intel dependency; baseline still has zero core tests.
- Optional IntelLLVM C++23 configure/build: pass.
- CTest `intel_usm_cli`: 1/1; help plus five invalid-argument cases checked exact exit codes.
- Actual Intel `8086:7d67`, Level Zero driver `1.15.39183+3`: all 2,099,200 uint32 comparisons pass
  across 4 KiB/4 MiB and two changed-input passes. [Raw output](intel-usm/2026-09-10/windows.txt).
- Missing `ffffffff` device: exit 1, explicit no-fallback error. MSVC with spike ON: configure rejects.
- Linux: WSL Ubuntu-24.04 has CMake 4.2.3 but no discovered g++/clang++/icpx compiler. Build/runtime
  qualification remains OPEN; no installation performed, and Windows evidence does not close it.

Self-review checked every helper's call from `main`, target dependency isolation, allocation/error
unwinding, explicit event ordering, all-element validation and exact identity reporting. Fixed the
installed SYCL API differences (`exception_list::size`, non-const event wait) before successful build.
No public paging API was added. No concurrent-device correctness or performance conclusion is drawn.
