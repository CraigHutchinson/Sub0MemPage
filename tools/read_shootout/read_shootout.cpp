// sub0mempage-read-shootout: how fast can one row get from a file into a RAM buffer, by each strategy?
//
// The scenario is a cache miss as Sub0Llm's expert cache sees it: one row of `--row-bytes` (default one
// 1,766,400-byte MoE expert) at a row-aligned offset, read into a pinned slot, with a gap between misses
// that stands in for the compute between them. Every strategy reads the same rows, in rotated order
// across rounds, under the same cache state, gap and optional DRAM load. A strategy that returns the
// wrong bytes cannot win: sampled rows are checksummed and compared with an independent read.
//
// The default file is small and generated (see README.md): quick comparisons, not exhaustive ones.

#include <sub0mempage/local_file_backend.hpp>
#include <sub0mempage/transfer_set.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if __has_include(<ioringapi.h>)
#include <ioringapi.h>
#define SHOOTOUT_HAS_IORING 1
#endif
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define SHOOTOUT_HAS_NT_COPY 1
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kAlign = 4096; // unbuffered I/O alignment: offset, length and buffer

std::size_t round_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

// --- platform layer --------------------------------------------------------------------------------------

#ifdef _WIN32
using Handle = HANDLE;
const Handle kNoHandle = INVALID_HANDLE_VALUE;
#else
using Handle = int;
constexpr Handle kNoHandle = -1;
#endif

struct File {
    Handle handle = kNoHandle;
    std::uint64_t size = 0;
    [[nodiscard]] bool ok() const { return handle != kNoHandle; }
};

File open_file(const std::filesystem::path& path, bool unbuffered) {
    File f;
#ifdef _WIN32
    // Overlapped so concurrent positional reads on one handle do not serialize on a file pointer.
    const DWORD flags = FILE_FLAG_OVERLAPPED | (unbuffered ? FILE_FLAG_NO_BUFFERING : 0);
    f.handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    LARGE_INTEGER size{};
    if (f.ok() && ::GetFileSizeEx(f.handle, &size)) f.size = static_cast<std::uint64_t>(size.QuadPart);
#else
    int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECT
    if (unbuffered) flags |= O_DIRECT;
#endif
    f.handle = ::open(path.c_str(), flags);
#ifdef F_NOCACHE
    if (f.ok() && unbuffered && ::fcntl(f.handle, F_NOCACHE, 1) != 0) { ::close(f.handle); f.handle = kNoHandle; }
#endif
    struct stat st {};
    if (f.ok() && ::fstat(f.handle, &st) == 0) f.size = static_cast<std::uint64_t>(st.st_size);
#endif
    return f;
}

void close_file(File& f) {
    if (!f.ok()) return;
#ifdef _WIN32
    ::CloseHandle(f.handle);
#else
    ::close(f.handle);
#endif
    f.handle = kNoHandle;
}

#ifdef _WIN32
struct ThreadEvent { // one manual-reset event per thread for overlapped positional reads
    HANDLE h = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ThreadEvent() { if (h) ::CloseHandle(h); }
    ThreadEvent() = default;
    ThreadEvent(const ThreadEvent&) = delete;
    ThreadEvent& operator=(const ThreadEvent&) = delete;
};
#endif

