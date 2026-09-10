#pragma once

/** @file sub0mempage.hpp
 *  @brief Entry points for Sub0MemPage's residency-orchestration contract. See REQUIREMENTS.md for the
 *         normative contract (R1-R13) and README.md sec 3 for the full call signatures this header will
 *         implement. See docs/design.md for the reasoning behind each call's shape.
 *
 *  STATUS: DESIGN SKELETON, NOT A WORKING LIBRARY. Every declaration below is commented-out and
 *  unimplemented -- this file exists to pin the call surface's names and shapes as design decisions are
 *  finalized, matching Sub0Firn's own include/sub0firn/sub0firn.hpp skeleton-only precedent one layer up.
 *  Do not add implementation code here without first checking README.md's status line and AGENTS.md
 *  Sec 10.
 */

namespace sub0mempage {

// Skeleton. register_region/prefetch/wait/resolve/release/try_resolve/wont_need/set_budget/stats/
// on_evict (and the optional open_stream/next pair) are not implemented yet -- REQUIREMENTS.md and
// README.md sec 3 are the contract they will be built to. See docs/design.md for the full reasoning
// behind each call's shape, docs/sub0firn-reconciliation.md for how this contract composes with
// Sub0Firn's own API one layer up, and docs/sub0llm-consumer-trace.md for how Sub0Llm's real
// ParallelExperts decode loop would call this contract once it exists.
//
// Intended call surface (README.md sec 3), named here as a design-skeleton reference only:
//
//   register_region(backing, budget_bytes, policy_hints) -> region_handle
//   prefetch(region, ranges[], class)                    -> ticket
//   wait(ticket, deadline?)                               -> outcome
//   resolve(region, ranges[], class)                      -> lease
//   release(lease)
//   try_resolve(region, ranges[], class)                  -> optional<lease>
//   wont_need(region, ranges[])
//   set_budget(region, budget_bytes)
//   stats(region)                                         -> { ... }
//   on_evict(region, callback)                             // optional
//
//   open_stream(region, next_range_callback, private_data) -> stream_handle   // optional second shape
//   next(stream_handle)                                     -> lease          // optional second shape

} // namespace sub0mempage
