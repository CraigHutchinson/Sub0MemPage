#pragma once

/** @file ProbeOptions.hpp
 *  @brief Offline argument validation for the optional Intel capability diagnostic.
 */

#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace sub0mempage::intel_probe {

/// Startup options requiring an explicit PCI device ID; no implicit device fallback.
struct ProbeOptions {
    /// Help never enumerates a runtime device.
    enum class Mode { help, inventory };

    Mode mode;
    std::uint16_t device_id;

    /** Accepts exactly --help or --device-id 0xNNNN (one to four hex digits).
     *  Arguments exclude argv[0]. Returns nullopt for malformed, missing or extra arguments.
     *  Borrows the strings only for this call; allocates nothing and has no shared state.
     */
    [[nodiscard]] static std::optional<ProbeOptions> parse(
        std::span<const char* const> arguments) noexcept;
};

inline std::optional<ProbeOptions> ProbeOptions::parse(
    std::span<const char* const> arguments) noexcept {
    if (arguments.size() == 1 && std::string_view{arguments[0]} == "--help")
        return ProbeOptions{Mode::help, 0};
    if (arguments.size() != 2 || std::string_view{arguments[0]} != "--device-id")
        return std::nullopt;

    const std::string_view id{arguments[1]};
    if (!id.starts_with("0x") || id.size() < 3 || id.size() > 6)
        return std::nullopt;
    std::uint16_t value{};
    const auto end = id.data() + id.size();
    const auto result = std::from_chars(id.data() + 2, end, value, 16);
    if (result.ec != std::errc{} || result.ptr != end)
        return std::nullopt;
    return ProbeOptions{Mode::inventory, value};
}

} // namespace sub0mempage::intel_probe
