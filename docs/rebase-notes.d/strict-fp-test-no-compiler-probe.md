## Strict-FP policy fixtures declare no language (2026-10-13)

- `core/test/test_strict_fp_compiler_args.py`: the five fixture `project()`
  calls name no language, and `_meson_setup()` fails a fixture whose
  `project()` does (`PROJECT_WITH_LANGUAGE`). Meson then skips compiler
  detection in each throwaway setup.
- **On sync**: fork-only test; no upstream counterpart. A new fixture that needs
  a compiler object (`meson.get_compiler()`) cannot use `_meson_setup()`; give
  it its own setup and keep the test under its 90 s limit on Windows ARM64.