/// Positional read until `len` bytes or EOF; returns the bytes read.
std::size_t read_at(const File& f, std::byte* dst, std::size_t len, std::uint64_t off) {
    std::size_t total = 0;
#ifdef _WIN32
    thread_local ThreadEvent event;
    while (total < len) {
        OVERLAPPED ov{};
        ov.hEvent = event.h;
        ov.Offset = static_cast<DWORD>((off + total) & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>((off + total) >> 32);
        const auto want = static_cast<DWORD>(std::min<std::size_t>(len - total, 1u << 30));
        if (!::ReadFile(f.handle, dst + total, want, nullptr, &ov) && ::GetLastError() != ERROR_IO_PENDING) break;
        DWORD got = 0;
        if (!::GetOverlappedResult(f.handle, &ov, &got, TRUE) || got == 0) break;
        total += got;
    }
#else
    while (total < len) {
        const ssize_t got = ::pread(f.handle, dst + total, len - total, static_cast<off_t>(off + total));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        total += static_cast<std::size_t>(got);
    }
#endif
    return total;
}

std::byte* alloc_pages(std::size_t bytes) {
#ifdef _WIN32
    return static_cast<std::byte*>(::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<std::byte*>(p);
#endif
}

void free_pages(std::byte* p, std::size_t bytes) {
#ifdef _WIN32
    (void)bytes;
    ::VirtualFree(p, 0, MEM_RELEASE);
#else
    ::munmap(p, bytes);
#endif
}

/// Best effort: lock the slots in RAM, as the owned cache does. Reported, not required.
bool pin(std::byte* p, std::size_t bytes) {
#ifdef _WIN32
    SIZE_T lo = 0, hi = 0;
    ::GetProcessWorkingSetSize(::GetCurrentProcess(), &lo, &hi);
    ::SetProcessWorkingSetSize(::GetCurrentProcess(), lo + bytes + (16u << 20), hi + bytes + (16u << 20));
    return ::VirtualLock(p, bytes) != 0;
#else
    return ::mlock(p, bytes) == 0;
#endif
}

struct Mapping {
    const std::byte* base = nullptr;
    std::uint64_t size = 0;
#ifdef _WIN32
    HANDLE section = nullptr;
#endif
};

Mapping map_file(const File& f) {
    Mapping m;
    m.size = f.size;
#ifdef _WIN32
    m.section = ::CreateFileMappingW(f.handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (m.section) m.base = static_cast<const std::byte*>(::MapViewOfFile(m.section, FILE_MAP_READ, 0, 0, 0));
#else
    void* p = ::mmap(nullptr, f.size, PROT_READ, MAP_SHARED, f.handle, 0);
    if (p != MAP_FAILED) m.base = static_cast<const std::byte*>(p);
#endif
    return m;
}

void unmap(Mapping& m) {
#ifdef _WIN32
    if (m.base) ::UnmapViewOfFile(m.base);
    if (m.section) ::CloseHandle(m.section);
    m.section = nullptr;
#else
    if (m.base) ::munmap(const_cast<std::byte*>(m.base), m.size);
#endif
    m.base = nullptr;
}

/// Asks the OS to start reading a mapped range in: PrefetchVirtualMemory / madvise(MADV_WILLNEED).
bool prefetch_mapped(const std::byte* p, std::size_t len) {
    const auto start = reinterpret_cast<std::uintptr_t>(p) / kAlign * kAlign;
    const std::size_t span = round_up(reinterpret_cast<std::uintptr_t>(p) + len - start, kAlign);
#ifdef _WIN32
    WIN32_MEMORY_RANGE_ENTRY range{reinterpret_cast<void*>(start), span};
    return ::PrefetchVirtualMemory(::GetCurrentProcess(), 1, &range, 0) != 0;
#else
    return ::madvise(reinterpret_cast<void*>(start), span, MADV_WILLNEED) == 0;
#endif
}

/// Drops the file from the OS page cache, unelevated. Windows: a non-cached open purges a file nothing
/// maps (undocumented, so the caller verifies). Linux: posix_fadvise(DONTNEED). macOS: unsupported.
bool evict(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_NO_BUFFERING, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
#elif defined(POSIX_FADV_DONTNEED)
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0;
    ::close(fd);
    return ok;
#else
    (void)path;
    return false;
#endif
}

// --- measurement helpers ---------------------------------------------------------------------------------

std::uint64_t fnv1a(const std::byte* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) h = (h ^ static_cast<std::uint8_t>(p[i])) * 1099511628211ull;
    return h;
}

double us_since(Clock::time_point t0) { return std::chrono::duration<double, std::micro>(Clock::now() - t0).count(); }

/// Median latency of random 4 KiB buffered reads: a few us from the page cache, ~50-200 us from NVMe.
double probe_cache_us(const std::filesystem::path& path) {
    File f = open_file(path, false);
    if (!f.ok() || f.size < 2 * kAlign) return 0;
    std::vector<std::byte> buf(kAlign);
    std::mt19937_64 rng(0x5eed);
    std::vector<double> t;
    for (int i = 0; i < 32; ++i) {
        const std::uint64_t off = rng() % (f.size / kAlign - 1) * kAlign;
        const auto t0 = Clock::now();
        (void)read_at(f, buf.data(), kAlign, off);
        t.push_back(us_since(t0));
    }
    close_file(f);
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

/// Streams reads over private buffers larger than L3, standing in for decode's weight streaming.
class DramLoad {
public:
    explicit DramLoad(unsigned threads) {
        for (unsigned t = 0; t < threads; ++t)
            threads_.emplace_back([this] {
                std::vector<std::uint64_t> buf((128u << 20) / sizeof(std::uint64_t), 1);
                std::uint64_t acc = 0;
                while (!stop_.load(std::memory_order_relaxed)) {
                    for (const std::uint64_t v : buf) acc += v;
                    bytes_.fetch_add(buf.size() * sizeof(std::uint64_t), std::memory_order_relaxed);
                }
                sink_.fetch_add(acc, std::memory_order_relaxed);
            });
        start_ = Clock::now();
    }
    ~DramLoad() { stop(); }
    DramLoad(const DramLoad&) = delete;
    DramLoad& operator=(const DramLoad&) = delete;
    double stop() {
        if (!threads_.empty()) {
            stop_ = true;
            for (auto& t : threads_) t.join();
            threads_.clear();
            gbps_ = static_cast<double>(bytes_.load()) / 1e3 / us_since(start_);
        }
        return gbps_;
    }

private:
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> bytes_{0}, sink_{0};
    Clock::time_point start_;
    double gbps_ = 0;
};

// --- strategies ------------------------------------------------------------------------------------------

struct Options {
    std::filesystem::path file = std::filesystem::temp_directory_path() / "sub0mempage-read-shootout.bin";
    std::uint64_t file_mib = 256;
    std::size_t row_bytes = 1'766'400;
    std::size_t chunk_bytes = 256 * 1024;
    unsigned readers = 8;
    int misses = 100;
    int rounds = 2;
    enum class Gap { none, busy, sleep } gap = Gap::busy;
    int gap_us = 2800;
    bool cold = false;
    unsigned load_threads = 0;
    int verify_every = 4;
    std::string only; // comma-separated strategy names; empty = all
    bool list = false;
    bool pin = true;  // lock the slots in RAM, as the owned cache does
    bool direct_session = false; // no buffered readers between uncached runs
};

struct Miss {
    std::uint64_t offset = 0;
    std::byte* slot = nullptr; // kAlign-aligned, row_bytes + 2*kAlign long
};

class Strategy {
public:
    virtual ~Strategy() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual std::string_view what() const = 0;
    /// Opens per run (after any eviction). Returns false, with `unavailable_` set, if this host cannot run it.
    virtual bool open(const Options& o) = 0;
    /// Reads one row; returns a pointer to its bytes (in the slot, or in a mapping), or nullptr on failure.
    virtual const std::byte* read(const Miss& m) = 0;
    virtual void close() = 0;
    [[nodiscard]] const std::string& unavailable() const { return unavailable_; }

protected:
    std::string unavailable_;
    std::size_t row_ = 0;
};

/// Unbuffered I/O needs aligned offsets and lengths: read the aligned window that covers the row into the
/// slot, and return where the row starts inside it.
struct Window {
    std::uint64_t offset;
    std::size_t length, head;
};
Window aligned_window(std::uint64_t off, std::size_t len) {
    const std::uint64_t start = off / kAlign * kAlign;
    const auto head = static_cast<std::size_t>(off - start);
    return {start, round_up(head + len, kAlign), head};
}

class NaiveFread final : public Strategy {
public:
    std::string_view name() const override { return "naive-fread"; }
    std::string_view what() const override { return "stdio fseek+fread on the calling thread (the baseline)"; }
    bool open(const Options& o) override {
        row_ = o.row_bytes;
        file_ = std::fopen(o.file.string().c_str(), "rb");
        if (!file_) unavailable_ = "fopen failed";
        return file_ != nullptr;
    }
    const std::byte* read(const Miss& m) override {
#ifdef _WIN32
        if (::_fseeki64(file_, static_cast<long long>(m.offset), SEEK_SET) != 0) return nullptr;
#else
        if (::fseeko(file_, static_cast<off_t>(m.offset), SEEK_SET) != 0) return nullptr;
#endif
        return std::fread(m.slot, 1, row_, file_) == row_ ? m.slot : nullptr;
    }
    void close() override {
        if (file_) std::fclose(file_);
        file_ = nullptr;
    }

private:
    std::FILE* file_ = nullptr;
};

class InlineRead final : public Strategy {
public:
    explicit InlineRead(bool unbuffered) : unbuffered_(unbuffered) {}
    std::string_view name() const override { return unbuffered_ ? "inline-unbuffered" : "inline-pread"; }
    std::string_view what() const override {
        return unbuffered_ ? "one aligned non-cached read (NO_BUFFERING / O_DIRECT / F_NOCACHE), calling thread"
                           : "one positional read (ReadFile / pread) on the calling thread";
    }
    bool open(const Options& o) override {
        row_ = o.row_bytes;
        file_ = open_file(o.file, unbuffered_);
        if (!file_.ok()) unavailable_ = unbuffered_ ? "non-cached open refused (filesystem?)" : "open failed";
        return file_.ok();
    }
    const std::byte* read(const Miss& m) override {
        if (!unbuffered_) return read_at(file_, m.slot, row_, m.offset) == row_ ? m.slot : nullptr;
        const Window w = aligned_window(m.offset, row_);
        return read_at(file_, m.slot, w.length, w.offset) >= w.head + row_ ? m.slot + w.head : nullptr;
    }
    void close() override { close_file(file_); }

private:
    bool unbuffered_;
    File file_;
};

/// Parked worker threads (like LocalFileBackend's): each miss is split into chunks the workers share.
class ChunkPool {
public:
    struct Task {
        std::byte* dst;
        std::size_t len;
        std::uint64_t off;
        std::size_t need; // bytes that must arrive (an unbuffered tail may run past EOF)
    };
    explicit ChunkPool(unsigned n) {
        for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { loop(); });
    }
    ~ChunkPool() {
        {
            const std::scoped_lock lock(mutex_);
            stop_ = true;
        }
        work_.notify_all();
        for (auto& t : threads_) t.join();
    }
    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;
    bool run(const File& f, std::span<const Task> tasks) {
        std::unique_lock lock(mutex_);
        file_ = &f;
        tasks_ = tasks;
        next_ = done_ = 0;
        failed_ = false;
        ++generation_;
        work_.notify_all();
        finished_.wait(lock, [&] { return done_ == tasks_.size(); });
        return !failed_;
    }

private:
    void loop() {
        std::uint64_t seen = 0;
        std::unique_lock lock(mutex_);
        while (true) {
            work_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            while (next_ < tasks_.size()) {
                const Task t = tasks_[next_++];
                lock.unlock();
                const bool ok = read_at(*file_, t.dst, t.len, t.off) >= t.need;
                lock.lock();
                failed_ = failed_ || !ok;
                if (++done_ == tasks_.size()) finished_.notify_one();
            }
        }
    }
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable work_, finished_;
    const File* file_ = nullptr;
    std::span<const Task> tasks_;
    std::size_t next_ = 0, done_ = 0;
    std::uint64_t generation_ = 0;
    bool failed_ = false, stop_ = false;
};

class PoolRead final : public Strategy {
public:
    explicit PoolRead(bool unbuffered) : unbuffered_(unbuffered) {}
    std::string_view name() const override { return unbuffered_ ? "pool-unbuffered" : "pool-pread"; }
    std::string_view what() const override {
        return unbuffered_ ? "chunks over parked threads, non-cached (DMA straight into the slot)"
                           : "chunks over parked threads, buffered";
    }
    bool open(const Options& o) override {
        row_ = o.row_bytes;
        chunk_ = std::max(kAlign, o.chunk_bytes / kAlign * kAlign);
        file_ = open_file(o.file, unbuffered_);
        if (!file_.ok()) {
            unavailable_ = "non-cached open refused (filesystem?)";
            return false;
        }
        pool_ = std::make_unique<ChunkPool>(o.readers);
        tasks_.reserve(row_ / chunk_ + 2);
        return true;
    }
    const std::byte* read(const Miss& m) override {
        const Window w = unbuffered_ ? aligned_window(m.offset, row_) : Window{m.offset, row_, 0};
        tasks_.clear();
        for (std::size_t at = 0; at < w.length; at += chunk_) {
            const std::size_t len = std::min(chunk_, w.length - at);
            const std::size_t need = std::min(len, w.head + row_ > at ? w.head + row_ - at : 0);
            tasks_.push_back({m.slot + at, len, w.offset + at, need});
        }
        return pool_->run(file_, tasks_) ? m.slot + w.head : nullptr;
    }
    void close() override {
        pool_.reset();
        close_file(file_);
    }

private:
    bool unbuffered_;
    std::size_t chunk_ = 0;
    File file_;
    std::unique_ptr<ChunkPool> pool_;
    std::vector<ChunkPool::Task> tasks_;
};

#ifdef _WIN32
/// Issue the whole aligned window before waiting. Persistent requests and an IOCP, no worker hand-off.
/// See docs/investigations/unbuffered-read-ceiling.md for the cached-reader lifetime requirement.
class IocpRead final : public Strategy {
public:
    std::string_view name() const override { return "iocp-unbuffered"; }
    std::string_view what() const override { return "non-cached chunks issued together, persistent IOCP completions"; }
    bool open(const Options& o) override {
        row_ = o.row_bytes;
        chunk_ = std::clamp(o.chunk_bytes / kAlign * kAlign, kAlign, std::size_t{1} << 30);
        file_ = open_file(o.file, true);
        if (file_.ok()) port_ = ::CreateIoCompletionPort(file_.handle, nullptr, 0, 1);
        if (!port_) {
            unavailable_ = "non-cached open or IOCP creation failed";
            close_file(file_);
            return false;
        }
        requests_.resize((round_up(row_ + kAlign - 1, kAlign) + chunk_ - 1) / chunk_);
        return true;
    }
    const std::byte* read(const Miss& m) override {
        const Window w = aligned_window(m.offset, row_);
        std::size_t issued = 0;
        bool ok = true;
        for (std::size_t at = 0; at < w.length; at += chunk_) {
            auto& request = requests_[issued];
            request = {};
            request.Offset = static_cast<DWORD>(w.offset + at);
            request.OffsetHigh = static_cast<DWORD>((w.offset + at) >> 32);
            const auto len = static_cast<DWORD>(std::min(chunk_, w.length - at));
            if (!::ReadFile(file_.handle, m.slot + at, len, nullptr, &request) && ::GetLastError() != ERROR_IO_PENDING) {
                ok = false;
                break;
            }
            ++issued;
        }
        // Even on an issue error, drain every accepted request before returning ownership of the slot.
        for (std::size_t i = 0; i < issued; ++i) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* request = nullptr;
            const bool completed = ::GetQueuedCompletionStatus(port_, &bytes, &key, &request, INFINITE) != 0;
            if (!request) { ok = false; continue; }
            const auto index = static_cast<std::size_t>(request - requests_.data());
            const auto at = index * chunk_;
            ok = ok && completed && index < issued && bytes == std::min(chunk_, w.length - at);
        }
        return ok ? m.slot + w.head : nullptr;
    }
    void close() override {
        close_file(file_);
        if (port_) ::CloseHandle(port_);
        port_ = nullptr;
    }
private:
    File file_;
    HANDLE port_ = nullptr;
    std::size_t chunk_ = 0;
    std::vector<OVERLAPPED> requests_;
};
#endif

/// The library as shipped: LocalFileBackend workers + a TransferSet over the slot pool, chunked.
class MemPageRead final : public Strategy {
public:
    explicit MemPageRead(std::span<std::byte> slots) : slots_(slots) {}
    std::string_view name() const override { return "mempage"; }
    std::string_view what() const override { return "Sub0MemPage LocalFileBackend + TransferSet, chunked (as shipped)"; }
    bool open(const Options& o) override {
        row_ = o.row_bytes;
        chunk_ = std::max<std::size_t>(1, o.chunk_bytes);
        const auto chunks = static_cast<std::uint32_t>((row_ + chunk_ - 1) / chunk_);
        auto backend = sub0mempage::LocalFileBackend::create(
            {.workers = o.readers, .queue_capacity = chunks + 4, .max_sources = 1});
        if (!backend || (*backend)->register_file(kSource, o.file) != sub0mempage::Status::ok) {
            unavailable_ = "backend create/register failed";
            return false;
        }
        backend_ = std::move(*backend);
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(o.file, ec);
        auto set = sub0mempage::TransferSet::create({.source = kSource, .source_bytes = bytes, .destination = slots_,
                                                     .max_claims = chunks + 4},
                                                    sub0mempage::FillBackendRef(*backend_));
        if (!set) {
            unavailable_ = "transfer set create failed";
            return false;
        }
        set_ = std::move(*set);
        claims_.reserve(chunks);
        return true;
    }
    const std::byte* read(const Miss& m) override {
        const auto base = static_cast<std::uint64_t>(m.slot - slots_.data());
        claims_.clear();
        for (std::size_t at = 0; at < row_; at += chunk_) {
            auto claim = set_->submit({m.offset + at, std::min(chunk_, row_ - at)}, base + at);
            if (!claim) return nullptr;
            claims_.push_back(std::move(*claim));
        }
        bool ok = true;
        for (auto& c : claims_) ok = c.wait() == sub0mempage::Status::ok && ok;
        claims_.clear();
        return ok ? m.slot : nullptr;
    }
    void close() override {
        claims_.clear();
        if (set_) (void)set_->drain();
        set_.reset();
        if (backend_) backend_->shutdown();
        backend_.reset();
    }

private:
    static constexpr auto kSource = static_cast<sub0mempage::SourceId>(1);
    std::span<std::byte> slots_;
    std::size_t chunk_ = 0;
    std::unique_ptr<sub0mempage::LocalFileBackend> backend_;
    std::unique_ptr<sub0mempage::TransferSet> set_;
    std::vector<sub0mempage::Claim> claims_;
};

class MappedRead final : public Strategy {
public:
    enum class Mode { copy, prefetch_copy, nt_copy, touch };
    explicit MappedRead(Mode mode) : mode_(mode) {}
    std::string_view name() const override {
        switch (mode_) {
        case Mode::copy: return "map-copy";
        case Mode::prefetch_copy: return "map-prefetch-copy";
        case Mode::nt_copy: return "map-ntcopy";
        case Mode::touch: return "map-touch";
        }
        return "?";
    }
    std::string_view what() const override {
        switch (mode_) {
        case Mode::copy: return "memory-map the file, memcpy the row into the slot (page faults on first touch)";
        case Mode::prefetch_copy: return "map, PrefetchVirtualMemory / madvise(WILLNEED) the row, then memcpy";
        case Mode::nt_copy: return "map, copy with x86-64 non-temporal stores (bypass the cache on write)";
        case Mode::touch: return "map and touch one byte per page, no copy (what reactive mmap pays)";
        }
        return "";
    }
    bool open(const Options& o) override {
        row_ = o.row_bytes;
#ifndef SHOOTOUT_HAS_NT_COPY
        if (mode_ == Mode::nt_copy) {
            unavailable_ = "x86-64 only";
            return false;
        }
#endif
        file_ = open_file(o.file, false);
        map_ = file_.ok() ? map_file(file_) : Mapping{};
        if (!map_.base) unavailable_ = "map failed";
        return map_.base != nullptr;
    }
    const std::byte* read(const Miss& m) override {
        const std::byte* src = map_.base + m.offset;
        switch (mode_) {
        case Mode::prefetch_copy:
            (void)prefetch_mapped(src, row_);
            [[fallthrough]];
        case Mode::copy: std::memcpy(m.slot, src, row_); return m.slot;
        case Mode::nt_copy: nt_copy(m.slot, src, row_); return m.slot;
        case Mode::touch: {
            volatile std::uint8_t sink = 0;
            for (std::size_t at = 0; at < row_; at += kAlign) sink = sink + static_cast<std::uint8_t>(src[at]);
            return src;
        }
        }
        return nullptr;
    }
    void close() override {
        unmap(map_);
        close_file(file_);
    }

private:
    static void nt_copy(std::byte* dst, const std::byte* src, std::size_t n) {
#ifdef SHOOTOUT_HAS_NT_COPY
        std::size_t i = 0; // dst (a slot) is page-aligned; the source row need not be
        for (; i + 64 <= n; i += 64) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 16));
            const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 32));
            const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 48));
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i), a);
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 16), b);
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 32), c);
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst + i + 48), d);
        }
        std::memcpy(dst + i, src + i, n - i);
        _mm_sfence();
