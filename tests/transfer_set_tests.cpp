// M2 explicit-destination gates (docs/transfer-contract.md "Portable test gates"), driven by FakeBackend.

#include "fake_backend.hpp"
#include "test_support.hpp"

#include <sub0mempage/transfer_set.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace sub0mempage;
using sub0mempage::test::FakeBackend;
using sub0mempage::test::FakeCompletion;
using sub0mempage::test::source_byte;
using sub0mempage::test::allocation_count;
using sub0mempage::test::check;
using sub0mempage::test::finish;
using sub0mempage::test::run;

constexpr std::byte CANARY{0xCD};

struct Fixture {
    FakeBackend backend;
    std::vector<std::byte> destination;
    std::unique_ptr<TransferSet> set;

    explicit Fixture(std::uint32_t max_claims = 4, std::size_t queue = 64, std::size_t destination_bytes = 256)
        : backend(queue), destination(destination_bytes, CANARY) {
        set = std::move(*TransferSet::create(
            {.source = SourceId{3}, .source_bytes = 1000, .destination = destination, .max_claims = max_claims},
            FillBackendRef(backend)));
    }
    ~Fixture() {
        backend.complete_all_newest_first();
        (void)set->drain();
    }

    [[nodiscard]] bool canaries_outside(std::uint64_t begin, std::uint64_t end) const {
        for (std::uint64_t i = 0; i < destination.size(); ++i) {
            if ((i < begin || i >= end) && destination[i] != CANARY) {
                return false;
            }
        }
        return true;
    }
};

[[nodiscard]] bool matches(std::span<const std::byte> bytes, std::uint64_t source_offset) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != source_byte(source_offset + i)) {
            return false;
        }
    }
    return true;
}

void test_validation() {
    FakeBackend backend(1);
    std::vector<std::byte> destination(16);
    check(TransferSet::create({.source_bytes = 10, .destination = destination, .max_claims = 0}, FillBackendRef(backend))
                  .error() == Status::invalid_argument,
          "zero claims rejected");
    Fixture f;
    check(f.set->submit({0, 0}, 0).error() == Status::empty_range, "empty source range");
    check(f.set->submit({995, 10}, 0).error() == Status::out_of_range, "source past EOF");
    check(f.set->submit({0, 10}, 250).error() == Status::out_of_range, "destination overrun");
    check(f.set->submit({0, 10}, UINT64_MAX - 3).error() == Status::out_of_range, "destination overflow");
    check(f.backend.accepted() == 0, "rejected submissions reach no backend");
}

void test_exact_bytes_and_canaries() {
    Fixture f;
    auto claim = f.set->submit({123, 37}, 50); // unaligned source and destination
    check(claim && claim->status() == Status::pending, "submitted claim pending");
    f.backend.complete(0);
    check(claim->wait() == Status::ok, "claim completes");
    check(claim->bytes().size() == 37 && matches(claim->bytes(), 123), "exact bytes at the chosen destination");
    check(f.canaries_outside(50, 87), "nothing written outside the claimed range");
}

void test_overlap_and_final_use() {
    Fixture f;
    auto first = f.set->submit({0, 32}, 0);
    check(f.set->submit({100, 8}, 31).error() == Status::busy, "overlapping destination rejected while in flight");
    check(f.set->submit({100, 8}, 32).has_value(), "adjacent destination accepted");
    f.backend.complete(0);
    check(first->wait() == Status::ok, "first completes");
    check(f.set->submit({100, 8}, 16).error() == Status::busy, "completed but held claim still reserves its range");
    first->reset();
    check(f.set->submit({100, 8}, 16).has_value(), "range reusable after final use");
}

void test_dropped_pending_claim_keeps_range() {
    Fixture f;
    auto claim = f.set->submit({0, 16}, 0);
    claim->reset(); // no cancellation: the backend may still be writing
    check(f.set->submit({0, 16}, 0).error() == Status::busy, "dropped pending claim still reserves its range");
    check(f.backend.pending() == 1, "dropping did not cancel");
    f.backend.complete(0);
    check(f.set->stats().records_in_use == 0, "record retired at terminal completion");
    check(f.set->submit({0, 16}, 0).has_value(), "range reusable after the writer finished");
}

