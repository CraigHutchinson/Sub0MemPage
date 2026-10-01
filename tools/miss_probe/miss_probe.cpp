// miss_probe: where does an expert miss's time go? Warm page cache, one 1.7 MB "expert" per miss.
//   pool   : MemPage TransferSet + LocalFileBackend, the expert split into 256 KiB chunks, then wait all.
//   inline : one synchronous positional ReadFile of the whole expert on the calling thread.
// Each mode runs with a gap between misses: none, busy (2.8 ms spin), sleep (2.8 ms), like decode's pace.
// usage: miss_probe <file> <readers> <misses> [expert_bytes chunk_bytes rounds]
#include <sub0mempage/local_file_backend.hpp>
#include <sub0mempage/transfer_set.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static std::uint64_t kExpert = 1'766'400, kChunk = 256 * 1024;
constexpr std::uint64_t kRegion = 2ull << 30; // 2 GiB: stays warm

static double us(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

static void gap(int mode) {
    if (mode == 1) { const auto until = Clock::now() + std::chrono::microseconds(2800); while (Clock::now() < until) {} }
    if (mode == 2) std::this_thread::sleep_for(std::chrono::microseconds(2800));
}

static void report(const char* name, const char* gapname, std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    auto p = [&](double q) { return v[static_cast<std::size_t>(q * (v.size() - 1))]; };
    std::printf("%-6s gap %-5s: p10 %6.0f  p50 %6.0f  p90 %6.0f  p99 %6.0f us\n", name, gapname, p(0.1), p(0.5), p(0.9), p(0.99));
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 7) { std::fprintf(stderr, "usage: miss_probe file readers misses [expert chunk rounds]\n"); return 1; }
    int rounds = 2;
    if (argc == 7) { kExpert = std::strtoull(argv[4], nullptr, 10); kChunk = std::strtoull(argv[5], nullptr, 10); rounds = std::atoi(argv[6]); }
    const std::string path = argv[1];
    const auto readers = static_cast<std::uint32_t>(std::atoi(argv[2]));
    const int misses = std::atoi(argv[3]);
    const std::uint32_t chunks = static_cast<std::uint32_t>((kExpert + kChunk - 1) / kChunk);

    std::vector<std::byte> dest(kExpert);
    auto backend = sub0mempage::LocalFileBackend::create({.workers = readers, .queue_capacity = 64, .max_sources = 1});
    if (!backend) return 2;
    constexpr auto kSource = static_cast<sub0mempage::SourceId>(1);
    if ((*backend)->register_file(kSource, path) != sub0mempage::Status::ok) return 3;
    auto set = sub0mempage::TransferSet::create({.source = kSource, .source_bytes = kRegion, .destination = dest, .max_claims = 64},
                                                sub0mempage::FillBackendRef(**backend));
    if (!set) return 4;
    const HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 5;

    std::mt19937_64 rng(7);
    std::vector<std::uint64_t> offsets(misses);
    for (auto& o : offsets) o = (rng() % (kRegion / kExpert)) * kExpert;
    // Warm the region once so every mode reads from the page cache.
    std::vector<std::byte> warm(1'766'400);
    for (std::uint64_t at = 0; at < kRegion; at += warm.size()) {
        OVERLAPPED ov{}; ov.Offset = static_cast<DWORD>(at); ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0; ::ReadFile(h, warm.data(), static_cast<DWORD>(warm.size()), &got, &ov);
    }

    std::vector<sub0mempage::Claim> claims;
    claims.reserve(chunks);
    const char* gaps[] = {"none", "busy", "sleep"};
    for (int round = 0; round < rounds; ++round)
    for (int g = 0; g < 3; ++g) {
        std::vector<double> pool, inl;
        for (int i = 0; i < misses; ++i) {
            gap(g);
            const auto t0 = Clock::now();
            claims.clear();
            for (std::uint32_t c = 0; c < chunks; ++c) {
                const std::uint64_t at = c * kChunk, len = std::min(kChunk, kExpert - at);
                auto claim = (*set)->submit({offsets[i] + at, len}, at);
                if (!claim) { std::fprintf(stderr, "submit failed\n"); return 6; }
                claims.push_back(std::move(*claim));
            }
            for (auto& c : claims) if (c.wait() != sub0mempage::Status::ok) return 7;
            pool.push_back(us(Clock::now() - t0));
            claims.clear();
        }
        for (int i = 0; i < misses; ++i) {
            gap(g);
            const auto t0 = Clock::now();
            OVERLAPPED ov{}; ov.Offset = static_cast<DWORD>(offsets[i]); ov.OffsetHigh = static_cast<DWORD>(offsets[i] >> 32);
            DWORD got = 0;
            if (!::ReadFile(h, dest.data(), static_cast<DWORD>(kExpert), &got, &ov) || got != kExpert) return 8;
            inl.push_back(us(Clock::now() - t0));
        }
        report("pool", gaps[g], pool);
        report("inline", gaps[g], inl);
    }
    ::CloseHandle(h);
    (void)(*set)->drain();
    return 0;
}
