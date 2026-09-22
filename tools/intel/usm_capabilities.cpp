#include "ProbeOptions.hpp"

#include <array>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

#include <sycl/sycl.hpp>

namespace {

/** Refuses missing or ambiguous Intel Level Zero matches instead of selecting another adapter.
 *  The device ID is a model ID, not a unique adapter identity; two matches require a future
 *  explicit UUID/BDF selector (docs/implementation-plan.md M1).
 */
[[nodiscard]] sycl::device select_device(std::uint16_t requested_id) {
    std::optional<sycl::device> match;
    for (const auto& platform : sycl::platform::get_platforms()) {
        if (platform.get_backend() != sycl::backend::ext_oneapi_level_zero) continue;
        for (const auto& device : platform.get_devices(sycl::info::device_type::gpu)) {
            if (device.get_info<sycl::info::device::vendor_id>() != 0x8086
                || !device.has(sycl::aspect::ext_intel_device_id)
                || device.get_info<sycl::ext::intel::info::device::device_id>() != requested_id)
                continue;
            if (match) throw std::runtime_error("ambiguous Intel Level Zero device ID; no fallback");
            match = device;
        }
    }
    if (!match) throw std::runtime_error("requested Intel Level Zero GPU not found; no fallback");
    return *match;
}

void print_report(const sycl::device& device) {
    constexpr std::array aspects{
        std::pair{"usm_host", sycl::aspect::usm_host_allocations},
        std::pair{"usm_shared", sycl::aspect::usm_shared_allocations},
        std::pair{"usm_device", sycl::aspect::usm_device_allocations},
        std::pair{"usm_system", sycl::aspect::usm_system_allocations},
        std::pair{"usm_atomic_host", sycl::aspect::usm_atomic_host_allocations},
        std::pair{"usm_atomic_shared", sycl::aspect::usm_atomic_shared_allocations}
    };
    std::cout << "schema=sub0mempage.intel-usm.v1\n"
              << "backend=level_zero\n"
              << "device=" << std::quoted(device.get_info<sycl::info::device::name>()) << '\n'
              << "platform=" << std::quoted(device.get_platform().get_info<sycl::info::platform::name>()) << '\n'
              << "vendor_id=0x" << std::hex << device.get_info<sycl::info::device::vendor_id>() << '\n'
              << "device_id=0x" << device.get_info<sycl::ext::intel::info::device::device_id>()
              << std::dec << '\n'
              << "sycl_driver=" << std::quoted(device.get_info<sycl::info::device::driver_version>()) << '\n'
              << "sycl_version=" << std::quoted(device.get_info<sycl::info::device::version>()) << '\n';
    for (const auto& [name, aspect] : aspects)
        std::cout << name << '=' << static_cast<int>(device.has(aspect)) << '\n';
#ifdef SYCL_EXT_ONEAPI_COPY_OPTIMIZE
    std::cout << "prepared_copy_header=" << SYCL_EXT_ONEAPI_COPY_OPTIMIZE << '\n';
#else
    std::cout << "prepared_copy_header=not_declared\n";
#endif
    std::cout << "prepared_copy_runtime=not_tested\n"
              << "allocation_runtime=not_tested\n"
              << "level_zero_extensions=not_built\n"
              << "direct_mapped_access=not_tested\n"
              << "concurrent_cpu_gpu_access=not_tested\n"
              << "physical_residency=not_tested\n"
              << "performance=not_measured\n"
              << "status=pass\n";
}

} // namespace

int main(int argc, char** argv) {
    using sub0mempage::intel_probe::ProbeOptions;
    const auto options = ProbeOptions::parse({argv + 1, static_cast<std::size_t>(argc - 1)});
    if (!options || options->mode == ProbeOptions::Mode::help) {
        auto& output = options ? std::cout : std::cerr;
        output << "Usage: sub0mempage-usm-capabilities --device-id 0xNNNN\n"
               << "       sub0mempage-usm-capabilities --help\n"
               << "Queries one Intel Level Zero GPU; no allocations, kernels or timing.\n";
        return options ? 0 : 2;
    }
    try {
        print_report(select_device(options->device_id));
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "status=fail\nerror=" << std::quoted(error.what()) << '\n';
        return 1;
    }
}
