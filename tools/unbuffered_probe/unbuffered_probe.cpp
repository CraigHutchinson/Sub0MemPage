// unbuffered_probe: buffered vs FILE_FLAG_NO_BUFFERING random reads in LocalFileBackend's threading model
// (overlapped handle; each thread does a synchronous positional ReadFile + per-thread event).
// See docs/investigations/unbuffered-read-ceiling.md. Windows only; a diagnostic, not part of the library.
// usage: unbuffered_probe <file> <buffered|unbuffered> <threads> <reads_per_thread> <block_bytes> <seed>
//                         <shared|perthread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 8) { std::fprintf(stderr, "usage: unbuffered_probe file mode threads reads block seed shared|perthread\n"); return 1; }
    const bool per_thread = std::string_view(argv[7]) == "perthread";
    const bool unbuffered = std::string_view(argv[2]) == "unbuffered";
    const int threads = std::atoi(argv[3]);
    const int reads = std::atoi(argv[4]);
    const std::uint64_t block = std::strtoull(argv[5], nullptr, 10);
    const unsigned seed = static_cast<unsigned>(std::atoi(argv[6]));

    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | (unbuffered ? FILE_FLAG_NO_BUFFERING : 0);
    const HANDLE h = ::CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "open failed %lu\n", ::GetLastError()); return 2; }

    FILE_STORAGE_INFO si{};
    if (::GetFileInformationByHandleEx(h, FileStorageInfo, &si, sizeof si))
        std::printf("sector: logical %lu, physical(atomicity) %lu, physical(perf) %lu, fs-effective %lu\n",
                    si.LogicalBytesPerSector, si.PhysicalBytesPerSectorForAtomicity,
                    si.PhysicalBytesPerSectorForPerformance, si.FileSystemEffectivePhysicalBytesPerSectorForAtomicity);
    BY_HANDLE_FILE_INFORMATION bi{};
    ::GetFileInformationByHandle(h, &bi);
    std::printf("attributes: 0x%lx compressed=%d sparse=%d encrypted=%d\n", bi.dwFileAttributes,
                !!(bi.dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED), !!(bi.dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE),
                !!(bi.dwFileAttributes & FILE_ATTRIBUTE_ENCRYPTED));
    LARGE_INTEGER size{};
    ::GetFileSizeEx(h, &size);
    const std::uint64_t pages = (static_cast<std::uint64_t>(size.QuadPart) - block) / 4096;

    std::atomic<std::uint64_t> bytes{0}, errors{0};
    std::vector<std::thread> pool;
    const auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            const HANDLE fh = per_thread ? ::CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr) : h;
            void* buf = ::VirtualAlloc(nullptr, block, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            const HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::mt19937_64 rng(seed * 1000003u + static_cast<unsigned>(t));
            std::uniform_int_distribution<std::uint64_t> pick(0, pages);
            for (int i = 0; i < reads; ++i) {
                const std::uint64_t off = pick(rng) * 4096;
                OVERLAPPED ov{};
                ov.hEvent = ev;
                ov.Offset = static_cast<DWORD>(off);
                ov.OffsetHigh = static_cast<DWORD>(off >> 32);
                DWORD got = 0;
                if (!::ReadFile(fh, buf, static_cast<DWORD>(block), nullptr, &ov) && ::GetLastError() != ERROR_IO_PENDING) {
                    ++errors; continue;
                }
                if (!::GetOverlappedResult(fh, &ov, &got, TRUE)) { ++errors; continue; }
                bytes += got;
            }
            ::CloseHandle(ev);
            if (per_thread) ::CloseHandle(fh);
            ::VirtualFree(buf, 0, MEM_RELEASE);
        });
    }
    for (auto& th : pool) th.join();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("RESULT handles=%s block=%llu mode=%s threads=%d bytes=%llu errors=%llu seconds=%.3f GBps=%.3f\n", argv[7], static_cast<unsigned long long>(block), argv[2], threads,
                static_cast<unsigned long long>(bytes.load()), static_cast<unsigned long long>(errors.load()), s,
                bytes.load() / s / 1e9);
    ::CloseHandle(h);
}
