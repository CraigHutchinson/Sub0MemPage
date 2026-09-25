// M2 cached-slot gates (docs/transfer-contract.md "Portable test gates"), driven by FakeBackend.

#include <sub0mempage/testing/fake_backend.hpp>
#include "test_support.hpp"

#include <sub0mempage/slot_pool.hpp>

#include <array>
#include <atomic>
#include <chrono>
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

[[nodiscard]] bool matches_source(const Lease& lease) {
    for (std::size_t i = 0; i < lease.bytes().size(); ++i) {
        if (lease.bytes()[i] != source_byte(lease.source_offset() + i)) {
            return false;
        }
    }
    return true;
}

/// One pool over a fake backend. Storage is poisoned so an unfilled byte cannot pass as source data.
struct Fixture {
    FakeBackend backend;
    std::vector<std::byte> storage;
    std::unique_ptr<SlotPool> pool;

    Fixture(std::uint32_t slots, std::size_t slot_bytes, std::uint64_t source_bytes, std::uint32_t max_tickets = 4,
            std::uint32_t max_batch = 8, std::size_t queue = 64)
        : backend(queue), storage(std::size_t{slots} * slot_bytes, std::byte{0xCD}) {
        auto created = SlotPool::create({.source = SourceId{7},
                                         .source_bytes = source_bytes,
                                         .slot_bytes = slot_bytes,
                                         .num_slots = slots,
                                         .slot_storage = storage,
                                         .max_tickets = max_tickets,
                                         .max_batch_chunks = max_batch},
                                        FillBackendRef(backend));
        pool = std::move(*created);
    }
    ~Fixture() {
        backend.complete_all_newest_first();
        (void)pool->drain();
    }
};

void test_registration_validation() {
    FakeBackend backend(4);
    std::vector<std::byte> storage(64);
    const SlotPoolConfig good{.source_bytes = 100, .slot_bytes = 16, .num_slots = 4, .slot_storage = storage,
                              .max_tickets = 1, .max_batch_chunks = 1};
    check(SlotPool::create(good, FillBackendRef(backend)).has_value(), "valid registration accepted");
    auto bad = good;
    bad.num_slots = 3;
    check(SlotPool::create(bad, FillBackendRef(backend)).error() == Status::invalid_argument,
          "storage size must equal slots*slot_bytes");
    bad = good;
    bad.slot_bytes = 0;
    check(SlotPool::create(bad, FillBackendRef(backend)).error() == Status::invalid_argument, "zero slot size");
    bad = good;
    bad.source_bytes = 0;
    check(SlotPool::create(bad, FillBackendRef(backend)).error() == Status::invalid_argument, "empty source");
    bad = good;
    bad.max_tickets = 0;
    check(SlotPool::create(bad, FillBackendRef(backend)).error() == Status::invalid_argument, "zero tickets");
}

void test_range_validation() {
    Fixture f(2, 16, 100);
    std::array<Lease, 8> out;
    const auto resolve_status = [&](ByteRange range) {
        auto r = f.pool->try_resolve(std::span(&range, 1), out);
        return r ? Status::ok : r.error();
    };
    check(resolve_status({0, 0}) == Status::empty_range, "empty range rejected");
    check(resolve_status({90, 11}) == Status::out_of_range, "range past EOF rejected");
    check(resolve_status({UINT64_MAX - 1, 4}) == Status::out_of_range, "overflowing range rejected");
    check(resolve_status({0, 8 * 16 + 1}) != Status::ok, "range beyond batch bound rejected");
    std::array<Lease, 1> tiny;
    const ByteRange two_chunks{0, 20};
    check(f.pool->try_resolve(std::span(&two_chunks, 1), tiny).error() == Status::batch_too_large,
          "lease output smaller than chunk count rejected");
    check(f.backend.accepted() == 0, "rejected requests submit nothing");
}

