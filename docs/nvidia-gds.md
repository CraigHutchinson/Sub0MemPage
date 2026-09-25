# NVIDIA CUDA and GPUDirect Storage plan

Reviewed 2026-09-25 against NVIDIA primary documentation. **Design only: no CUDA transport or GDS
backend has been implemented or qualified in this repository.** CUDA availability is not GDS support.
[Transfer contract](transfer-contract.md) defines portable lifetime and budget semantics.

## Backend choices and honest platform support

| Path | Target | Meaning / gate |
|---|---|---|
| Explicit staged CUDA | Supported Windows/Linux CUDA deployment | File -> bounded host staging -> CUDA device allocation; correctness baseline, no direct-path claim |
| cuFile compatibility | Qualified native-Linux cuFile deployment | May use host staging; report compatibility or unknown, never call it direct |
| cuFile direct | Qualified native-Linux GPU/driver/kernel/filesystem/storage/topology tuple | Prove supported configuration and actual route; compare against staged baseline |
| Intel USM | Separately qualified SYCL context | Independent adapter; shared memory does not imply CUDA/GDS behavior |
| Portable host-only | Windows/Linux/macOS baseline | No GPU SDK required |

NVIDIA's documented GDS stack is Linux-based (`libcufile`, Linux storage drivers). We have no qualified
Windows or WSL cuFile path; use explicit staging there and record GDS as unavailable/unqualified.
Microsoft DirectStorage is a separate possible future backend, not another spelling of CUDA cuFile;
no DirectStorage support is promised by this plan. See NVIDIA's
[overview](https://docs.nvidia.com/gpudirect-storage/overview-guide/index.html) and
[installation guide](https://docs.nvidia.com/gpudirect-storage/troubleshooting-guide/index.html).

## Primary-source constraints and design consequences

1. **Directness is conditional.** The overview describes a path that "avoids a bounce buffer through the
   CPU"; successful cuFile calls can nevertheless use compatibility/staging paths. CUDA managed/system
   allocations are not assumed suitable for the direct peer path. Start qualification with explicit
   caller-owned CUDA device allocations; retain allocation-kind checks.
2. **File opening alone does not prove the path.** NVIDIA's
   [O_DIRECT guide](https://docs.nvidia.com/gpudirect-storage/o-direct-guide/index.html) distinguishes
   versions and filesystem constraints. CUDA 12.2/GDS 1.7 introduced non-O_DIRECT handling that can still
   take a direct path for suitable aligned requests. Use aligned requests as the first qualification
   case; keep unaligned/tail cases in the suite and report their actual path. Do not round outside a file
   or reserved destination. Alignments are backend properties, not a universal row-size requirement.
3. **Submission is not success.** The
   [cuFile API reference](https://docs.nvidia.com/gpudirect-storage/api-reference-guide/index.html)
   reports submission separately from completed byte count/error. Keep request parameters and result
   storage alive through stream completion; some inputs are evaluated at execution. Use an explicit
   non-null stream for async reads; the documented null-stream read is synchronous. Registration and
   deregistration may synchronize, so they are outside submit/release. Publish Ready only after checking
   both completion and the exact byte count. Preserve the registered base pointer when using offsets.
4. **Resources have a cost.** NVIDIA's
   [best practices](https://docs.nvidia.com/gpudirect-storage/best-practices-guide/index.html) calls for
   amortizing buffer registration. Register bounded pools up front; budget staging, registration and
   SDK scratch separately from useful payload. Direct transfers can still involve intermediate GPU
   buffers; avoid claiming end-to-end zero-copy without evidence.
5. **Qualification is versioned.** Record GPU identity, CUDA/cuFile/driver versions, kernel, mount/filesystem,
   NVMe/NIC and PCIe topology plus diagnostic output from the installed `gdscheck -p`. Newer native
   PCI-P2PDMA routes can have different kernel/driver prerequisites from `nvidia-fs`; do not hard-code a
   universal driver-module requirement. Recheck the installed-version support matrix rather than copying
   a historical GPU list into the portable API.

Confidence: the linked NVIDIA contract text was checked; this host's GDS support was not measured.
No driver install, kernel setting, ACS/IOMMU change or storage reformat is part of this design pass.

## Fallback policy and measurement

Registration/session policy distinguishes staged, prefer-direct-with-fallback and require-direct.
These are planned semantics, not new unused CLI flags. Require-direct rejects unsupported geometry or
unverifiable routing. Disable cuFile compatibility for direct-only qualification and collect corroborating
route diagnostics; that setting alone is not proof of a bounce-free transfer. Prefer-direct logs the
fallback reason and actual/unknown path. If route observability is unavailable, return unknown and do
not pass the direct-path gate. SDK compatibility and explicit staged CUDA are distinct paths.

Fallback uses already-budgeted host staging or fails capacity checks. A failed/partial device write
cannot be retried into storage still visible to compute; retain the destination claim, drain writers,
then retry or fail. Concurrent reads and consumer events must not reuse the same staging slot early.
Only GPU consumers of the stored representation avoid a CPU conversion round trip; a CPU-only codec
may make staged transfer the sensible path. Measure the complete conversion/transfer/compute pipeline.

## Milestones and independent tests

- **N0 inventory (M6a):** compile-only optional cuFile target, exact installed tuple, diagnostic report,
  file/buffer registration and clear unsupported results. No core SDK dependency.
- **N1 staged correctness (M4 prerequisite):** deterministic real-file contents, preallocated pinned
  host/device pools, async copy completion, readback and an independently checked checksum kernel.
  Exercise errors, EOF/tails, unaligned ranges, wrong context, budget exhaustion and delayed consumers.
- **N2 direct/compatibility correctness (M6b):** replay N1 on a supported native-Linux target with explicit
  staged, compatibility and direct-only modes. Inject short reads/async errors through the common fake
  adapter; test real failure paths too. Verify request/result lifetime, final-event retention and teardown.
- **N3 joined qualification:** TieredCache T4 reuses the same row/generation tests; Llm runs an advertised,
  actually supported CUDA compute case. Missing MoE/ngram kernels are separate blockers, not storage bugs.
- **N4 performance:** reserve hardware; compare complete cold/warm pipelines and realistic large-plane
  and small-row batches at equal total memory budgets. Record bytes/path, extra copies, queue depth,
  registration cost, overlap, latency distribution and application throughput. No universal GDS winner
  is selected in advance. Follow Sub0Llm's optimization protocol, including combinations and three passes.

Default CI runs fake completion and host I/O tests only. Optional CUDA correctness runs can qualify
staging without GDS. Linux GDS tests require an explicitly provisioned runner and exact tuple evidence;
missing infrastructure is a skip/unqualified result, not a pass. No automatic install or machine tuning.
