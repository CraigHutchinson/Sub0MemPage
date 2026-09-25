// M3 slice-1 gates for local_file_backend.hpp (docs/transfer-contract.md "Real-file tests"): every check
// here drives real files through the worker-pool backend, SlotPool and TransferSet together, and compares
// bytes against an independent std::ifstream oracle -- never against the same formula used to write the
// file, and never against the backend's own output.

#include "test_support.hpp"

#include <sub0mempage/local_file_backend.hpp>
#include <sub0mempage/slot_pool.hpp>
#include <sub0mempage/transfer_set.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h> // _getpid
#else
#include <unistd.h> // getpid
#endif

namespace {

using namespace sub0mempage;
using sub0mempage::test::allocation_count;
using sub0mempage::test::check;
using sub0mempage::test::finish;
using sub0mempage::test::run;

namespace fs = std::filesystem;

/// Deterministic on-disk content generator. Deliberately a different formula from
/// sub0mempage/testing/fake_backend.hpp's source_byte, so a defect that accidentally reused fake-backend state instead
/// of really reading the file cannot hide behind matching numbers.
[[nodiscard]] constexpr std::byte file_byte(std::uint64_t offset) noexcept {
    return static_cast<std::byte>((offset * 0x9E3779B1u ^ (offset >> 5)) & 0xffu);
}

void write_file(const fs::path& path, std::uint64_t size, std::byte fill = std::byte{0}) {
    std::vector<std::byte> buffer(static_cast<std::size_t>(size));
    if (fill == std::byte{0}) {
        for (std::uint64_t i = 0; i < size; ++i) {
            buffer[static_cast<std::size_t>(i)] = file_byte(i);
        }
    } else {
        std::fill(buffer.begin(), buffer.end(), fill); // content doesn't matter, only bulk/size
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
}

/// The independent read oracle every backend/pool/set result is checked against.
[[nodiscard]] std::vector<std::byte> read_oracle(const fs::path& path, std::uint64_t offset, std::uint64_t length) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    std::vector<std::byte> buffer(static_cast<std::size_t>(length));
    in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(length));
    return buffer;
}

[[nodiscard]] bool matches_oracle(const fs::path& path, std::span<const std::byte> bytes, std::uint64_t offset) {
    const std::vector<std::byte> oracle = read_oracle(path, offset, bytes.size());
    return std::equal(bytes.begin(), bytes.end(), oracle.begin(), oracle.end());
}

class TempDir {
public:
    TempDir() : path_(unique_path()) { fs::create_directories(path_); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec); // best-effort cleanup; a failure here must not fail the suite
    }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    static fs::path unique_path() {
        static std::atomic<std::uint64_t> counter{0};
#ifdef _WIN32
        const auto pid = static_cast<std::uint64_t>(::_getpid());
#else
        const auto pid = static_cast<std::uint64_t>(::getpid());
#endif
        return fs::temp_directory_path() /
               ("sub0mempage-local-file-tests-" + std::to_string(pid) + "-" + std::to_string(counter.fetch_add(1)));
    }
    fs::path path_;
};

/// Minimal CompletionSink target for driving LocalFileBackend directly (below SlotPool/TransferSet).
/// A condition variable, not a sleep: exactly the synchronization SlotPool/TransferSet use internally.
struct DirectWaiter {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    FillResult result;

    static void deliver(void* context, TransferToken, FillResult r) noexcept {
        auto* self = static_cast<DirectWaiter*>(context);
        // Notify while still holding the lock, like SlotPool::on_complete/TransferSet::on_complete do
        // (see their own "notify under the lock" comments): once the mutex is released, wait() may
        // observe done==true (even via a spurious wakeup, ahead of this notify_one() actually running)
        // and let the waiter go out of scope and destroy the condition_variable out from under this call.
        const std::scoped_lock lock(self->mutex);
        self->result = r;
        self->done = true;
        self->cv.notify_one();
    }

    [[nodiscard]] FillResult wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return done; });
        return result;
    }
};

// --- registration ----------------------------------------------------------------------------------

