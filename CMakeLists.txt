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

# The paging API remains a skeleton. Experimental executables are separate targets below;
# their toolchain requirements never propagate to this dependency-free interface.
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

# Tools are opt-in and never impose a SYCL dependency on the interface library.
option(SUB0MEMPAGE_BUILD_INTEL_USM_SPIKE "Build the experimental Intel USM diagnostic" OFF)
if(SUB0MEMPAGE_BUILD_INTEL_USM_SPIKE)
    add_subdirectory(tools/intel_usm)
endif()
