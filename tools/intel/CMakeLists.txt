# This diagnostic is independent of the portable interface target.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
    message(FATAL_ERROR "Intel USM inventory requires a SYCL-capable IntelLLVM compiler; keep SUB0MEMPAGE_BUILD_INTEL_USM_PROBE=OFF for the portable build")
endif()

add_executable(sub0mempage-usm-capabilities usm_capabilities.cpp)
target_compile_features(sub0mempage-usm-capabilities PRIVATE cxx_std_23)
target_compile_options(sub0mempage-usm-capabilities PRIVATE -fsycl)
target_link_options(sub0mempage-usm-capabilities PRIVATE -fsycl)
if(MSVC)
    target_compile_options(sub0mempage-usm-capabilities PRIVATE /W4 /EHsc)
else()
    target_compile_options(sub0mempage-usm-capabilities PRIVATE -Wall -Wextra -Wpedantic)
endif()