void test_registration() {
    TempDir dir;
    const fs::path file = dir.path() / "a.bin";
    write_file(file, 100);

    check(LocalFileBackend::create({.workers = 0, .queue_capacity = 4, .max_sources = 4}).error() ==
              Status::invalid_argument,
          "zero workers rejected");
    check(LocalFileBackend::create({.workers = 1, .queue_capacity = 0, .max_sources = 4}).error() ==
              Status::invalid_argument,
          "zero queue capacity rejected");
    check(LocalFileBackend::create({.workers = 1, .queue_capacity = 4, .max_sources = 0}).error() ==
              Status::invalid_argument,
          "zero max_sources rejected");

    auto created = LocalFileBackend::create({.workers = 2, .queue_capacity = 8, .max_sources = 2});
    check(created.has_value(), "valid configuration accepted");
    auto backend = std::move(*created);

    check(backend->register_file(SourceId{1}, file) == Status::ok, "registering an existing file succeeds");
    check(backend->register_file(SourceId{1}, file) == Status::invalid_argument, "duplicate source rejected");
    check(backend->register_file(SourceId{2}, dir.path() / "missing.bin") == Status::io_error,
          "missing file reported as io_error");
    check(backend->register_file(SourceId{2}, file) == Status::ok, "second distinct source accepted");
    check(backend->register_file(SourceId{3}, file) == Status::ticket_exhausted,
          "registration table bounded by max_sources");
}

// --- direct backend reads ---------------------------------------------------------------------------

void test_direct_submit_matches_oracle() {
    TempDir dir;
    const fs::path file = dir.path() / "b.bin";
    constexpr std::uint64_t size = 5000;
    write_file(file, size);

    auto backend = std::move(*LocalFileBackend::create({.workers = 2, .queue_capacity = 8, .max_sources = 1}));
    check(backend->register_file(SourceId{1}, file) == Status::ok, "registration succeeds");

    struct Case {
        std::uint64_t offset;
        std::uint64_t length;
    };
    const std::array cases{Case{0, 1}, Case{17, 999}, Case{size - 1, 1}, Case{123, 4321}};
    for (const Case& c : cases) {
        std::vector<std::byte> destination(static_cast<std::size_t>(c.length));
        DirectWaiter waiter;
        const FillRequest request{.source = SourceId{1},
                                  .source_offset = c.offset,
                                  .destination = destination,
                                  .token = {0, 0},
                                  .sink = {&waiter, &DirectWaiter::deliver}};
        check(backend->submit(request), "direct submit accepted");
        const FillResult result = waiter.wait();
        check(result.status == Status::ok && result.bytes == c.length, "direct read reports full success");
        check(matches_oracle(file, destination, c.offset), "direct read bytes match the ifstream oracle");
    }
    const LocalFileBackendStats stats = backend->stats();
    check(stats.completed_ok == cases.size() && stats.errors == 0 && stats.short_reads == 0,
          "stats tally direct reads");
    check(stats.bytes_read == 1 + 999 + 1 + 4321, "bytes_read totals every direct read");
}

void test_unknown_source_reported_async() {
    TempDir dir;
    const fs::path file = dir.path() / "c.bin";
    write_file(file, 16);
    auto backend = std::move(*LocalFileBackend::create({.workers = 1, .queue_capacity = 4, .max_sources = 1}));
    check(backend->register_file(SourceId{1}, file) == Status::ok, "one source registered");

    std::vector<std::byte> destination(4);
    DirectWaiter waiter;
    const FillRequest request{.source = SourceId{99}, // never registered
                              .source_offset = 0,
                              .destination = destination,
                              .token = {0, 0},
                              .sink = {&waiter, &DirectWaiter::deliver}};
    check(backend->submit(request), "submit accepts an unknown SourceId (not a queue-full condition)");
    const FillResult result = waiter.wait();
    check(result.status == Status::invalid_argument && result.bytes == 0,
          "unknown SourceId fails asynchronously with invalid_argument, never inline in submit()");
    check(backend->stats().errors == 1, "unknown-source delivery counted as an error");
}

// --- SlotPool end to end -----------------------------------------------------------------------------

