// Bookkeeping microbenchmarks for the M2 state machines (docs/DEVELOPMENT_WORKFLOW.md, "bench" stage).
//
// What this measures: the library's own per-call overhead -- lock, index lookup, lease construction,
// completion publication -- against a backend that moves no bytes. It deliberately excludes I/O, which
// M3's real backends own; a regression here is a regression in code this repository controls.
//
// Output: one JSON object on stdout (consumed by scripts/dev.py), human-readable lines on stderr.
// Every timed region also counts global allocations; the hot-path contract says that count is zero.

#include "../tests/test_support.hpp"

#include <sub0mempage/slot_pool.hpp>
#include <sub0mempage/transfer_set.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace sub0mempage;
using sub0mempage::test::allocation_count;
using BenchClock = std::chrono::steady_clock;

/// Accepts requests into a fixed ring and completes them without touching destination bytes.
class NullBackend {
public:
    explicit NullBackend(std::size_t capacity) { pending_.reserve(capacity); }

    [[nodiscard]] bool submit(const FillRequest& request) noexcept {
        const std::scoped_lock lock(mutex_);
        if (pending_.size() == pending_.capacity()) {
            return false;
        }
        pending_.push_back(request);
        return true;
    }

    void complete_all() {
        for (;;) {
            FillRequest request;
            {
                const std::scoped_lock lock(mutex_);
                if (pending_.empty()) {
                    return;
                }
                request = pending_.back();
                pending_.pop_back();
            }
            request.sink.deliver(request.token, {.status = Status::ok, .bytes = request.destination.size()});
        }
    }

private:
    std::mutex mutex_;
    std::vector<FillRequest> pending_;
};

struct Result {
    std::string name;
    double ns_per_op = 0;
    std::vector<double> samples;
    std::uint64_t ops_per_sample = 0;
    std::uint64_t allocations = 0;
};

/// Calibrates an iteration count to ~`target` per sample, then takes `samples` timed samples of `body(n)`.
/// Reports the median; allocations are summed over all timed samples only.
template <class Body>
Result measure(const char* name, int samples, Body&& body) {
    constexpr auto target = std::chrono::milliseconds(40);
    std::uint64_t n = 64;
    for (;;) {
        const auto start = BenchClock::now();
        body(n);
        if (BenchClock::now() - start >= target / 4 || n >= (1ull << 30)) {
            const auto elapsed = std::chrono::duration<double>(BenchClock::now() - start).count();
            n = std::max<std::uint64_t>(64, static_cast<std::uint64_t>(n * (0.040 / std::max(elapsed, 1e-9))));
            break;
        }
        n *= 4;
    }
    Result result{.name = name, .ops_per_sample = n};
    for (int i = 0; i < samples; ++i) {
        const std::uint64_t allocs_before = allocation_count();
        const auto start = BenchClock::now();
        body(n);
        const auto ns = std::chrono::duration<double, std::nano>(BenchClock::now() - start).count();
        result.allocations += allocation_count() - allocs_before;
        result.samples.push_back(ns / static_cast<double>(n));
    }
    std::vector<double> sorted = result.samples;
    std::sort(sorted.begin(), sorted.end());
    result.ns_per_op = sorted[sorted.size() / 2];
    return result;
}

/// A pool whose first `resident` chunks are already Ready, over a NullBackend.
struct PoolRig {
    NullBackend backend;
    std::vector<std::byte> storage;
    std::unique_ptr<SlotPool> pool;

    PoolRig(std::uint32_t slots, std::size_t slot_bytes, std::uint32_t resident, std::uint32_t max_batch = 16)
        : backend(slots), storage(std::size_t{slots} * slot_bytes) {
        pool = std::move(*SlotPool::create({.source = SourceId{1},
                                            .source_bytes = std::uint64_t{slots} * slot_bytes * 4,
                                            .slot_bytes = slot_bytes,
                                            .num_slots = slots,
                                            .slot_storage = storage,
                                            .max_tickets = 8,
                                            .max_batch_chunks = max_batch},
                                           FillBackendRef(backend)));
        for (std::uint32_t chunk = 0; chunk < resident; ++chunk) {
            const ByteRange range{std::uint64_t{chunk} * slot_bytes, slot_bytes};
            auto ticket = pool->prefetch(std::span(&range, 1), AdmissionClass::declared);
            backend.complete_all();
        }
    }
    ~PoolRig() {
        backend.complete_all();
        (void)pool->drain();
    }
};

Result bench_try_resolve_hit(const char* name, std::uint32_t slots, int samples) {
    PoolRig rig(slots, 4096, slots);
    std::array<Lease, 1> out;
    std::uint32_t chunk = 0;
    return measure(name, samples, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const ByteRange range{std::uint64_t{chunk} * 4096 + 16, 256};
            chunk = chunk + 1 == slots ? 0 : chunk + 1;
            if (rig.pool->try_resolve(std::span(&range, 1), out)) {
                out[0].reset();
            }
        }
    });
}

Result bench_resolve_hit_batch8(int samples) {
    PoolRig rig(64, 4096, 64);
    std::array<Lease, 8> out;
    std::array<ByteRange, 8> ranges;
    std::uint32_t base = 0;
    return measure("slot_pool.resolve_hit_batch8", samples, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            for (std::uint32_t k = 0; k < ranges.size(); ++k) {
                ranges[k] = {std::uint64_t{(base + k * 7) % 64} * 4096, 4096};
            }
            base = (base + 1) % 64;
            if (rig.pool->resolve(ranges, out)) {
                for (Lease& lease : out) {
                    lease.reset();
                }
            }
        }
    });
}

