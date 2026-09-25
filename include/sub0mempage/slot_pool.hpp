#pragma once

/** @file slot_pool.hpp
 *  @brief Cached-slot ownership mode: Sub0MemPage decides which caller-owned slot holds which
 *         slot-sized source chunk, schedules fills into them, and hands out leases that pin them.
 *
 *  STATUS: M2 draft (docs/implementation-plan.md), proven against the deterministic fake backend in
 *  tests/. Not frozen. Contract: REQUIREMENTS.md R3-R13, docs/transfer-contract.md.
 *
 *  Slot state machine (transfer-contract.md "Completion, byte validity and reuse"):
 *
 *      Free --claim--> Filling --ok--> Ready --evict (unpinned only)--> Free
 *                         |
 *                         +--error/short/cancel--> Free, or Failed while resolvers still pin it
 *                                                  (Failed --last unpin--> Free)
 *
 *  A slot leaves Filling only on the backend's terminal delivery for its current generation, so it is
 *  never reused while a worker can still write it. Pinned and Filling slots are never eviction victims.
 *
 *  Allocation: everything is sized once in create() (REQUIREMENTS.md R9, Sub0Llm AGENTS.md sec 1).
 *  prefetch/try_resolve/resolve/release/wait/completion never allocate; resolve writes leases into
 *  caller-provided storage. Synchronization: one mutex per pool, never held across backend I/O (the
 *  backend's submit is non-blocking by contract), plus one condition variable that only resolve/wait/
 *  drain sleep on. Thread-safe, not wait-free.
 *
 *  Deferred (no consumer yet, AGENTS.md sec 8 of Sub0Llm / README sec 3): on_evict callbacks, the
 *  open_stream/next shape, and a cross-instance guard that one allocation is not registered with both a
 *  SlotPool and a TransferSet (R18).
 */

