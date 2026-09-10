# Experimental executable only: no SYCL requirements propagate to Sub0MemPage.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "IntelLLVM")
    message(FATAL_ERROR "Intel USM spike requires an IntelLLVM SYCL compiler; configure a separate build with icx/icpx")
endif()
add_executable(sub0mempage-usm-probe usm_probe.cpp)
target_compile_features(sub0mempage-usm-probe PRIVATE cxx_std_23)
target_compile_options(sub0mempage-usm-probe PRIVATE -fsycl)
target_link_options(sub0mempage-usm-probe PRIVATE -fsycl)
if(SUB0MEMPAGE_BUILD_TESTING)
    add_test(NAME intel_usm_cli
        COMMAND ${CMAKE_COMMAND} -DPROBE=$<TARGET_FILE:sub0mempage-usm-probe>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_cli.cmake)
endif()
