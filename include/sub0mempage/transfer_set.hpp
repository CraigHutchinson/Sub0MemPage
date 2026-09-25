#pragma once

/** @file transfer_set.hpp
 *  @brief Explicit-destination ownership mode: the caller chooses where each transfer lands in its own
 *         registered destination and owns all replacement policy; Sub0MemPage only moves the bytes and
 *         guarantees the range is not reused while a writer may still touch it.
 *
 *  STATUS: M2 draft (docs/implementation-plan.md). Contract: docs/transfer-contract.md "Two uses of one
 *  transfer scheduler" and REQUIREMENTS.md R16/R18. This is the form Sub0TieredCache's transformed
 *  output pools and contiguous rows need; there is no second, lower cache here.
 *
 *  Claim record states: Free -> Submitted -> Completed | Failed -> Free. A record returns to Free only
 *  when it is terminal AND its Claim handle is gone, so dropping a claim early never frees a range a
 *  backend is still writing (transfer-contract.md "Completion, byte validity and reuse").
 *
 *  Uses the same FillBackendRef/FillRequest seam as SlotPool: one scheduler, two ownership modes.
 *  Not enforced here: that the destination is not also registered with a SlotPool (R18 forbids it; a
 *  cross-instance registry is deferred until a consumer registers both kinds).
 */

#include "transfer.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace sub0mempage {

class TransferSet;

/** @brief Move-only claim on one destination range for one transfer.
 *
 *  While held, the caller may poll or wait; after `status() == ok` the bytes are valid and remain
 *  reserved until the claim is dropped (final use precedes reuse, R16). Dropping a pending claim does
 *  not cancel the transfer: the range stays reserved until the backend's terminal delivery.
 */
class Claim {
public:
    Claim() noexcept = default;
    Claim(Claim&& other) noexcept { *this = std::move(other); }
    Claim& operator=(Claim&& other) noexcept;
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;
    ~Claim() { reset(); }

    /// Non-blocking: pending, ok, or the terminal failure.
    [[nodiscard]] Status status() const noexcept;
    /// Blocks until terminal or `deadline`; timeout leaves the transfer and its claim live.
    [[nodiscard]] Status wait(Deadline deadline = std::nullopt) const noexcept;
    /// The claimed destination range. Holds source bytes only once status() == ok.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool is_held() const noexcept { return set_ != nullptr; }
    /// Ends final use. A still-pending transfer keeps its range reserved until terminal. Terminates on a
    /// lock failure, like Lease::reset.
    void reset() noexcept;

private:
    friend class TransferSet;

    TransferSet* set_ = nullptr; // non-owning; the set must outlive every claim (checked at destruction)
    std::uint32_t record_ = 0;
    std::uint32_t generation_ = 0;
    std::span<const std::byte> bytes_;
};

struct TransferSetConfig {
    SourceId source{};
    std::uint64_t source_bytes = 0;
    std::span<std::byte> destination; ///< Caller-owned; must outlive the set.
    std::uint32_t max_claims = 0;     ///< Bound on live + dropped-but-in-flight transfers.
};

struct TransferSetStats {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t stale_completions = 0;
    std::uint64_t bytes_read = 0;
    std::uint32_t in_flight = 0;
    std::uint32_t records_in_use = 0;
};

/** @brief Transfers from one immutable source into caller-chosen ranges of one caller-owned destination.
 *
 *  Non-movable for the same reason as SlotPool (it is the backend's completion sink). Destroying it
 *  with a held claim or an in-flight transfer calls std::terminate; drain() first.
 */
class TransferSet {
    struct Passkey {};

public:
    [[nodiscard]] static std::expected<std::unique_ptr<TransferSet>, Status> create(const TransferSetConfig& config,
                                                                                   FillBackendRef backend);

    TransferSet(Passkey, const TransferSetConfig& config, FillBackendRef backend);
    TransferSet(const TransferSet&) = delete;
    TransferSet& operator=(const TransferSet&) = delete;
    ~TransferSet();

