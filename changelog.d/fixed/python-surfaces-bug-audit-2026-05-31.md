- **Python-surfaces bug-audit bundle (14 defects across `ai/src/corpus/`,
  `ai/src/vmaf_train/data/`, and `mcp-server/vmaf-mcp/src/vmaf_mcp/`).**
  Closes a class of hang / locale-leak / NaN-propagation / pickle-execution /
  concurrent-tempdir-race defects that survived the master-tip rebase. Per-fix
  summary:
  - `probe_geometry` (corpus/base.py) now accepts a `timeout_s` kwarg and
    returns `None` on `subprocess.TimeoutExpired`; a wedged ffprobe no longer
    stalls the whole ingest pipeline indefinitely (default 60 s, override via
    kwarg).
  - `download_clip` (corpus/base.py) caps the runner wall-clock at
    `timeout_s + 30 s` so a wedged DNS / signal-handler / process-spawn path
    cannot stall ingest forever — curl's `--max-time` only covers the
    in-flight transfer.
  - `load_manifest` / `load_mos_csv` / `write_manifest`
    (vmaf_train/data/{datasets,manifest_scan}.py) pin `encoding="utf-8"` on
    every file open so the parse / serialise does not silently drift across
    hosts with mismatched LC_ALL. The YAML writer also passes
    `allow_unicode=True`.
  - `_run_vmaf` (vmaf_train/data/feature_dump.py) takes a `timeout_s` kwarg
    (default 600 s) and reads its JSON output with explicit UTF-8.
  - `iter_frames` (vmaf_train/data/frame_loader.py) now pipes ffmpeg's
    stderr, caps the post-EOF `wait()` at 30 s (kills the child on overrun),
    and raises `RuntimeError` when ffmpeg exits non-zero — previously
    decoder failures silently produced empty iterators that callers treated
    as healthy zero-frame clips.
  - `_load_frame` (vmaf_train/data/frame_dataset.py) calls `np.load` with
    `allow_pickle=False`, closing a pickled-class remote-code-execution gap
    on untrusted `.npy` under `VMAF_DATA_ROOT`.
  - MCP server: four `read_text(...)` sites in `server.py` now pin
    `encoding="utf-8"` (`_run_vmaf_score`, `_list_extractors`,
    `_describe_model_file`, `_probe_backend`).
  - MCP server: `_pick_worst_frames` filters NaN / inf VMAF scores before
    sorting — Python's `list.sort` is not a total order over NaN, so the
    pre-fix ranking became non-deterministic when a backend emitted NaN for
    a partially decoded frame; it also catches non-float metrics values
    gracefully (no longer crashes on a bogus string score).
  - MCP server: `_describe_worst_frames` uses `tempfile.mkdtemp` per call
    instead of the shared `/tmp/vmaf-mcp-worst-<pid>` directory, fixing a
    race where one tool call's `shutil.rmtree` would delete the PNGs another
    concurrent call had emitted but not yet returned. The corresponding
    `test_describe_worst_frames_tmpdir_cleared_on_next_call` regression test
    was replaced (not weakened) with a stricter invariant that asserts each
    call allocates its own root and peer-call PNGs survive.

  Regression coverage: 10 new tests in
  `ai/tests/test_python_surfaces_bug_audit.py` and 6 new tests in
  `mcp-server/vmaf-mcp/tests/test_python_surfaces_bug_audit.py`. Netflix CPU
  golden tests unaffected (no extractor or scoring change).
