- CI: the required `Tidy Changed` and `Tidy Ratchet` gates failed on every C/C++
  pull request. `core/meson.build`'s `b_lto_threads=4` (ADR-1172) renders as
  GCC's `-flto=4`, which the tidy lane's gcc build writes into
  `compile_commands.json`; clang-tidy parses those entries with clang, which
  rejects the argument (`unsupported argument '4' to option '-flto='`) and
  errored on every translation unit. Both tidy builds now configure with
  `-Db_lto=false` — they exist only to emit `compile_commands.json` and the
  generated headers.
