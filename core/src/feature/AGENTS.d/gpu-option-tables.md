---
paths:
  - core/src/feature/feature_extractor.h
  - core/src/feature/feature_name.cpp
invariant: GPU-twin VmafOption tables mirror CPU table entry-for-entry with full semantics.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# GPU-Twin Option Tables CPU Mirror and Semantics

## GPU-twin `VmafOption` tables mirror the CPU table (2026-09-05)

Option iteration terminates on table entry's null `name`, not address
of entry. Preserve that sentinel, aliases and default-value omission when
rebasing `feature_extractor.cpp` or `feature_name.cpp`; existing
`test_feature` and `test_feature_extractor` cases pin those behaviors.

two shared pool structs in `feature_extractor.h` keep C-compatible layouts
under ADR-0772. Their narrowly scoped `uninitMemberVarNoCtor` markers depend
on `vmaf_fex_ctx_pool_create()` zero-initializing outer pool and all three
slot construction paths value-initializing entries before activation. New
construction sites must preserve and re-verify that contract; do not broaden
those markers to other types or uninitialized-use checks.

`vmaf_feature_name_from_options()` (`feature_name.cpp`) builds emitted
feature key from extractor's **own** `options[]` table: every entry that
carries `VMAF_OPT_FLAG_FEATURE_PARAM` and holds non-default value appends
`_<alias>_<value>`. GPU twin whose table is missing one FEATURE_PARAM entry
therefore emits *different key* than its CPU twin for same opts dict, and
model lookup misses. default model
(`model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json`) is live example: it requests
`VMAF_integer_feature_adm3_score` with `adm_csf_mode=2`, `adm_dlm_weight=0.7`,
`adm_enhn_gain_limit=1.0`, `adm_min_val=0.5`, `adm_noise_weight=0.02`, and
key it looks up is
`integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02`.

Two rules follow, and they are not same rule:

1. **table mirrors CPU table entry-for-entry** — name, alias,
   `type`, `default_val`, `min`, `max`, and `VMAF_OPT_FLAG_FEATURE_PARAM`
   bit. Anything that diverges (different alias, different default,
   missing flag) silently changes key. `core/src/feature/integer_adm.c` is
   reference for `adm` family.
   extractor-local capability bit such as `VMAF_OPT_FLAG_DEFAULT_ONLY`
   may differ; it describes what twin can execute without changing
   CPU-authoritative collector-key schema.
2. **Never declared-and-ignored.** Option that changes value twin
   emits must change it. FEATURE_PARAM option whose arithmetic
   only feeds feature twin does **not** emit is one legitimate
   exception. It stays in table for key parity, exactly as
   `adm_dlm_weight` and `adm_min_val` have no arithmetic effect on `adm2` in
   CPU reference either. Entry must carry comment saying so.
   non-FEATURE_PARAM option (`adm_skip_aim`, `debug`) never affects key,
   so it has no key-parity excuse: implement it or leave it out.

**Never fabricate feature to make name resolve.** If twin cannot compute
feature, leave that feature out of its `provided_features[]`.
`vmaf_get_feature_extractor_by_feature_name()` then routes request to
CPU twin through ADR-0530 fallback, which produces correct value under
correct key. Emitting feature from hard-coded stand-in (SYCL and
HIP `integer_adm` twins briefly emitted `VMAF_integer_feature_aim_score` from
literal `aim_num = 0.0`) is strictly worse than not providing it: fallback
stops firing and model silently consumes fabricated score.

**`numden_limit` scales with full-frame area.** `1e-10 * (w * h) /
(1920.0 * 1080.0)` uses picture dimensions, not scale-3 dimensions
per-scale loop variables hold once loop has run. All three GPU twins had
inherited post-loop values (256× too-small floor).

## Twin option tables mirror the CPU's aliases and semantics (ADR-1214, ADR-1312)

When GPU twin copies option from CPU extractor, copy `alias` and
range too: ADR-1183 builds emitted feature name from alias and value of
every non-default option, so `cs` on twin and `scf` on CPU means two
different keys for one feature. And copy *semantics* from branch
twin implements — `adm_csf_scale` is Barten-mode argument, so in
Watson-only twins it must be no-op exactly as it is on CPU.
`core/test/test_gpu_option_alias_contract.py` is device-free inventory for
eighteen known motion, VIF, and ADM alias sites; extend it whenever
equivalent twin option is added.
