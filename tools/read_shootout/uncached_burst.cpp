// Minimal Windows repro: seven adjacent uncached reads, with/without a live cached reader.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr DWORD block = 256 * 1024;
constexpr std::size_t depth = 7;
constexpr DWORD row = block * static_cast<DWORD>(depth);

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

[[noreturn]] void fail(const char* operation) {
    std::fprintf(stderr, "%s: error %lu\n", operation, ::GetLastError());
    std::exit(2);
}

std::uint64_t checksum(const std::byte* data, std::size_t bytes) {
    std::uint64_t sum = 1469598103934665603ull;
    for (std::size_t i = 0; i < bytes; ++i)
        sum = (sum ^ static_cast<std::uint8_t>(data[i])) * 1099511628211ull;
    return sum;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: uncached-burst FILE event|iocp none|held|closed SETTLE_MS\n");
        return 2;
    }
    const std::string_view completion = argv[2], cache = argv[3];
    if ((completion != "event" && completion != "iocp") ||
        (cache != "none" && cache != "held" && cache != "closed")) return 2;
    HANDLE cached = INVALID_HANDLE_VALUE;
    if (cache != "none") {
        cached = ::CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (cached == INVALID_HANDLE_VALUE) fail("cached open");
        std::array<std::byte, 4096> page{};
        DWORD got = 0;
        if (!::ReadFile(cached, page.data(), static_cast<DWORD>(page.size()), &got, nullptr) || got != page.size())
            fail("cached read");
        if (cache == "closed") { ::CloseHandle(cached); cached = INVALID_HANDLE_VALUE; }
    }
    // Setup only. A closed cached reader has deferred cleanup; a held reader is the negative control.
    ::Sleep(static_cast<DWORD>(std::strtoul(argv[4], nullptr, 10)));
    const DWORD flags = FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING;
    const HANDLE file = ::CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) fail("open");
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < row * 2) fail("size");
    const HANDLE port = completion == "iocp" ? ::CreateIoCompletionPort(file, nullptr, 1, 1) : nullptr;
    if (completion == "iocp" && !port) fail("IOCP");
    auto* slot = static_cast<std::byte*>(::VirtualAlloc(nullptr, row, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!slot) fail("allocate");
    std::fill_n(slot, row, std::byte{0});
    std::array<OVERLAPPED, depth> requests{};
    std::array<HANDLE, depth> events{};
    if (!port) for (auto& event : events) {
        event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) fail("event");
    }
    // Unique shuffled rows keep the data cold. Every mode uses the same offsets.
    std::vector<std::uint64_t> offsets(static_cast<std::size_t>(size.QuadPart / row));
    for (std::size_t i = 0; i < offsets.size(); ++i) offsets[i] = i * row;
    std::shuffle(offsets.begin(), offsets.end(), std::mt19937_64(1));
    offsets.resize(std::min<std::size_t>(100, offsets.size()));
    std::vector<double> times;
    std::vector<std::uint64_t> sums;
    times.reserve(offsets.size());
    sums.reserve(offsets.size());
    for (std::size_t miss = 0; miss < offsets.size(); ++miss) {
        const auto offset = offsets[miss];
        const auto until = Clock::now() + std::chrono::microseconds(2800);
        while (Clock::now() < until) {}
        std::array<double, depth> issued{}, completed{};
        const auto start = Clock::now();
        for (std::size_t i = 0; i < depth; ++i) {
            requests[i] = {};
            requests[i].Offset = static_cast<DWORD>(offset + i * block);
            requests[i].OffsetHigh = static_cast<DWORD>((offset + i * block) >> 32);
            requests[i].hEvent = events[i];
            if (!::ReadFile(file, slot + i * block, block, nullptr, &requests[i]) && ::GetLastError() != ERROR_IO_PENDING)
                fail("read");
            issued[i] = elapsed(start);
        }
        for (std::size_t i = 0; i < depth; ++i) {
            DWORD got = 0;
            if (port) {
                ULONG_PTR key = 0;
                OVERLAPPED* request = nullptr;
                if (!::GetQueuedCompletionStatus(port, &got, &key, &request, INFINITE) || !request) fail("completion");
                completed[static_cast<std::size_t>(request - requests.data())] = elapsed(start);
            } else {
                if (!::GetOverlappedResult(file, &requests[i], &got, TRUE)) fail("wait");
                completed[i] = elapsed(start);
            }
            if (got != block) fail("short read");
        }
        times.push_back(elapsed(start));
        sums.push_back(checksum(slot, row));
        if (miss < 3) {
            std::printf("miss %zu issue_us:", miss);
            for (double t : issued) std::printf(" %.1f", t);
            std::printf(" observed_completion_us:");
            for (double t : completed) std::printf(" %.1f", t);
            std::printf("\n");
        }
    }
    ::CloseHandle(file);
    // Independent whole-row oracle also bypasses the cache: verification must not contaminate the next run.
    const HANDLE reference = ::CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, flags, nullptr);
    if (reference == INVALID_HANDLE_VALUE) fail("reference open");
    const HANDLE reference_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!reference_event) fail("reference event");
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        OVERLAPPED request{};
        request.Offset = static_cast<DWORD>(offsets[i]);
        request.OffsetHigh = static_cast<DWORD>(offsets[i] >> 32);
        request.hEvent = reference_event;
        if (!::ReadFile(reference, slot, row, nullptr, &request) && ::GetLastError() != ERROR_IO_PENDING)
            fail("reference read");
        DWORD got = 0;
        if (!::GetOverlappedResult(reference, &request, &got, TRUE) || got != row) fail("reference wait");
        if (checksum(slot, row) != sums[i]) fail("checksum");
    }
    std::sort(times.begin(), times.end());
    const auto percentile = [&](double q) { return times[static_cast<std::size_t>(q * (times.size() - 1) + 0.5)]; };
    std::printf("completion %s cached_reader %s settle_ms %s p50 %.1f us p90 %.1f us p99 %.1f us verified %zu\n",
                argv[2], argv[3], argv[4], percentile(0.5), percentile(0.9), percentile(0.99), sums.size());
    ::CloseHandle(reference_event);
    ::CloseHandle(reference);
    if (cached != INVALID_HANDLE_VALUE) ::CloseHandle(cached);
    if (port) ::CloseHandle(port);
    for (auto event : events) if (event) ::CloseHandle(event);
    ::VirtualFree(slot, 0, MEM_RELEASE);
}
#else
#include <cstdio>
int main() { std::puts("uncached-burst: Windows issue-path probe unavailable on this platform"); }
#endif