#include "transfer.hpp"

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace sub0mempage {

class SlotPool;

/** @brief Move-only pin on one resident source chunk; the only thing that prevents slot reuse (R8).
 *
 *  `bytes()` is the part of the originally requested range that lies in this chunk, so a multi-chunk
 *  request yields several leases (segments), never a falsely contiguous pointer (R13). Destruction or
 *  reset() is `release`: it never blocks on I/O and never drains anything (R3).
 */
class Lease {
public:
    Lease() noexcept = default;
    Lease(Lease&& other) noexcept { *this = std::move(other); }
    Lease& operator=(Lease&& other) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease() { reset(); }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    /// Source offset of bytes().front().
    [[nodiscard]] std::uint64_t source_offset() const noexcept { return source_offset_; }
    [[nodiscard]] bool is_held() const noexcept { return pool_ != nullptr; }

    /** @brief Unpins the slot (no-op on an empty lease); the slot becomes a reuse candidate, not reused eagerly.
     *  @note noexcept over a mutex lock: a lock failure (std::system_error) terminates. A release that
     *        cannot complete would leave the slot pinned forever, which is not recoverable.
     */
    void reset() noexcept;

private:
    friend class SlotPool;

    SlotPool* pool_ = nullptr; // non-owning; the pool must outlive every lease (checked at destruction)
    std::uint32_t slot_ = 0;
    std::uint32_t generation_ = 0;
    std::span<const std::byte> bytes_;
    std::uint64_t source_offset_ = 0;
};

/** @brief Move-only handle on a prefetch's completion record. Records completion, NOT ownership.
 *
 *  Dropping a ticket relinquishes only its record; in-flight fills continue, and the record's table
 *  entry is recycled once they finish. It never pins a slot: follow wait() with resolve/try_resolve.
 */
class Ticket {
public:
    Ticket() noexcept = default;
    Ticket(Ticket&& other) noexcept { *this = std::move(other); }
    Ticket& operator=(Ticket&& other) noexcept;
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    ~Ticket() { reset(); }

    [[nodiscard]] bool is_held() const noexcept { return pool_ != nullptr; }
    /// Relinquishes the record (no-op on an empty ticket); in-flight fills continue. Same terminate
    /// contract as Lease::reset.
    void reset() noexcept;

private:
    friend class SlotPool;

    SlotPool* pool_ = nullptr; // non-owning
    std::uint32_t record_ = 0;
    std::uint32_t generation_ = 0;
};

/// Per-chunk tally from wait(). `status` is ok only if every chunk filled; timeout if any is still
/// pending (those fills and their slots stay live); otherwise the first failure in request order.
struct WaitOutcome {
    Status status = Status::ok;
    std::uint32_t filled = 0;
    std::uint32_t failed = 0;
    std::uint32_t declined = 0; ///< Not admitted: speculative decline, pool/queue exhaustion.
    std::uint32_t pending = 0;
};

/// Registration parameters. `slot_storage.size()` must equal `slot_bytes * num_slots` exactly: that
/// product IS the hard budget (R7); there is no separate budget and no resize.
struct SlotPoolConfig {
    SourceId source{};
    std::uint64_t source_bytes = 0;     ///< Immutable source extent; the last chunk is clipped here.
    std::size_t slot_bytes = 0;         ///< Also the source chunk size.
    std::uint32_t num_slots = 0;
    std::span<std::byte> slot_storage;  ///< Caller-owned; must outlive the pool.
    std::uint32_t max_tickets = 0;      ///< Bound on live + dropped-but-in-flight prefetch records.
    std::uint32_t max_batch_chunks = 0; ///< Bound on chunks in one prefetch/resolve/try_resolve call.
};

/// Observability snapshot (R10 extension, AGENTS.md sec 9). Nothing's behaviour depends on it.
struct SlotPoolStats {
    std::uint32_t slots_total = 0;
    std::uint32_t slots_pinned = 0;
    std::uint32_t slots_ready = 0;
    std::uint32_t slots_filling = 0;
    std::uint32_t high_water_slots = 0; ///< Most simultaneously non-free slots.
    std::uint64_t hint_issued = 0;      ///< Chunks named by prefetch.
    std::uint64_t hint_admitted = 0;    ///< Chunks for which prefetch started a fill.
    std::uint64_t hint_resident = 0;    ///< Chunks already Ready or Filling when hinted.
    std::uint64_t hint_declined = 0;    ///< Speculative chunks refused by admission.
    std::uint64_t hint_unconsumed = 0;  ///< Prefetched fills evicted/failed before any resolve (R6).
    std::uint64_t admission_failures = 0; ///< Declared chunks refused: no victim or backend queue full.
    std::uint64_t resolve_hits = 0;
    std::uint64_t resolve_misses = 0;
    std::uint64_t coalesced = 0;        ///< Requests that joined an already-live fill (R13).
    std::uint64_t evictions = 0;
    std::uint64_t fills_failed = 0;
    std::uint64_t stale_completions = 0; ///< Duplicate/late deliveries rejected by generation.
    std::uint64_t bytes_read = 0;
};

/** @brief Cached raw-byte pool over caller-owned slots. See the file comment for the state machine.
 *
 *  Non-movable (the backend holds a pointer to it as the completion sink), hence create() returns a
 *  unique_ptr. Destroying a pool with outstanding leases, tickets or in-flight fills calls
 *  std::terminate: those would be use-after-free, and teardown must never hide a drain (R3). Call
 *  drain() on an administrative path first.
 */
class SlotPool {
    struct Passkey {};

public:
    [[nodiscard]] static std::expected<std::unique_ptr<SlotPool>, Status> create(const SlotPoolConfig& config,
                                                                                FillBackendRef backend);

    SlotPool(Passkey, const SlotPoolConfig& config, FillBackendRef backend);
    SlotPool(const SlotPool&) = delete;
    SlotPool& operator=(const SlotPool&) = delete;
    ~SlotPool();

    /** @brief Hint: start fills for every missing chunk of `ranges`. Never blocks on I/O (R3, R4).
     *
     *  Per-chunk admission failures are recorded in the ticket, not returned here. Returns an error
     *  only for a request that could not be recorded at all (validation, batch bound, ticket table).
     *  Hinted fills are not accesses (R6): they do not set the CLOCK reference bit.
     */
    [[nodiscard]] std::expected<Ticket, Status> prefetch(std::span<const ByteRange> ranges,
                                                         AdmissionClass admission) noexcept;

    /// Blocks until every fill the ticket names is terminal or `deadline` passes. Pins nothing.
    [[nodiscard]] WaitOutcome wait(const Ticket& ticket, Deadline deadline = std::nullopt) noexcept;

    /** @brief Pin every chunk of `ranges`, filling misses, and block until all are Ready (R3).
     *
     *  All-or-nothing: on any failure no lease is left held. Fails with pool_exhausted when no victim
     *  exists, batch_too_large when this call's own pins fill the pool (never waits on itself).
     *  @param out Receives one lease per chunk in request order; must hold at least that many. Leases
     *             already held in the entries it overwrites are released first.
     *  @return Number of leases written.
     */
    [[nodiscard]] std::expected<std::size_t, Status> resolve(std::span<const ByteRange> ranges,
                                                             std::span<Lease> out) noexcept;

    /// Non-blocking, never starts I/O. Pins all chunks iff all are already Ready; else not_resident.
    [[nodiscard]] std::expected<std::size_t, Status> try_resolve(std::span<const ByteRange> ranges,
                                                                 std::span<Lease> out) noexcept;

    /// Demotion hint: clears the reference bit of resident chunks so CLOCK takes them next. Never
    /// forces eviction, never blocks on I/O. Out-of-range or non-resident ranges are ignored.
    void wont_need(std::span<const ByteRange> ranges) noexcept;

    /// Administrative: blocks until no fill is in flight. Does not touch leases or tickets.
    [[nodiscard]] Status drain(Deadline deadline = std::nullopt) noexcept;

    [[nodiscard]] SlotPoolStats stats() const noexcept;

private:
    friend class Lease;
    friend class Ticket;

    static constexpr std::uint32_t NONE = UINT32_MAX;

    enum class SlotState : std::uint8_t { free, filling, ready, failed };

    struct Slot {
        std::uint64_t chunk = 0;
        std::uint32_t generation = 0;
        std::uint32_t pins = 0;
        std::uint32_t waiter_head = NONE; ///< Intrusive list of WaiterEntry indices awaiting this fill.
        SlotState state = SlotState::free;
        Status failure = Status::ok;      ///< Valid in state failed.
        bool referenced = false;          ///< CLOCK second-chance bit; set by resolve, not prefetch.
        bool hint_unconsumed = false;
    };

    struct TicketRecord {
        std::uint32_t generation = 0;
        std::uint32_t chunks = 0;
        std::uint32_t pending = 0;
        bool held = false;
        bool in_use = false;
    };

    struct WaiterEntry {
        std::uint32_t next = NONE;
        Status outcome = Status::pending;
    };

    static void deliver(void* self, TransferToken token, FillResult result) noexcept;
    void on_complete(TransferToken token, FillResult result) noexcept;

    template <class Fn>
    void for_each_chunk(std::span<const ByteRange> ranges, Fn&& fn) const noexcept;
    [[nodiscard]] Status validate_batch(std::span<const ByteRange> ranges, std::size_t& chunks) const noexcept;

    [[nodiscard]] std::uint64_t chunk_length(std::uint64_t chunk) const noexcept;
    [[nodiscard]] std::byte* slot_data(std::uint32_t slot) const noexcept;
    [[nodiscard]] std::uint32_t claim_victim(AdmissionClass admission) noexcept;
    [[nodiscard]] Status start_fill(std::uint32_t slot, std::uint64_t chunk) noexcept;
    void make_free(std::uint32_t slot) noexcept;
    void unpin_locked(std::uint32_t slot) noexcept;
    void release_lease(std::uint32_t slot, std::uint32_t generation) noexcept;
    void discard_ticket(std::uint32_t record, std::uint32_t generation) noexcept;
    [[nodiscard]] Lease make_lease(std::uint32_t slot, std::uint64_t chunk, ByteRange range) noexcept;
    static void release_all(std::span<Lease> leases) noexcept;
    [[nodiscard]] std::size_t distinct_slots(std::span<const Lease> leases) const noexcept;

    [[nodiscard]] std::uint32_t index_find(std::uint64_t chunk) const noexcept;
    void index_insert(std::uint64_t chunk, std::uint32_t slot) noexcept;
    void index_erase(std::uint64_t chunk) noexcept;
    [[nodiscard]] std::size_t index_home(std::uint64_t chunk) const noexcept;

    SlotPoolConfig config_;
    FillBackendRef backend_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> index_;   ///< Open-addressed chunk -> slot, linear probing, no tombstones.
    std::vector<TicketRecord> tickets_;
    std::vector<WaiterEntry> waiters_;   ///< max_tickets * max_batch_chunks, record-major.
    std::uint32_t clock_hand_ = 0;
    std::uint32_t in_flight_ = 0;
    std::uint64_t pins_total_ = 0;
    std::uint32_t tickets_held_ = 0;
    std::uint32_t non_free_ = 0;
    int index_shift_ = 0;
    SlotPoolStats counters_;

    mutable std::mutex mutex_;
    std::condition_variable progress_; ///< Signalled on every terminal fill.
};

// ---------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------

inline Lease& Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        reset();
        pool_ = std::exchange(other.pool_, nullptr);
        slot_ = other.slot_;
        generation_ = other.generation_;
        bytes_ = other.bytes_;
        source_offset_ = other.source_offset_;
    }
    return *this;
}

