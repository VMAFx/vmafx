<!-- markdownlint-disable MD060 -->
# pytest suite audit — ai/, tools/vmaf-tune/, mcp-server/vmaf-mcp/ (2026-05-27)

<!-- Copyright 2026 Lusoris -->

## Summary

Full pytest sweep of the three Python test suites that live outside
`python/test/` (the Netflix golden-data gate).  Run against
`origin/master` detached HEAD in an isolated worktree.  No test files
were modified.

| Suite | Passed | Failed | Skipped | xfail | Errors | Wall time |
|---|---|---|---|---|---|---|
| `ai/tests/` | 645 | 0 | 1 | 0 | 0 | ~25 s |
| `tools/vmaf-tune/tests/` | 1394 | **1** | 15 | 2 | 0 | ~32 s |
| `mcp-server/vmaf-mcp/tests/` | 102 | 0 | 2 | 0 | **8 (collection, initial run)** | ~2 s |
| **Total** | **2141** | **1** | **18** | **2** | **0** | **~59 s** |

> Note: the 8 mcp-server collection errors only occur when
> `mcp-server/vmaf-mcp/src` is absent from `PYTHONPATH`; they resolve
> immediately once the correct path is added.  The table above reflects
> results with the correct `PYTHONPATH=ai/src:tools/vmaf-tune/src:mcp-server/vmaf-mcp/src`.

---

## Environment

```text
Python  : 3.14.5
pytest  : 9.0.3
pytest-timeout : 2.4.0
PYTHONPATH : ai/src:tools/vmaf-tune/src:mcp-server/vmaf-mcp/src
Branch  : origin/master (detached, worktree agent-a2e3712a5695edabc)
Date    : 2026-05-27
```

---

## Failures

### 1. `tools/vmaf-tune/tests/test_adr_0543_backend_enforcement.py::test_adr_0543_per_feature_pinned_to_inactive_backend_fails`

**File:** `tools/vmaf-tune/tests/test_adr_0543_backend_enforcement.py:235`

**Error (truncated):**

```text
AssertionError: ADR-0543 regression: --feature motion_hip on --backend cpu exited 255 (expected 100)
stderr="/usr/local/bin/vmaf: unrecognized option '--backend'\nproblem loading feature extractor: motion_hip\n"
assert 255 == 100
```

**Root cause:** The installed `vmaf` binary at `/usr/local/bin/vmaf`
does not recognise `--backend`.  The test calls the system binary
directly and expects exit code 100 (ADR-0543 structured per-feature
conflict exit) but receives 255 (unrecognised option from an older
build that predates `--backend`).  This is an environment mismatch,
not a code regression — the test is correct; the system binary is stale.

**Classification:** Environment / stale system binary; not a code bug.

---

## Collection error (without mcp-server/vmaf-mcp/src in PYTHONPATH)

All 8 mcp-server test files fail to collect with:

```text
ModuleNotFoundError: No module named 'vmaf_mcp'
```

because `pytest` is invoked from the repo root and
`mcp-server/vmaf-mcp/src/` is not on `sys.path` by default.  Adding
`:mcp-server/vmaf-mcp/src` resolves all 8 immediately.

**Recommendation:** Add a `conftest.py` at the repo root or a
`pytest.ini` / `pyproject.toml` `pythonpath` entry that covers
`mcp-server/vmaf-mcp/src` so `pytest mcp-server/` works without a
manual `PYTHONPATH` prefix.

---

## Skipped tests

### `ai/tests/` — 1 skip

| Test | Reason |
|---|---|
| `test_e2e_frame_to_score.py::test_e2e_frame_to_score` | `vmaf` binary not built at `libvmaf/build-cpu/tools/vmaf` |

### `tools/vmaf-tune/tests/` — 15 skips

