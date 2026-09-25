#pragma once

/** @file transfer.hpp
 *  @brief Vocabulary shared by both ownership modes (cached slots and explicit destinations): status
 *         codes, byte ranges, transfer tokens and the one backend seam every fill goes through.
 *
 *  STATUS: M2 draft (docs/implementation-plan.md). Signatures here are proven by the deterministic fake
 *  backend tests, not frozen -- docs/transfer-contract.md is the normative text they implement.
 *
 *  One scheduler, two ownership modes (transfer-contract.md "Two uses of one transfer scheduler"): a
 *  backend sees only FillRequests. It never learns whether the destination is a cached slot or a
 *  caller-reserved range, so a real backend (M3+) is written once and serves both.
 */

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace sub0mempage {

/// Every outcome a call or a fill can report. Exhaustion and failure are values, never exceptions:
/// the hot-path calls are noexcept (REQUIREMENTS.md R3, R7).
enum class Status : std::uint8_t {
    ok,
    pending,          ///< Fill still in flight (only from non-blocking observers).
    not_resident,     ///< try_resolve: at least one chunk is not Ready; nothing was pinned.
    pool_exhausted,   ///< Every slot is pinned or filling; no reclaimable victim (R7).
    batch_too_large,  ///< Request exceeds the registered per-call chunk bound or its own pool.
    queue_exhausted,  ///< Backend refused submission (its bounded queue is full).
    ticket_exhausted, ///< Ticket/claim table full, including dropped-but-still-in-flight records.
    out_of_range,     ///< Range extends past the source extent or destination span, or overflows.
    empty_range,
    invalid_argument, ///< Registration parameters are inconsistent (zero sizes, span mismatch...).
    busy,             ///< Destination overlaps a live claim.
    short_read,       ///< Backend completed with fewer bytes than requested; nothing published.
    io_error,
    cancelled,        ///< Backend terminated the fill without writing a complete range.
    timeout,          ///< Deadline passed; the fill and its destination are STILL live.
    declined,         ///< Speculative hint not admitted by the replacement policy (R5).
};

/// A byte extent inside a registered source. `length == 0` is rejected as `empty_range`.
struct ByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

/// Admission class for prefetch/resolve (REQUIREMENTS.md R5): governs admission, not priority.
enum class AdmissionClass : std::uint8_t {
    declared,    ///< The caller has computed it will read this range.
    speculative, ///< A predictor guesses it might; admitted only if it beats the eviction candidate.
};

/// Caller-chosen identity of an immutable source (file + generation). Opaque to the core; the backend
/// uses it to find the file. A new generation of the same file is a different SourceId.
enum class SourceId : std::uint32_t {};

using Clock = std::chrono::steady_clock;
using Deadline = std::optional<Clock::time_point>;

/// Identifies one fill attempt. `generation` lets a sink reject a stale or duplicate completion for a
/// record that has since been retired and reused (transfer-contract.md "Completion, byte validity").
struct TransferToken {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
};

/// What a backend reports when a fill reaches a terminal state. `bytes` is the count actually written;
/// anything other than `status == ok && bytes == requested` is a failed fill and never published.
struct FillResult {
    Status status = Status::ok;
    std::uint64_t bytes = 0;
};

/** @brief Where a backend delivers a FillResult. Type-erased so one backend serves both modes.
 *
 *  `deliver` may be invoked on any backend thread, but never from inside FillBackendRef::submit --
 *  the sink holds its own lock while submitting.
 */
class CompletionSink {
public:
    using Fn = void (*)(void* context, TransferToken token, FillResult result) noexcept;

    constexpr CompletionSink() noexcept = default;
    constexpr CompletionSink(void* context, Fn fn) noexcept : context_(context), fn_(fn) {}

    void deliver(TransferToken token, FillResult result) const noexcept { fn_(context_, token, result); }

private:
    void* context_ = nullptr; // non-owning; the pool/transfer set that issued the request
    Fn fn_ = nullptr;
};

/// One unit of work handed to a backend: read `destination.size()` bytes from `source` at
/// `source_offset` into `destination`, then deliver exactly one FillResult carrying `token` to `sink`.
struct FillRequest {
    SourceId source{};
    std::uint64_t source_offset = 0;
    std::span<std::byte> destination; // non-owning; caller-owned, pinned by the issuer until terminal
    TransferToken token;
    CompletionSink sink;
};

/** @brief Type-erased, non-owning reference to a backend.
 *
 *  Backend contract (checked by the FillBackend concept, documented here because a concept cannot):
 *  - `submit` never blocks on I/O and never allocates; it returns false if its bounded queue is full.
 *  - `submit` is called with the issuer's lock held, so it must not deliver a completion inline or
 *    call back into any Sub0MemPage object.
 *  - Every accepted request gets exactly one terminal delivery. Only that delivery proves the backend
 *    has stopped writing the destination.
 *  Erased (not a template parameter) so Lease/Ticket/Claim stay plain types; the cost is one indirect
 *  call per submitted fill, negligible next to the I/O it issues.
 */
class FillBackendRef {
public:
    // Excludes FillBackendRef itself: it satisfies the submit() requirement, so without this a copy
    // from a non-const lvalue would pick this constructor and wrap a reference to the source ref.
    template <class Backend>
        requires(!std::same_as<std::remove_cv_t<Backend>, FillBackendRef>) &&
                requires(Backend& backend, const FillRequest& request) {
            { backend.submit(request) } noexcept -> std::same_as<bool>;
        }
    explicit FillBackendRef(Backend& backend) noexcept
        : backend_(&backend),
          submit_([](void* self, const FillRequest& request) noexcept {
              return static_cast<Backend*>(self)->submit(request);
          }) {}

    [[nodiscard]] bool submit(const FillRequest& request) const noexcept { return submit_(backend_, request); }

private:
    void* backend_; // non-owning; must outlive every pool/transfer set registered against it
    bool (*submit_)(void*, const FillRequest&) noexcept;
};

namespace detail {

/// `offset + length` without wraparound, or nullopt.
[[nodiscard]] constexpr std::optional<std::uint64_t> checked_end(std::uint64_t offset,
                                                                 std::uint64_t length) noexcept {
    if (length > UINT64_MAX - offset) {
        return std::nullopt;
    }
    return offset + length;
}

/// Validates a range against an extent: ok, empty_range or out_of_range.
[[nodiscard]] constexpr Status validate_range(ByteRange range, std::uint64_t extent) noexcept {
    if (range.length == 0) {
        return Status::empty_range;
    }
    const auto end = checked_end(range.offset, range.length);
    return end && *end <= extent ? Status::ok : Status::out_of_range;
}

/// A fill succeeded iff the backend says ok AND wrote exactly what was asked. Short reads never count.
[[nodiscard]] constexpr Status classify_fill(FillResult result, std::uint64_t requested) noexcept {
    if (result.status != Status::ok) {
        return result.status;
    }
    return result.bytes == requested ? Status::ok : Status::short_read;
}

} // namespace detail

} // namespace sub0mempage
