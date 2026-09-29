<!-- markdownlint-disable MD013 MD060 -->

# ADR-1359: The CLI maps `--feature <cpu-name>` to the explicit `--backend`'s twin

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: Lusoris
- **Tags**: cli, api, gpu, dispatch, output-schema, fork-local

## Context

`vmaf --backend sycl --feature ciede` initialised the SYCL device and then
computed `ciede2000` with the CPU `ciede` extractor. `register_cli_feature()` in
`core/tools/vmaf.cpp` passed the name to `vmaf_use_feature()`, which selects an
extractor by exact name. Only a model's features reach a device twin:
`vmaf_use_features_from_model()` looks each feature up with
`vmaf_get_feature_extractor_by_feature_name(name, fex_flags)`, keeps the twin
only when it honours the model's options
([ADR-1183](1183-model-options-gate-gpu-twin-selection.md),
[ADR-1316](1316-gpu-option-value-capability-fallback.md)), and swaps in the
CPU extractor at the first picture when the twin cannot run its geometry
([ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md)). On a 4K 8-bit 4:2:0
clip, `--feature ciede --backend sycl` took 1714 ms per frame on an Arc B580,
the same as the serial CPU extractor, against 8.4 ms for
`--feature ciede_sycl` ([Research-2120](../research/2120-sycl-ciede-throughput.md)).
The JSON still said `"backend_used": "sycl"`, because the CLI reported the
backend it had initialised, not the one that computed anything. Any
per-feature GPU benchmark written as `--feature <cpu-name> --backend <gpu>`
measured the CPU (`T-CLI-FEATURE-NAME-BYPASSES-GPU-BACKEND-2026-09-29`).

Three places could resolve the name: the CLI, `vmaf_use_feature()` (which also
changes the C API and the FFmpeg filters), or nowhere, with only the receipt
corrected. The maintainer chose the CLI.

The CLI cannot do the lookup on its own. It links the shared `libvmaf`, which
is built with `-fvisibility=hidden`
([ADR-0379](0379-libvmaf-symbol-visibility.md)), so the extractor registry,
`vmaf_get_feature_extractor_by_feature_name()` and
`vmaf_feature_extractor_honours_options()` are not reachable from it. Twin
names follow no pattern the CLI could reconstruct: the twin of `float_ssim` is
`float_ssim_cuda`, of `ssim` `integer_ssim_cuda`, of `motion` `motion_sycl`, of
`ciede` on Metal `integer_ciede_metal`.

## Decision

With an explicit device `--backend` (`cuda`, `sycl`, `hip` or `metal`), the CLI
resolves every `--feature <name>[=options]` whose name is a CPU extractor
through a new, additive libvmaf entry point, `vmaf_feature_backend_twin()`, and
registers what it returns.

- **Pairing** is the model-dispatch lookup, not a second table. The CPU
  extractor's provided features are looked up in order with
  `vmaf_get_feature_extractor_by_feature_name()` under the context's extractor
  flags (`compute_fex_flags()`, so a non-zero `gpumask` disables it as it does
  for models); the first result that carries the backend's flag is the twin
  (`vmaf_get_feature_extractor_twin()` in `feature_extractor.cpp`).
- **Acceptance** is the model-dispatch fallback applied up front. The twin must
  honour every option in the `--feature` string (ADR-1183 / ADR-1316) and, for
  the input's width, height, bit depth and pixel format, pass the ADR-1324
  first-picture check, which runs on a scratch context that is never
  initialised.
- **Fallback**: when there is no twin, the twin cannot honour an option or the
  geometry, or the backend's extractors are disabled by `--gpumask`, the CLI
  registers the CPU extractor and prints one line to stderr that names the
  feature and the reason, for example
  `vmaf: warning: --feature brisque: the sycl backend has no twin of this extractor; computing it on the CPU`.
  The run continues; nothing fails that worked before.
- **Unchanged**: a twin-suffixed name (`--feature ciede_sycl`) keeps its exact
  registration and its [ADR-0543](0543-adr-0498-enforcement-hardening.md)
  exit-100 gate; `--backend cpu`, `--backend auto` and no `--backend` keep
  exact-name registration and never call the lookup; `vmaf_use_feature()` and
  the FFmpeg filters keep selecting by exact name.
- **Receipt**: `backend_used` names the backend the registered extractors ran
  on, read after the final flush through a second additive entry point,
  `vmaf_registered_feature_extractor()`. It is the device backend of the first
  extractor that ran on a device, or `cpu` when every extractor ran on the CPU,
  including a run where a device was initialised and nothing was dispatched to
  it. Its value set is unchanged (`cpu`, `cuda`, `sycl`, `hip`, `metal`). A new
  top-level array, `feature_backends`, lists every registered extractor in
  registration order as `{"extractor": <registry name>, "backend": <backend>}`.
  A run that mixes device twins and CPU extractors is one whose
  `backend_used` names a device and whose `feature_backends` holds at least one
  `cpu` entry. The receipt is written for every JSON output, whatever
  `--backend` says.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Resolve in the CLI through a libvmaf query (chosen) | Fixes the CLI foot-gun; one pairing implementation shared with model dispatch; `vmaf_use_feature()` and FFmpeg unchanged | Two new exported symbols | Maintainer decision; the smallest change that gives the CLI the model-dispatch answer |
