---
paths:
  - core/src/feature/motion_window.h
  - core/src/feature/integer_motion.c
  - core/src/feature/integer_motion_v2.c
  - core/src/feature/feature_extractor.h
  - core/test/test_motion_window_incremental.c
  - core/test/test_motion_window_advance_contract.py
  - core/test/test_score_pooled_eagain.c
invariant: motion2/motion3 of frame i final once SADs 0..max(i+1,min_idx) in; same statements as flush; state carried.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# motion2 / motion3 frame by frame (ADR-2090)

- Rule: frame i final (appended, write-once) once SAD scores of frames
  `0 .. max(i + 1, min_idx)` in collector; `min_idx` 1, or 2 with
  `motion_five_frame_window`. Last frame, and every frame of a stream with
  `<= min_idx` frames: flush. Never earlier (`has_hi` would be false:
  `motion2 = sad_i`, wrong).
- `vmaf_motion_window_advance()` derives complete frames,
  `vmaf_motion_window_flush()` the rest; both run `motion_flush_one()`
  (upstream's per-frame statements, unchanged) in index order on
  `VmafMotionWindowState` (`n_sad`, `next`, `stamp_value`,
  `prev_processed`) = values upstream's flush loop carries. Stamp
  computed once by whichever call derives frame 0 (`motion_window_stamp()`,
  upstream statement). Do not recompute `prev_processed` from stored
  `motion2`, do not scan collector from 0 per call.
- `window->state == NULL` in flush = whole stream in one call (pre-ADR-2090
  path; test oracle). Advance requires a state (`-EINVAL`). Retried flush /
  advance after flush append nothing.
- State lives in extractor's priv; every touch serialised with
  feeding thread: engine calls `VmafFeatureExtractor.advance`
  (`advance_extractors()` in `libvmaf.c`) after each accepted frame and read
  fence, and from VMAFx completion thread under engine lock
  (`vmaf_engine_advance()` <- `window.c::advance_engine()`). Never from a
  worker (frame listener): workers have private copies. With workers
  registered context advances and flushes (its init never runs): build
  name dict on first use (`motion_ensure_dict()`, `motion_v2` advance).
- Every extractor calling `vmaf_motion_window_flush()` also registers
  `.advance` -> `vmaf_motion_window_advance()` on same window builder
  (`*_window_of()`, `.state = &s->window_state`): CPU `motion`, `motion_v2`,
  CUDA / SYCL / HIP / Metal twins. New twin or Rust twin that copies
  C descriptor must set or clear `advance`.
- Upstream sync touching `flush()` of `integer_motion.c` /
  `integer_motion_v2.c`: take upstream's per-frame arithmetic into
  `motion_flush_one()` / `motion_window_stamp()`; keep advance split.
- Guards: `test_motion_window_incremental` (advance + flush == transcription
  of upstream flush, bit for bit, in / out of order, context timing serial and
  threaded), `test_motion_window_advance_contract.py` (every TU, engine call
  sites), `test_score_pooled_eagain` (streaming pattern),
  `test_vmafx_window`, twin `test_*_motion_five_frame_window` (finality by
  `lag_motion*`). Planted off-by-one (`end = n_sad` in advance) fails four
  test binaries.