| Group | Reason |
|---|---|
| `test_adr_0543_backend_enforcement.py` ×5 (line 142) | Host has a working {cuda,sycl,hip,vulkan,metal} device; refusal path not exercised |
| `test_adr_0543_backend_enforcement.py` ×5 (line 173) | Same — structured-json variant |
| `test_bbb_e2e_v14_bug_cluster.py::test_v14_b_qsv_probe_succeeds_on_gpu_host` | `h264_qsv` not compiled into ffmpeg or VA-API driver too old / missing |
| `test_bbb_e2e_v4_bug_cluster.py` ×1 | Host has a working Vulkan device; refusal path not exercised |
| `test_bbb_e2e_v5_bug_cluster.py` ×1 | Host has a working Vulkan device; refusal path not exercised |
| `test_codec_adapter_x265.py` | `VMAF_TUNE_INTEGRATION=1` not set |
| `test_codec_adapter_x265_two_pass.py` | `VMAF_TUNE_INTEGRATION=1` not set |

### `mcp-server/vmaf-mcp/tests/` — 2 skips

| Test | Reason |
|---|---|
| `test_server.py` (line 24) | Netflix golden YUV not present |
| `test_smoke_e2e.py` (line 142) | Netflix golden YUV fixtures not present under `python/test/resource/yuv/` |

---

## xfail tests

| Test | Reason |
|---|---|
| `test_encode_dispatcher_per_adapter.py::test_build_ffmpeg_command_emits_codec_correct_argv[av1_videotoolbox]` | `av1_videotoolbox` raises `Av1VideoToolboxUnavailableError` (ADR-0339) |
| `test_encode_dispatcher_per_adapter.py::test_run_encode_passes_codec_correct_argv_to_subprocess[av1_videotoolbox]` | Same — run variant |

Both are correctly xfailed on non-macOS hosts.

---

## Top 20 slowest tests (combined, across all suites)

| # | Duration | Test | Suite |
|---|---|---|---|
| 1 | 18.81 s | `test_bbb_e2e_v5_bug_cluster.py::test_ladder_against_bbb_container_yields_plausible_vmaf` | vmaf-tune |
| 2 | 3.28 s | `test_train_konvid_mos_head.py::test_smoke_run_is_deterministic` | ai |
| 3 | 2.67 s | `test_qat_smoke.py::test_qat_train_cli_smoke` | ai |
| 4 | 2.46 s | `test_train_smoke.py::test_train_epochs_zero_smoke` | ai |
| 5 | 1.66 s | `test_train_konvid_mos_head.py::test_smoke_run_produces_allowlist_conformant_onnx` | ai |
| 6 | 1.24 s | `test_export_roundtrip.py::test_export_roundtrip[<lambda>-in_shape1-input]` | ai |
| 7 | 1.09 s | `test_ptq_scripts.py::test_ptq_dynamic_full_roundtrip` | ai |
| 8 | 1.01 s | `test_export_roundtrip.py::test_export_roundtrip[<lambda>-in_shape0-features]` | ai |
| 9 | 0.91 s | `test_bbb_e2e_v14_bug_cluster.py::test_v14_a_nvenc_probe_succeeds_on_gpu_host` | vmaf-tune |
| 10 | 0.80 s | `test_train_konvid_mos_head.py::test_chug_hdr_entrypoint_accepts_feature_jsonl_and_custom_model_id` | ai |
| 11 | 0.80 s | `test_registry.py::test_register_roundtrip` | ai |
| 12 | 0.59 s | `test_train_konvid_mos_head.py::test_chug_hdr_entrypoint_display_profile_selects_display_schema` | ai |
| 13 | 0.56 s | `test_train_fr_regressor_v3.py::test_one_epoch_train_and_export` | ai |
| 14 | 0.56 s | `test_predictor_train.py::test_train_synthetic_corpus_emits_onnx` | vmaf-tune |
| 15 | 0.53 s | `test_registry.py::test_register_rejects_unknown_kind` | ai |
| 16 | 0.48 s | `test_export_roundtrip.py::test_export_roundtrip[<lambda>-in_shape2-input]` | ai |
| 17 | 0.47 s | `test_variance_mode.py::test_fr_variance_onnx_export` | ai |
| 18 | 0.47 s | `test_profile.py::test_profile_infers_shape_from_graph` | ai |
| 19 | 0.45 s | `test_train_combined_smoke.py::test_combined_trainer_epochs_zero_with_konvid_only` | ai |
| 20 | 0.40 s | `test_train_combined_smoke.py::test_combined_trainer_no_data_produces_initial_onnx` | ai |

