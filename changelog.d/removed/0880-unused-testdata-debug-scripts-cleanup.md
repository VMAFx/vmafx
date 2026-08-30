- **testdata: orphan debug scripts and slim-schema snapshot removed** — three
  fork-added files under `testdata/` with zero in-tree references were deleted:
  `check_borders.py` (one-off DWT-subband / ADM-border arithmetic debug script
  used during initial SYCL porting), `compare_a380.py` (frame-by-frame
  comparator superseded by `compare_combined.py`, which is strictly more
  capable and is the one the `/run-netflix-bench` skill invokes), and
  `scores_sycl_b580_576_mq.json` (orphan slim-schema 12-metric snapshot for the
  B580 GPU; the `run_sycl_scores.py` generator emits only `{gpu_tag}_{tag}.json`
  variants and never produces the non-standard `_mq` suffix). Net delta:
  -3 files, -36 KB. Companion B580 snapshots at 1080/4k/576 (full 34-metric
  schema) remain in place. (ADR-0880)