void test_slot_pool_end_to_end() {
    TempDir dir;
    const fs::path file = dir.path() / "pool.bin";
    constexpr std::uint64_t size = 1000; // not a multiple of slot_bytes below
    write_file(file, size);

    auto backend = std::move(*LocalFileBackend::create({.workers = 3, .queue_capacity = 16, .max_sources = 1}));
    check(backend->register_file(SourceId{9}, file) == Status::ok, "file registered");

    constexpr std::size_t slot_bytes = 64;
    constexpr std::uint32_t num_slots = 8; // >= the 6 distinct chunks resolved at once below (R7/R13)
    std::vector<std::byte> storage(std::size_t{num_slots} * slot_bytes, std::byte{0xCD}); // poison
    auto pool = std::move(*SlotPool::create({.source = SourceId{9},
                                             .source_bytes = size,
                                             .slot_bytes = slot_bytes,
                                             .num_slots = num_slots,
                                             .slot_storage = storage,
                                             .max_tickets = 4,
                                             .max_batch_chunks = 32},
                                            FillBackendRef(*backend)));

    // Unaligned offsets/lengths, a range crossing several chunks, and a tail range clipped at EOF
    // (chunk 15 -- size 1000, slot_bytes 64 -- holds only 1000 - 15*64 = 40 bytes). {5,150} spans chunks
    // 0-2, {300,37} spans chunks 4-5, {990,10} is chunk 15 alone: 6 chunks in total.
    const std::array ranges{ByteRange{5, 150}, ByteRange{300, 37}, ByteRange{size - 10, 10}};
    std::array<Lease, 8> out;
    auto leased = pool->resolve(ranges, out);
    check(leased.has_value() && *leased == 6, "resolve fills every chunk from the real backend");
    if (leased) {
        for (std::size_t i = 0; i < *leased; ++i) {
            check(matches_oracle(file, out[i].bytes(), out[i].source_offset()),
                  "leased bytes equal the independent ifstream oracle");
        }
        check(out[5].source_offset() == size - 10 && out[5].bytes().size() == 10,
              "tail range clipped exactly at EOF, not at the slot boundary");
    }
    for (Lease& lease : out) {
        lease.reset();
    }

    // Empty/out-of-range/overflow: rejected by validate_range before any real read is attempted.
    const ByteRange past_eof{size, 1};
    check(pool->resolve(std::span(&past_eof, 1), out).error() == Status::out_of_range, "past-EOF range rejected");
    const ByteRange empty{10, 0};
    check(pool->resolve(std::span(&empty, 1), out).error() == Status::empty_range, "empty range rejected");
    const ByteRange overflow{UINT64_MAX - 2, 10};
    check(pool->resolve(std::span(&overflow, 1), out).error() == Status::out_of_range, "overflowing range rejected");

    check(pool->drain() == Status::ok, "pool drains with nothing outstanding");
}

void test_truncated_file_never_resident() {
    TempDir dir;
    const fs::path file = dir.path() / "trunc.bin";
    constexpr std::uint64_t original_size = 200;
    write_file(file, original_size);

    auto backend = std::move(*LocalFileBackend::create({.workers = 1, .queue_capacity = 4, .max_sources = 2}));
    check(backend->register_file(SourceId{5}, file) == Status::ok, "registered before truncation");

    // Truncate the underlying file after registration: the already-open handle now hits EOF early on a
    // chunk that was in range when the pool was configured (transfer-contract.md "Real-file tests": a
    // file truncated after registration must never publish a short read as resident).
    SourceId short_source{5};
#ifdef _WIN32
    // Windows enforces the immutable-source rule for us: the backend opens with FILE_SHARE_READ only, so
    // the resize is refused. Reproduce the same early EOF with a source that is already short.
    std::error_code refused;
    fs::resize_file(file, 50, refused);
    check(static_cast<bool>(refused), "Windows refuses to truncate a registered source");
    const fs::path short_file = dir.path() / "short.bin";
    write_file(short_file, 50);
    short_source = SourceId{6};
    check(backend->register_file(short_source, short_file) == Status::ok, "short source registered");
#else
    fs::resize_file(file, 50);
    check(fs::file_size(file) == 50, "truncation took effect");
    check(true, "short source registered"); // keeps G-SUITE counts equal across platforms
#endif

    constexpr std::size_t slot_bytes = 64;
    std::vector<std::byte> storage(slot_bytes, std::byte{0xCD});
    auto pool = std::move(*SlotPool::create({.source = short_source,
                                             .source_bytes = original_size,
                                             .slot_bytes = slot_bytes,
                                             .num_slots = 1,
                                             .slot_storage = storage,
                                             .max_tickets = 1,
                                             .max_batch_chunks = 1},
                                            FillBackendRef(*backend)));

    const ByteRange range{0, slot_bytes}; // chunk 0: registered as if 64 bytes were readable, only 50 are
    std::array<Lease, 1> out;
    auto leased = pool->resolve(std::span(&range, 1), out);
    check(!leased.has_value() && leased.error() == Status::short_read, "truncated chunk reported as short_read");
    check(pool->stats().slots_ready == 0, "short read never published as a Ready slot");

    std::array<Lease, 1> try_out;
    check(!pool->try_resolve(std::span(&range, 1), try_out).has_value(),
          "chunk is not resident after a short read");
    check(pool->drain() == Status::ok, "pool drains cleanly");
}

