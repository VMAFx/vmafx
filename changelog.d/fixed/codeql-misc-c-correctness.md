- **Resolved miscellaneous CodeQL C/C++ alerts across core library, CLI, and CI.**
  - `core/src/feature/iqa/convolve.c`: decoupled single-rounded float
    multiplication (`const float prod = ...`) from double accumulation
    (`sum += (double)prod`), eliminating compiler-generated widening conversions
    flagged by CodeQL (Alert 1005) while preserving ADR-0138 bit-exact parity
    with AVX2/AVX-512/NEON SIMD twins.
  - `core/src/feature/moment.c`: decoupled single-rounded float squaring
    (`const float term = pic_ * pic_`) from double accumulation
    (`cum += (double)term`), eliminating compiler-generated widening
    conversions underlying historically dismissed Alert 707 under the
    ADR-0179 and ADR-0987 tolerance-bounded non-byte-exact reduction contract.
  - `core/src/pdjson.h`: sequentially numbered all enumerators in
    `enum json_type` (`JSON_NONE = 0` .. `JSON_NULL = 11`), satisfying AV Rule
    145 / Alert 1064 while preserving the public ABI contract.
  - `core/tools/cli_parse.cpp`: replaced variadic `usage()` parameter pack with
    discrete overloads for 1, 2, and 3 arguments, preventing empty template
    parameter pack instantiations flagged as unused variables (Alerts 1002 and
    1003).
  - `.github/workflows/security-scans.yml`: configured Meson before CodeQL
    database initialization with the build directory in `${{ runner.temp }}/build`,
    preventing Meson compiler probe snippets (`testfile.c`) and generated build
    artifacts from being extracted into the CodeQL database (current hosted
    probe alert 1279, historical 1278).
