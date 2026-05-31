- **`ai/scripts/`: hardcoded `/tmp/...` paths and `/home/kilian/...`
  docstring examples replaced with portable tempfile + env-var honouring
  defaults.** Six scripts were routing scratch state through predictable
  `/tmp/` filenames (collision-prone on multi-tenant hosts, and
  username-leaking through `/home/kilian/...` example invocations):
  `extract_ugc_features.py` (per-pair vmaf JSON output),
  `export_fastdvdnet_pre.py` (`--upstream-dir` default),
  `export_transnet_v2.py` (`--wrapped-savedmodel` default),
  `konvid_to_vmaf_pairs.py` (`--scratch` default), plus
  `train_saliency_student.py` / `train_saliency_student_v2.py` usage
  docstrings. All scratch defaults now read from the existing
  `VMAF_TINY_AI_SCRATCH` env-var (same convention already used by
  `bvi_dvc_to_full_features.py` and `konvid_to_full_features.py`),
  falling back to `tempfile.gettempdir()`; the per-pair UGC JSON moved
  to a `tempfile.NamedTemporaryFile` wrapped in `try/finally` so the
  scratch file is unlinked even on subprocess failure. Docstrings now
  use `~/datasets/duts` as the portable example. No runtime contract
  change; CLI flags accept the same `--upstream-dir`/`--scratch`
  overrides as before.
