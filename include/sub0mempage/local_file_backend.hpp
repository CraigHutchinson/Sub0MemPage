#pragma once

/** @file local_file_backend.hpp
 *  @brief M3 slice 1: a portable, bounded worker-pool FillBackend that reads bytes out of local files
 *         registered ahead of time.
 *
 *  STATUS: M3 draft (docs/implementation-plan.md M3 row; docs/transfer-contract.md "Real-file tests").
 *  Implements the FillBackendRef backend contract from transfer.hpp: `submit()` never blocks on I/O,
 *  never allocates, and returns false only when its bounded queue is full; every accepted request gets
 *  exactly one terminal delivery, made from a worker thread, never inline in submit(). One backend
 *  instance serves both ownership modes (SlotPool and TransferSet) -- it only ever sees FillRequests.
 *
 *  Scope of this slice (docs/implementation-plan.md checkpoint "Start M3", item 6): a worker-thread-pool
 *  baseline using synchronous positional reads (POSIX `pread`; on Windows, overlapped `ReadFile` used
 *  synchronously purely for its positional-offset form, via `GetOverlappedResult(..., TRUE)`). This is
 *  the same "blocking reads on worker threads" shape docs/prior-art.md sec 4 already flags for libuv --
 *  quoted there: *"the thread pool is internally used to run all file system operations"* -- i.e.
 *  structurally the same as the production defect this project exists to fix, just with the library's
 *  own threads instead of a caller's. That trade-off is accepted here, explicitly, as the portable M3
 *  baseline that Windows/Linux/macOS all share; `io_uring` (Linux, docs/prior-art.md sec 2) and
 *  IOCP/`CreateThreadpoolIo` (Windows, docs/prior-art.md sec 1) are deferred, named optimizations behind
 *  this same FillBackendRef seam, not silently dropped.
 *
 *  Alignment: this backend does ordinary buffered reads (no `O_DIRECT`/unbuffered I/O), so the open
 *  slot-storage-alignment question (implementation-plan.md "Checkpoint: M2 draft") does not apply to it;
 *  it would need revisiting only if a future unbuffered/GDS-style backend is added.
 *
 *  Administrative (may block and allocate; never on the hot path, transfer-contract.md "Endpoint and
 *  region registration"): create(), register_file(), shutdown() and the destructor. Only submit() runs
 *  on the hot path and follows the noexcept/no-alloc/non-blocking rules in transfer.hpp.
 *
 *  Unknown SourceId at submit time (documented per this file's task): looked up in a bounded, in-memory
 *  table under the same lock that guards the queue -- no allocation, no I/O. A miss is NOT reported by
 *  returning false: FillBackendRef's contract fixes false's meaning as "the bounded queue is full", and
 *  SlotPool/TransferSet translate that specifically into Status::queue_exhausted, so reusing it for a
 *  registration error would misreport a config mistake as transient backpressure. Instead the request is
 *  accepted (it still gets exactly one terminal delivery) and a worker delivers Status::invalid_argument
 *  with zero bytes once it is dequeued, exactly like a real I/O failure would be delivered.
 *
 *  Shutdown semantics (destructor must not leave a worker writing into a caller's destination):
 *  shutdown() first refuses further submits, then delivers Status::cancelled (zero bytes) for every
 *  request still queued and not yet picked up by a worker -- one delivery each, same as any other
 *  terminal outcome -- then lets any request a worker has already started finish normally (it gets its
 *  ordinary ok/short_read/io_error delivery), then joins every worker thread. The destructor calls
 *  shutdown() if the caller has not already; calling it explicitly first is preferred so the caller
 *  controls when in-flight destinations stop being written (transfer-contract.md "Unregister/close fails
 *  busy or explicitly drains on an administrative path").
 */

#include "transfer.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <limits>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sub0mempage {

#ifdef _WIN32
using NativeFileHandle = HANDLE;
inline constexpr NativeFileHandle kInvalidFileHandle = INVALID_HANDLE_VALUE;
#else
using NativeFileHandle = int;
inline constexpr NativeFileHandle kInvalidFileHandle = -1;
#endif

/// Bounded worker-pool sizing. `queue_capacity` is preallocated (R9/AGENTS.md sec 1): it IS the bound
/// submit() enforces. `max_sources` bounds the registered-file table, also preallocated.
struct LocalFileBackendConfig {
    std::uint32_t workers = 1;
    std::uint32_t queue_capacity = 0;
    std::uint32_t max_sources = 0;
};

