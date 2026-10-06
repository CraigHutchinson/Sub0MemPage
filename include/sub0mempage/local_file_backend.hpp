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
 *  Access mode: a file is registered buffered (the default: reads go through the OS page cache) or
 *  FileAccess::uncached (they bypass it and the device writes straight into the destination). Uncached
 *  requests must be aligned -- see FileAccess. A caller-owned cache that is meant to be the only copy
 *  wants uncached fills: a buffered fill leaves a second copy in the OS cache and pays a kernel copy.
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
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sub0mempage {

#ifdef _WIN32
using NativeFileHandle = HANDLE;
// Not constexpr: INVALID_HANDLE_VALUE is a pointer cast, which MSVC rejects in a constant expression.
inline const NativeFileHandle kInvalidFileHandle = INVALID_HANDLE_VALUE;
#else
using NativeFileHandle = int;
inline constexpr NativeFileHandle kInvalidFileHandle = -1;
#endif

/// Offset, length and destination alignment that FileAccess::uncached requests must meet. 4 KiB covers
/// the logical and physical sector sizes of current NVMe/SATA devices.
inline constexpr std::size_t kUncachedAlignment = 4096;

/** @brief How a registered file is read.
 *
 *  `uncached` opens the file with FILE_FLAG_NO_BUFFERING (Windows), O_DIRECT (Linux) or F_NOCACHE
 *  (macOS). Each request against it must have a `source_offset` and a destination address that are
 *  multiples of kUncachedAlignment, and a length that is too -- except a request that runs to the end
 *  of the file, whose length may stop there. A request that breaks this is delivered
 *  Status::invalid_argument, like an unknown SourceId.
 *
 *  @note On Windows, uncached reads of a file stop overlapping (each waits for the previous one) while
 *  the same file also has a buffered handle or a mapping open, or had one moments ago: measured 2026-10-01
 *  (docs/investigations/unbuffered-read-ceiling.md), 7 x 256 KiB took ~2.4 ms beside a held buffered
 *  reader against ~0.6 ms alone. Give an uncached source a direct-only lifetime: read its metadata
 *  through this mode too, and do not map it.
 */
enum class FileAccess : std::uint8_t { buffered, uncached };

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
     *  @param access buffered, or uncached with the alignment rules and lifetime note on FileAccess.
     *  @return ok; invalid_argument if `source` is already registered; ticket_exhausted if the
     *          registration table (sized by `max_sources`) is full; io_error if the file could not be
     *          opened or sized (including a filesystem that refuses uncached opens).
     */
    [[nodiscard]] Status register_file(SourceId source, const std::filesystem::path& path,
                                       FileAccess access = FileAccess::buffered);

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
        FileAccess access = FileAccess::buffered;
    };

    struct WorkItem {
        FillRequest request;
        NativeFileHandle handle = kInvalidFileHandle;
        std::uint64_t source_size = 0;
        FileAccess access = FileAccess::buffered;
        bool source_known = false;
    };

    /// What one worker thread owns for its reads. `event` (Windows only) is its manual-reset event: each
    /// overlapped read waits on it rather than on the shared file handle. `tail` receives the last,
    /// partial block of an uncached file, which cannot be read straight into a shorter destination.
    struct WorkerScratch {
        void* event = nullptr;
        std::byte* tail = nullptr; // non-owning; kUncachedAlignment bytes, aligned
    };

    void worker_loop();
    void perform(WorkItem& item, const WorkerScratch& scratch);
    /// Positional read of up to `length` bytes; stops early at EOF. Sets `status` to io_error on failure.
    /// `access` uncached: a short count is end-of-file, since a retry would start mid-block and be refused.
    [[nodiscard]] static std::uint64_t read_range(NativeFileHandle handle, void* event, FileAccess access,
                                                  std::byte* destination, std::size_t length, std::uint64_t offset,
                                                  Status& status) noexcept;
    [[nodiscard]] static bool uncached_request_valid(const WorkItem& item) noexcept;
    [[nodiscard]] const SourceEntry* find_source_locked(SourceId source) const noexcept;
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

