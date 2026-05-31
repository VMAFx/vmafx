- **CHUG/K150K extractor I/O optimisations** (`ai/scripts/extract_k150k_features.py`):
  - **Win 1 — at-end parquet write**: rows are accumulated in memory and
    written to a JSONL staging file (`<out>.rows.jsonl`) throughout the run;
    the parquet is written exactly once at the end.  Eliminates the O(N²)
    read-concat-write pattern of the old 200-clip periodic flush.  On the
    full 5992-clip CHUG run this removes ~30 growing-file reads and ~1.1 GB
    of redundant I/O.  Crash durability is unchanged: the `.done` checkpoint
    remains the resume gate; the staging file recovers in-flight rows after an
    unclean exit (Research-0135).
  - **Win 2 — ffprobe skip via JSONL sidecar**: when `--metadata-jsonl` is
    provided and the sidecar row contains `chug_width_manifest`,
    `chug_height_manifest`, and `chug_framerate_manifest`, the `ffprobe`
    subprocess is skipped for that clip.  Saves ~100–300 ms per clip for CHUG
    runs; ~2–5 min total on 5992 clips at 8 workers (Research-0135).
  - New `--progress-every` flag replaces `--flush-every` for the progress-log
    interval (former name implied a parquet write that no longer happens).
    `--flush-every` is kept as a hidden legacy alias.