inline void Lease::reset() noexcept {
    if (pool_ != nullptr) {
        std::exchange(pool_, nullptr)->release_lease(slot_, generation_);
        bytes_ = {};
    }
}

inline Ticket& Ticket::operator=(Ticket&& other) noexcept {
    if (this != &other) {
        reset();
        pool_ = std::exchange(other.pool_, nullptr);
        record_ = other.record_;
        generation_ = other.generation_;
    }
    return *this;
}

inline void Ticket::reset() noexcept {
    if (pool_ != nullptr) {
        std::exchange(pool_, nullptr)->discard_ticket(record_, generation_);
    }
}

inline std::expected<std::unique_ptr<SlotPool>, Status> SlotPool::create(const SlotPoolConfig& config,
                                                                         FillBackendRef backend) {
    if (config.slot_bytes == 0 || config.num_slots == 0 || config.num_slots >= NONE || config.source_bytes == 0 ||
        config.max_tickets == 0 || config.max_batch_chunks == 0 || config.max_tickets >= NONE) {
        return std::unexpected(Status::invalid_argument);
    }
    if (config.slot_bytes > SIZE_MAX / config.num_slots ||
        config.slot_storage.size() != config.slot_bytes * config.num_slots) {
        return std::unexpected(Status::invalid_argument);
    }
    if (config.max_batch_chunks > SIZE_MAX / config.max_tickets ||
        std::size_t{config.max_batch_chunks} * config.max_tickets >= NONE) {
        return std::unexpected(Status::invalid_argument);
    }
    return std::make_unique<SlotPool>(Passkey{}, config, backend);
}

