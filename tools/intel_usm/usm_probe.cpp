// Optional qualification tool; evidence boundary and provenance: docs/intel-usm.md.
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include <sycl/sycl.hpp>

namespace {

/// Diagnostic arguments are parsed before touching the GPU runtime.
struct Options {
    std::uint32_t device_id = 0x7d67;
    bool copy_check = false;
    bool help = false;
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") options.help = true;
        else if (arg == "--copy-check") options.copy_check = true;
        else if (arg.starts_with("--device-id=")) {
            auto value = arg.substr(std::string_view("--device-id=").size());
            if (value.starts_with("0x")) value.remove_prefix(2);
            const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                                options.device_id, 16);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
                throw std::invalid_argument("device ID must be a uint32 hexadecimal value");
        } else throw std::invalid_argument("unknown option");
    }
    return options;
}

sycl::device select_device(std::uint32_t id) {
    for (const auto& platform : sycl::platform::get_platforms()) {
        if (platform.get_backend() != sycl::backend::ext_oneapi_level_zero) continue;
        for (const auto& device : platform.get_devices(sycl::info::device_type::gpu)) {
            if (device.get_info<sycl::info::device::vendor_id>() == 0x8086
                && device.has(sycl::aspect::ext_intel_device_id)
                && device.get_info<sycl::ext::intel::info::device::device_id>() == id)
                return device;
        }
    }
    throw std::runtime_error("requested Intel Level Zero GPU missing; no fallback");
}

void print_capabilities(const sycl::device& device) {
    std::cout << "schema=sub0mempage.intel-usm.v1\n"
              << "device=" << device.get_info<sycl::info::device::name>() << '\n'
              << "backend=level_zero\nvendor_id=0x8086\ndevice_id=0x" << std::hex
              << device.get_info<sycl::ext::intel::info::device::device_id>() << std::dec << '\n'
              << "driver=" << device.get_info<sycl::info::device::driver_version>() << '\n'
              << "compiler=" << __VERSION__ << '\n';
    constexpr std::array aspects{
        sycl::aspect::usm_host_allocations, sycl::aspect::usm_shared_allocations,
        sycl::aspect::usm_device_allocations, sycl::aspect::usm_system_allocations,
        sycl::aspect::usm_atomic_host_allocations, sycl::aspect::usm_atomic_shared_allocations};
    constexpr std::array names{"usm_host", "usm_shared", "usm_device", "usm_system",
                               "usm_atomic_host", "usm_atomic_shared"};
    for (std::size_t i = 0; i < aspects.size(); ++i)
        std::cout << names[i] << '=' << device.has(aspects[i]) << '\n';
#ifdef SYCL_EXT_ONEAPI_COPY_OPTIMIZE
    std::cout << "prepared_copy_header=" << SYCL_EXT_ONEAPI_COPY_OPTIMIZE << '\n';
#else
    std::cout << "prepared_copy_header=not_declared\n";
#endif
    std::cout << "prepared_copy_runtime=not_tested\n"
              << "level_zero_extension_inventory=not_built\n"
              << "mapped_pointer_kernel_access=not_tested\n";
}

/// Keeps even exception-path submissions alive until their borrowed allocations are safe to free.
struct UsmDeleter {
    sycl::queue* queue; // non-owning; queue outlives the unique_ptr
    // A drain/free failure cannot safely recover by releasing live storage; terminate the diagnostic.
    void operator()(std::uint32_t* pointer) const noexcept {
        try {
            queue->wait();
            sycl::free(pointer, *queue);
        } catch (...) { std::terminate(); }
    }
};

void check_staged_copy(const sycl::device& device) {
    if (!device.has(sycl::aspect::usm_host_allocations)
        || !device.has(sycl::aspect::usm_device_allocations))
        throw std::runtime_error("copy check requires host and device USM allocation aspects");
    sycl::queue queue(device, [](sycl::exception_list errors) {
        if (errors.size() != 0) std::rethrow_exception(*errors.begin());
    });
    using Allocation = std::unique_ptr<std::uint32_t, UsmDeleter>;
    constexpr std::array<std::size_t, 2> SIZES{4096, 4 * 1024 * 1024};
    for (const auto bytes : SIZES) {
        const auto count = bytes / sizeof(std::uint32_t);
        // Declared before USM so exception unwinding drains work before destroying the readback.
        std::vector<std::uint32_t> readback(count);
        Allocation host(sycl::malloc_host<std::uint32_t>(count, queue), UsmDeleter{&queue});
        if (!host) throw std::bad_alloc();
        Allocation gpu(sycl::malloc_device<std::uint32_t>(count, queue), UsmDeleter{&queue});
        if (!gpu) throw std::bad_alloc();
        for (std::uint32_t pass = 0; pass < 2; ++pass) {
            for (std::size_t i = 0; i < count; ++i)
                host.get()[i] = static_cast<std::uint32_t>(i) + pass * 17u;
            const auto uploaded = queue.memcpy(gpu.get(), host.get(), bytes);
            auto* data = gpu.get();
            const auto computed = queue.submit([&](sycl::handler& command) {
                command.depends_on(uploaded);
                command.parallel_for(sycl::range<1>(count), [=](sycl::id<1> id) {
                    data[id[0]] = data[id[0]] * 3u + 7u;
                });
            });
            auto downloaded = queue.submit([&](sycl::handler& command) {
                command.depends_on(computed);
                command.memcpy(readback.data(), gpu.get(), bytes);
            });
            downloaded.wait_and_throw();
            queue.wait_and_throw();
            std::size_t mismatches = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const auto expected = (static_cast<std::uint32_t>(i) + pass * 17u) * 3u + 7u;
                if (readback[i] != expected) ++mismatches;
            }
            std::cout << "copy_check_bytes=" << bytes << ",pass=" << pass
                      << ",checked=" << count << ",mismatches=" << mismatches << '\n';
            if (mismatches != 0) throw std::runtime_error("staged-copy reference mismatch");
        }
    }
    std::cout << "copy_check=pass\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    try { options = parse_options(argc, argv); }
    catch (const std::invalid_argument& error) {
        std::cerr << "argument_error=" << error.what() << '\n';
        return 2;
    }
    if (options.help) {
        std::cout << "Usage: sub0mempage-usm-probe [--device-id=HEX] [--copy-check]\n"
                  << "Default: inventory Intel 7d67 through Level Zero; no kernel submission.\n";
        return 0;
    }
    try {
        const auto device = select_device(options.device_id);
        print_capabilities(device);
        if (options.copy_check) check_staged_copy(device);
        else std::cout << "copy_check=not_requested\n";
        std::cout << "status=pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "status=fail\nerror=" << error.what() << '\n';
        return 1;
    }
}