void test_out_of_order_completion_and_segments() {
    Fixture f(4, 16, 100);
    const std::array ranges{ByteRange{10, 30}, ByteRange{50, 5}}; // chunks 0,1,2 and 3
    auto ticket = f.pool->prefetch(ranges, AdmissionClass::declared);
    check(ticket.has_value() && f.backend.pending() == 4, "prefetch issues one fill per chunk");

    std::array<Lease, 8> out;
    check(f.pool->try_resolve(ranges, out).error() == Status::not_resident, "try_resolve misses while filling");
    check(f.pool->stats().slots_pinned == 0, "failed try_resolve pins nothing");

    f.backend.complete_all_newest_first();
    const WaitOutcome outcome = f.pool->wait(*ticket);
    check(outcome.status == Status::ok && outcome.filled == 4, "wait sees all out-of-order fills");

    auto leased = f.pool->try_resolve(ranges, out);
    check(leased && *leased == 4, "multi-chunk range yields one lease per chunk");
    check(out[0].source_offset() == 10 && out[0].bytes().size() == 6, "first segment clipped to request");
    check(out[1].source_offset() == 16 && out[1].bytes().size() == 16, "middle segment is a full chunk");
    check(out[2].source_offset() == 32 && out[2].bytes().size() == 8, "last segment clipped to request");
    check(out[3].source_offset() == 50 && out[3].bytes().size() == 5, "second range segment");
    bool all_match = true;
    for (const Lease& lease : out) {
        all_match = all_match && (!lease.is_held() || matches_source(lease));
    }
    check(all_match, "leased bytes equal the independent source oracle");
}

void test_eof_tail_chunk() {
    Fixture f(2, 16, 40); // chunk 2 holds only 8 bytes
    const ByteRange tail{35, 5};
    std::array<Lease, 1> out;
    std::thread completer([&] {
        while (!f.backend.complete(0)) {
            std::this_thread::yield();
        }
    });
    auto leased = f.pool->resolve(std::span(&tail, 1), out);
    completer.join();
    check(leased && out[0].bytes().size() == 5 && matches_source(out[0]), "tail chunk read and clipped at EOF");
    check(f.pool->stats().bytes_read == 8, "tail fill requested only the bytes before EOF");
}

void test_failed_fills_never_publish() {
    for (const FakeCompletion how : {FakeCompletion::short_read, FakeCompletion::io_error, FakeCompletion::cancelled}) {
        Fixture f(2, 16, 64);
        const ByteRange range{0, 16};
        auto ticket = f.pool->prefetch(std::span(&range, 1), AdmissionClass::declared);
        f.backend.complete(0, how);
        const WaitOutcome outcome = f.pool->wait(*ticket);
        const Status expected = how == FakeCompletion::short_read ? Status::short_read
                                : how == FakeCompletion::io_error ? Status::io_error
                                                                  : Status::cancelled;
        check(outcome.status == expected && outcome.failed == 1, "failed fill reported with its cause");
        std::array<Lease, 1> out;
        check(f.pool->try_resolve(std::span(&range, 1), out).error() == Status::not_resident,
              "failed fill is not resident");
        check(f.pool->stats().slots_filling == 0 && f.pool->stats().slots_ready == 0, "failed slot returned free");

        auto retry = f.pool->prefetch(std::span(&range, 1), AdmissionClass::declared);
        f.backend.complete(0);
        check(f.pool->wait(*retry).status == Status::ok, "re-fetch after failure is allowed");
    }
}

void test_resolve_propagates_failure_and_unwinds() {
    Fixture f(4, 16, 64);
    const std::array ranges{ByteRange{0, 16}, ByteRange{16, 16}};
    std::array<Lease, 2> out;
    std::thread completer([&] {
        while (f.backend.pending() < 2) {
            std::this_thread::yield();
        }
        f.backend.complete(1, FakeCompletion::io_error);
        f.backend.complete(0);
    });
    auto leased = f.pool->resolve(ranges, out);
    completer.join();
    check(!leased && leased.error() == Status::io_error, "resolve reports a pinned fill's failure");
    check(!out[0].is_held() && !out[1].is_held() && f.pool->stats().slots_pinned == 0,
          "resolve failure leaves no lease held");
    check(f.pool->stats().slots_ready == 1, "sibling chunk that succeeded stays cached, unpinned");
}