inline SlotPool::SlotPool(Passkey, const SlotPoolConfig& config, FillBackendRef backend)
    : config_(config),
      backend_(backend),
      slots_(config.num_slots),
      index_(std::bit_ceil(std::size_t{config.num_slots} * 2), NONE),
      tickets_(config.max_tickets),
      waiters_(std::size_t{config.max_tickets} * config.max_batch_chunks) {
    index_shift_ = 64 - std::countr_zero(index_.size());
    counters_.slots_total = config.num_slots;
}

inline SlotPool::~SlotPool() {
    const std::scoped_lock lock(mutex_);
    if (in_flight_ != 0 || pins_total_ != 0 || tickets_held_ != 0) {
        // A backend could still write into caller storage / deliver into this object, or a lease or
        // ticket would dangle. Teardown never drains implicitly (transfer-contract.md), so fail loudly.
        std::terminate();
    }
}

// --- request normalisation ----------------------------------------------------------------------------

template <class Fn>
void SlotPool::for_each_chunk(std::span<const ByteRange> ranges, Fn&& fn) const noexcept {
    for (const ByteRange& range : ranges) {
        const std::uint64_t first = range.offset / config_.slot_bytes;
        const std::uint64_t last = (range.offset + range.length - 1) / config_.slot_bytes;
        for (std::uint64_t chunk = first; chunk <= last; ++chunk) {
            fn(chunk, range);
        }
    }
}

inline Status SlotPool::validate_batch(std::span<const ByteRange> ranges, std::size_t& chunks) const noexcept {
    chunks = 0;
    for (const ByteRange& range : ranges) {
        if (const Status status = detail::validate_range(range, config_.source_bytes); status != Status::ok) {
            return status;
        }
        const std::uint64_t span_chunks =
            (range.offset + range.length - 1) / config_.slot_bytes - range.offset / config_.slot_bytes + 1;
        if (span_chunks > config_.max_batch_chunks - chunks) {
            return Status::batch_too_large;
        }
        chunks += static_cast<std::size_t>(span_chunks);
    }
    return chunks == 0 ? Status::empty_range : Status::ok;
}

inline std::uint64_t SlotPool::chunk_length(std::uint64_t chunk) const noexcept {
    const std::uint64_t start = chunk * config_.slot_bytes;
    return std::min<std::uint64_t>(config_.slot_bytes, config_.source_bytes - start);
}

inline std::byte* SlotPool::slot_data(std::uint32_t slot) const noexcept {
    return config_.slot_storage.data() + std::size_t{slot} * config_.slot_bytes;
}

// --- chunk index: open addressing, linear probing, backward-shift deletion ----------------------------
// Fixed capacity (>= 2x slots, so load <= 0.5) sized at registration; no tombstones to accumulate.

