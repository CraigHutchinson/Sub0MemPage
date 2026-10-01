# Read-strategy shootout: a kept diagnostic, off by default (SUB0MEMPAGE_BUILD_DIAGNOSTICS). See README.md.
find_package(Threads REQUIRED)

add_executable(sub0mempage-read-shootout read_shootout.cpp)
target_link_libraries(sub0mempage-read-shootout PRIVATE Sub0MemPage::Sub0MemPage Threads::Threads)
target_compile_features(sub0mempage-read-shootout PRIVATE cxx_std_23)
add_executable(sub0mempage-uncached-burst uncached_burst.cpp)
target_compile_features(sub0mempage-uncached-burst PRIVATE cxx_std_23)
if(WIN32)
    # The naive baseline uses stdio fopen deliberately; the MSVC CRT deprecates it in favour of fopen_s.
    target_compile_definitions(sub0mempage-read-shootout PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
    target_compile_options(sub0mempage-read-shootout PRIVATE /W4 /WX)
    target_compile_options(sub0mempage-uncached-burst PRIVATE /W4 /WX)
else()
    target_compile_options(sub0mempage-read-shootout PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(sub0mempage-uncached-burst PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Smoke test: every strategy on a small generated file, warm, every row verified. Keeps the tool building
# and honest on each platform; it is not a measurement.
if(SUB0MEMPAGE_BUILD_TESTING)
    add_test(NAME sub0mempage.read-shootout-smoke
        COMMAND sub0mempage-read-shootout --file "${CMAKE_CURRENT_BINARY_DIR}/read-shootout-smoke.bin"
                --file-mib 16 --misses 6 --rounds 1 --gap none --verify-every 1)
    set_tests_properties(sub0mempage.read-shootout-smoke PROPERTIES TIMEOUT 120)
endif()