void test_duplicate_and_stale_completion() {
    Fixture f(1, 16, 64);
    const ByteRange a{0, 16};
    const ByteRange b{16, 16};
    auto ticket = f.pool->prefetch(std::span(&a, 1), AdmissionClass::declared);
    f.backend.complete(0);
    f.backend.redeliver_last(FakeCompletion::io_error);
    check(f.pool->stats().stale_completions == 1, "duplicate completion rejected");
    check(f.pool->wait(*ticket).status == Status::ok, "duplicate did not overwrite the first result");

    auto reuse = f.pool->prefetch(std::span(&b, 1), AdmissionClass::declared); // evicts a's slot
    f.backend.redeliver_last();                                                // a's old token again
    check(f.pool->stats().stale_completions == 2, "old-generation completion rejected after reuse");
    check(f.pool->stats().slots_filling == 1, "stale completion did not publish the reused slot");
    f.backend.complete(0);
    std::array<Lease, 1> out;
    check(f.pool->try_resolve(std::span(&b, 1), out) && matches_source(out[0]), "reused slot holds new chunk");
}

void test_no_reuse_while_leased_or_filling() {
    Fixture f(1, 16, 64);
    const ByteRange a{0, 16};
    const ByteRange b{16, 16};
    auto filling = f.pool->prefetch(std::span(&a, 1), AdmissionClass::declared);
    auto blocked = f.pool->prefetch(std::span(&b, 1), AdmissionClass::declared);
    check(f.pool->wait(*blocked).status == Status::pool_exhausted, "filling slot is never a victim");
    f.backend.complete(0);

    std::array<Lease, 1> held;
    check(f.pool->try_resolve(std::span(&a, 1), held).has_value(), "lease acquired");
    std::array<Lease, 1> other;
    check(f.pool->resolve(std::span(&b, 1), other).error() == Status::pool_exhausted,
          "resolve cannot take a slot another lease pins");
    auto again = f.pool->prefetch(std::span(&b, 1), AdmissionClass::declared);
    check(f.pool->wait(*again).status == Status::pool_exhausted, "leased slot is never a victim");
    check(matches_source(held[0]), "leased bytes unchanged under pressure");

    held[0].reset();
    auto admitted = f.pool->prefetch(std::span(&b, 1), AdmissionClass::declared);
    f.backend.complete(0);
    check(f.pool->wait(*admitted).status == Status::ok, "released slot becomes reusable");
}

void test_batch_larger_than_pool() {
    Fixture f(2, 16, 64);
    const std::array ranges{ByteRange{0, 16}, ByteRange{16, 16}, ByteRange{32, 16}};
    std::array<Lease, 3> out;
    auto leased = f.pool->resolve(ranges, out); // fails before waiting: must not deadlock on itself
    check(!leased && leased.error() == Status::batch_too_large, "batch exceeding pool fails, never waits");
    check(f.pool->stats().slots_pinned == 0, "failed batch unpinned everything");

    Fixture repeat(2, 16, 64);
    const std::array same{ByteRange{0, 8}, ByteRange{4, 8}, ByteRange{8, 8}};
    std::thread completer([&] {
        while (!repeat.backend.complete(0)) {
            std::this_thread::yield();
        }
    });
    auto shared = repeat.pool->resolve(same, out);
    completer.join();
    check(shared && *shared == 3 && repeat.backend.accepted() == 1, "repeated chunk in one batch filled once");
    for (Lease& lease : out) {
        lease.reset(); // before `repeat` (declared later, destroyed first) goes away
    }
}

void test_coalescing() {
    Fixture f(2, 16, 64);
    const ByteRange a{0, 16};
    auto first = f.pool->prefetch(std::span(&a, 1), AdmissionClass::declared);
    auto second = f.pool->prefetch(std::span(&a, 1), AdmissionClass::speculative);
    check(f.backend.accepted() == 1, "overlapping live requests share one fill");
    f.backend.complete(0);
    check(f.pool->wait(*first).status == Status::ok && f.pool->wait(*second).status == Status::ok,
          "both tickets see the shared fill");
    check(f.pool->stats().coalesced == 1, "coalescing counted");
}

