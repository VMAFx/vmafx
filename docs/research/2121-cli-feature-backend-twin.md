<!-- markdownlint-disable MD013 -->
# Research-2121: `--feature` with an explicit GPU `--backend` — twin pairing and receipt

- **Status**: Active
- **Workstream**: [ADR-1359](../adr/1359-cli-feature-backend-twin.md); follows [Research-2120](2120-sycl-ciede-throughput.md)
- **Last updated**: 2026-09-29

## Question

[Research-2120](2120-sycl-ciede-throughput.md) found that
`vmaf --backend sycl --feature ciede` computes on the CPU and reports
`"backend_used": "sycl"`. The maintainer chose to fix it in the CLI. How can the
CLI find a CPU extractor's twin, which checks decide whether the twin can serve
the request, and how should the JSON say what ran?

## Sources

- `core/src/libvmaf.c`: `vmaf_use_feature()`, `vmaf_use_features_from_model()`,
  `compute_fex_flags()`, `fex_honouring_model_options()` (ADR-1183) and
  `resolve_context_fallback()` (ADR-1324).
- `core/src/feature/feature_extractor.cpp`: `feature_extractor_list[]`,
  `vmaf_get_feature_extractor_by_feature_name()` (ADR-1100 first pass, ADR-0530
  second pass), `vmaf_feature_extractor_honours_options()`.
- `core/src/meson.build` (`-fvisibility=hidden`, ADR-0379) and
  `core/tools/meson.build` (the CLI links the shared library when
  `default_library=both`).
- `core/tools/vmaf.cpp`: `register_cli_feature()`,
  `amend_json_with_backend_used()`, `active_backend_name()`.

## Findings

**The CLI cannot see the registry.** Every internal libvmaf symbol is hidden,
and the CLI links `libvmaf.so`. `vmaf_get_feature_extractor_by_feature_name()`,
`vmaf_feature_extractor_honours_options()` and `VmafFeatureExtractor` are not
reachable from `core/tools/`, so any CLI-side resolution needs a public entry
point.

**Twin names follow no rule.** Pairing by `<name>_<backend>` gets `ciede` and
`float_ssim` right but misses `ssim` (`integer_ssim_sycl`), `ciede` on Metal
(`integer_ciede_metal`) and `psnr` on Metal (`integer_psnr_metal`). Only four
GPU twins declare a CPU pairing (`context_fallback_name`, all `float_ssim`).

**The model-dispatch lookup pairs correctly when it is restricted to the
backend flag.** `vmaf_get_feature_extractor_by_feature_name(feature, flags)`
returns an extractor with one of `flags` if one provides `feature`, and
otherwise, through its ADR-0530 second pass, any extractor that provides it:
another backend's twin or the CPU extractor. Scanning the CPU extractor's
`provided_features[]` in order and accepting the first result that carries the
backend flag gives the twin for every CPU extractor that has one. The first
listed feature is not enough on its own: the CPU `motion` extractor lists
`VMAF_integer_feature_motion_sad_score` first, which no twin provides, and
`VMAF_integer_feature_motion_score` second, which `motion_sycl` does.

**Pairing on the SYCL registry, measured.** `vmaf --backend sycl --feature <name>
--no_prediction --frame_cnt 2` on the Netflix 576x324 pair, Arc B580
(`ONEAPI_DEVICE_SELECTOR=level_zero:0`), 2026-09-29. "Ran" is the
`feature_backends` entry; "keys" is the number of metrics in frame 0.

| `--feature` | Ran | Warning | Keys |
| --- | --- | --- | --- |
| `adm` | `adm_sycl` (sycl) | | 5 |
| `cambi` | `cambi_sycl` (sycl) | | 1 |
| `ciede` | `ciede_sycl` (sycl) | | 1 |
| `float_adm` | `float_adm_sycl` (sycl) | | 7 |
| `float_motion` | `float_motion_sycl` (sycl) | | 2 |
| `float_ms_ssim` | `float_ms_ssim_sycl` (sycl) | | 1 |
| `float_psnr` | `float_psnr_sycl` (sycl) | | 1 |
| `float_ssim` | `float_ssim_sycl` (sycl) | | 1 |
| `float_vif` | `float_vif_sycl` (sycl) | | 4 |
| `motion` | `motion_sycl` (sycl) | | 3 |
| `motion_v2` | `motion_v2_sycl` (sycl) | | 3 |
| `psnr` | `psnr_sycl` (sycl) | | 3 |
| `psnr_hvs` | `psnr_hvs_sycl` (sycl) | crashed: SIGSEGV on the B580 (`T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29`); the explicit twin name crashes too, the UHD 770 runs it | — |
| `speed_chroma` | `speed_chroma_sycl` (sycl) | | 3 |
| `speed_temporal` | `speed_temporal_sycl` (sycl) | | 1 |
| `ssim` | `integer_ssim_sycl` (sycl) | | 1 |
| `ssimulacra2` | `ssimulacra2_sycl` (sycl) | | 1 |
| `vif` | `vif_sycl` (sycl) | | 15 |
| `brisque`, `delta_e_itp`, `float_moment`, `niqe`, `pu21`, `speed_qa`, `y_funque_plus` | the CPU extractor (cpu) | `the sycl backend has no twin of this extractor` | 1 to 4 |

