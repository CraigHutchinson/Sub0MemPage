// bw_hog: saturate DRAM read bandwidth like decode's weight streaming. usage: bw_hog <threads> <seconds>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <immintrin.h>
#include <thread>
#include <vector>
int main(int argc, char** argv) {
    const int threads = std::atoi(argv[1]); const int seconds = std::atoi(argv[2]);
    std::atomic<bool> stop{false}; std::atomic<unsigned long long> bytes{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back([&] {
        std::vector<__m256i> buf((512u << 20) / sizeof(__m256i), _mm256_set1_epi32(1)); // 512 MiB each: beyond L3
        __m256i acc = _mm256_setzero_si256();
        while (!stop) {
            for (const auto& v : buf) acc = _mm256_add_epi32(acc, v);
            bytes += buf.size() * sizeof(__m256i);
        }
        volatile int sink = _mm256_extract_epi32(acc, 0); (void)sink;
    });
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop = true; for (auto& th : pool) th.join();
    std::printf("hog: %.1f GB/s\n", bytes / 1e9 / seconds);
}
