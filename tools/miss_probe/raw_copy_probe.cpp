// raw_copy_probe: best-case parallel buffered-read copy of one 1.7 MB expert from a warm page cache.
// Persistent threads spin on a generation counter (no wake-up cost), each reads its chunk; the caller
// spins until all are done. Compares one shared handle against one handle per thread.
// usage: raw_copy_probe <file> <threads> <misses> <chunk_bytes>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kExpert = 1'766'400, kRegion = 2ull << 30;

static HANDLE open_file(const char* p) {
    return ::CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
}
static bool pread(HANDLE h, HANDLE ev, void* dst, std::uint64_t off, DWORD len) {
    OVERLAPPED ov{}; ov.Offset = static_cast<DWORD>(off); ov.OffsetHigh = static_cast<DWORD>(off >> 32); ov.hEvent = ev;
    DWORD got = 0;
    if (!::ReadFile(h, dst, len, nullptr, &ov) && ::GetLastError() != ERROR_IO_PENDING) return false;
    return ::GetOverlappedResult(h, &ov, &got, TRUE) && got == len;
}

int main(int argc, char** argv) {
    if (argc != 5) return 1;
    const int threads = std::atoi(argv[2]), misses = std::atoi(argv[3]);
    const std::uint64_t chunk = std::strtoull(argv[4], nullptr, 10);
    const int chunks = static_cast<int>((kExpert + chunk - 1) / chunk);
    std::vector<std::byte> dest(kExpert);
    std::mt19937_64 rng(7);
    std::vector<std::uint64_t> offsets(misses);
    for (auto& o : offsets) o = (rng() % (kRegion / kExpert)) * kExpert;
    {   // warm
        const HANDLE h = open_file(argv[1]); const HANDLE ev = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
        for (std::uint64_t at = 0; at < kRegion; at += kExpert) pread(h, ev, dest.data(), at, static_cast<DWORD>(kExpert));
        ::CloseHandle(ev); ::CloseHandle(h);
    }
    for (int per_thread = 0; per_thread < 2; ++per_thread) {
        const HANDLE shared = open_file(argv[1]);
        std::atomic<int> gen{0}, done{0}, next{0};
        std::atomic<bool> stop{false};
        std::uint64_t cur = 0;
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                const HANDLE h = per_thread ? open_file(argv[1]) : shared;
                const HANDLE ev = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
                int seen = 0;
                while (true) {
                    while (gen.load(std::memory_order_acquire) == seen && !stop.load(std::memory_order_relaxed)) _mm_pause();
                    if (stop.load()) break;
                    seen = gen.load(std::memory_order_acquire);
                    for (int c; (c = next.fetch_add(1)) < chunks;) {
                        const std::uint64_t at = c * chunk, len = std::min(chunk, kExpert - at);
                        if (!pread(h, ev, dest.data() + at, cur + at, static_cast<DWORD>(len))) std::abort();
                        done.fetch_add(1, std::memory_order_release);
                    }
                }
                ::CloseHandle(ev);
                if (per_thread) ::CloseHandle(h);
            });
        std::vector<double> t_us;
        for (int i = 0; i < misses; ++i) {
            cur = offsets[i];
            done.store(0); next.store(0);
            const auto t0 = Clock::now();
            gen.fetch_add(1, std::memory_order_release);
            while (done.load(std::memory_order_acquire) < chunks) _mm_pause();
            t_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
        }
        stop = true;
        for (auto& th : pool) th.join();
        ::CloseHandle(shared);
        std::sort(t_us.begin(), t_us.end());
        auto p = [&](double q) { return t_us[static_cast<std::size_t>(q * (t_us.size() - 1))]; };
        std::printf("%-10s threads %2d chunk %7llu: p10 %5.0f  p50 %5.0f  p90 %5.0f us  (%.1f GB/s at p50)\n",
                    per_thread ? "per-thread" : "shared", threads, static_cast<unsigned long long>(chunk), p(0.1), p(0.5), p(0.9),
                    kExpert / p(0.5) / 1e3);
    }
}