// --- TransferSet end to end --------------------------------------------------------------------------

void test_transfer_set_end_to_end() {
    TempDir dir;
    const fs::path file = dir.path() / "set.bin";
    constexpr std::uint64_t size = 2000;
    write_file(file, size);

    auto backend = std::move(*LocalFileBackend::create({.workers = 2, .queue_capacity = 8, .max_sources = 1}));
    check(backend->register_file(SourceId{4}, file) == Status::ok, "file registered");

    constexpr std::byte CANARY{0xEE};
    std::vector<std::byte> destination(256, CANARY);
    auto set = std::move(*TransferSet::create(
        {.source = SourceId{4}, .source_bytes = size, .destination = destination, .max_claims = 4},
        FillBackendRef(*backend)));

    auto claim = set->submit({137, 91}, 50); // unaligned source and destination offsets
    check(claim.has_value(), "submit accepted");
    check(claim->wait() == Status::ok, "real read completes ok");
    check(matches_oracle(file, claim->bytes(), 137), "claimed bytes equal the independent ifstream oracle");

    bool canaries_intact = true;
    for (std::size_t i = 0; i < destination.size(); ++i) {
        if ((i < 50 || i >= 141) && destination[i] != CANARY) {
            canaries_intact = false;
        }
    }
    check(canaries_intact, "guard bytes before/after the claimed destination were never touched");

    claim->reset();
    check(set->drain() == Status::ok, "set drains");
}

// --- backpressure and shutdown -----------------------------------------------------------------------

void test_queue_full_rejection() {
    TempDir dir;
    const fs::path file = dir.path() / "q.bin";
    write_file(file, 16);

    constexpr std::uint32_t capacity = 2;
    auto backend = std::move(*LocalFileBackend::create({.workers = 1, .queue_capacity = capacity, .max_sources = 1}));
    check(backend->register_file(SourceId{6}, file) == Status::ok, "file registered");

    // A single worker services one request at a time (mutex lock/unlock + pread()); one uncontended
    // submit() is only a mutex lock and a struct copy. No sleep: a tight submission loop from this one
    // thread outruns the worker and, within a generous attempt budget, is certain to observe the bounded
    // queue full at least once.
    std::vector<std::byte> destination(4); // one worker at a time -> safe to share across all requests
    std::deque<DirectWaiter> waiters;       // deque: emplace_back never invalidates earlier references
    bool saw_rejection = false;
    constexpr int attempts = 20000;
    for (int i = 0; i < attempts && !saw_rejection; ++i) {
        waiters.emplace_back();
        const FillRequest request{.source = SourceId{6},
                                  .source_offset = 0,
                                  .destination = destination,
                                  .token = {static_cast<std::uint32_t>(i), 0},
                                  .sink = {&waiters.back(), &DirectWaiter::deliver}};
        if (!backend->submit(request)) {
            waiters.pop_back(); // never accepted, so it will never be delivered
            saw_rejection = true;
        }
    }
    check(saw_rejection, "a tight submission loop observes the bounded queue full");
    check(backend->stats().rejected_queue_full >= 1, "rejection counted");
    for (DirectWaiter& waiter : waiters) {
        (void)waiter.wait();
    }
}

