# Optional Intel USM capability inventory

This executable queries one explicitly selected Intel Level Zero GPU. It adapts the six-aspect
inventory from Sub0Llm's spike without an engine dependency. It does not allocate USM, submit kernels,
prepare host ranges or measure performance. `status=pass` means the queries completed, not that every
aspect is supported. Read [the integration boundary](../../docs/intel-usm.md) before interpreting it.

## Windows (validated with oneAPI 2025.3.3)

Use an x64 Visual Studio developer shell with the matching oneAPI compiler environment initialized.
`icx-cl.exe`, its SYCL runtime DLLs and Ninja must be on PATH; the compiler library directory must be
on LIB. Use a fresh build directory when selecting a compiler; CMake can reset options when changing
an existing build's compiler.

```powershell
cmake -S . -B build/intel-usm -G Ninja -DCMAKE_CXX_COMPILER=icx-cl -DCMAKE_BUILD_TYPE=Release -DSUB0MEMPAGE_BUILD_INTEL_USM_PROBE=ON
cmake --build build/intel-usm --parallel 1
ctest --test-dir build/intel-usm --output-on-failure
$env:ONEAPI_DEVICE_SELECTOR = 'level_zero:gpu'
.\build\intel-usm\tools\intel\sub0mempage-usm-capabilities.exe --device-id 0x7d67
```

## Linux (recipe; SYCL runtime not validated here)

With an existing Intel DPC++ environment initialized:

```sh
cmake -S . -B build/intel-usm -G Ninja -DCMAKE_CXX_COMPILER=icpx -DCMAKE_BUILD_TYPE=Release -DSUB0MEMPAGE_BUILD_INTEL_USM_PROBE=ON
cmake --build build/intel-usm --parallel 1
ONEAPI_DEVICE_SELECTOR=level_zero:gpu build/intel-usm/tools/intel/sub0mempage-usm-capabilities --device-id 0x7d67
```

The compiler check rejects non-IntelLLVM toolchains for the optional target. Default portable builds
never include SYCL headers. Ordinary CTest runs only the offline argument tests, never device queries.

## Selection and evidence

`--device-id` accepts one to four hex digits following lowercase `0x`. The example ID is the measured
machine, not a built-in default. The probe requires vendor `8086`, GPU type and Level Zero; zero matches
or multiple visible matches fail. A model ID cannot distinguish two identical adapters. Runtime device
filters affect visibility and must be recorded alongside results; there is no first-device fallback.
`--help` works without enumerating devices. Unknown, missing, duplicate and extra arguments are errors.
Exit codes: 0 completed inventory/help, 1 runtime/selection/output failure, 2 usage error.

Archive stdout, exit code, OS, compiler version, runtime environment filters, git revision/dirty state,
and hashes of the executable, source and `ProbeOptions.hpp` for each qualification. The checked-in
[manifest](../../docs/validation/2026-09-22/usm-manifest.json) is an example. Device identity and the
SYCL driver string come from the selected runtime device. The driver string is not a ZE API version.
Output is a line-oriented diagnostic (quoted names), not JSON or a stable public library ABI.

The report distinguishes header declaration from runtime qualification. Direct Level Zero extension
inventory, allocation execution, prepared-copy execution, mapped import, concurrent access, physical
residency and performance are explicitly untested. Those experiments remain in Sub0Llm until separately
qualified here. No inference of kernel access to ordinary file mappings is valid from this inventory.