#else
        std::memcpy(dst, src, n);
#endif
    }
    Mode mode_;
    File file_;
    Mapping map_;
};

#ifdef SHOOTOUT_HAS_IORING
/// Windows 11 IoRing: one submission per miss carrying every chunk, one wait. Resolved at run time from
/// kernelbase.dll, so the tool still runs (and reports this arm unavailable) on older Windows.
class IoRingRead final : public Strategy {
public:
    explicit IoRingRead(bool unbuffered) : unbuffered_(unbuffered) {}
    std::string_view name() const override { return unbuffered_ ? "ioring-unbuffered" : "ioring"; }
    std::string_view what() const override {
        return unbuffered_ ? "Windows IoRing, all chunks in one submission, non-cached"
                           : "Windows IoRing, all chunks in one submission, buffered";
    }
    bool open(const Options& o) override {
        row_ = o.row_bytes;
        chunk_ = std::max(kAlign, o.chunk_bytes / kAlign * kAlign);
        const HMODULE kb = ::GetModuleHandleW(L"kernelbase.dll");
        create_ = kb ? reinterpret_cast<decltype(&::CreateIoRing)>(::GetProcAddress(kb, "CreateIoRing")) : nullptr;
        build_ = kb ? reinterpret_cast<decltype(&::BuildIoRingReadFile)>(::GetProcAddress(kb, "BuildIoRingReadFile")) : nullptr;
        submit_ = kb ? reinterpret_cast<decltype(&::SubmitIoRing)>(::GetProcAddress(kb, "SubmitIoRing")) : nullptr;
        pop_ = kb ? reinterpret_cast<decltype(&::PopIoRingCompletion)>(::GetProcAddress(kb, "PopIoRingCompletion")) : nullptr;
        close_ = kb ? reinterpret_cast<decltype(&::CloseIoRing)>(::GetProcAddress(kb, "CloseIoRing")) : nullptr;
        if (!create_ || !build_ || !submit_ || !pop_ || !close_) {
            unavailable_ = "IoRing not exported by this Windows";
            return false;
        }
        file_ = open_file(o.file, unbuffered_);
        if (!file_.ok()) {
            unavailable_ = "open failed";
            return false;
        }
        const IORING_CREATE_FLAGS flags{IORING_CREATE_REQUIRED_FLAGS_NONE, IORING_CREATE_ADVISORY_FLAGS_NONE};
        const auto entries = static_cast<UINT32>(round_up(row_ / chunk_ + 2, 8));
        if (FAILED(create_(IORING_VERSION_1, flags, entries, entries * 2, &ring_))) {
            unavailable_ = "CreateIoRing failed";
            close_file(file_);
            return false;
        }
        return true;
    }
    const std::byte* read(const Miss& m) override {
        const Window w = unbuffered_ ? aligned_window(m.offset, row_) : Window{m.offset, row_, 0};
        UINT32 count = 0;
        for (std::size_t at = 0; at < w.length; at += chunk_, ++count) {
            const auto len = static_cast<UINT32>(std::min(chunk_, w.length - at));
            if (FAILED(build_(ring_, IoRingHandleRefFromHandle(file_.handle), IoRingBufferRefFromPointer(m.slot + at),
                              len, w.offset + at, count, IOSQE_FLAGS_NONE)))
                return nullptr;
        }
        UINT32 submitted = 0;
        if (FAILED(submit_(ring_, count, INFINITE, &submitted))) return nullptr;
        bool ok = true;
        std::size_t bytes = 0;
        for (UINT32 got = 0; got < count;) {
            IORING_CQE cqe{};
            if (pop_(ring_, &cqe) != S_OK) continue; // all were waited for, so this does not spin long
            ++got;
            ok = ok && SUCCEEDED(cqe.ResultCode);
            bytes += cqe.Information;
        }
        return ok && bytes >= w.head + row_ ? m.slot + w.head : nullptr;
    }
    void close() override {
        if (ring_) close_(ring_);
        ring_ = nullptr;
        close_file(file_);
    }

private:
    bool unbuffered_;
    std::size_t chunk_ = 0;
    File file_;
    HIORING ring_ = nullptr;
    decltype(&::CreateIoRing) create_ = nullptr;
    decltype(&::BuildIoRingReadFile) build_ = nullptr;
    decltype(&::SubmitIoRing) submit_ = nullptr;
    decltype(&::PopIoRingCompletion) pop_ = nullptr;
    decltype(&::CloseIoRing) close_ = nullptr;
};
#endif

