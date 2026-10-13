## Strict-FP policy test fixtures declare no language (2026-10-13)

- `core/test/test_strict_fp_compiler_args.py`: the five fixture templates call
  `project('<name>')` without a language, and `_meson_setup()` refuses a
  fixture whose `project()` declares more than a name
  (`_fixture_project_failure()`, planted cases in
  `test_fixture_project_with_a_language_is_refused`). Fork-only test, nothing
  upstream to merge. **On sync**: a new fixture keeps `project()` to its name;
  the policy blocks it evaluates call no compiler.
