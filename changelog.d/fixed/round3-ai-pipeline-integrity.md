### Fixed

Round-3 bug-hunt — AI training/extraction pipeline data-integrity (training-harness only; golden-safe).

- **online_trainer feature-length validation (R3-3)** (`ai/sidecar/online_trainer.py`): `ingest()` now rejects a feature vector whose length ≠ `n_features` before it reaches the replay buffer, and the connection handler catches the structured error instead of letting it escape the socket.
- **online_trainer checkpoint race (R3-7)** (`ai/sidecar/online_trainer.py`): the `_checkpoint_counter` read-increment-export is now under the lock, preventing colliding ONNX checkpoint versions across concurrent connection threads.
- **extract_full_features stale-cache misalignment (R3-8)** (`ai/scripts/extract_full_features.py`): zips against the cache payload's own `feature_names` (and asserts/recomputes on mismatch) instead of the global `FULL_FEATURES`, so a stale per-clip cache can't silently misalign feature columns.
- **extract_ugc_features degenerate-probe crash (R3-18)** (`ai/scripts/extract_ugc_features.py`): zero/missing ffprobe height is handled per-clip (skip + count) instead of an uncaught `ZeroDivisionError`/`KeyError` aborting the batch.
- **chug_extract_features per-clip isolation (R3-19)** (`ai/scripts/chug_extract_features.py`): the write loop wraps each clip in try/except so one ffmpeg/vmaf failure skips that clip rather than aborting the whole batch.