// --- driver ----------------------------------------------------------------------------------------------

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "read-shootout: %s\n", message.c_str());
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) fail("missing value for " + std::string(a));
            return argv[++i];
        };
        if (a == "--file") o.file = next();
        else if (a == "--file-mib") o.file_mib = std::stoull(next());
        else if (a == "--row-bytes") o.row_bytes = std::stoull(next());
        else if (a == "--chunk-kib") o.chunk_bytes = std::stoull(next()) * 1024;
        else if (a == "--readers") o.readers = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--misses") o.misses = std::stoi(next());
        else if (a == "--rounds") o.rounds = std::stoi(next());
        else if (a == "--gap") {
            const std::string g = next();
            o.gap = g == "none" ? Options::Gap::none : g == "sleep" ? Options::Gap::sleep : Options::Gap::busy;
        } else if (a == "--gap-us") o.gap_us = std::stoi(next());
        else if (a == "--cold") o.cold = true;
        else if (a == "--load-threads") o.load_threads = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--verify-every") o.verify_every = std::max(1, std::stoi(next()));
        else if (a == "--only") o.only = next();
        else if (a == "--list") o.list = true;
        else if (a == "--no-pin") o.pin = false;
        else if (a == "--direct-session") o.direct_session = true;
        else fail("unknown argument " + std::string(a) + " (see tools/read_shootout/README.md)");
    }
    if (o.row_bytes == 0 || o.readers == 0 || o.misses <= 0 || o.rounds <= 0) fail("row, readers, misses, rounds must be > 0");
    return o;
}