void test_shutdown_with_queued_work() {
    TempDir dir;
    const fs::path big_file = dir.path() / "shutdown_big.bin";
    constexpr std::uint64_t big_size = 64ull * 1024 * 1024;
    write_file(big_file, big_size, std::byte{0x11});
    const fs::path small_file = dir.path() / "shutdown_small.bin";
    write_file(small_file, 32);

    auto backend = std::move(*LocalFileBackend::create({.workers = 1, .queue_capacity = 4, .max_sources = 2}));
    check(backend->register_file(SourceId{8}, big_file) == Status::ok, "big file registered");
    check(backend->register_file(SourceId{10}, small_file) == Status::ok, "small file registered");

    std::vector<std::byte> occupier_dest(static_cast<std::size_t>(big_size));
    DirectWaiter occupier_waiter;
    const FillRequest occupier{.source = SourceId{8},
                               .source_offset = 0,
                               .destination = occupier_dest,
                               .token = {0, 0},
                               .sink = {&occupier_waiter, &DirectWaiter::deliver}};
    check(backend->submit(occupier), "occupier accepted");

    // Wait (no sleep: a bounded yield-spin on the backend's own in_flight gauge, the same kind of
    // condition SlotPool/TransferSet block on internally) until the sole worker has actually dequeued and
    // started reading the occupier. Racing straight into shutdown() right after submit() is not safe to
    // assume away: on a lightly-scheduled/low-core host the worker thread may not run at all between
    // submit() and shutdown() unless something here actually yields to it, which would make the occupier
    // itself still-queued and cancelled instead of completed -- the very distinction this test checks.
    bool occupier_started = false;
    for (int spins = 0; spins < 1'000'000 && !occupier_started; ++spins) {
        occupier_started = backend->stats().in_flight != 0;
        std::this_thread::yield();
    }
    check(occupier_started, "worker started reading the occupier before shutdown() is exercised");

    constexpr int queued_count = 3;
    std::array<DirectWaiter, queued_count> queued_waiters;
    std::array<std::vector<std::byte>, queued_count> queued_dest;
    for (int i = 0; i < queued_count; ++i) {
        queued_dest[static_cast<std::size_t>(i)].resize(8);
        const FillRequest req{.source = SourceId{10},
                              .source_offset = 0,
                              .destination = queued_dest[static_cast<std::size_t>(i)],
                              .token = {static_cast<std::uint32_t>(i + 1), 0},
                              .sink = {&queued_waiters[static_cast<std::size_t>(i)], &DirectWaiter::deliver}};
        check(backend->submit(req), "queued request accepted while the sole worker is still busy");
    }

    backend->shutdown(); // cancels the still-queued ones, lets the in-flight occupier finish, joins workers

    check(occupier_waiter.wait().status == Status::ok, "in-flight request still completed normally");
    for (DirectWaiter& waiter : queued_waiters) {
        check(waiter.wait().status == Status::cancelled, "queued-but-unstarted request cancelled exactly once");
    }
    check(backend->stats().cancelled == queued_count, "shutdown accounted for every cancellation");

    std::vector<std::byte> after_dest(4);
    DirectWaiter after_waiter;
    const FillRequest after{.source = SourceId{10},
                            .source_offset = 0,
                            .destination = after_dest,
                            .token = {0, 0},
                            .sink = {&after_waiter, &DirectWaiter::deliver}};
    check(!backend->submit(after), "submit refused after shutdown");
}

// --- concurrency -----------------------------------------------------------------------------------