inline std::size_t SlotPool::index_home(std::uint64_t chunk) const noexcept {
    return static_cast<std::size_t>((chunk * 0x9E3779B97F4A7C15ull) >> index_shift_);
}

inline std::uint32_t SlotPool::index_find(std::uint64_t chunk) const noexcept {
    const std::size_t mask = index_.size() - 1;
    for (std::size_t pos = index_home(chunk);; pos = (pos + 1) & mask) {
        const std::uint32_t slot = index_[pos];
        if (slot == NONE || slots_[slot].chunk == chunk) {
            return slot;
        }
    }
}

inline void SlotPool::index_insert(std::uint64_t chunk, std::uint32_t slot) noexcept {
    const std::size_t mask = index_.size() - 1;
    std::size_t pos = index_home(chunk);
    while (index_[pos] != NONE) {
        pos = (pos + 1) & mask;
    }
    index_[pos] = slot;
}

inline void SlotPool::index_erase(std::uint64_t chunk) noexcept {
    const std::size_t mask = index_.size() - 1;
    std::size_t hole = index_home(chunk);
    while (slots_[index_[hole]].chunk != chunk) {
        hole = (hole + 1) & mask;
    }
    for (std::size_t pos = (hole + 1) & mask; index_[pos] != NONE; pos = (pos + 1) & mask) {
        const std::size_t home = index_home(slots_[index_[pos]].chunk);
        // Move the entry back into the hole unless its home lies cyclically in (hole, pos].
        if (((pos - home) & mask) >= ((pos - hole) & mask)) {
            index_[hole] = index_[pos];
            hole = pos;
        }
    }
    index_[hole] = NONE;
}

// --- slot lifecycle -----------------------------------------------------------------------------------

/// CLOCK (second chance): approximate recency with one bit per slot and a bounded sweep, no ordered
/// list reshuffled per access (REQUIREMENTS.md R12). This is PostgreSQL's buffer-manager clock sweep with a
/// one-bit usage count (docs/prior-art.md sec 5, quoted from its buffer README). Declared admission may
/// clear reference bits while sweeping (at most two revolutions); speculative admission only takes a
/// free or already-unreferenced slot and ages nothing, so a guess never displaces a recent access (R5).
/// Initial deterministic policy only: no frequency sketch is claimed (implementation-plan.md).
inline std::uint32_t SlotPool::claim_victim(AdmissionClass admission) noexcept {
    const std::uint32_t n = config_.num_slots;
    const std::uint32_t steps = admission == AdmissionClass::declared ? 2 * n : n;
    for (std::uint32_t step = 0; step < steps; ++step) {
        const std::uint32_t slot = clock_hand_;
        clock_hand_ = clock_hand_ + 1 == n ? 0 : clock_hand_ + 1;
        Slot& candidate = slots_[slot];
        if (candidate.pins != 0) {
            continue;
        }
        if (candidate.state == SlotState::free) {
            return slot;
        }
        if (candidate.state != SlotState::ready) {
            continue;
        }
        if (candidate.referenced) {
            if (admission == AdmissionClass::declared) {
                candidate.referenced = false;
            }
            continue;
        }
        ++counters_.evictions;
        if (candidate.hint_unconsumed) {
            ++counters_.hint_unconsumed;
        }
        make_free(slot);
        return slot;
    }
    return NONE;
}

inline void SlotPool::make_free(std::uint32_t slot) noexcept {
    Slot& s = slots_[slot];
    if (s.state == SlotState::filling || s.state == SlotState::ready) {
        index_erase(s.chunk);
    }
    if (s.state != SlotState::free) {
        --non_free_;
    }
    s.state = SlotState::free;
    s.referenced = false;
    s.hint_unconsumed = false;
}

inline Status SlotPool::start_fill(std::uint32_t slot, std::uint64_t chunk) noexcept {
    Slot& s = slots_[slot];
    s.chunk = chunk;
    ++s.generation;
    s.state = SlotState::filling;
    s.waiter_head = NONE;
    s.failure = Status::ok;
    index_insert(chunk, slot);
    counters_.high_water_slots = std::max(counters_.high_water_slots, ++non_free_);

    const FillRequest request{
        .source = config_.source,
        .source_offset = chunk * config_.slot_bytes,
        .destination = {slot_data(slot), static_cast<std::size_t>(chunk_length(chunk))},
        .token = {slot, s.generation},
        .sink = {this, &SlotPool::deliver},
    };
    if (!backend_.submit(request)) {
        make_free(slot);
        return Status::queue_exhausted;
    }
    ++in_flight_;
    return Status::ok;
}