void test_admission_classes_and_unconsumed_hints() {
    Fixture f(1, 16, 64);
    const ByteRange a{0, 16};
    const ByteRange b{16, 16};
    std::array<Lease, 1> out;
    std::thread completer([&] {
        while (!f.backend.complete(0)) {
            std::this_thread::yield();
        }
    });
    check(f.pool->resolve(std::span(&a, 1), out).has_value(), "resolved a");
    completer.join();
    out[0].reset(); // a is resident, unpinned, recently accessed

    auto guess = f.pool->prefetch(std::span(&b, 1), AdmissionClass::speculative);
    check(f.pool->wait(*guess).status == Status::declined, "speculative hint cannot displace a recent access");
    check(f.pool->stats().hint_declined == 1, "decline counted");
    auto again = f.pool->prefetch(std::span(&b, 1), AdmissionClass::speculative);
    check(f.pool->wait(*again).status == Status::declined, "a declined speculative hint aged nothing");

    auto declared = f.pool->prefetch(std::span(&b, 1), AdmissionClass::declared);
    f.backend.complete(0);
    check(f.pool->wait(*declared).status == Status::ok, "declared hint evicts an unpinned resident");
    check(f.pool->stats().hint_unconsumed == 0, "evicting a resolved chunk is not an unconsumed hint");

    auto displace = f.pool->prefetch(std::span(&a, 1), AdmissionClass::speculative);
    f.backend.complete(0);
    check(f.pool->wait(*displace).status == Status::ok, "speculative takes an unreferenced resident");
    check(f.pool->stats().hint_unconsumed == 1, "prefetched-never-resolved chunk counted at eviction");
    check(f.pool->stats().evictions == 2, "evictions counted");
}

void test_wont_need() {
    Fixture f(1, 16, 64);
    const ByteRange a{0, 16};
    const ByteRange b{16, 16};
    std::array<Lease, 1> out;
    std::thread completer([&] {
        while (!f.backend.complete(0)) {
            std::this_thread::yield();
        }
    });
    (void)f.pool->resolve(std::span(&a, 1), out);
    completer.join();
    out[0].reset();
    f.pool->wont_need(std::span(&a, 1));
    auto guess = f.pool->prefetch(std::span(&b, 1), AdmissionClass::speculative);
    check(f.backend.accepted() == 2, "wont_need made the resident a speculative-admissible victim");
    f.backend.complete(0);
    (void)f.pool->wait(*guess);
}

void test_ticket_and_queue_exhaustion() {
    Fixture f(4, 16, 64, /*max_tickets=*/1);
    const ByteRange a{0, 16};
    const ByteRange b{16, 16};
    auto held = f.pool->prefetch(std::span(&a, 1), AdmissionClass::declared);
    check(f.pool->prefetch(std::span(&b, 1), AdmissionClass::declared).error() == Status::ticket_exhausted,
          "ticket table exhaustion reported");
    held->reset(); // dropped while its fill is in flight: record stays until completion
    check(f.pool->prefetch(std::span(&b, 1), AdmissionClass::declared).error() == Status::ticket_exhausted,
          "dropped ticket's record lives until its fill completes");
    check(f.backend.pending() == 1, "dropping a ticket did not cancel its fill");
    f.backend.complete(0);
    check(f.pool->prefetch(std::span(&b, 1), AdmissionClass::declared).has_value(), "record recycled");

    Fixture q(4, 16, 64, 4, 8, /*queue=*/1);
    const std::array both{a, b};
    auto ticket = q.pool->prefetch(both, AdmissionClass::declared);
    q.backend.complete(0);
    const WaitOutcome outcome = q.pool->wait(*ticket);
    check(outcome.filled == 1 && outcome.declined == 1 && outcome.status == Status::queue_exhausted,
          "backend queue exhaustion reported per chunk");
    check(q.pool->stats().slots_filling == 0 && q.pool->stats().slots_ready == 1, "refused slot freed");
}

void test_wait_timeout_leaves_fill_live() {
    Fixture f(2, 16, 64);
    const ByteRange a{0, 16};
    auto ticket = f.pool->prefetch(std::span(&a, 1), AdmissionClass::declared);
    const WaitOutcome early = f.pool->wait(*ticket, Clock::now());
    check(early.status == Status::timeout && early.pending == 1, "deadline reports timeout");
    check(f.pool->stats().slots_filling == 1 && f.backend.pending() == 1, "timeout did not cancel or free");
    check(f.pool->drain(Clock::now()) == Status::timeout, "drain honours its deadline");
    f.backend.complete(0);
    check(f.pool->wait(*ticket).status == Status::ok, "fill completes after the timeout");
}