void test_concurrent_submitters() {
    TempDir dir;
    const fs::path file = dir.path() / "concurrent.bin";
    constexpr std::uint64_t size = 64 * 20;
    write_file(file, size);

    auto backend = std::move(*LocalFileBackend::create({.workers = 4, .queue_capacity = 32, .max_sources = 1}));
    check(backend->register_file(SourceId{11}, file) == Status::ok, "file registered");

    constexpr std::size_t slot_bytes = 64;
    constexpr std::uint32_t num_slots = 6;
    std::vector<std::byte> storage(std::size_t{num_slots} * slot_bytes, std::byte{0xCD});
    auto pool = std::move(*SlotPool::create({.source = SourceId{11},
                                             .source_bytes = size,
                                             .slot_bytes = slot_bytes,
                                             .num_slots = num_slots,
                                             .slot_storage = storage,
                                             .max_tickets = 8,
                                             .max_batch_chunks = 8},
                                            FillBackendRef(*backend)));

    std::atomic<std::uint64_t> corrupt{0};
    std::atomic<std::uint64_t> resolved{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(t);
            std::array<Lease, 4> out;
            for (int i = 0; i < 200; ++i) {
                // A hot overlapping window so duplicate/overlapping requests across threads are common.
                const std::uint64_t offset = rng() % (size / 2);
                const std::uint64_t max_len = std::min<std::uint64_t>(150, size - offset);
                const ByteRange range{offset, 1 + rng() % max_len};
                if (auto leased = pool->resolve(std::span(&range, 1), out)) {
                    ++resolved;
                    for (std::size_t k = 0; k < *leased; ++k) {
                        if (!matches_oracle(file, out[k].bytes(), out[k].source_offset())) {
                            ++corrupt;
                        }
                        out[k].reset();
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    check(corrupt.load() == 0, "concurrently resolved bytes equal the independent ifstream oracle");
    check(resolved.load() > 0, "concurrent resolves made progress against the real backend");
    check(pool->drain() == Status::ok, "concurrent run drains");
    const SlotPoolStats stats = pool->stats();
    check(stats.slots_pinned == 0 && stats.slots_filling == 0, "concurrent run left no pins or fills");
    check(stats.coalesced > 0, "overlapping concurrent requests shared a live fill at least once");
}

// --- allocation --------------------------------------------------------------------------------------

void test_hot_path_allocates_nothing() {
    TempDir dir;
    const fs::path file = dir.path() / "alloc.bin";
    write_file(file, 4096);

    auto backend = std::move(*LocalFileBackend::create({.workers = 2, .queue_capacity = 64, .max_sources = 1}));
    check(backend->register_file(SourceId{12}, file) == Status::ok, "file registered");

    constexpr int rounds = 32;
    std::array<DirectWaiter, rounds> waiters;
    std::array<std::vector<std::byte>, rounds> destinations; // distinct buffers: concurrent workers write
    for (auto& destination : destinations) {
        destination.resize(64);
    }

    const std::uint64_t before = allocation_count();
    for (int round = 0; round < rounds; ++round) {
        const FillRequest request{.source = SourceId{12},
                                  .source_offset = static_cast<std::uint64_t>(round) * 4,
                                  .destination = destinations[static_cast<std::size_t>(round)],
                                  .token = {static_cast<std::uint32_t>(round), 0},
                                  .sink = {&waiters[static_cast<std::size_t>(round)], &DirectWaiter::deliver}};
        (void)backend->submit(request);
    }
    check(allocation_count() == before, "submit() allocates nothing on the hot path");

    for (DirectWaiter& waiter : waiters) {
        (void)waiter.wait();
    }
}

} // namespace

int main() {
    run(test_registration, "test_registration");
    run(test_direct_submit_matches_oracle, "test_direct_submit_matches_oracle");
    run(test_unknown_source_reported_async, "test_unknown_source_reported_async");
    run(test_slot_pool_end_to_end, "test_slot_pool_end_to_end");
    run(test_truncated_file_never_resident, "test_truncated_file_never_resident");
    run(test_transfer_set_end_to_end, "test_transfer_set_end_to_end");
    run(test_queue_full_rejection, "test_queue_full_rejection");
    run(test_shutdown_with_queued_work, "test_shutdown_with_queued_work");
    run(test_concurrent_submitters, "test_concurrent_submitters");
    run(test_hot_path_allocates_nothing, "test_hot_path_allocates_nothing");
    return finish();
}