inline void SlotPool::deliver(void* self, TransferToken token, FillResult result) noexcept {
    static_cast<SlotPool*>(self)->on_complete(token, result);
}

inline void SlotPool::on_complete(TransferToken token, FillResult result) noexcept {
    {
        const std::scoped_lock lock(mutex_);
        if (token.index >= slots_.size() || slots_[token.index].generation != token.generation ||
            slots_[token.index].state != SlotState::filling) {
            ++counters_.stale_completions;
            return;
        }
        Slot& s = slots_[token.index];
        const Status status = detail::classify_fill(result, chunk_length(s.chunk));
        --in_flight_;
        if (status == Status::ok) {
            s.state = SlotState::ready;
            counters_.bytes_read += result.bytes;
        } else {
            ++counters_.fills_failed;
            if (s.hint_unconsumed) {
                ++counters_.hint_unconsumed;
            }
            // Never publish partial bytes: drop from the index at once; resolvers that pinned the fill
            // see Failed until the last of them unpins.
            index_erase(s.chunk);
            s.state = SlotState::failed;
            s.failure = status;
            s.hint_unconsumed = false;
            if (s.pins == 0) {
                make_free(token.index);
            }
        }
        for (std::uint32_t entry = std::exchange(s.waiter_head, NONE); entry != NONE;) {
            WaiterEntry& waiter = waiters_[entry];
            waiter.outcome = status;
            TicketRecord& record = tickets_[entry / config_.max_batch_chunks];
            if (--record.pending == 0 && !record.held) {
                record.in_use = false;
            }
            entry = std::exchange(waiter.next, NONE);
        }
        // Notify under the lock: once it is released a waiter may observe completion, return, and let
        // the owner destroy this pool, so no member may be touched after the unlock.
        progress_.notify_all();
    }
}

inline void SlotPool::unpin_locked(std::uint32_t slot) noexcept {
    Slot& s = slots_[slot];
    --pins_total_;
    if (--s.pins == 0 && s.state == SlotState::failed) {
        make_free(slot);
    }
}

inline void SlotPool::release_lease(std::uint32_t slot, std::uint32_t generation) noexcept {
    const std::scoped_lock lock(mutex_);
    // A pinned slot is never reclaimed, so its generation cannot have moved under a held lease.
    if (slots_[slot].generation != generation || slots_[slot].pins == 0) {
        std::terminate();
    }
    unpin_locked(slot);
}

inline Lease SlotPool::make_lease(std::uint32_t slot, std::uint64_t chunk, ByteRange range) noexcept {
    Slot& s = slots_[slot];
    ++s.pins;
    ++pins_total_;
    const std::uint64_t chunk_start = chunk * config_.slot_bytes;
    const std::uint64_t begin = std::max(range.offset, chunk_start);
    const std::uint64_t end = std::min(range.offset + range.length, chunk_start + chunk_length(chunk));
    Lease lease;
    lease.pool_ = this;
    lease.slot_ = slot;
    lease.generation_ = s.generation;
    lease.bytes_ = {slot_data(slot) + (begin - chunk_start), static_cast<std::size_t>(end - begin)};
    lease.source_offset_ = begin;
    return lease;
}

inline void SlotPool::release_all(std::span<Lease> leases) noexcept {
    for (Lease& lease : leases) {
        lease.reset();
    }
}

inline std::size_t SlotPool::distinct_slots(std::span<const Lease> leases) const noexcept {
    std::size_t distinct = 0;
    for (std::size_t i = 0; i < leases.size(); ++i) {
        const bool seen = std::any_of(leases.begin(), leases.begin() + static_cast<std::ptrdiff_t>(i),
                                      [&](const Lease& earlier) { return earlier.slot_ == leases[i].slot_; });
        distinct += seen ? 0 : 1;
    }
    return distinct;
}

// --- public operations --------------------------------------------------------------------------------

