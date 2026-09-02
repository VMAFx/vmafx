# Research-0086 — Stale-marker sweep, 2026-05-08

**Status**: Accepted (audit + landed trivial fix).
**Author**: Claude Code (sweep agent), 2026-05-08.
**Scope**: full-tree audit of "scaffold-shaped" markers (skip / TODO /
`NotImplementedError` / `-ENOSYS`) for stale-vs-still-valid
classification. Three previous spot sweeps closed `T6-1` (Netflix
public dataset became locally available 2026-04-29), `T6-2a-A` (placeholder
saliency model retired by ADR-0265), and the Vulkan "Step A" precise-decoration
(ADR-0269); the user requested a full sweep before more drift accrued.

## 1. Method

For each marker class the sweep:

1. Located every occurrence in fork-touched paths (`tools/`, `python/vmaf/`,
   `python/test/`, `ai/`, `mcp-server/`, `core/src/`, `core/test/`).
2. Classified the file as fork-added vs upstream-mirrored
   (`git log upstream/master -- <path>`).
3. Read the surrounding context for the documented reopen trigger.
4. Verified the trigger PR's status via `gh pr view` (open / merged / not
   created yet).

`python/test/` skip markers were inspected only to confirm they are
Netflix-golden-territory; per CLAUDE §1 they are off-limits regardless of
classification.

## 2. Findings

### 2.1 Python skip markers (14 total)

| Marker | File | Class | Notes |
| --- | --- | --- | --- |
| `@unittest.skipIf(sys.version_info < (3,))` ×4 | `python/test/{cross_validation,perf_metric}_test.py` | DEFERRED-VALID (upstream py2-bridge artefact) | Netflix file; do not touch |
| `@unittest.skip("numerical value has changed.")` | `python/test/result_test.py:206` | DEFERRED-VALID (Netflix file) | CLAUDE §1 protected |
| `@unittest.skip("vifdiff alternative needed, vmaf_feature executable deprecated")` | `python/test/feature_extractor_test.py:344` | DEFERRED-VALID (Netflix file) | CLAUDE §1 protected |
| `@unittest.skip("Inconsistent numerical values.")` | `python/test/routine_test.py:398` | DEFERRED-VALID (Netflix file) | CLAUDE §1 protected |
| `@pytest.mark.skipif(jsonschema not in sys.modules)` | `python/test/model_registry_schema_test.py:134` | DEFERRED-VALID (env gate) | Optional dep, structural fallback covers |
| `@pytest.mark.skipif(VMAF_TUNE_INTEGRATION != "1")` | `tools/vmaf-tune/tests/test_codec_adapter_x265.py:309` | DEFERRED-VALID (env gate) | Real-binary opt-in gate |
| `@pytest.mark.skipif(onnxruntime missing)` | `ai/tests/test_ptq_scripts.py:63` | DEFERRED-VALID (env gate) | Optional dep |
| `pytest.skip("Netflix golden YUV not present")` | `mcp-server/vmaf-mcp/tests/test_server.py:21` | DEFERRED-VALID (env gate) | Fixture availability check |
| `pytest.skip(f"{ONNX_PATH} not present")` ×2 | `ai/tests/test_saliency_student_v1.py:36,42` | DEFERRED-VALID (env gate) | Smoke-only when artefact absent |
| `pytest.skip(...)` for missing vmaf binary / Netflix YUVs | `ai/tests/test_e2e_frame_to_score.py:77,79` | DEFERRED-VALID (env gate) | E2E gate |
| `_HDR_ITER_ROWS_DEFERRED = pytest.mark.skip(...)` | `tools/vmaf-tune/tests/test_hdr.py:295` | **TRIVIAL-FIX (cross-link)** + DEFERRED-VALID | Reopen on PR #466 (open). Marker reason did not name the follow-up PR or carry a `state.md` link; this PR fixes that. |

### 2.2 Python `raise NotImplementedError` (~30 total)

Abstract-base patterns under `python/vmaf/core/` (executor, feature_extractor,
quality_runner, matlab_quality_runner, train_test_model, mixin, perf_metric):
**all** sit on `__metaclass__ = ABCMeta` or `@abstractmethod` decorators. These
are correct interface declarations, not scaffolds. Skipped from this sweep's
remediation list. Same for `_Runner.infer` in
`ai/scripts/measure_quant_drop_per_ep.py:146` (abstract `_Runner` class).

The four production-path raises in `tools/vmaf-tune/src/vmaftune/fast.py`
(lines 214, 291, 440 — `_build_prod_predictor`, `_gpu_verify`,
`fast_recommend` source-required) all carry self-describing reopen
prose pointing at "the encode-extract follow-up PR". That PR is **#467**
(`feat(vmaf-tune): fast subparser + production runners (HP-3, ADR-0276)`,
DRAFT 2026-05-08). DEFERRED-VALID; the marker text is descriptive enough
that no cross-link edit is required.

`tools/vmaf-roi-score/src/vmafroiscore/mask.py:78,128` reference the
T6-2c phased rollout in ADR-0288. DEFERRED-VALID; reopen trigger named
inline.

`ai/src/vmaf_train/data/frame_loader.py:24` rejects non-`gray` pix_fmt
with a stable "for now" message; the public surface is currently single-
channel-only by design. DEFERRED-VALID.

### 2.3 Python TODO/FIXME comments

Seven occurrences in `python/`: every one is in an upstream Netflix-mirrored
file (`python/vmaf/__init__.py`, `python/vmaf/core/result.py`,
`python/vmaf/core/train_test_model.py`, `python/vmaf/tools/scanf.py`,
`python/test/local_explainer_test.py`). Not touched in this sweep — fork
policy is to avoid drive-by edits to upstream-mirrored files.

