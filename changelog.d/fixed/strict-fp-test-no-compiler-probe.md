- CI: `test_strict_fp_compiler_args` no longer times out on `Windows ARM64 MSVC`.
  Its throwaway Meson fixtures declared a language, so each of its Meson setups
  detected a C compiler; on master the test took 41 to 90.7 s against its 90 s
  limit and failed twice. The fixtures now declare no language (the policy
  blocks they execute need none), and `_meson_setup()` refuses a fixture that
  declares one.
