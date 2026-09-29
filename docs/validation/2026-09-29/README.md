# S1b packaging validation — 2026-09-29

Embedded builds now default standalone tests OFF; top-level builds retain ON, and explicit caller settings still win. A parent-project compile regression verifies both the default and the public target.

Windows Clang 22.1.8 / Ninja / Release: **5/5 CTest tests passed**.
WSL Linux GCC 15 / Ninja / Release: **5/5 CTest tests passed**.
The complete standalone suite ran; consumer tests configure/build real executables rather than
checking target-property strings. No performance measurement or GPU qualification is claimed.

Reproduce with CMake configure, build, then `ctest --test-dir <build> --output-on-failure`.
TieredCache additionally uses `FETCHCONTENT_SOURCE_DIR_SUB0MEMPAGE` to select the audited local
MemPage checkout and `FETCHCONTENT_FULLY_DISCONNECTED=ON`. No dependency pin changed in this package.
These results test the local packaging fixes; prior published CI is not evidence for these new commits.