/// Creates (or keeps) the generated test file: deterministic pseudo-random bytes, so checksums mean something.
void ensure_file(const Options& o) {
    const std::uint64_t want = o.file_mib << 20;
    std::error_code ec;
    if (std::filesystem::exists(o.file, ec) && std::filesystem::file_size(o.file, ec) >= want) return;
    std::FILE* f = std::fopen(o.file.string().c_str(), "wb");
    if (!f) fail("cannot create " + o.file.string());
    std::vector<std::uint64_t> block((1u << 20) / sizeof(std::uint64_t));
    std::uint64_t x = 0x9E3779B97F4A7C15ull;
    for (std::uint64_t written = 0; written < want; written += 1u << 20) {
        for (auto& v : block) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; v = x; }
        if (std::fwrite(block.data(), 1, 1u << 20, f) != 1u << 20) fail("short write to " + o.file.string());
    }
    std::fclose(f);
}

void gap(const Options& o) {
    if (o.gap == Options::Gap::busy) {
        const auto until = Clock::now() + std::chrono::microseconds(o.gap_us);
        while (Clock::now() < until) {}
    } else if (o.gap == Options::Gap::sleep) {
        std::this_thread::sleep_for(std::chrono::microseconds(o.gap_us));
    }
}

double pct(const std::vector<double>& sorted, double q) {
    return sorted.empty() ? 0 : sorted[static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1) + 0.5)];
}

