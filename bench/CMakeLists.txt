# Bookkeeping microbenchmarks. Built by scripts/dev.py's bench stage; off by default (not a test).
#
# SUB0MEMPAGE_BENCH_LIBRARY_INCLUDE lets dev.py compile THIS bench source against another revision's
# library headers (the baseline arm of an A/B), so a comparison isolates library changes.
set(SUB0MEMPAGE_BENCH_LIBRARY_INCLUDE "" CACHE PATH "Library include dir for the bench (empty = this tree)")

add_executable(sub0mempage-bench bench_main.cpp "${PROJECT_SOURCE_DIR}/tests/allocation_counter.cpp")
if(SUB0MEMPAGE_BENCH_LIBRARY_INCLUDE)
    target_include_directories(sub0mempage-bench BEFORE PRIVATE "${SUB0MEMPAGE_BENCH_LIBRARY_INCLUDE}")
    target_compile_features(sub0mempage-bench PRIVATE cxx_std_23)
    target_link_libraries(sub0mempage-bench PRIVATE Threads::Threads)
else()
    target_link_libraries(sub0mempage-bench PRIVATE Sub0MemPage::Sub0MemPage)
endif()
if(MSVC)
    target_compile_options(sub0mempage-bench PRIVATE /W4 /WX)
else()
    target_compile_options(sub0mempage-bench PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
