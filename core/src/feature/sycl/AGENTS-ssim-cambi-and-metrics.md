<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# SSIM, CAMBI, SpEED, and metrics: `core/src/feature/sycl/` agent invariants

integer_ssim_sycl.cpp, integer_ms_ssim_sycl.cpp, integer_cambi_sycl.cpp,
integer_ciede_sycl.cpp, sycl_ciede_math.h, speed*, sycl_speed_cov_math.h,
ssimulacra2_sycl.cpp, sycl*ssim*.h, core/test/test_sycl*ssim*.c,
core/test/test_sycl_cambi_parity.c, core/test/test_sycl_ciede_parity.c,
core/test/test_sycl_speed*; decimation, CAMBI options, CIEDE fp32, SpEED cov,
IIR.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `integer_cambi_sycl.cpp`, `/core/test/test_sycl_cambi_parity.c` | [cambi](AGENTS.d/cambi.md) | integer_cambi_sycl.cpp — fully device-resident, graph-registered; options_cambi_sycl sync. |
| `integer_ciede_sycl.cpp`, `sycl_ciede_math.h`, `/core/test/test_sycl_ciede_parity.c` | [ciede](AGENTS.d/ciede.md) | integer_ciede_sycl.cpp stages Y/U/V at native size; statements on fp32 pairs match ciede.c. |
| `integer_ssim_sycl.cpp`, `sycl_ssim_terms.h`, `/core/test/test_sycl_ssim_parity.c` | [float-ssim](AGENTS.d/float-ssim.md) | float_ssim_sycl terms = CPU doubles, frame sums = CPU raster order; decimation is bit-exact with CPU. |
| `integer_ssim_sycl.cpp`, `sycl_integer_ssim_math.h`, `/core/test/test_sycl_ssim_parity.c` | [integer-ssim](AGENTS.d/integer-ssim.md) | integer_ssim_sycl = CPU ssim, bit for bit (ADR-1443); SSIM twins take CPU options. |
| `integer_ms_ssim_sycl.cpp`, `/core/test/test_sycl_ms_ssim_parity.c` | [ms-ssim](AGENTS.d/ms-ssim.md) | integer_ms_ssim_sycl.cpp = CPU arithmetic, type for type; honours enable_chroma, enable_lcs, enable_db. |
| `speed_*_sycl.cpp`, `speed_sycl_*`, `/core/test/test_sycl_speed_*`, `sycl_speed_cov_math.h` | [speed](AGENTS.d/speed.md) | SpEED pipeline arithmetic contract and singular-covariance contract; device-resident twins. |
| `ssimulacra2_sycl.cpp`, `sycl_ssimulacra2_math.h`, `/core/test/test_sycl_ssimulacra2_parity.c` | [ssimulacra2](AGENTS.d/ssimulacra2.md) | ssimulacra2_sycl = CPU ssimulacra2, bit for bit; device-resident; IIR recurrence has no running accumulator. |