    /** @brief Start reading `source` into the destination at `destination_offset`. Never blocks on I/O.
     *  The overlap check scans all max_claims records under the lock: bounded, O(max_claims).
     *  @return busy if the destination range overlaps any record still in use (held or in flight);
     *          ticket_exhausted/queue_exhausted when bounded resources are full.
     */
    [[nodiscard]] std::expected<Claim, Status> submit(ByteRange source, std::uint64_t destination_offset) noexcept;

    /// Administrative: blocks until no transfer is in flight.
    [[nodiscard]] Status drain(Deadline deadline = std::nullopt) noexcept;

    [[nodiscard]] TransferSetStats stats() const noexcept;

private:
    friend class Claim;

    enum class RecordState : std::uint8_t { free, submitted, completed, failed };

    struct Record {
        std::uint64_t destination_offset = 0;
        std::uint64_t length = 0;
        std::uint32_t generation = 0;
        RecordState state = RecordState::free;
        Status result = Status::pending;
        bool held = false;
    };

    static void deliver(void* self, TransferToken token, FillResult result) noexcept;
    void on_complete(TransferToken token, FillResult result) noexcept;
    [[nodiscard]] Status claim_status(std::uint32_t record, Deadline deadline, bool block) const noexcept;
    void release_claim(std::uint32_t record, std::uint32_t generation) noexcept;

    TransferSetConfig config_;
    FillBackendRef backend_;
    std::vector<Record> records_;
    TransferSetStats counters_;
    std::uint32_t held_ = 0;

    mutable std::mutex mutex_;
    mutable std::condition_variable progress_;
};

// ---------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------

inline Claim& Claim::operator=(Claim&& other) noexcept {
    if (this != &other) {
        reset();
        set_ = std::exchange(other.set_, nullptr);
        record_ = other.record_;
        generation_ = other.generation_;
        bytes_ = std::exchange(other.bytes_, {});
    }
    return *this;
}

inline void Claim::reset() noexcept {
    if (set_ != nullptr) {
        std::exchange(set_, nullptr)->release_claim(record_, generation_);
        bytes_ = {};
    }
}

inline Status Claim::status() const noexcept {
    return set_ == nullptr ? Status::invalid_argument : set_->claim_status(record_, std::nullopt, false);
}

inline Status Claim::wait(Deadline deadline) const noexcept {
    return set_ == nullptr ? Status::invalid_argument : set_->claim_status(record_, deadline, true);
}

inline std::expected<std::unique_ptr<TransferSet>, Status> TransferSet::create(const TransferSetConfig& config,
                                                                               FillBackendRef backend) {
    if (config.source_bytes == 0 || config.destination.empty() || config.max_claims == 0 ||
        config.max_claims == UINT32_MAX) {
        return std::unexpected(Status::invalid_argument);
    }
    return std::make_unique<TransferSet>(Passkey{}, config, backend);
}

inline TransferSet::TransferSet(Passkey, const TransferSetConfig& config, FillBackendRef backend)
    : config_(config), backend_(backend), records_(config.max_claims) {}

inline TransferSet::~TransferSet() {
    const std::scoped_lock lock(mutex_);
    if (counters_.in_flight != 0 || held_ != 0) {
        std::terminate(); // a writer or a claim would outlive this object; teardown never drains implicitly
    }
}