inline std::expected<Ticket, Status> SlotPool::prefetch(std::span<const ByteRange> ranges,
                                                        AdmissionClass admission) noexcept {
    std::size_t chunks = 0;
    if (const Status status = validate_batch(ranges, chunks); status != Status::ok) {
        return std::unexpected(status);
    }
    const std::scoped_lock lock(mutex_);
    const auto free_record = std::find_if(tickets_.begin(), tickets_.end(),
                                          [](const TicketRecord& record) { return !record.in_use; });
    if (free_record == tickets_.end()) {
        return std::unexpected(Status::ticket_exhausted);
    }
    const auto record_index = static_cast<std::uint32_t>(free_record - tickets_.begin());
    TicketRecord& record = *free_record;
    record = {.generation = record.generation + 1, .chunks = static_cast<std::uint32_t>(chunks),
              .pending = 0, .held = true, .in_use = true};
    ++tickets_held_;

    std::uint32_t entry = record_index * config_.max_batch_chunks;
    for_each_chunk(ranges, [&](std::uint64_t chunk, const ByteRange&) {
        WaiterEntry& waiter = waiters_[entry];
        waiter.next = NONE;
        ++counters_.hint_issued;
        std::uint32_t slot = index_find(chunk);
        if (slot != NONE && slots_[slot].state == SlotState::ready) {
            ++counters_.hint_resident;
            waiter.outcome = Status::ok;
        } else if (slot != NONE) {
            ++counters_.hint_resident;
            ++counters_.coalesced;
        } else if (slot = claim_victim(admission); slot == NONE) {
            const bool speculative = admission == AdmissionClass::speculative;
            ++(speculative ? counters_.hint_declined : counters_.admission_failures);
            waiter.outcome = speculative ? Status::declined : Status::pool_exhausted;
        } else if (const Status submitted = start_fill(slot, chunk); submitted != Status::ok) {
            ++counters_.admission_failures;
            waiter.outcome = submitted;
            slot = NONE;
        } else {
            ++counters_.hint_admitted;
            slots_[slot].hint_unconsumed = true;
        }
        if (slot != NONE && slots_[slot].state == SlotState::filling) {
            waiter.outcome = Status::pending;
            waiter.next = std::exchange(slots_[slot].waiter_head, entry);
            ++record.pending;
        }
        ++entry;
    });

    Ticket ticket;
    ticket.pool_ = this;
    ticket.record_ = record_index;
    ticket.generation_ = record.generation;
    return ticket;
}

inline WaitOutcome SlotPool::wait(const Ticket& ticket, Deadline deadline) noexcept {
    if (ticket.pool_ != this) {
        return {.status = Status::invalid_argument};
    }
    std::unique_lock lock(mutex_);
    const TicketRecord& record = tickets_[ticket.record_];
    const auto done = [&] { return record.pending == 0; };
    if (deadline) {
        progress_.wait_until(lock, *deadline, done);
    } else {
        progress_.wait(lock, done);
    }
    WaitOutcome outcome;
    const auto entries = std::span(waiters_).subspan(std::size_t{ticket.record_} * config_.max_batch_chunks,
                                                     record.chunks);
    for (const WaiterEntry& waiter : entries) {
        switch (waiter.outcome) {
        case Status::ok: ++outcome.filled; break;
        case Status::pending: ++outcome.pending; break;
        case Status::declined:
        case Status::pool_exhausted:
        case Status::queue_exhausted: ++outcome.declined; break;
        default: ++outcome.failed; break;
        }
        if (outcome.status == Status::ok && waiter.outcome != Status::ok && waiter.outcome != Status::pending) {
            outcome.status = waiter.outcome;
        }
    }
    if (outcome.pending != 0) {
        outcome.status = Status::timeout;
    }
    return outcome;
}

inline void SlotPool::discard_ticket(std::uint32_t record_index, std::uint32_t generation) noexcept {
    const std::scoped_lock lock(mutex_);
    TicketRecord& record = tickets_[record_index];
    if (record.generation != generation || !record.held) {
        std::terminate();
    }
    record.held = false;
    --tickets_held_;
    if (record.pending == 0) {
        record.in_use = false;
    }
}

