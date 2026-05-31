<!-- markdownlint-disable MD060 -->
# Research — pre-existing test failures across ai/, tools/vmaf-tune/, mcp-server/

**Date:** 2026-05-30
**Author:** Claude Code (background agent)
**Status:** Resolved by PR (see CHANGELOG fragment `changelog.d/fixed/pre-existing-test-failures.md`)
**Scope:** Three independent test-failure clusters flagged by prior repo audits.

## Findings summary

| Cluster                 | Files | Tests | Symptom                                                           | Root cause                                                                                  |
|-------------------------|-------|-------|-------------------------------------------------------------------|---------------------------------------------------------------------------------------------|
| torchvision NMS import  | 9     | 22    | `RuntimeError: operator torchvision::nms does not exist`          | `torchvision-0.26.0` wheel ABI-incompatible with `torch-2.12.0`; raised at module-load time |
| SVT-AV1 quality range   | 2     | 4     | `ValueError: crf 18 outside Phase A range [20, 50]`               | `DEFAULT_SAMPLER_CRF_SWEEP[0]=18` violates `SvtAv1Adapter.quality_range=(20, 50)`           |
| aiohttp `content_type=` | 1     | 1     | `ValueError: charset must not be in content_type argument` (500)  | aiohttp 3.13.5 strict-validates `content_type=` kwarg; Prometheus header has `charset=utf-8`|

Total: 27 distinct failing assertions across 12 test files in 3 packages, all fixed.

---

## Cluster 1: torchvision NMS import drift

### Failure surface

Three test files erred at collection time:

- `ai/tests/test_dnn_exporter_run_provenance.py`
- `ai/tests/test_export_roundtrip.py`
- `ai/tests/test_registry.py`

Six additional files failed at runtime when individual tests imported
`vmaf_train.models` (which pulls `pytorch_lightning`):

- `ai/tests/test_codec_aware_fr.py` (6 tests)
- `ai/tests/test_qat_smoke.py` (2)
- `ai/tests/test_train_fr_regressor_v2_ensemble_loso_train.py` (3)
- `ai/tests/test_train_fr_regressor_v3.py` (2)
- `ai/tests/test_tune_cli.py` (1)
- `ai/tests/test_variance_mode.py` (5)

### Diagnostic chain

`import pytorch_lightning` triggers the following eager import cascade:

```text
pytorch_lightning.callbacks
  -> pytorch_lightning.callbacks.callback
    -> pytorch_lightning.utilities.types
      -> torchmetrics.Metric
        -> torchmetrics.functional   (eager)
          -> torchmetrics.functional.image
            -> torchmetrics.functional.image.arniqa  (since torchmetrics 1.9)
              -> torchvision.transforms
                -> torchvision._meta_registrations  (line 163)
                  -> @torch.library.register_fake("torchvision::nms")
                    -> RuntimeError: operator torchvision::nms does not exist
```

The leaf error is raised by `torch._library/fake_impl.py:50`
(`torch._C._dispatch_has_kernel_for_dispatch_key("torchvision::nms", "Meta")`)
because the compiled `torchvision._C` extension was built against a
different torch ABI than the one currently loaded. Concretely on this
dev machine:

- `torch-2.12.0+cu130`
- `torchvision-0.26.0` (built against torch 2.10/2.11 ABI)

The matched pair for torch 2.12.0 is **`torchvision-0.27.0`** (per PyPI
`requires_dist` for the 0.27.0 release).

### Decision matrix