inline Status LocalFileBackend::register_file(SourceId source, const std::filesystem::path& path, FileAccess access) {
    const bool uncached = access == FileAccess::uncached;
    NativeFileHandle handle = kInvalidFileHandle;
    std::uint64_t size = 0;
#ifdef _WIN32
    // FILE_FLAG_OVERLAPPED so each read below can carry its own OVERLAPPED.Offset -- the positional
    // read this backend needs, safe under concurrent workers reading the same handle at different
    // offsets (a plain synchronous handle instead shares one implicit file pointer across threads).
    handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | (uncached ? FILE_FLAG_NO_BUFFERING : 0), nullptr);
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
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECT
    if (uncached) {
        flags |= O_DIRECT;
    }
#endif
    handle = ::open(path.c_str(), flags);
    if (handle == kInvalidFileHandle) {
        return Status::io_error;
    }
#ifdef F_NOCACHE
    if (uncached && ::fcntl(handle, F_NOCACHE, 1) != 0) {
        close_handle(handle);
        return Status::io_error;
    }
#endif
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
    sources_.push_back({.source = source, .handle = handle, .size = size, .access = access});
    return Status::ok;
}

inline const LocalFileBackend::SourceEntry* LocalFileBackend::find_source_locked(SourceId source) const noexcept {
    for (const SourceEntry& entry : sources_) {
        if (entry.source == source) {
            return &entry;
        }
    }
    return nullptr;
}

inline bool LocalFileBackend::submit(const FillRequest& request) noexcept {
    const std::scoped_lock lock(mutex_);
    if (shutting_down_ || ring_size_ == ring_.size()) {
        ++stats_.rejected_queue_full;
        return false;
    }
    WorkItem& item = ring_[(head_ + ring_size_) % ring_.size()];
    item.request = request;
    const SourceEntry* const entry = find_source_locked(request.source);
    item.source_known = entry != nullptr;
    item.handle = entry ? entry->handle : kInvalidFileHandle;
    item.source_size = entry ? entry->size : 0;
    item.access = entry ? entry->access : FileAccess::buffered;
    ++ring_size_;
    ++stats_.accepted;
    stats_.queue_high_water = std::max(stats_.queue_high_water, static_cast<std::uint32_t>(ring_size_));
    cv_.notify_one();
    return true;
}

inline void LocalFileBackend::worker_loop() {
#ifdef _WIN32
    // One event per worker: several workers read the same overlapped handle concurrently, and with a
    // null OVERLAPPED.hEvent GetOverlappedResult waits on the file handle itself, which any of those
    // reads can signal ("Use of file handles for this purpose is discouraged", Win32 OVERLAPPED docs).
    // A null event here is not fatal: perform() reports io_error for every read this worker takes.
    const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct EventCloser {
        HANDLE handle;
        ~EventCloser() {
            if (handle != nullptr) {
                ::CloseHandle(handle);
            }
        }
    } const closer{event};
#else
    void* const event = nullptr;
#endif
    alignas(kUncachedAlignment) std::byte tail[kUncachedAlignment];
    const WorkerScratch scratch{.event = event, .tail = tail};
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
        perform(item, scratch);
    }
}

inline bool LocalFileBackend::uncached_request_valid(const WorkItem& item) noexcept {
    const std::span<std::byte> destination = item.request.destination;
    const bool to_end_of_file = item.request.source_offset + destination.size() >= item.source_size;
    return item.request.source_offset % kUncachedAlignment == 0 &&
           reinterpret_cast<std::uintptr_t>(destination.data()) % kUncachedAlignment == 0 &&
           (destination.size() % kUncachedAlignment == 0 || to_end_of_file);
}