struct Result {
    double p50 = 0, p90 = 0, p99 = 0, mean = 0;
};

} // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    ensure_file(o);
    File probe = open_file(o.file, false);
    const std::uint64_t file_bytes = probe.size;
    close_file(probe);
    if (file_bytes < o.row_bytes + 2 * kAlign) fail("file smaller than one row");
    const auto rows = static_cast<int>((file_bytes - 2 * kAlign) / o.row_bytes); // aligned windows stay in the file
    const int misses = std::min(o.misses, rows); // each miss reads a distinct row, so cold stays cold

    constexpr int kSlots = 8;
    const std::size_t stride = round_up(o.row_bytes + 2 * kAlign, kAlign);
    std::byte* slots = alloc_pages(stride * kSlots);
    if (!slots) fail("slot allocation failed");
    const bool pinned = o.pin && pin(slots, stride * kSlots);

    std::vector<std::unique_ptr<Strategy>> all;
    all.push_back(std::make_unique<NaiveFread>());
    all.push_back(std::make_unique<InlineRead>(false));
    all.push_back(std::make_unique<InlineRead>(true));
    all.push_back(std::make_unique<PoolRead>(false));
    all.push_back(std::make_unique<PoolRead>(true));
#ifdef _WIN32
    all.push_back(std::make_unique<IocpRead>());