/// Observability (AGENTS.md sec 9). `errors` covers both io_error deliveries and unknown-SourceId
/// deliveries; `cancelled` counts only shutdown()'s cancellation of still-queued requests, not the
/// ordinary Status::cancelled a backend could otherwise report (this one never reports it itself).
/// `in_flight` is a worker actually mid-perform() (dequeued, not just queued): the gauge that tells a
/// caller (or a test) a request has genuinely started, matching SlotPoolStats/TransferSetStats' own
/// in-flight gauges.
struct LocalFileBackendStats {
    std::uint64_t accepted = 0;
    std::uint64_t rejected_queue_full = 0;
    std::uint64_t completed_ok = 0;
    std::uint64_t short_reads = 0;
    std::uint64_t errors = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t bytes_read = 0;
    std::uint32_t queue_high_water = 0;
    std::uint32_t in_flight = 0;
};

/** @brief Portable worker-pool FillBackend over registered local files. See the file comment for scope,
 *  shutdown semantics and the unknown-SourceId decision.
 *
 *  Non-movable and non-copyable (worker threads and every WorkItem's CompletionSink close over `this`
 *  indirectly through this object's own address), hence create() returns a unique_ptr, matching
 *  SlotPool/TransferSet's own shape.
 */
class LocalFileBackend {
    struct Passkey {};

public:
    [[nodiscard]] static std::expected<std::unique_ptr<LocalFileBackend>, Status>
    create(const LocalFileBackendConfig& config);

    LocalFileBackend(Passkey, const LocalFileBackendConfig& config);
    LocalFileBackend(const LocalFileBackend&) = delete;
    LocalFileBackend& operator=(const LocalFileBackend&) = delete;
    ~LocalFileBackend();

    /** @brief Administrative: opens `path` read-only and records its current size under `source`.
     *  May block and allocate. Not safe to call concurrently with another register_file() naming the
     *  same `source`, nor with a submit() naming a `source` that is still mid-registration -- register
     *  every source the backend will serve before handing FillBackendRef(*this) to a pool/transfer set.
     *  @return ok; invalid_argument if `source` is already registered; ticket_exhausted if the
     *          registration table (sized by `max_sources`) is full; io_error if the file could not be
     *          opened or sized.
     */
    [[nodiscard]] Status register_file(SourceId source, const std::filesystem::path& path);

    /// Backend contract (transfer.hpp FillBackendRef): see the file comment. `false` means only "the
    /// bounded queue is full or shutdown() has been called"; an unknown SourceId is still accepted.
    [[nodiscard]] bool submit(const FillRequest& request) noexcept;

    /// Administrative: see the file comment's "Shutdown semantics". Idempotent.
    void shutdown();

    [[nodiscard]] LocalFileBackendStats stats() const noexcept;

private:
    struct SourceEntry {
        SourceId source{};
        NativeFileHandle handle = kInvalidFileHandle;
        std::uint64_t size = 0;
    };

    struct WorkItem {
        FillRequest request;
        NativeFileHandle handle = kInvalidFileHandle;
        bool source_known = false;
    };

    void worker_loop();
    void perform(WorkItem& item);
    [[nodiscard]] NativeFileHandle find_source_locked(SourceId source) const noexcept;
    static void close_handle(NativeFileHandle handle) noexcept;

    std::vector<SourceEntry> sources_; ///< Reserved to max_sources at construction; append-only.
    std::vector<WorkItem> ring_;       ///< Fixed-size ring buffer, sized to queue_capacity.
    std::size_t head_ = 0;
    std::size_t ring_size_ = 0;
    bool shutting_down_ = false;
    std::vector<std::thread> workers_;
    LocalFileBackendStats stats_;

    mutable std::mutex mutex_;
    std::condition_variable cv_; ///< Signalled on submit() and on shutdown(); workers wait on it.
};

// ---------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------

inline void LocalFileBackend::close_handle(NativeFileHandle handle) noexcept {
    if (handle == kInvalidFileHandle) {
        return;
    }
#ifdef _WIN32
    ::CloseHandle(handle);
#else
    ::close(handle);
#endif
}

inline std::expected<std::unique_ptr<LocalFileBackend>, Status>
LocalFileBackend::create(const LocalFileBackendConfig& config) {
    if (config.workers == 0 || config.queue_capacity == 0 || config.max_sources == 0) {
        return std::unexpected(Status::invalid_argument);
    }
    return std::make_unique<LocalFileBackend>(Passkey{}, config);
}

