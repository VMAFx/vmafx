Fix 23 pre-existing test failures across three packages.

* `ai/tests/` (22 tests, 3 collection errors + 19 runtime failures): the
  installed `torchvision-0.26.0` wheel is ABI-incompatible with
  `torch-2.12.0`, so `import pytorch_lightning` (transitively via
  `torchmetrics.functional.image.arniqa`) raises
  `RuntimeError: operator torchvision::nms does not exist` at module
  load.  `pytest.importorskip` only catches `ImportError`, so the
  failure surfaced as a hard error instead of a clean skip.  Added a
  `conftest.requires_pytorch_lightning()` helper that probes the import
  once with a broader `except Exception` and routes a session-level
  `pytest.skip(..., allow_module_level=True)`; nine affected test files
  now opt in to the guard.  The deployment-side fix remains
  `pip install -U torchvision` to pull the matching `0.27.0` wheel.

* `tools/vmaf-tune/tests/` (4 tests):
  `ladder.DEFAULT_SAMPLER_CRF_SWEEP` started at CRF 18, below the
  `SvtAv1Adapter` Phase A lower bound of 20, so
  `ladder --encoder libsvtav1` exited 2 before any encode could run
  (Bug N-2).  Shifted the canonical 5-point sweep to
  `(20, 25, 30, 35, 40)` so it is valid for every shipped adapter
  without an explicit `--crf-sweep` override; updated the synthetic
  R-D test in `tests/test_ladder.py` to match the new expected
  best-cell (`CRF=20, VMAF=97.0`).

* `mcp-server/vmaf-mcp/tests/` (1 test): `aiohttp-3.13.5` added strict
  validation that rejects a `charset=...` fragment inside the
  `content_type=` kwarg of `web.Response`, raising
  `ValueError("charset must not be in content_type argument")`.
  `prometheus_client.CONTENT_TYPE_LATEST` is the full RFC 1341 value
  (`text/plain; version=1.0.0; charset=utf-8`), so `/metrics` was
  500-ing with that error.  Switched the handler to set the header via
  `headers={"Content-Type": ...}` so the verbatim Prometheus
  exposition Content-Type is preserved; added
  `test_metrics_full_content_type_header_preserved` to pin the
  wire-level invariant.