inline std::uint64_t LocalFileBackend::read_range(NativeFileHandle handle, [[maybe_unused]] void* event,
                                                  FileAccess access, std::byte* destination, std::size_t length,
                                                  std::uint64_t offset, Status& status) noexcept {
    const bool short_is_eof = access == FileAccess::uncached;
    std::uint64_t total = 0;
    std::size_t remaining = length;
#ifdef _WIN32
    if (event == nullptr) {
        status = Status::io_error;
        return 0;
    }
    while (remaining > 0) {
        const std::uint64_t pos = offset + total;
        OVERLAPPED overlapped{};
        overlapped.hEvent = static_cast<HANDLE>(event); // manual-reset; ReadFile resets it on entry
        overlapped.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFull);
        overlapped.OffsetHigh = static_cast<DWORD>(pos >> 32);
        // Capped at 1 GiB, a multiple of kUncachedAlignment, so a split uncached read stays aligned.
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, std::size_t{1} << 30));
        const BOOL immediate = ::ReadFile(handle, destination + total, chunk, nullptr, &overlapped);
        if (!immediate && ::GetLastError() != ERROR_IO_PENDING) {
            if (::GetLastError() == ERROR_HANDLE_EOF) {
                break; // EOF before the requested count: report what was actually read
            }
            status = Status::io_error;
            break;
        }
        // Overlapped handle: always retrieve the true byte count via GetOverlappedResult, whether
        // ReadFile completed synchronously or is still pending -- its own lpNumberOfBytesRead output
        // is not reliable for an overlapped handle (Win32 docs for ReadFile/GetOverlappedResult).
        DWORD read_now = 0;
        if (!::GetOverlappedResult(handle, &overlapped, &read_now, /*bWait=*/TRUE)) {
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
        if (short_is_eof && read_now < chunk) {
            break;
        }
    }
#else
    while (remaining > 0) {
        const ssize_t got = ::pread(handle, destination + total, remaining, static_cast<off_t>(offset + total));
        if (got < 0) {
            if (errno == EINTR) {
                continue; // retry the same offset/length, per the task's pread-loop contract
            }
            status = Status::io_error;
            break;
        }
        if (got == 0) {
            break; // EOF before the requested count: report what was actually read
        }
        total += static_cast<std::uint64_t>(got);
        if (short_is_eof && static_cast<std::size_t>(got) < remaining) {
            break;
        }
        remaining -= static_cast<std::size_t>(got);
    }
#endif
    return total;
}

inline void LocalFileBackend::perform(WorkItem& item, const WorkerScratch& scratch) {
    const bool uncached = item.access == FileAccess::uncached;
    if (!item.source_known || (uncached && !uncached_request_valid(item))) {
        // Unknown SourceId (decided at submit() time; see the file comment) or a misaligned uncached
        // request (see FileAccess). Never inline in submit().
        item.request.sink.deliver(item.request.token, {.status = Status::invalid_argument, .bytes = 0});
        const std::scoped_lock lock(mutex_);
        ++stats_.errors;
        --stats_.in_flight;
        return;
    }

    Status status = Status::ok;
    std::byte* const dst = item.request.destination.data();
    const std::size_t wanted = item.request.destination.size();
    // An uncached read must be whole blocks: read those in place, then the file's last partial block
    // through the worker's aligned scratch (only a request that runs to end-of-file has one).
    const std::size_t in_place = uncached ? wanted / kUncachedAlignment * kUncachedAlignment : wanted;
    std::uint64_t total = read_range(item.handle, scratch.event, item.access, dst, in_place, item.request.source_offset, status);
    if (status == Status::ok && total == in_place && in_place < wanted) {
        const std::uint64_t got = read_range(item.handle, scratch.event, item.access, scratch.tail, kUncachedAlignment,
                                             item.request.source_offset + in_place, status);
        const std::size_t tail = std::min<std::size_t>(static_cast<std::size_t>(got), wanted - in_place);
        std::copy_n(scratch.tail, tail, dst + in_place);
        total += tail;
    }

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