Result bench_miss_cycle(int samples) {
    // 8 slots streaming over 256 chunks: every prefetch evicts, fills, waits and is then resolved.
    PoolRig rig(8, 4096, 0);
    std::array<Lease, 1> out;
    std::uint64_t chunk = 0;
    return measure("slot_pool.miss_cycle", samples, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            const ByteRange range{chunk * 4096, 4096};
            chunk = (chunk + 1) % 32;
            auto ticket = rig.pool->prefetch(std::span(&range, 1), AdmissionClass::declared);
            rig.backend.complete_all();
            (void)rig.pool->wait(*ticket);
            if (rig.pool->try_resolve(std::span(&range, 1), out)) {
                out[0].reset();
            }
        }
    });
}

Result bench_contended_try_resolve(int samples, unsigned threads) {
    // Persistent workers released by a barrier, so thread start-up (and its allocations) stays out of
    // the timed region. ns/op is wall time over the total ops of all threads.
    PoolRig rig(64, 4096, 64);
    std::barrier sync(static_cast<std::ptrdiff_t>(threads + 1));
    std::atomic<std::uint64_t> per_thread{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::array<Lease, 1> out;
            std::uint32_t chunk = t * 13;
            for (;;) {
                sync.arrive_and_wait();
                if (stop.load()) {
                    return;
                }
                for (std::uint64_t i = 0, n = per_thread.load(); i < n; ++i) {
                    const ByteRange range{std::uint64_t{chunk % 64} * 4096, 512};
                    ++chunk;
                    if (rig.pool->try_resolve(std::span(&range, 1), out)) {
                        out[0].reset();
                    }
                }
                sync.arrive_and_wait();
            }
        });
    }
    auto result = measure("slot_pool.try_resolve_hit_contended_4t", samples, [&](std::uint64_t n) {
        per_thread = std::max<std::uint64_t>(1, n / threads);
        sync.arrive_and_wait();
        sync.arrive_and_wait();
    });
    stop = true;
    sync.arrive_and_wait();
    for (std::thread& worker : workers) {
        worker.join();
    }
    return result;
}

Result bench_transfer_submit_complete(int samples) {
    NullBackend backend(8);
    std::vector<std::byte> destination(1 << 16);
    auto set = std::move(*TransferSet::create(
        {.source = SourceId{2}, .source_bytes = 1 << 20, .destination = destination, .max_claims = 8},
        FillBackendRef(backend)));
    std::uint64_t offset = 0;
    auto result = measure("transfer_set.submit_complete", samples, [&](std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            auto claim = set->submit({offset, 4096}, (offset % 16) * 4096);
            offset = (offset + 4096) % (1 << 19);
            backend.complete_all();
            (void)claim->wait();
        }
    });
    (void)set->drain();
    return result;
}

void print_json(const std::vector<Result>& results) {
    std::printf("{\"schema\": 1, \"benchmarks\": [");
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Result& r = results[i];
        std::printf("%s\n  {\"name\": \"%s\", \"ns_per_op\": %.3f, \"ops_per_sample\": %llu, \"allocations\": %llu, "
                    "\"samples\": [",
                    i == 0 ? "" : ",", r.name.c_str(), r.ns_per_op, static_cast<unsigned long long>(r.ops_per_sample),
                    static_cast<unsigned long long>(r.allocations));
        for (std::size_t k = 0; k < r.samples.size(); ++k) {
            std::printf("%s%.3f", k == 0 ? "" : ", ", r.samples[k]);
        }
        std::printf("]}");
    }
    std::printf("\n]}\n");
}

} // namespace

int main(int argc, char** argv) {
    int samples = 7;
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            samples = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--samples N] [--filter substring]\n", argv[0]);
            return 2;
        }
    }
    const auto wanted = [&](const char* name) { return filter == nullptr || std::strstr(name, filter) != nullptr; };

    std::vector<Result> results;
    const auto add = [&](const char* name, auto&& run) {
        if (wanted(name)) {
            results.push_back(run());
            const Result& r = results.back();
            std::fprintf(stderr, "%-44s %10.1f ns/op  (%llu allocs)\n", r.name.c_str(), r.ns_per_op,
                         static_cast<unsigned long long>(r.allocations));
        }
    };
    add("slot_pool.try_resolve_hit_64slots", [&] { return bench_try_resolve_hit("slot_pool.try_resolve_hit_64slots", 64, samples); });
    add("slot_pool.try_resolve_hit_4096slots", [&] { return bench_try_resolve_hit("slot_pool.try_resolve_hit_4096slots", 4096, samples); });
    add("slot_pool.resolve_hit_batch8", [&] { return bench_resolve_hit_batch8(samples); });
    add("slot_pool.miss_cycle", [&] { return bench_miss_cycle(samples); });
    add("slot_pool.try_resolve_hit_contended_4t", [&] { return bench_contended_try_resolve(samples, 4); });
    add("transfer_set.submit_complete", [&] { return bench_transfer_submit_complete(samples); });
    print_json(results);
    return 0;
}
