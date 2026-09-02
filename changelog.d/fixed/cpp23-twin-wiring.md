- The C++23 Wave 1–5 conversions now actually ship. Twelve `.cpp` files under
  `core/src/` (`cpu`, `dict`, `mem`, `output`, `ref`, `thread_locale`,
  `fex_ctx_vector`, `feature_name`, `luminance_tools`, `mkdirp`, `picture_copy`,
  `psnr_tools`) had been written and accepted (ADR-0720/0721/0723/0727/0729/
  0731/0733/0735) but were never referenced by Meson, so `libvmaf.so` kept
  compiling the legacy `.c` twin and the modernized source was dead. Worse, 174
  test-target references pointed at the dead twin, so coverage and fuzzing were
  measuring code that did not ship — which let `output.c` receive the ADR-0602
  NULL-guard fix (adversarial audit 2026-05-31) while `output.cpp` silently
  did not.
- Wiring them up surfaced two real defects and eight build errors in code that
  had never once been compiled. Both defects are fixed:
  **`feature_name.cpp`** treated `vmaf_dictionary_copy`'s `-EINVAL` as fatal,
  but that is the ordinary "this feature has no options dict" return, so every
  such feature failed name derivation (16 failing tests); genuine OOM is still
  fatal. **`output.cpp`** lacked the `-EINVAL` NULL guards in
  `vmaf_write_output_csv` / `_sub` and SIGSEGV'd on NULL input where the sibling
  writers returned an error.
- Build errors fixed alongside: `output.cpp` called `vmaf_log` without including
  `log.h`; `mkdirp.h`, `psnr_tools.h`, `alias.h`, `output.h`, `fex_ctx_vector.h`
  and `core/test/test.h` declared C-linkage symbols with no `extern "C"` guard,
  which collided once their implementations compiled as C++. The superseded
  `.c` twins and the fully-dead `metadata_handler.c` are removed, along with the
  now-pointless `cpp_std` override on `libvmaf_cpu_static_lib` that existed only
  for `cpu.cpp` (ADR-0755).