inline std::expected<std::size_t, Status> SlotPool::resolve(std::span<const ByteRange> ranges,
                                                            std::span<Lease> out) noexcept {
    std::size_t chunks = 0;
    if (const Status status = validate_batch(ranges, chunks); status != Status::ok) {
        return std::unexpected(status);
    }
    if (out.size() < chunks) {
        return std::unexpected(Status::batch_too_large);
    }
    release_all(out.first(chunks)); // before locking: releasing re-enters the pool lock
    std::unique_lock lock(mutex_);
    std::size_t pinned = 0;
    Status failure = Status::ok;
    // `out` doubles as the record of what this call pinned, so unwinding needs no scratch allocation.
    const auto unwind = [&] {
        for (Lease& lease : out.first(pinned)) {
            unpin_locked(lease.slot_);
            lease.pool_ = nullptr;
            lease.bytes_ = {};
        }
    };
    for_each_chunk(ranges, [&](std::uint64_t chunk, const ByteRange& range) {
        if (failure != Status::ok) {
            return;
        }
        std::uint32_t slot = index_find(chunk);
        if (slot != NONE) {
            ++(slots_[slot].state == SlotState::ready ? counters_.resolve_hits : counters_.coalesced);
        } else {
            ++counters_.resolve_misses;
            slot = claim_victim(AdmissionClass::declared);
            if (slot == NONE) {
                const bool own_pins_fill_pool = distinct_slots(out.first(pinned)) >= config_.num_slots;
                failure = own_pins_fill_pool ? Status::batch_too_large : Status::pool_exhausted;
                return;
            }
            if (failure = start_fill(slot, chunk); failure != Status::ok) {
                return;
            }
        }
        slots_[slot].referenced = true;
        slots_[slot].hint_unconsumed = false;
        out[pinned++] = make_lease(slot, chunk, range);
    });
    if (failure != Status::ok) {
        unwind();
        return std::unexpected(failure);
    }
    // Pinned Filling slots cannot be reclaimed, so waiting here cannot lose them to eviction.
    progress_.wait(lock, [&] {
        return std::none_of(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(pinned),
                            [&](const Lease& lease) { return slots_[lease.slot_].state == SlotState::filling; });
    });
    for (const Lease& lease : out.first(pinned)) {
        if (slots_[lease.slot_].state == SlotState::failed) {
            failure = slots_[lease.slot_].failure;
            break;
        }
    }
    if (failure != Status::ok) {
        unwind();
        return std::unexpected(failure);
    }
    return pinned;
}

inline std::expected<std::size_t, Status> SlotPool::try_resolve(std::span<const ByteRange> ranges,
                                                                std::span<Lease> out) noexcept {
    std::size_t chunks = 0;
    if (const Status status = validate_batch(ranges, chunks); status != Status::ok) {
        return std::unexpected(status);
    }
    if (out.size() < chunks) {
        return std::unexpected(Status::batch_too_large);
    }
    release_all(out.first(chunks)); // before locking: releasing re-enters the pool lock
    const std::scoped_lock lock(mutex_);
    bool all_ready = true;
    for_each_chunk(ranges, [&](std::uint64_t chunk, const ByteRange&) {
        const std::uint32_t slot = index_find(chunk);
        all_ready = all_ready && slot != NONE && slots_[slot].state == SlotState::ready;
    });
    if (!all_ready) {
        return std::unexpected(Status::not_resident);
    }
    std::size_t pinned = 0;
    for_each_chunk(ranges, [&](std::uint64_t chunk, const ByteRange& range) {
        const std::uint32_t slot = index_find(chunk);
        ++counters_.resolve_hits;
        slots_[slot].referenced = true;
        slots_[slot].hint_unconsumed = false;
        out[pinned++] = make_lease(slot, chunk, range);
    });
    return pinned;
}

inline void SlotPool::wont_need(std::span<const ByteRange> ranges) noexcept {
    const std::scoped_lock lock(mutex_);
    for (const ByteRange& range : ranges) {
        if (detail::validate_range(range, config_.source_bytes) != Status::ok) {
            continue;
        }
        for_each_chunk(std::span(&range, 1), [&](std::uint64_t chunk, const ByteRange&) {
            if (const std::uint32_t slot = index_find(chunk); slot != NONE) {
                slots_[slot].referenced = false;
            }
        });
    }
}

inline Status SlotPool::drain(Deadline deadline) noexcept {
    std::unique_lock lock(mutex_);
    const auto idle = [&] { return in_flight_ == 0; };
    if (deadline) {
        return progress_.wait_until(lock, *deadline, idle) ? Status::ok : Status::timeout;
    }
    progress_.wait(lock, idle);
    return Status::ok;
}

inline SlotPoolStats SlotPool::stats() const noexcept {
    const std::scoped_lock lock(mutex_);
    SlotPoolStats snapshot = counters_;
    for (const Slot& s : slots_) {
        snapshot.slots_pinned += s.pins != 0 ? 1 : 0;
        snapshot.slots_ready += s.state == SlotState::ready ? 1 : 0;
        snapshot.slots_filling += s.state == SlotState::filling ? 1 : 0;
    }
    return snapshot;
}

} // namespace sub0mempage
