cmake_minimum_required(VERSION 3.21)

# Detect if Sub0MemPage is being included inside another project (e.g. vendored into Sub0Llm or
# Sub0Firn -- see README.md sec 2/5 for the intended integration seams).
if(DEFINED PROJECT_NAME)
    set(IS_SUBPROJECT ON)
endif()

project(Sub0MemPage VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(SUB0MEMPAGE_BUILD_TESTING  "Build unit-tests" ON)
option(SUB0MEMPAGE_BUILD_EXAMPLES "Build examples"   OFF)

if(SUB0MEMPAGE_BUILD_TESTING AND NOT IS_SUBPROJECT)
    enable_testing()
endif()

# Header-only, interface library -- the recommended first implementation shape, matching Sub0Firn's own
# CMakeLists.txt precedent one layer up: no working implementation exists yet (README.md's status line),
# so this target currently exposes only the design-skeleton header in include/. Grows a real compiled
# component only once the disk/network async-I/O machinery needs background-thread infrastructure
# substantial enough that header-only stops being the right packaging (docs/design.md sec 2's platform
# seam -- Windows IOCP/PrefetchVirtualMemory, Linux io_uring -- is where that need would first appear).
add_library(Sub0MemPage INTERFACE)
add_library(Sub0MemPage::Sub0MemPage ALIAS Sub0MemPage)

target_include_directories(Sub0MemPage INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# C++23, matching the Sub0 project family's own baseline (Sub0Llm, Sub0Firn, Sub0Log) -- see
# STYLE_GUIDE.md for why this is a hard requirement rather than a preference.
target_compile_features(Sub0MemPage INTERFACE cxx_std_23)

if(SUB0MEMPAGE_BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

if(SUB0MEMPAGE_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
