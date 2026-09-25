#pragma once

/** @file sub0mempage.hpp
 *  @brief Entry points for Sub0MemPage's residency-orchestration contract. See REQUIREMENTS.md for the
 *         normative contract (R1-R14) and README.md sec 3 for the full call signatures this header will
 *         implement. See docs/design.md for the reasoning behind each call's shape, including sec 8's
 *         ownership-model resolution (caller-owned destination slots) this header's names reflect.
 *
 *  STATUS: umbrella header. The M2 draft implementations live in slot_pool.hpp (cached-slot mode) and
 *  transfer_set.hpp (explicit-destination mode), both over the backend seam in transfer.hpp. The call
 *  surface below is the README sec 3 contract. Each call names where it is implemented, or that it is
 *  deferred.
 */

#include "slot_pool.hpp"
#include "transfer_set.hpp"

namespace sub0mempage {

// README.md sec 3 contract -> M2 draft implementation:
//
//   register_region + register_slots   -> SlotPool::create(SlotPoolConfig{source, slot_storage, ...})
//   prefetch(pool, ranges[], class)    -> SlotPool::prefetch -> Ticket
//   wait(ticket, deadline?)            -> SlotPool::wait -> WaitOutcome
//   resolve / try_resolve              -> SlotPool::resolve / try_resolve -> leases into caller storage
//   release(lease)                     -> Lease::reset / destructor
//   wont_need / stats                  -> SlotPool::wont_need / stats
//   explicit-destination transfers     -> TransferSet::submit -> Claim (transfer-contract.md)
//   on_evict, open_stream/next         -> deferred: no consumer yet
//
// Ownership (docs/design.md sec 8): Sub0MemPage never allocates bulk destination storage; callers
// register their own slot/destination memory and Sub0MemPage schedules fills and tracks residency.

} // namespace sub0mempage