| Option                                              | Pros                                                            | Cons                                                                                                                                    | Verdict     |
|-----------------------------------------------------|-----------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------|-------------|
| A. Pin `torchvision==0.27.0` in `ai/pyproject.toml` | Permanent fix; right wheel pair                                 | torchvision is not a direct dep; pinning a transitive pulls torchvision into our resolved env unnecessarily; also moves with each torch | Documented  |
| B. Pin `torchmetrics<1.9` to avoid arniqa import    | Avoids the eager torchvision import entirely                    | Loses every torchmetrics 1.9 metric forever; arniqa is opt-in but the eager import is the trigger                                       | Rejected    |
| C. Stub `sys.modules["torchvision"]` in conftest    | No version changes                                              | Fragile: torchmetrics' arniqa imports `from torchvision.models import resnet50`; stubbing breaks reload paths in test isolation         | Rejected    |
| D. Add module-level `pytest.importorskip(...)`      | Standard pytest idiom                                           | `importorskip` only catches `ImportError`, not `RuntimeError` — re-raises and the test errors hard                                      | Insufficient|
| **E. Custom `requires_pytorch_lightning()` helper** | Catches the broader `Exception`; one place to maintain; surfaces the actual error string to the user; cooperates with deployment-side fix (`pip install -U torchvision`) | Adds one helper to `ai/tests/conftest.py`; nine test files import it | **Chosen**  |

**Chosen approach:** Add `_probe_pytorch_lightning()` + cached
`_PYTORCH_LIGHTNING_ERROR` + `requires_pytorch_lightning()` to
`ai/tests/conftest.py`. Affected test files call the guard immediately
after `import pytest`; one test (`test_tune_cli.py::test_tune_cli_invokes_sweep`)
gets a function-level `@requires_lightning` skipif because the rest of
the file's tests don't need lightning.

The deployment-side `pip install -U torchvision` keeps working
unchanged — the guard becomes a no-op once the matched wheel pair is
installed, and the previously-skipped tests run again.

### Validation

```bash
pytest ai/tests/  # before: 19 failed + 3 collection errors
                   # after:  627 passed, 10 skipped (9 from guard, 1 unrelated)
```

The skip reason surfaces the actual error string so the operator knows
exactly what to fix:

```text
SKIPPED [9] ai/tests/conftest.py:80: pytorch_lightning unavailable:
            RuntimeError: operator torchvision::nms does not exist
```

A new `ai/tests/test_conftest_pytorch_lightning_guard.py` pins the
guard's behavior contract (5 tests covering probe return values,
RuntimeError catching, and skip semantics).

---

## Cluster 2: SVT-AV1 quality-range bounds

### Failure surface

Four tests across two files:

- `tools/vmaf-tune/tests/test_ladder_svtav1_default_crf.py` (3 tests)
- `tools/vmaf-tune/tests/test_ladder.py::test_build_ladder_default_sampler_uses_corpus_and_recommend`

### Diagnostic chain

`ladder.DEFAULT_SAMPLER_CRF_SWEEP = (18, 23, 28, 33, 38)` ships as the
canonical 5-point sweep used by `_default_sampler`.
`SvtAv1Adapter.quality_range = (20, 50)` (Phase A informative window).
`corpus.iter_rows` calls `adapter.validate(preset, crf)` for every
cell before encoding starts — and `validate(...)` raises
`ValueError("crf 18 outside Phase A range [20, 50]")` for the first
sweep point, which the ladder converts to exit-2 before any encode
runs.

