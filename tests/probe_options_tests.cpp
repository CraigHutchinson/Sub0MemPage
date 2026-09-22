#include "ProbeOptions.hpp"
#include <sub0mempage/sub0mempage.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <span>

namespace {

using sub0mempage::intel_probe::ProbeOptions;

[[nodiscard]] bool accepts_id(const char* argument, std::uint16_t expected) {
    const std::array arguments{"--device-id", argument};
    const auto options = ProbeOptions::parse(arguments);
    return options && options->mode == ProbeOptions::Mode::inventory && options->device_id == expected;
}

} // namespace

int main() {
    unsigned checks = 0;
    unsigned failures = 0;
    const auto check = [&](bool condition, const char* description) {
        ++checks;
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << description << '\n';
        }
    };
    check(accepts_id("0x7d67", 0x7d67), "measured device ID");
    check(accepts_id("0x7D67", 0x7d67), "uppercase hex digits");
    check(accepts_id("0x0", 0), "lower numeric bound");
    check(accepts_id("0xffff", 0xffff), "upper numeric bound");
    check(accepts_id("0x0001", 1), "leading zeroes");

    constexpr std::array invalid_ids{
        "", "0x", "7d67", "0X7d67", "0x10000", "0x00000", "0x-1", "0x+1", "-1",
        "0x7d67x", " 0x7d67", "0x7d67 ", "0x7d67\n", "0x7g67", "0x 1", "0x1.0"
    };
    for (const auto id : invalid_ids) {
        const std::array arguments{"--device-id", id};
        check(!ProbeOptions::parse(arguments), id);
    }
    check(!ProbeOptions::parse({}), "missing arguments never selects a default device");
    const std::array missing_id{"--device-id"};
    check(!ProbeOptions::parse(missing_id), "missing ID");
    const std::array unknown{"--device", "0x7d67"};
    check(!ProbeOptions::parse(unknown), "unknown switch");
    const std::array duplicate{"--device-id", "0x7d67", "--device-id", "0x7d67"};
    check(!ProbeOptions::parse(duplicate), "duplicate selector");
    const std::array extra{"--device-id", "0x7d67", "--run"};
    check(!ProbeOptions::parse(extra), "extra argument");
    const std::array help{"--help"};
    const auto options = ProbeOptions::parse(help);
    check(options && options->mode == ProbeOptions::Mode::help, "offline help");
    const std::array mixed_help{"--help", "--device-id", "0x7d67"};
    check(!ProbeOptions::parse(mixed_help), "help must stand alone");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