`tools/`, `ai/`, `mcp-server/`: **zero** TODO/FIXME/XXX comments. The fork's
self-imposed lint discipline holds.

### 2.4 C `return -ENOSYS` (~50 total)

| Cluster | Files | Class | Reopen trigger |
| --- | --- | --- | --- |
| HIP feature scaffolds | `core/src/feature/hip/{adm,float_ssim,float_psnr,float_ansnr,integer_motion_v2,integer_psnr,float_moment,motion,float_motion,ciede,vif}_hip.c` | DEFERRED-VALID | T7-10b runtime PR (ADR-0212 / ADR-0274). Per-TU module headers carry the reopen prose. Smoke test `test_hip_smoke.c` pins the contract. |
| HIP runtime scaffolds | `core/src/hip/{common,picture_hip,kernel_template}.c` | DEFERRED-VALID | T7-10b. Header-pinned. |
| MCP server scaffold | `core/src/mcp/mcp.c` | DEFERRED-VALID | T5-2b runtime PR (ADR-0209). Smoke test `test_mcp_smoke.c` pins `-ENOSYS`. |
| DNN built-without stubs | `core/src/dnn/dnn_api.c:319,334,350,362`, `core/src/dnn/dnn_attach_api.c:88`, `core/src/dnn/ort_backend.c` (5 sites) | DEFERRED-VALID (compile-time gate) | These fire only when `VMAF_HAVE_DNN=0`; the with-DNN build links the real implementations. Stubs are correct. |
| Windows `vmaf_dnn_verify_signature` stub | `core/src/dnn/model_loader.c:497` | DEFERRED-VALID (platform gate) | Documented "Linux/macOS-only today" — Windows supply-chain workflow is out of scope. |
| `vmaf_roi.c:258` saliency-model fallback | `core/tools/vmaf_roi.c` | DEFERRED-VALID (runtime propagation) | Built-without-DNN propagation, not a scaffold. |
| `core/src/vulkan/common.c:41` | volk-init failure | DEFERRED-VALID (error path) | Genuine runtime error; not a scaffold. |
| `test_mcp_smoke.c:61`, `test_dnn_session_api.c:46` | Test assertions on the contract | DEFERRED-VALID | Tests *pin* the contract. |

### 2.5 C `// TODO/FIXME/XXX` comments

11 occurrences. **All** are in upstream-mirrored files
(`core/src/feature/adm_tools.c` ×3, `core/src/feature/integer_adm.c` ×4,
`core/src/feature/cuda/integer_adm/adm_dwt2.cu` ×1, `core/src/svm.cpp`
×3 — third-party libsvm vendored from Netflix). Not in scope for the fork's
sweep policy.

## 3. Classification breakdown

| Class | Count |
| --- | --- |
| TRIVIAL-FIX | **1** (HDR cross-link) |
| DEFERRED-VALID | **~95** (everything else) |
| DEFERRED-NEEDS-NEW-ROW | **1** (T-HDR-ITER-ROWS — added to `state.md` in this PR) |

The 3 most surprising findings:

1. **Zero new orphan markers since the prior three spot-sweeps**. Every
   ENOSYS scaffold ships with a per-file module header naming its reopen
   trigger. The fork's discipline on this is genuinely robust. The only
   marker missing a cross-link was `_HDR_ITER_ROWS_DEFERRED`, and that
   PR (#466) had been opened the same day the marker was committed.
2. **Every Python TODO/FIXME in fork-added paths is gone**. The seven
   remaining comments are all in upstream-mirrored files (`python/vmaf/...`,
   `python/test/...`). The fork's lint sweep PRs from 2026-04 onward
   eradicated fork-added drive-by TODOs.
3. **Production-path `NotImplementedError` in `tools/vmaf-tune/fast.py`
   are not stale** — PR #467 is OPEN as of 2026-05-08, the same day this
   sweep ran. The previous "encode-extract follow-up" mention in the
   marker prose is accurate. No retro-edit needed.

## 4. Actions landed in this PR

1. `tools/vmaf-tune/tests/test_hdr.py` — extended the `_HDR_ITER_ROWS_DEFERRED`
   skip reason with the explicit reopen trigger (PR #466, HP-2) and a
   `docs/state.md` cross-link.
2. `docs/state.md` — new row `T-HDR-ITER-ROWS` under "Open bugs" / "Deferred"
   covering the marker until PR #466 lands.
3. `changelog.d/changed/stale-marker-sweep-2026-05-08.md` — fork CHANGELOG
   fragment.

No NIE replacements, no skip un-skips: the deferral is real for every other
marker. Per memory `feedback_no_test_weakening`, un-skipping a test that is
deliberately gated on a follow-up PR would be reopening a flake, not a fix.

## 5. References

- ADR-0209 (MCP scaffold contract).
- ADR-0212 / ADR-0274 (HIP scaffold + kernel-template T7-10b).
- ADR-0276 / ADR-0304 (vmaf-tune fast-path production wiring).
- ADR-0288 (vmaf-roi-score phased rollout).
- ADR-0295 (HDR codec flag dispatch — original PR for `_HDR_ITER_ROWS_DEFERRED`).
- PR #466 (HP-2 follow-up, OPEN).
- PR #467 (HP-3 fast subparser, DRAFT).
- Prior closures referenced for context: T6-1, T6-2a, T6-2a', "VK Step A" /
  ADR-0269.