void test_hot_path_allocates_nothing() {
    Fixture f(4, 16, 256);
    const std::array ranges{ByteRange{0, 40}, ByteRange{100, 8}};
    std::array<Lease, 8> out;
    const std::uint64_t before = allocation_count();
    for (int round = 0; round < 50; ++round) {
        auto ticket = f.pool->prefetch(ranges, round % 2 ? AdmissionClass::speculative : AdmissionClass::declared);
        f.backend.complete_all_newest_first();
        (void)f.pool->wait(*ticket);
        if (f.pool->try_resolve(ranges, out)) {
            (void)f.pool->resolve(ranges, out); // releases the try_resolve leases, then all hits
            f.pool->wont_need(ranges);
        }
        for (Lease& lease : out) {
            lease.reset();
        }
        const ByteRange shifted{static_cast<std::uint64_t>(round % 4) * 64, 64};
        auto evictor = f.pool->prefetch(std::span(&shifted, 1), AdmissionClass::declared);
        f.backend.complete_all_newest_first();
        (void)f.pool->stats();
    }
    check(allocation_count() == before, "prefetch/wait/resolve/try_resolve/release/completion allocate nothing");
}

void test_concurrent_overlapping_resolves() {
    Fixture f(8, 64, 64 * 32, 4, 8, 64);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> resolved{0};
    std::atomic<std::uint64_t> corrupt{0};
    std::thread completer([&] {
        std::mt19937 rng(1234);
        while (!stop.load()) {
            const std::size_t pending = f.backend.pending();
            if (pending == 0) {
                std::this_thread::yield();
                continue;
            }
            const auto roll = rng() % 16;
            const FakeCompletion how = roll == 0 ? FakeCompletion::io_error
                                       : roll == 1 ? FakeCompletion::short_read
                                                   : FakeCompletion::full;
            f.backend.complete(rng() % pending, how);
        }
        f.backend.complete_all_newest_first();
    });
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < 4; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937 rng(t);
            std::array<Lease, 4> out;
            for (int i = 0; i < 2000; ++i) {
                const std::uint64_t offset = rng() % (64 * 12); // hot overlapping window
                const ByteRange range{offset, 1 + rng() % 100};
                if (auto leased = f.pool->resolve(std::span(&range, 1), out)) {
                    ++resolved;
                    for (std::size_t k = 0; k < *leased; ++k) {
                        corrupt += matches_source(out[k]) ? 0 : 1;
                    }
                    std::this_thread::yield(); // hold across other threads' pressure
                    for (std::size_t k = 0; k < *leased; ++k) {
                        corrupt += matches_source(out[k]) ? 0 : 1;
                        out[k].reset();
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
    check(f.pool->drain() == Status::ok, "concurrent run drains");
    const SlotPoolStats stats = f.pool->stats();
    check(corrupt.load() == 0, "no leased slot was overwritten under concurrent pressure");
    check(resolved.load() > 0, "concurrent resolves made progress");
    check(stats.slots_pinned == 0 && stats.slots_filling == 0, "concurrent run left no pins or fills");
    check(stats.high_water_slots <= 8, "never more slots than registered");
    check(stats.coalesced > 0, "concurrent overlapping requests coalesced");
}

} // namespace

int main() {
    run(test_registration_validation, "test_registration_validation");
    run(test_range_validation, "test_range_validation");
    run(test_out_of_order_completion_and_segments, "test_out_of_order_completion_and_segments");
    run(test_eof_tail_chunk, "test_eof_tail_chunk");
    run(test_failed_fills_never_publish, "test_failed_fills_never_publish");
    run(test_resolve_propagates_failure_and_unwinds, "test_resolve_propagates_failure_and_unwinds");
    run(test_duplicate_and_stale_completion, "test_duplicate_and_stale_completion");
    run(test_no_reuse_while_leased_or_filling, "test_no_reuse_while_leased_or_filling");
    run(test_batch_larger_than_pool, "test_batch_larger_than_pool");
    run(test_coalescing, "test_coalescing");
    run(test_admission_classes_and_unconsumed_hints, "test_admission_classes_and_unconsumed_hints");
    run(test_wont_need, "test_wont_need");
    run(test_ticket_and_queue_exhaustion, "test_ticket_and_queue_exhaustion");
    run(test_wait_timeout_leaves_fill_live, "test_wait_timeout_leaves_fill_live");
    run(test_hot_path_allocates_nothing, "test_hot_path_allocates_nothing");
    run(test_concurrent_overlapping_resolves, "test_concurrent_overlapping_resolves");
    return finish();
}
