<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# ADM and VIF twins: `core/src/feature/sycl/` agent invariants

integer_adm_sycl.cpp, float_adm_sycl.cpp, sycl_float_adm_math.h,
integer_vif_sycl.cpp, sycl_integer_vif_math.h, float_vif_sycl.cpp,
sycl_float_vif_math.h, core/test/test_sycl_adm_parity.c,
core/test/test_sycl_float_adm_parity.c, core/test/test_sycl_vif_parity.c,
core/test/test_sycl_float_vif_parity.c; ADM AIM pass, int16 scale 0, viewing
distances, VIF odd-width stride, bit-exact float ADM/VIF.

Parent index: [AGENTS.md](AGENTS.md).

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../../docs/development/agents-index.md)).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `integer_adm_sycl.cpp`, `/core/test/test_sycl_adm_parity.c` | [adm-aim](AGENTS.d/adm-aim.md) | Integer ADM AIM pass; adm_sycl provides aim_score; 2 kernels per scale decouple CSF; adm_skip_aim mirrors CPU. |
| `integer_adm_sycl.cpp`, `/core/test/test_sycl_adm_parity.c` | [adm](AGENTS.d/adm.md) | Integer ADM tiny frames and linkage; Scale 0 = CPU int16 semantics; scales 1-3 >> 32 rounding term. |
| `float_adm_sycl.cpp`, `sycl_float_adm_math.h`, `/core/test/test_sycl_float_adm_parity.c` | [float-adm](AGENTS.d/float-adm.md) | float_adm_sycl.cpp = CPU float_adm, bit for bit; options must be captured, not hardcoded. |
| `float_vif_sycl.cpp`, `sycl_float_vif_math.h`, `/core/test/test_sycl_float_vif_parity.c` | [float-vif](AGENTS.d/float-vif.md) | float_vif_sycl.cpp = CPU arithmetic, bit for bit; options must be captured, not hardcoded. |
| `integer_vif_sycl.cpp`, `sycl_integer_vif_math.h`, `/core/test/test_sycl_vif_parity.c` | [integer-vif](AGENTS.d/integer-vif.md) | integer_vif_sycl.cpp = CPU vif, bit for bit; rd_stride uses ceiling division for odd widths. |
