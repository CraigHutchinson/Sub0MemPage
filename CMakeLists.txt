cmake_minimum_required(VERSION 3.21)

# Detect if Sub0MemPage is being included inside another project (e.g. vendored into Sub0Llm or
# Sub0TieredCache -- see README.md sec 2/5 for the intended integration seams).
if(DEFINED PROJECT_NAME)
    set(IS_SUBPROJECT ON)
endif()

project(Sub0MemPage VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(SUB0MEMPAGE_BUILD_TESTING  "Build unit-tests" ON)
option(SUB0MEMPAGE_BUILD_EXAMPLES "Build examples"   OFF)
option(SUB0MEMPAGE_BUILD_BENCHMARKS "Build bookkeeping microbenchmarks (scripts/dev.py bench)" OFF)
option(SUB0MEMPAGE_BUILD_INTEL_USM_PROBE "Build optional Intel SYCL capability inventory" OFF)

if(SUB0MEMPAGE_BUILD_TESTING AND NOT IS_SUBPROJECT)
    enable_testing()
endif()

# Header-only interface library, matching Sub0TieredCache's precedent one layer up. It ships the M2 state
# machines (slot_pool.hpp, transfer_set.hpp), which need no compiled component. It gains one only when a
# real async-I/O backend needs background-thread infrastructure (docs/design.md sec 2's platform seam:
# Windows IOCP, Linux io_uring).
add_library(Sub0MemPage INTERFACE)
add_library(Sub0MemPage::Sub0MemPage ALIAS Sub0MemPage)

target_include_directories(Sub0MemPage INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# C++23, matching the Sub0 project family's own baseline (Sub0Llm, Sub0TieredCache, Sub0Log) -- see
# STYLE_GUIDE.md for why this is a hard requirement rather than a preference.
target_compile_features(Sub0MemPage INTERFACE cxx_std_23)

# The slot pool and transfer set synchronize with std::mutex/condition_variable.
find_package(Threads REQUIRED)
target_link_libraries(Sub0MemPage INTERFACE Threads::Threads)

# Test support for consumers (Sub0TieredCache's suites): the deterministic fake FillBackend. A
# transport test double only, never a correctness oracle for a consumer's own results.
add_library(Sub0MemPageTesting INTERFACE)
add_library(Sub0MemPage::testing ALIAS Sub0MemPageTesting)
target_include_directories(Sub0MemPageTesting INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/testing/include>)
target_link_libraries(Sub0MemPageTesting INTERFACE Sub0MemPage)

if(SUB0MEMPAGE_BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

if(SUB0MEMPAGE_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

if(SUB0MEMPAGE_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

if(SUB0MEMPAGE_BUILD_INTEL_USM_PROBE)
    add_subdirectory(tools/intel)
endif()
