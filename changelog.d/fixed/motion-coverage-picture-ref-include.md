Fix missing `#include "picture.h"` in `test_integer_motion_coverage.c` that caused
`vmaf_picture_ref` undeclared-function errors under strict C99/C11 compilers (icx,
Apple Clang). Introduced by PR #747 which added `vmaf_picture_ref` calls without
the corresponding internal header include. Unblocks the FFmpeg-SYCL and FFmpeg-macOS
CI jobs whose "Build vmaf" step failed before the patch-apply step was reached.