No test outside `test_bbb_e2e_v5_bug_cluster.py` exceeded the 5 s mark.  The
vmaf-tune BBB end-to-end test at rank 1 is an integration test that runs a full
ladder encode; it is expected to be slow.

---

## Deprecation warnings (non-fatal, for awareness)

All 58 warnings in `ai/tests/` are `DeprecationWarning` / `UserWarning` from
upstream PyTorch / PyTorch Lightning:

1. **`torch.ao.quantization` deprecated** (removal in PyTorch 2.10) —
   `ai/train/qat.py` uses `prepare_qat_fx` / eager-mode quantization.
   Migration target: `torchao` pt2e API.
2. **TorchScript ONNX exporter deprecated** (removal in PyTorch 2.9 default) —
   multiple `torch.onnx.export(...)` calls across `ai/` and
   `tools/vmaf-tune/src/vmaftune/`.
   Migration target: `torch.export`-based exporter + `dynamic_shapes` argument.
3. **`dynamic_axes` deprecated when `dynamo=True`** — companion to (2).
4. **`self.log()` before `Trainer` attached** — cosmetic PL warning, no impact.

---

## Recommendations for follow-up fix PRs

### P0 — Immediate (blocking CI confidence)

| # | PR title (suggested) | Scope | Effort |
|---|---|---|---|
| 1 | `fix(tests): add mcp-server/vmaf-mcp/src to pytest pythonpath` | Add `pythonpath = ["ai/src", "tools/vmaf-tune/src", "mcp-server/vmaf-mcp/src"]` under `[tool.pytest.ini_options]` in `pyproject.toml`. | < 1 h |
| 2 | `fix(vmaf-tune): build vmaf binary into worktree or skip ADR-0543 backend test when system binary is stale` | The ADR-0543 `--backend` test calls `/usr/local/bin/vmaf`; it should either call the in-tree binary from the build dir or detect that the installed binary is too old and skip.  Exit-255 vs exit-100 is a real ADR regression risk on machines where the system binary has not been updated. | 1–2 h |

### P1 — Short-term (hygiene)

| # | PR title (suggested) | Scope |
|---|---|---|
| 3 | `fix(ai): migrate qat.py from torch.ao.quantization to torchao pt2e API` | Removes 4 DeprecationWarnings; mandatory before PyTorch 2.10. |
| 4 | `fix(ai): migrate torch.onnx.export to dynamic_shapes / torch.export` | Removes TorchScript ONNX deprecation warnings across `ai/` and `tools/vmaf-tune/src/vmaftune/predictor_train.py`. |

### P2 — Nice-to-have

| # | PR title (suggested) | Scope |
|---|---|---|
| 5 | `ci(tests): add fast subset marker to vmaf-tune BBB e2e tests` | The 18 s BBB e2e test is untagged; adding `@pytest.mark.slow` would let the fast-suite gate skip it. |
| 6 | `fix(ai): build vmaf CPU binary in worktree CI to unblock test_e2e_frame_to_score` | One skip in ai/ is purely environmental; easy to unblock in CI. |

---

## Methodology notes

- `pytest --timeout=60 --tb=short -p no:cacheprovider --durations=20`
- All three suites run in parallel background processes against the same
  detached `origin/master` worktree.
- Netflix golden-data assertions in `python/test/` were **not** touched
  and are outside scope.
- No test file was modified.