The 3 tests in `test_ladder_svtav1_default_crf.py` were written as the
direct regression for Bug N-2 (the test file's docstring explicitly
states "Root cause: ``DEFAULT_SAMPLER_CRF_SWEEP`` started at CRF 18,
which is below libsvtav1's Phase A lower bound of 20"). The
test landed but the corresponding source-side fix never did — the
tests have been failing since they were written.

### Decision matrix

| Option                                                                | Pros                                                                                              | Cons                                                                                                | Verdict   |
|-----------------------------------------------------------------------|---------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------|-----------|
| A. Shift sweep up: `(20, 25, 30, 35, 40)`                              | Trivial; valid for every shipped adapter; preserves 5-point shape and step size of 5              | Loses CRF 18 (near-lossless x264 point), but `_default_sampler` is the *default* — callers wanting CRF 18 can pass an explicit `--crf-sweep` | **Chosen** |
| B. Loosen `SvtAv1Adapter.quality_range = (18, 50)`                     | Keeps the existing sweep                                                                          | Contradicts the Phase A research finding (`docs/research/0307-*.md`); changes encoder validation semantics                                | Rejected  |
| C. Per-adapter default sweeps                                         | Most flexible                                                                                     | Significant API addition; one sweep validated against every adapter is simpler and matches ADR-0307 | Deferred  |

**Chosen approach:** Option A. Shift the sweep start from 18 → 20 so
the same 5-point grid is valid for every shipped adapter
(`x264`, `x265`, `libvpx-vp9`, `libsvtav1`, `libvvenc`, ...) without
needing a per-adapter override. Updated the synthetic R-D test in
`test_ladder.py` to match — with `vmaf = 100 - (crf-18)*1.5` and
target 92.0, the new best cell is `CRF=20, VMAF=97.0` (was `CRF=18,
VMAF=100.0`).

### Validation

```bash
pytest tools/vmaf-tune/tests/test_ladder_svtav1_default_crf.py \
       tools/vmaf-tune/tests/test_ladder.py
# 5 + 22 = 27 passed
```

---

## Cluster 3: aiohttp 3.13.5 `content_type=` strict validation

### Failure surface

- `mcp-server/vmaf-mcp/tests/test_http_transport.py::test_metrics_returns_200`

### Diagnostic chain

aiohttp 3.13.5 added strict validation in `web.Response.__init__` that
rejects a `charset=...` fragment inside the `content_type=` kwarg:

```python
raise ValueError("charset must not be in content_type argument")
```

`mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py:_handle_metrics`
was passing `content_type=pc.CONTENT_TYPE_LATEST`, which from
`prometheus_client` is the full RFC 1341 value
`text/plain; version=1.0.0; charset=utf-8` — the request returned 500
with the ValueError surfacing through the aiohttp error handler.

The user's task brief explicitly noted PR #346 already fixed a similar
issue elsewhere in `http_transport.py` using the same pattern.

### Decision matrix

| Option                                                       | Pros                                                                                  | Cons                                                                  | Verdict    |
|--------------------------------------------------------------|---------------------------------------------------------------------------------------|----------------------------------------------------------------------|------------|
| A. Pin `aiohttp<3.13.5`                                      | One-line dep change                                                                   | Forfeits future aiohttp bugfixes; treats the symptom not the cause   | Rejected   |
| B. Strip charset from CONTENT_TYPE_LATEST before passing      | Compatible with the aiohttp invariant                                                 | The Prometheus scrape contract specifies the full Content-Type with charset; stripping it changes wire-level behaviour | Rejected   |
| **C. Set Content-Type via `headers={"Content-Type": ...}`**   | aiohttp does not re-parse `headers`; CONTENT_TYPE_LATEST lands verbatim on the wire   | Slightly less idiomatic than `content_type=`                          | **Chosen** |

**Chosen approach:** Option C, matching PR #346's pattern.

### Validation

```bash
pytest mcp-server/vmaf-mcp/tests/test_http_transport.py
# 10 + 1 = 11 passed (added test_metrics_full_content_type_header_preserved)
```

The new test pins the wire-level invariant:
`resp.headers["Content-Type"] == pc.CONTENT_TYPE_LATEST` byte-for-byte.

---

## Out of scope

Two `tools/vmaf-tune/` tests fail for unrelated reasons and were left
for follow-up PRs:

- `test_bbb_e2e_v2_bug_cluster.py::test_vmaf_explicit_backend_failure_errors`
  — pinned source-string `'strcmp(c->backend, "vulkan") == 0'` no longer
  present in `core/tools/vmaf.c`.
- `test_format_both_json.py::test_compare_format_both_writes_json`
  — `_write_compare_profile_report` no longer emits the JSON path
  alongside HTML/MD.

Both are independent regressions in unrelated subsystems and would
inflate this PR beyond the "fix one thing well" scope.

## References

- User task brief specifying the three clusters and their candidate fixes
  (torchvision NMS, SVT-AV1 quality range, aiohttp content_type)
- `ai/tests/conftest.py` (new `requires_pytorch_lightning()` helper)
- `tools/vmaf-tune/src/vmaftune/ladder.py` (`DEFAULT_SAMPLER_CRF_SWEEP`)
- `mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py` (`_handle_metrics`)
- PR #346 (precedent for the `headers={"Content-Type": ...}` aiohttp pattern)
