- Tests: `core/test/test_strict_fp_compiler_args.py` no longer declares the C
  language in the throwaway Meson projects it configures. The fixtures only
  evaluate Meson code, but the language made Meson detect and sanity-check the
  host C compiler on each of the 19 setups, which took the test past its
  90-second timeout under MSVC on the Windows ARM64 runner. A fixture that
  declares a language again is refused.