inline LocalFileBackend::LocalFileBackend(Passkey, const LocalFileBackendConfig& config)
    : ring_(config.queue_capacity) {
    sources_.reserve(config.max_sources); // never exceeded: register_file rejects once full.
    workers_.reserve(config.workers);
    // Threads start only after every member above is fully constructed and sized. worker_loop() only
    // ever touches mutex_-guarded state, so starting it here (nothing else in the constructor runs
    // concurrently with it) is safe.
    for (std::uint32_t i = 0; i < config.workers; ++i) {
        workers_.emplace_back(&LocalFileBackend::worker_loop, this);
    }
}

inline LocalFileBackend::~LocalFileBackend() {
    shutdown(); // idempotent; guarantees no worker still writes a destination (see file comment).
    for (const SourceEntry& entry : sources_) {
        close_handle(entry.handle);
    }
}

inline Status LocalFileBackend::register_file(SourceId source, const std::filesystem::path& path) {
    NativeFileHandle handle = kInvalidFileHandle;
    std::uint64_t size = 0;
#ifdef _WIN32
    // FILE_FLAG_OVERLAPPED so each read below can carry its own OVERLAPPED.Offset -- the positional
    // read this backend needs, safe under concurrent workers reading the same handle at different
    // offsets (a plain synchronous handle instead shares one implicit file pointer across threads).
    handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == kInvalidFileHandle) {
        return Status::io_error;
    }
    LARGE_INTEGER large_size{};
    if (!::GetFileSizeEx(handle, &large_size)) {
        close_handle(handle);
        return Status::io_error;
    }
    size = static_cast<std::uint64_t>(large_size.QuadPart);
#else
    handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle == kInvalidFileHandle) {
        return Status::io_error;
    }
    struct stat info {};
    if (::fstat(handle, &info) != 0) {
        close_handle(handle);
        return Status::io_error;
    }
    size = static_cast<std::uint64_t>(info.st_size);
#endif
    const std::scoped_lock lock(mutex_);
    for (const SourceEntry& entry : sources_) {
        if (entry.source == source) {
            close_handle(handle);
            return Status::invalid_argument; // duplicate registration
        }
    }
    if (sources_.size() == sources_.capacity()) {
        close_handle(handle);
        // Reusing ticket_exhausted for "this bounded administrative table is full": no dedicated
        // status exists, and SlotPool already reuses it the same way for its own bounded record table.
        return Status::ticket_exhausted;
    }
    sources_.push_back({.source = source, .handle = handle, .size = size});
    return Status::ok;
}

inline NativeFileHandle LocalFileBackend::find_source_locked(SourceId source) const noexcept {
    for (const SourceEntry& entry : sources_) {
        if (entry.source == source) {
            return entry.handle;
        }
    }
    return kInvalidFileHandle;
}

inline bool LocalFileBackend::submit(const FillRequest& request) noexcept {
    const std::scoped_lock lock(mutex_);
    if (shutting_down_ || ring_size_ == ring_.size()) {
        ++stats_.rejected_queue_full;
        return false;
    }
    WorkItem& item = ring_[(head_ + ring_size_) % ring_.size()];
    item.request = request;
    item.handle = find_source_locked(request.source);
    item.source_known = item.handle != kInvalidFileHandle;
    ++ring_size_;
    ++stats_.accepted;
    stats_.queue_high_water = std::max(stats_.queue_high_water, static_cast<std::uint32_t>(ring_size_));
    cv_.notify_one();
    return true;
}

inline void LocalFileBackend::worker_loop() {
    for (;;) {
        WorkItem item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return ring_size_ != 0 || shutting_down_; });
            if (ring_size_ == 0) {
                return; // shutting_down_ and the queue is drained: nothing left to read
            }
            item = std::move(ring_[head_]);
            head_ = (head_ + 1) % ring_.size();
            --ring_size_;
            ++stats_.in_flight; // still under the lock: visible to stats() the instant this item starts
        }
        perform(item);
    }
}