`float_moment` has no twin through this lookup: the CPU extractor lists
`float_moment`, and the SYCL, CUDA, HIP and Metal moment extractors provide only
`float_moment_ref1st` and the other three per-moment names. Model dispatch
never asks for `float_moment`, so this is the same answer it would give.

**Checks before a twin is accepted.** Model dispatch keeps a twin only when it
honours the model's options (ADR-1183 unknown keys, ADR-1316 default-only
values) and, at the first picture, only when the ADR-1324 `context_check`
accepts the geometry. The CLI knows the geometry before registration
(`vmaf_preallocate_pictures()` has run), so both checks can run up front. The
geometry hook reads parsed options from `fex->priv`, so it needs a context; a
scratch context from `vmaf_feature_extractor_context_create()` that is never
initialised or registered is enough, and destroying an uninitialised context
is the path `resolve_context_fallback()` already takes. Measured on the B580:
`float_ssim=enable_lcs=true:scale=1` falls back with
`float_ssim_sycl cannot honour option 'enable_lcs'`, and `float_ssim=scale=2` at
576x324 with `float_ssim_sycl cannot run 576x324 8-bit pictures with these
options`.

**What ran, measured.** Netflix pair, 48 frames, `--precision max`:
`--feature ciede --backend sycl` and `--feature ciede_sycl --backend sycl` agree
on every frame (pooled mean 33.10754581952854 both), 1.18e-5 at most from the
CPU `ciede` (33.10755659567523). `SYCL_UR_TRACE=2` counts 10
`urEnqueueKernelLaunch` calls over 5 frames for both, and 0 for
`--backend cpu`. BBB 3840x2160, 22 frames: 395 ms wall with `--feature ciede`
and 504 ms with `--feature ciede_sycl` (both including start-up, same
per-frame scores), against 787 ms per frame for 3 frames of serial CPU `ciede`
on the same host while other builds ran.

**Receipt.** `backend_used` used to come from `active_backend_name()`, which
reads which backend state was initialised. The registered extractor list after
the final flush is the only place that knows what ran, including a model
feature swapped for its CPU extractor at the first picture (ADR-1324). A new
`vmaf_registered_feature_extractor()` exposes it; the CLI derives
`backend_used` from it and lists every entry in `feature_backends`. Consumers
that compare `backend_used` with a backend name (`dev/scripts/smoke-probe-loop.sh`,
the RC1 tester report, both MCP servers) keep working because the value set is
unchanged.

## Alternatives explored

- **Resolve inside `vmaf_use_feature()`.** Would also change the FFmpeg filters
  and the documented exact-name contract; not what the maintainer chose.
- **CLI-side `<name>_<backend>` table.** Wrong for `ssim`, and for `ciede` and
  `psnr` on Metal; cannot apply the option and geometry checks.
- **First provided feature only.** Misses `motion` (see above).
- **Require the twin to provide every feature the CPU extractor lists.**
  `provided_features[]` is not an emission contract: `adm`, `float_adm`,
  `float_motion` and `float_ms_ssim` list names their twins do not, yet model
  dispatch uses those twins every run.
- **`"backend_used": "mixed"`.** A new value breaks the consumers above; a new
  array does not.

## Open questions

- CUDA, HIP and Metal use the same lookup, and the white-box unit test checks
  the `ciede`, `brisque` and `float_ssim` cases against whichever backend a
  build compiles. A device run on those backends
  (`test_vmaf_feature_backend_<backend>`) has not been done here.
- `psnr_hvs_sycl` crashes on the B580 on this WSL2 host
  (`T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29`); native Linux not checked.

## Related

- ADRs: [ADR-0379](../adr/0379-libvmaf-symbol-visibility.md),
  [ADR-0530](../adr/0530-hip-feature-flag-promotion-and-picture-buffer.md),
  [ADR-0543](../adr/0543-adr-0498-enforcement-hardening.md),
  [ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md),
  [ADR-1316](../adr/1316-gpu-option-value-capability-fallback.md),
  [ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md),
  [ADR-1359](../adr/1359-cli-feature-backend-twin.md).
- State rows: `T-CLI-FEATURE-NAME-BYPASSES-GPU-BACKEND-2026-09-29` (closed),
  `T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29` (open).