void test_failures() {
    for (const FakeCompletion how : {FakeCompletion::short_read, FakeCompletion::io_error, FakeCompletion::cancelled}) {
        Fixture f;
        auto claim = f.set->submit({0, 20}, 0);
        f.backend.complete(0, how);
        const Status expected = how == FakeCompletion::short_read ? Status::short_read
                                : how == FakeCompletion::io_error ? Status::io_error
                                                                  : Status::cancelled;
        check(claim->status() == expected, "failure reported with its cause, never as ok");
        check(f.canaries_outside(0, 20), "failed transfer wrote nothing outside its range");
        check(f.set->stats().failed == 1, "failure counted");
    }
}

void test_duplicate_stale_and_exhaustion() {
    Fixture f(/*max_claims=*/1, /*queue=*/1);
    auto claim = f.set->submit({0, 8}, 0);
    check(f.set->submit({8, 8}, 8).error() == Status::ticket_exhausted, "claim table exhaustion reported");
    f.backend.complete(0);
    f.backend.redeliver_last(FakeCompletion::io_error);
    check(f.set->stats().stale_completions == 1 && claim->status() == Status::ok, "duplicate completion ignored");
    claim->reset();
    auto reused = f.set->submit({8, 8}, 0);
    f.backend.redeliver_last(); // previous generation's token
    check(f.set->stats().stale_completions == 2 && reused->status() == Status::pending,
          "old-generation completion cannot complete a reused record");
    f.backend.complete(0);
    reused->reset();

    Fixture q(4, /*queue=*/1);
    auto held = q.set->submit({0, 8}, 0);
    check(q.set->submit({8, 8}, 8).error() == Status::queue_exhausted, "backend queue exhaustion reported");
    check(q.set->stats().records_in_use == 1, "refused submission consumed no record");
}

void test_timeout_leaves_claim_live() {
    Fixture f;
    auto claim = f.set->submit({0, 8}, 0);
    check(claim->wait(Clock::now()) == Status::timeout, "deadline reports timeout");
    check(f.set->submit({0, 8}, 0).error() == Status::busy && f.backend.pending() == 1, "timeout freed nothing");
    f.backend.complete(0);
    check(claim->wait() == Status::ok, "completes after the timeout");
}

void test_hot_path_allocates_nothing() {
    Fixture f;
    const std::uint64_t before = allocation_count();
    for (int round = 0; round < 100; ++round) {
        auto a = f.set->submit({static_cast<std::uint64_t>(round), 64}, 0);
        auto b = f.set->submit({500, 64}, 64);
        f.backend.complete_all_newest_first();
        (void)a->wait();
        (void)b->status();
        (void)f.set->stats();
    }
    check(allocation_count() == before, "submit/wait/status/release/completion allocate nothing");
}

void test_concurrent_disjoint_claims() {
    Fixture f(8, 64, 8 * 64);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> corrupt{0};
    std::thread completer([&] {
        std::mt19937 rng(99);
        while (!stop.load()) {
            if (const std::size_t pending = f.backend.pending(); pending != 0) {
                f.backend.complete(rng() % pending);
            } else {
                std::this_thread::yield();
            }
        }
    });
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937 rng(t);
            for (int i = 0; i < 1000; ++i) {
                const std::uint64_t source = rng() % 900;
                // Two threads share each 64-byte lane, so busy rejections are expected and exercised.
                const std::uint64_t lane = (t + (rng() % 2) * 4) % 8;
                if (auto claim = f.set->submit({source, 64}, lane * 64)) {
                    if (claim->wait() == Status::ok && !matches(claim->bytes(), source)) {
                        ++corrupt;
                    }
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    stop = true;
    completer.join();
    check(corrupt.load() == 0, "no claimed range overwritten by a concurrent transfer");
    check(f.set->stats().records_in_use == 0 && f.set->stats().in_flight == 0, "concurrent run left nothing live");
}

} // namespace

int main() {
    run(test_validation, "test_validation");
    run(test_exact_bytes_and_canaries, "test_exact_bytes_and_canaries");
    run(test_overlap_and_final_use, "test_overlap_and_final_use");
    run(test_dropped_pending_claim_keeps_range, "test_dropped_pending_claim_keeps_range");
    run(test_failures, "test_failures");
    run(test_duplicate_stale_and_exhaustion, "test_duplicate_stale_and_exhaustion");
    run(test_timeout_leaves_claim_live, "test_timeout_leaves_claim_live");
    run(test_hot_path_allocates_nothing, "test_hot_path_allocates_nothing");
    run(test_concurrent_disjoint_claims, "test_concurrent_disjoint_claims");
    return finish();
}