inline void LocalFileBackend::perform(WorkItem& item) {
    if (!item.source_known) {
        // Unknown SourceId, decided at submit() time; see the file comment. Never inline in submit().
        item.request.sink.deliver(item.request.token, {.status = Status::invalid_argument, .bytes = 0});
        const std::scoped_lock lock(mutex_);
        ++stats_.errors;
        --stats_.in_flight;
        return;
    }

    std::uint64_t total = 0;
    Status status = Status::ok;
    std::byte* dst = item.request.destination.data();
    std::size_t remaining = item.request.destination.size();

#ifdef _WIN32
    while (remaining > 0) {
        const std::uint64_t pos = item.request.source_offset + total;
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFull);
        overlapped.OffsetHigh = static_cast<DWORD>(pos >> 32);
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, (std::numeric_limits<DWORD>::max)()));
        const BOOL immediate = ::ReadFile(item.handle, dst + total, chunk, nullptr, &overlapped);
        if (!immediate && ::GetLastError() != ERROR_IO_PENDING) {
            if (::GetLastError() == ERROR_HANDLE_EOF) {
                break; // EOF before the requested count: report what was actually read (see below)
            }
            status = Status::io_error;
            break;
        }
        // Overlapped handle: always retrieve the true byte count via GetOverlappedResult, whether
        // ReadFile completed synchronously or is still pending -- its own lpNumberOfBytesRead output
        // is not reliable for an overlapped handle (Win32 docs for ReadFile/GetOverlappedResult).
        DWORD read_now = 0;
        if (!::GetOverlappedResult(item.handle, &overlapped, &read_now, /*bWait=*/TRUE)) {
            if (::GetLastError() == ERROR_HANDLE_EOF) {
                break;
            }
            status = Status::io_error;
            break;
        }
        if (read_now == 0) {
            break; // EOF
        }
        total += read_now;
        remaining -= read_now;
    }
#else
    while (remaining > 0) {
        const auto pos = static_cast<off_t>(item.request.source_offset + total);
        const ssize_t got = ::pread(item.handle, dst + total, remaining, pos);
        if (got < 0) {
            if (errno == EINTR) {
                continue; // retry the same offset/length, per the task's pread-loop contract
            }
            status = Status::io_error;
            break;
        }
        if (got == 0) {
            break; // EOF before the requested count: report what was actually read (see below)
        }
        total += static_cast<std::uint64_t>(got);
        remaining -= static_cast<std::size_t>(got);
    }
#endif

    // Always report status=ok with the actual byte count, even on a short/EOF-clipped read, matching
    // tests/fake_backend.hpp's convention: transfer.hpp's detail::classify_fill (shared by SlotPool and
    // TransferSet) is the single place that turns "ok but bytes < requested" into Status::short_read, so
    // a short read is never published as resident/complete however it is discovered.
    item.request.sink.deliver(item.request.token, {.status = status, .bytes = total});

    const std::scoped_lock lock(mutex_);
    stats_.bytes_read += total;
    --stats_.in_flight;
    if (status != Status::ok) {
        ++stats_.errors;
    } else if (total == item.request.destination.size()) {
        ++stats_.completed_ok;
    } else {
        ++stats_.short_reads;
    }
}

inline void LocalFileBackend::shutdown() {
    std::vector<WorkItem> queued;
    {
        const std::scoped_lock lock(mutex_);
        if (shutting_down_) {
            return; // idempotent: already drained (or draining) by an earlier call
        }
        shutting_down_ = true;
        queued.reserve(ring_size_);
        while (ring_size_ != 0) {
            queued.push_back(std::move(ring_[head_]));
            head_ = (head_ + 1) % ring_.size();
            --ring_size_;
        }
        cv_.notify_all(); // wake every worker so idle ones observe shutting_down_ and exit
    }
    // Delivered outside the lock, like every other completion (CompletionSink::deliver may run
    // arbitrary pool/transfer-set code that takes its own lock). Each of these requests was accepted
    // by submit() and had not yet reached a worker, so this is its one and only terminal delivery.
    for (WorkItem& item : queued) {
        item.request.sink.deliver(item.request.token, {.status = Status::cancelled, .bytes = 0});
        const std::scoped_lock lock(mutex_);
        ++stats_.cancelled;
    }
    // Any request a worker had already dequeued finishes perform() normally (its own single delivery)
    // and the worker then exits its loop once the drained queue is empty; join() waits for exactly that.
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

inline LocalFileBackendStats LocalFileBackend::stats() const noexcept {
    const std::scoped_lock lock(mutex_);
    return stats_;
}

} // namespace sub0mempage
