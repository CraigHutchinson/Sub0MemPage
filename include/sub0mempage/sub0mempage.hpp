#pragma once

/** @file sub0mempage.hpp
 *  @brief Entry points for Sub0MemPage's residency-orchestration contract. See REQUIREMENTS.md for the
 *         normative contract (R1-R14) and README.md sec 3 for the full call signatures this header will
 *         implement. See docs/design.md for the reasoning behind each call's shape, including sec 8's
 *         ownership-model resolution (caller-owned destination slots) this header's names reflect.
 *
 *  STATUS: DESIGN SKELETON, NOT A WORKING LIBRARY. Every declaration below is commented-out and
 *  unimplemented -- this file exists to pin the call surface's names and shapes as design decisions are
 *  finalized, matching Sub0Firn's own include/sub0firn/sub0firn.hpp skeleton-only precedent one layer up.
 *  Do not add implementation code here without first checking README.md's status line and AGENTS.md
 *  Sec 10.
 */

namespace sub0mempage {

// Skeleton. register_region/register_slots/prefetch/wait/resolve/release/try_resolve/wont_need/stats/
// on_evict (and the optional open_stream/next pair) are not implemented yet -- REQUIREMENTS.md and
// README.md sec 3 are the contract they will be built to. See docs/design.md for the full reasoning
// behind each call's shape (sec 8 specifically for why destinations are caller-owned slots, not
// library-owned memory), docs/sub0firn-reconciliation.md for how this contract composes with Sub0Firn's
// own API one layer up, and docs/sub0llm-consumer-trace.md for how Sub0Llm's real ExpertCache /
// ParallelExperts decode loop would call this contract once it exists -- including the concrete point
// that ExpertCache's own pool_ array is registered via register_slots UNCHANGED, never reallocated.
//
// Ownership model, stated once here because it shapes every signature below (docs/design.md sec 8,
// docs/prior-art.md sec 5a): Sub0MemPage NEVER allocates bulk destination storage. The caller allocates
// register_slots' backing array itself; Sub0MemPage only tracks which byte range currently lives in
// which slot and schedules the async fills that keep that true.
//
// Intended call surface (README.md sec 3), named here as a design-skeleton reference only:
//
//   register_region(backing, policy_hints)                       -> region_handle
//   register_slots(region, slot_bytes, num_slots, slots_ptr)      -> pool_handle
//   prefetch(pool, ranges[], class)                                -> ticket
//   wait(ticket, deadline?)                                        -> outcome
//   resolve(pool, ranges[], class)                                 -> lease[]
//   release(lease)
//   try_resolve(pool, ranges[], class)                             -> optional<lease[]>
//   wont_need(pool, ranges[])
//   stats(pool)                                                    -> { ... }
//   on_evict(pool, callback)                                        // optional
//
//   open_stream(pool, next_range_callback, private_data)           -> stream_handle   // optional second shape
//   next(stream_handle)                                             -> lease           // optional second shape

} // namespace sub0mempage