| Resolve inside `vmaf_use_feature()` | Every caller, FFmpeg included, gets the twin | Changes the documented exact-name contract of a public function and the FFmpeg filters' behaviour; needs the FFmpeg patch series updated | Maintainer chose CLI-only |
| Keep exact names, only fix `backend_used` | No routing change | The foot-gun stays: `--feature ciede --backend sycl` still measures the CPU | Does not fix the reported problem |
| CLI-side `<name>_<backend>` name table | No library change | A second pairing implementation (HISS-19); names do not follow the pattern (`ssim` → `integer_ssim_cuda`, `ciede` → `integer_ciede_metal`); the option and geometry gates are unreachable from the CLI | Wrong for several extractors and cannot apply ADR-1183 |
| Export `vmaf_get_feature_extractor_by_feature_name()` and the option helpers | Reuses them verbatim | Puts the internal `VmafFeatureExtractor` and `VmafDictionary` layouts into the ABI | Undoes ADR-0379 for three internal symbols |
| Fail with exit 100 when a feature has no twin | Strictest; no silent CPU work | Breaks invocations that work today (`--backend sycl --feature brisque` with a model); CPU-only extractors would need a separate run | The warning plus the `feature_backends` receipt already make the CPU work visible |
| Report a mixed run as `"backend_used": "mixed"` | One field to read | Adds a value that consumers comparing against a backend name (`dev/scripts/smoke-probe-loop.sh`, the RC1 tester report, both MCP servers) do not expect | HISS-14: a new field instead of a new value |
| Require the twin to provide every feature the CPU extractor lists | Output columns identical to the CPU run | `provided_features` lists are not emission contracts; it would reject the ADM, float ADM, float motion and MS-SSIM twins that model dispatch uses every run | Rejects working twins |

## Consequences

- **Positive**: `--feature ciede --backend sycl` runs `ciede_sycl`, with the same
  scores as `--feature ciede_sycl`. A benchmark written with CPU names now
  measures the device it names, or says on stderr and in the receipt that it
  did not. `feature_backends` is the per-extractor dispatch evidence that
  [ADR-1342](1342-rc1-external-tester-report-bundle.md) point 11 says
  `backend_used` alone cannot give.
- **Negative**: The twin's scores differ from the CPU extractor's within the
  cross-backend tolerances of [ADR-0214](0214-gpu-parity-ci-gate.md), so the
  same command line now gives slightly different numbers with an explicit
  device backend. A twin emits the metrics it implements, so auxiliary columns
  can differ: with 576x324 input the CPU `motion` extractor reports
  `VMAF_integer_feature_motion_sad_score`, `integer_motion2` and
  `integer_motion3`, and `motion_sycl` reports `integer_motion`,
  `integer_motion2` and `integer_motion3`. `--backend cpu` restores the CPU
  output. `backend_used` now reads `cpu` for a run that initialised a device
  but ran nothing on it, where it used to name the device.
- **Neutral / follow-ups**: The receipt's pairing is verified on a SYCL device
  here; CUDA, HIP and Metal use the same lookup and are covered by the
  device-free unit tests and the `test_vmaf_feature_backend_<backend>` device
  runs on hosts that have those GPUs.

## References

- Popup, 2026-09-29: "CLI picks the twin (Recommended)" (maintainer answer to
  where `--feature <cpu-name>` with an explicit GPU `--backend` should be
  resolved to that backend's twin).
- [Research-2120](../research/2120-sycl-ciede-throughput.md) (the measurement
  that found the bypass) and
  [Research-2121](../research/2121-cli-feature-backend-twin.md) (the pairing
  and receipt evidence for this decision).
- `docs/state.md` row `T-CLI-FEATURE-NAME-BYPASSES-GPU-BACKEND-2026-09-29`.
- [ADR-0379](0379-libvmaf-symbol-visibility.md),
  [ADR-0498](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md),
  [ADR-0530](0530-hip-feature-flag-promotion-and-picture-buffer.md),
  [ADR-0543](0543-adr-0498-enforcement-hardening.md),
  [ADR-0804](0804-vmaf-context-get-backend.md),
  [ADR-1183](1183-model-options-gate-gpu-twin-selection.md),
  [ADR-1316](1316-gpu-option-value-capability-fallback.md),
  [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md).