#endif
    all.push_back(std::make_unique<MemPageRead>(std::span(slots, stride * kSlots)));
#ifdef SHOOTOUT_HAS_IORING
    all.push_back(std::make_unique<IoRingRead>(false));
    all.push_back(std::make_unique<IoRingRead>(true));
#endif
    all.push_back(std::make_unique<MappedRead>(MappedRead::Mode::copy));
    all.push_back(std::make_unique<MappedRead>(MappedRead::Mode::prefetch_copy));
    all.push_back(std::make_unique<MappedRead>(MappedRead::Mode::nt_copy));
    all.push_back(std::make_unique<MappedRead>(MappedRead::Mode::touch));
    if (o.list) {
        for (const auto& s : all) std::printf("%-18s %s\n", std::string(s->name()).c_str(), std::string(s->what()).c_str());
        return 0;
    }
    std::vector<Strategy*> chosen;
    for (const auto& s : all)
        if (o.only.empty() || ("," + o.only + ",").find("," + std::string(s->name()) + ",") != std::string::npos)
            chosen.push_back(s.get());
    if (chosen.empty()) fail("--only matched no strategy (try --list)");
    if (o.direct_session) {
        if (!o.cold) fail("--direct-session requires --cold");
        for (const auto* s : chosen)
            if (s->name().find("unbuffered") == std::string_view::npos)
                fail("--direct-session requires --only containing exclusively unbuffered arms");
    }

    std::printf("read-shootout: %s (%.0f MiB), row %zu B, %d misses x %d rounds, %u readers, chunk %zu KiB, "
                "gap %s %d us, cache %s, DRAM load %u threads, slots %s\n",
                o.file.string().c_str(), static_cast<double>(file_bytes) / (1 << 20), o.row_bytes, misses, o.rounds,
                o.readers, o.chunk_bytes / 1024,
                o.gap == Options::Gap::none ? "none" : o.gap == Options::Gap::busy ? "busy" : "sleep", o.gap_us,
                o.direct_session ? "cold (direct-only session; one setup probe)" : o.cold ? "cold (evicted per run)" : "warm",
                o.load_threads, pinned ? "pinned" : "NOT pinned");

    std::vector<std::vector<Result>> results(chosen.size());
    std::vector<std::string> skipped(chosen.size());
    std::vector<std::byte> reference(o.row_bytes);
    std::byte* direct_reference = o.direct_session ? alloc_pages(stride) : nullptr;
    if (o.direct_session && !direct_reference) fail("direct reference allocation failed");
    double direct_cache_us = 0;
    if (o.direct_session) {
        (void)probe_cache_us(o.file); // prime these exact canary pages before attempting eviction
        if (!evict(o.file)) fail("cannot evict the file on this platform");
        direct_cache_us = probe_cache_us(o.file);
        if (direct_cache_us < 25) fail("eviction did not take before direct session");
#ifdef _WIN32
        // Diagnostic setup only: allow the buffered canary's deferred cache-map cleanup to finish.
        // A delay is not a production synchronization contract; the session must stay direct-only.
        std::this_thread::sleep_for(std::chrono::seconds(2));
#endif
    }
    auto load = o.load_threads ? std::make_unique<DramLoad>(o.load_threads) : nullptr;
    for (int round = 0; round < o.rounds; ++round) {
        std::vector<int> order(rows);
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), std::mt19937_64(static_cast<std::uint64_t>(round) + 1));
        for (std::size_t k = 0; k < chosen.size(); ++k) {
            const std::size_t si = (k + static_cast<std::size_t>(round)) % chosen.size(); // rotate the order
            Strategy& s = *chosen[si];
            if (!skipped[si].empty()) continue;
            double cache_us = 0;
            if (o.direct_session) {
                cache_us = direct_cache_us;
            } else if (o.cold) {
                (void)probe_cache_us(o.file); // prime the same deterministic sample: random cold pages cannot prove eviction
                if (!evict(o.file)) fail("cannot evict the file on this platform; run warm");
                cache_us = probe_cache_us(o.file);
                if (cache_us < 25) fail("eviction did not take (4 KiB probe median " + std::to_string(cache_us) +
                                        " us): something still maps or caches the file");
            } else {
                File warm = open_file(o.file, false); // read every row once so all come from the page cache
                for (int r = 0; r < misses; ++r)
                    (void)read_at(warm, reference.data(), o.row_bytes, static_cast<std::uint64_t>(order[r]) * o.row_bytes);
                close_file(warm);
            }
            if (!s.open(o)) {
                skipped[si] = s.unavailable();
                continue;
            }
            std::vector<double> t;
            std::vector<std::pair<int, std::uint64_t>> sampled;
            t.reserve(misses);
            sampled.reserve(static_cast<std::size_t>(misses / o.verify_every + 1));
            const auto loop_start = Clock::now();
            for (int i = 0; i < misses; ++i) {
                const Miss m{static_cast<std::uint64_t>(order[i]) * o.row_bytes, slots + stride * static_cast<std::size_t>(i % kSlots)};
                gap(o);
                const auto t0 = Clock::now();
                const std::byte* got = s.read(m);
                t.push_back(us_since(t0));
                if (!got) fail(std::string(s.name()) + ": read failed at miss " + std::to_string(i));
                if (i % o.verify_every == 0) sampled.emplace_back(order[i], fnv1a(got, o.row_bytes));
            }
            const double loop_us = us_since(loop_start);
            s.close();
            File ref = open_file(o.file, o.direct_session); // independent read, outside the timing
            for (const auto& [row, sum] : sampled) {
                const auto offset = static_cast<std::uint64_t>(row) * o.row_bytes;
                const Window window = aligned_window(offset, o.row_bytes);
                const auto* checked = o.direct_session ? direct_reference + window.head : reference.data();
                const bool read_ok = o.direct_session
                    ? read_at(ref, direct_reference, window.length, window.offset) == window.length
                    : read_at(ref, reference.data(), o.row_bytes, offset) == o.row_bytes;
                if (!read_ok || fnv1a(checked, o.row_bytes) != sum)
                    fail(std::string(s.name()) + ": WRONG BYTES for row " + std::to_string(row));
            }
            close_file(ref);
            std::sort(t.begin(), t.end());
            const Result r{pct(t, 0.5), pct(t, 0.9), pct(t, 0.99), std::accumulate(t.begin(), t.end(), 0.0) / t.size()};
            results[si].push_back(r);
            std::printf("JSON {\"round\":%d,\"strategy\":\"%s\",\"cold\":%s,\"cache_probe_us\":%.1f,\"p50_us\":%.1f,"
                        "\"p90_us\":%.1f,\"p99_us\":%.1f,\"mean_us\":%.1f,\"verified\":%zu,"
                        "\"direct_session\":%s,\"fill_gbps\":%.3f,\"loop_gbps\":%.3f}\n",
                        round + 1, std::string(s.name()).c_str(), o.cold ? "true" : "false", cache_us, r.p50, r.p90, r.p99,
                        r.mean, sampled.size(), o.direct_session ? "true" : "false",
                        static_cast<double>(o.row_bytes) / r.mean / 1e3,
                        static_cast<double>(o.row_bytes) * misses / loop_us / 1e3);
        }
    }
    const double load_gbps = load ? load->stop() : 0;

    // Summary: median over rounds, fastest mean first, relative to the naive baseline.
    struct Row {
        std::string name, what;
        Result r;
    };
    std::vector<Row> table;
    double naive = 0;
    for (std::size_t si = 0; si < chosen.size(); ++si) {
        if (results[si].empty()) continue;
        auto median = [&](double Result::*f) {
            std::vector<double> v;
            for (const auto& r : results[si]) v.push_back(r.*f);
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        table.push_back({std::string(chosen[si]->name()), std::string(chosen[si]->what()),
                         {median(&Result::p50), median(&Result::p90), median(&Result::p99), median(&Result::mean)}});
        if (table.back().name == "naive-fread") naive = table.back().r.mean;
    }
    std::sort(table.begin(), table.end(), [](const Row& a, const Row& b) { return a.r.mean < b.r.mean; });
    std::printf("\n%-18s %8s %8s %8s %8s %8s %8s\n", "strategy", "p50 us", "p90 us", "p99 us", "mean us", "GB/s", "vs naive");
    for (const Row& row : table)
        std::printf("%-18s %8.0f %8.0f %8.0f %8.0f %8.2f %7.2fx\n", row.name.c_str(), row.r.p50, row.r.p90, row.r.p99,
                    row.r.mean, static_cast<double>(o.row_bytes) / row.r.mean / 1e3, naive > 0 ? naive / row.r.mean : 0.0);
    for (std::size_t si = 0; si < chosen.size(); ++si)
        if (!skipped[si].empty()) std::printf("%-18s unavailable: %s\n", std::string(chosen[si]->name()).c_str(), skipped[si].c_str());
    if (load_gbps > 0) std::printf("DRAM load sustained %.1f GB/s\n", load_gbps);
    free_pages(slots, stride * kSlots);
    if (direct_reference) free_pages(direct_reference, stride);
    return 0;
}
