# Sub0TieredCache / Sub0MemPage boundary

Revision S1, 2026-09-25. This replaces the earlier claim that adopting the lower layer required no
contract changes. The draft's unowned `try_get` view, implicit budgets and invalidation lifetime were
not sufficient under concurrent reuse. Both projects remain pre-implementation at this boundary.

See [transfer contract](transfer-contract.md), [cache integration plan](../../Sub0TieredCache/docs/integration-plan.md)
and [application acceptance](../../Sub0Llm/docs/STORAGE_STACK_PLAN.md).

| Responsibility | Owner | Composition |
|---|---|---|
| Row index -> source extents | TieredCache via caller adapter | MemPage receives validated byte ranges only |
| Formats/model codecs | Llm adapter | TieredCache schedules conversion and caches its output |
| Row keys/version/representation and eviction | TieredCache | No second lower eviction policy on those output allocations |
| Raw byte cache and transfer completion | MemPage | Cache retains raw leases until gather/codec completion |
| Bulk allocation | Caller of MemPage, often TieredCache | Register at startup; separate encoded, decoded, host and device budgets |
| Zero-copy lifetime | RowLease above, byte lease/transfer claim below | Hold backing ownership through final CPU/GPU use |
| Local file / CUDA / Intel / GDS transport | MemPage optional backend | Explicit capability, domain and fallback status |
| HTTP and persistent cache versions | TieredCache | Validate/publish immutable local files before lower local-file use |

## Required draft API refinements

- `try_get` returns `optional<RowLease>`; a ticket is not a lease.
- Table registration distinguishes source/output width and representation, domain, codec and budgets.
- Invalidation creates a generation; old readers drain, and stale completions cannot publish new rows.
- MemPage supports transfers into explicitly reserved destinations as well as the cached-byte facade.
  A row requiring contiguity uses a reserved contiguous output/gather; raw chunk leases remain segmented.
- Registration and explicit shutdown may block. Steady-state submission/release cannot do I/O or hidden
  GPU synchronization. Allocation/SDK setup is performed ahead of the hot path.

## What remains unchanged

The libraries do not derive model IDs, parse weight files below the application adapter, or change
model values. Host resolve-before-compute remains supported. Row coalescing and byte-chunk coalescing
are separate optimizations scoped to generation, representation and destination; neither promises a
single DMA into several GPUs. Advisory eviction callbacks are not a coherence protocol.

## Acceptance order

MemPage M2/M3 standalone -> TieredCache T0/T1 with real lower transport -> Llm S1 CPU fixture.
CUDA staging follows with the same leases/errors; native-Linux GDS and Intel USM qualify separately.
Each upper stage defines its fixtures early and feeds missing contracts downward; implementation and
dependency pins advance upward only after the lower gate passes. No cyclic build dependencies.
