# CMake toolchain file for cross-compiling Sub0MemPage to aarch64 Linux with the Ubuntu
# `g++-aarch64-linux-gnu` cross toolchain, then running the resulting tests under `qemu-user`
# (`qemu-aarch64`) so `ctest` works without real ARM hardware.
#
# Used by `scripts/dev.py arm` (see docs/DEVELOPMENT_WORKFLOW.md). Correctness only -- CI/dev.py
# never takes perf numbers under emulation (AGENTS.md rule 4/8: an emulated timing is not a
# measurement of the target).
#
# Install on Debian/Ubuntu:
#   apt-get install -y g++-aarch64-linux-gnu qemu-user
#
# Usage:
#   cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake -G Ninja
#   cmake --build build-arm
#   ctest --test-dir build-arm   # each test binary runs as `qemu-aarch64 -L /usr/aarch64-linux-gnu <binary>`

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Cross sysroot the Ubuntu package installs the target's libc/libstdc++ under; used only for qemu's
# `-L` (dynamic loader search path) below. Do NOT set CMAKE_SYSROOT / pass --sysroot to the compiler:
# this cross g++ package embeds its own default library search paths (`-print-sysroot` reports `/`),
# and Debian's libc/libm .so link scripts under /usr/aarch64-linux-gnu contain bare absolute paths
# (e.g. `/usr/aarch64-linux-gnu/lib/libm.so.6`) that ld then re-prefixes with an explicit --sysroot,
# producing a doubled, nonexistent path ("cannot find ... inside /usr/aarch64-linux-gnu").
set(SUB0MEMPAGE_AARCH64_SYSROOT "/usr/aarch64-linux-gnu")

set(CMAKE_FIND_ROOT_PATH "${SUB0MEMPAGE_AARCH64_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Run cross-built test binaries through qemu-user so `ctest`/`add_test` work unmodified. `-L` points
# qemu at the target sysroot for its dynamic loader and shared libs (libstdc++, libasan, libtsan, ...).
find_program(SUB0MEMPAGE_QEMU_AARCH64 qemu-aarch64)
if(SUB0MEMPAGE_QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${SUB0MEMPAGE_QEMU_AARCH64};-L;${SUB0MEMPAGE_AARCH64_SYSROOT}")
endif()
