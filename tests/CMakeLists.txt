find_package(Threads REQUIRED)

# One portable offline test executable per file; warnings are errors on every toolchain.
function(sub0mempage_add_test name source)
    add_executable(${name} ${source} allocation_counter.cpp)
    target_link_libraries(${name} PRIVATE Sub0MemPage::Sub0MemPage Threads::Threads)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    string(REPLACE "sub0mempage-" "sub0mempage." test_name ${name})
    string(REGEX REPLACE "-tests$" "" test_name ${test_name})
    add_test(NAME ${test_name} COMMAND ${name})
    # State-machine defects can surface as a hang (a waiter never woken); fail instead of stalling CI.
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
endfunction()

sub0mempage_add_test(sub0mempage-probe-options-tests probe_options_tests.cpp)
target_include_directories(sub0mempage-probe-options-tests PRIVATE "${PROJECT_SOURCE_DIR}/tools/intel")
sub0mempage_add_test(sub0mempage-slot-pool-tests slot_pool_tests.cpp)
sub0mempage_add_test(sub0mempage-transfer-set-tests transfer_set_tests.cpp)
sub0mempage_add_test(sub0mempage-local-file-backend-tests local_file_backend_tests.cpp)
