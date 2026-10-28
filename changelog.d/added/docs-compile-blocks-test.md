- **Documentation C code blocks compile test (`test_docs_compile_blocks`, #2436).**
  Fenced C code blocks marked with an `<!-- compile -->` comment in documentation
  are extracted and verified against public headers under `core/include/` using
  `-std=c11 -Wall -Wextra -Werror -fsyntax-only` in the `fast` Meson test suite,
  with a `--self-test` mode covering positive, negative, and invalid marker cases.
