**fix(ci): restore Coverage Gate — remove dangling `vmaf_fex_integer_motion_v2` ref and guard `motion_five_frame_window` in `integer_motion.c` (ADR-0994)**

The Coverage Gate (ADR-0922) has been aborting at the build step on every master
push since `c658b3c452` due to two bugs introduced when `integer_motion_v2` was
removed as a standalone CPU extractor:

1. `integer_motion.c` referenced `fex->prev_prev_ref` (a struct field that does
   not exist) in its `extract()` function for the `motion_five_frame_window` path.
   The fix adds an `-ENOTSUP` guard in `init()` (matching `integer_motion_v2.c`'s
   existing handling per ADR-0337) and replaces the dead reference with `&fex->prev_ref`.

2. `feature_extractor.cpp` still declared `extern VmafFeatureExtractor vmaf_fex_integer_motion_v2`
   and included it in `feature_extractor_list[]` even though the source file was
   removed from the meson build, causing an undefined-reference linker error.

With these fixes the coverage build completes and the gate produces an actual
coverage report instead of failing with a compile error.
