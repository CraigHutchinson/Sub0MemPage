#pragma once

/** @file test_support.hpp
 *  @brief Minimal shared harness for the offline test executables: check/run bookkeeping and the global
 *         allocation counter behind the "zero hot-path allocations" gate (transfer-contract.md).
 *
 *  No framework on purpose: the core has no third-party dependencies (STYLE_GUIDE.md) and the tests
 *  follow the same rule. allocation_counter.cpp replaces global operator new in every test executable.
 */

#include <cstdint>
#include <iostream>

namespace sub0mempage::test {

/// Total global operator-new calls so far in this process (all threads).
[[nodiscard]] std::uint64_t allocation_count() noexcept;

inline unsigned g_checks = 0;
inline unsigned g_failures = 0;

inline void check(bool condition, const char* description) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

/// Runs one test function, naming it first so a crash or hang points at the test.
inline void run(void (*test)(), const char* name) {
    std::cerr << "-- " << name << '\n';
    test();
}

/// Prints the tally and returns the process exit code.
[[nodiscard]] inline int finish() {
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}

} // namespace sub0mempage::test