inline std::expected<Claim, Status> TransferSet::submit(ByteRange source, std::uint64_t destination_offset) noexcept {
    if (const Status status = detail::validate_range(source, config_.source_bytes); status != Status::ok) {
        return std::unexpected(status);
    }
    const auto destination_end = detail::checked_end(destination_offset, source.length);
    if (!destination_end || *destination_end > config_.destination.size()) {
        return std::unexpected(Status::out_of_range);
    }
    const std::scoped_lock lock(mutex_);
    std::uint32_t free_record = UINT32_MAX;
    for (std::uint32_t i = 0; i < records_.size(); ++i) {
        const Record& record = records_[i];
        if (record.state == RecordState::free) {
            free_record = std::min(free_record, i);
        } else if (destination_offset < record.destination_offset + record.length &&
                   record.destination_offset < *destination_end) {
            return std::unexpected(Status::busy);
        }
    }
    if (free_record == UINT32_MAX) {
        return std::unexpected(Status::ticket_exhausted);
    }
    Record& record = records_[free_record];
    const std::span<std::byte> destination =
        config_.destination.subspan(static_cast<std::size_t>(destination_offset), static_cast<std::size_t>(source.length));
    const FillRequest request{
        .source = config_.source,
        .source_offset = source.offset,
        .destination = destination,
        .token = {free_record, record.generation + 1},
        .sink = {this, &TransferSet::deliver},
    };
    if (!backend_.submit(request)) {
        return std::unexpected(Status::queue_exhausted);
    }
    record = {.destination_offset = destination_offset, .length = source.length, .generation = record.generation + 1,
              .state = RecordState::submitted, .result = Status::pending, .held = true};
    ++held_;
    ++counters_.submitted;
    ++counters_.in_flight;
    ++counters_.records_in_use;

    Claim claim;
    claim.set_ = this;
    claim.record_ = free_record;
    claim.generation_ = record.generation;
    claim.bytes_ = destination;
    return claim;
}

inline void TransferSet::deliver(void* self, TransferToken token, FillResult result) noexcept {
    static_cast<TransferSet*>(self)->on_complete(token, result);
}

inline void TransferSet::on_complete(TransferToken token, FillResult result) noexcept {
    {
        const std::scoped_lock lock(mutex_);
        if (token.index >= records_.size() || records_[token.index].generation != token.generation ||
            records_[token.index].state != RecordState::submitted) {
            ++counters_.stale_completions;
            return;
        }
        Record& record = records_[token.index];
        record.result = detail::classify_fill(result, record.length);
        --counters_.in_flight;
        if (record.result == Status::ok) {
            record.state = RecordState::completed;
            ++counters_.completed;
            counters_.bytes_read += result.bytes;
        } else {
            record.state = RecordState::failed;
            ++counters_.failed;
        }
        if (!record.held) {
            record.state = RecordState::free;
            --counters_.records_in_use;
        }
        progress_.notify_all(); // under the lock: see SlotPool::on_complete
    }
}

inline Status TransferSet::claim_status(std::uint32_t record_index, Deadline deadline, bool block) const noexcept {
    std::unique_lock lock(mutex_);
    const Record& record = records_[record_index];
    const auto terminal = [&] { return record.state != RecordState::submitted; };
    if (block && deadline) {
        progress_.wait_until(lock, *deadline, terminal);
    } else if (block) {
        progress_.wait(lock, terminal);
    }
    if (!terminal()) {
        return block ? Status::timeout : Status::pending;
    }
    return record.result;
}

inline void TransferSet::release_claim(std::uint32_t record_index, std::uint32_t generation) noexcept {
    const std::scoped_lock lock(mutex_);
    Record& record = records_[record_index];
    if (record.generation != generation || !record.held) {
        std::terminate();
    }
    record.held = false;
    --held_;
    if (record.state != RecordState::submitted) {
        record.state = RecordState::free;
        --counters_.records_in_use;
    }
}

inline Status TransferSet::drain(Deadline deadline) noexcept {
    std::unique_lock lock(mutex_);
    const auto idle = [&] { return counters_.in_flight == 0; };
    if (deadline) {
        return progress_.wait_until(lock, *deadline, idle) ? Status::ok : Status::timeout;
    }
    progress_.wait(lock, idle);
    return Status::ok;
}

inline TransferSetStats TransferSet::stats() const noexcept {
    const std::scoped_lock lock(mutex_);
    return counters_;
}

} // namespace sub0mempage
