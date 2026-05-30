- **`ai/` tempfile + path-safety audit (bandit HIGH+MEDIUM sweep)** —
  routed two more `ai/scripts/` scratch defaults
  (`bvi_dvc_to_full_features.py`, `konvid_to_full_features.py`)
  through `tempfile.gettempdir()` instead of the hardcoded `/tmp/...`
  literals (the `VMAF_TINY_AI_SCRATCH` override path is unchanged;
  only the fallback moves). Annotated five `torch.load(...,
  weights_only=False)` callsites in the model-export scripts
  (`export_tiny_models.py`, `export_u2netp_mirror.py`,
  `export_vmaf_tiny_v{2,3,4}.py`) with `# nosec B614` plus an inline
  citation: `weights_only=True` rejects the developer-supplied
  Lightning checkpoints because they pickle `hyper_parameters` /
  scaler stats / train metrics as plain Python objects alongside
  tensors. Added defensive `http(s)://`-scheme guards to the three
  `urllib.request.urlopen` callsites in `fetch_konvid_1k.py` and
  `fetch_youtube_ugc_subset.py` so a future refactor that threads
  URLs from CLI args cannot accidentally allow `file://` or other
  custom schemes (bandit B310). Test-fixture `/tmp/...` literals in
  `ai/tests/` switched to either `pytest`'s `tmp_path` fixture (when
  the path is opened) or to a `fixtures/...` placeholder (when the
  string is only stored in a JSON-serialisable corpus row). After
  the sweep, `bandit -r ai/ -ll` reports zero HIGH and only the four
  MEDIUMs owned by PR #303 remain on the touched-file set
  (16 MEDIUM → 0 on the files this PR touches). No CLI contract
  change, no model-format change, no behavioural delta.
