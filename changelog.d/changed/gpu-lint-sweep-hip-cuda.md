- **Linted the HIP and CUDA backends: 1,008 fewer clang-tidy warnings.**
  ADR-1142 puts GPU code under the same standards as everything else,
  and neither lane had been swept. HIP went 139 → 54 and CUDA 978 → 155.
  Three changes did the work:
  - **One real defect fixed in `core/src/cuda/cuda_helper.cuh`.** The
    `CHECK_CUDA*` macros discarded the return of `cuGetErrorName()` and
    `fprintf()`, which `principles.md` forbids — every non-void return
    is checked or explicitly `(void)`-discarded. Because the macros
    expand at ~250 call sites, that single pair of lines accounted for
    **502 of CUDA's 978 warnings**; two `(void)` casts clear all of them.
  - The ADR-1138 `NOLINTBEGIN(modernize-use-nullptr)` bracket applied to
    43 C translation units (21 HIP, 22 CUDA) that did not carry it. C
    TUs must keep `NULL` because MSVC's `/std:clatest` has no C23
    `nullptr` and the required Windows build compiles them with cl.exe;
    the bracket is the house remedy, cited per ADR-0278.
  - `readability-braces-around-statements` and
    `readability-isolate-declaration` across both lanes.
  Verified by building both backends and running their suites on real
  hardware: CUDA 203/203 on an RTX 4090, plus CPU 148/148. No numerical
  surface is touched.
