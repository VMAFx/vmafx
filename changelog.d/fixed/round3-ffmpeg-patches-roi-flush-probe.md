### Fixed

- **ffmpeg-patches round-3 bug batch (4 findings)**: Four correctness
  defects in the fork's FFmpeg patch stack are fixed.
  - **R3-4** (`0005`): the `libvmaf_sycl` filter's software-frame path
    feeds the CPU/threaded `vmaf_read_pictures()` entrypoint but
    `uninit_sycl()` always flushed with `vmaf_flush_sycl()`, which drains
    only the SYCL extractors and never joins the worker thread pool. On
    `n_threads>0` software input the last frames could be missing from the
    pooled score. The flush now matches the read path used:
    `vmaf_flush_sycl()` for the QSV zero-copy path (`va_display` set) and
    the canonical `vmaf_read_pictures(NULL, NULL, 0)` drain for software
    frames.
  - **R3-5** (`0007`): the libaom-av1 and libsvtav1 qpfile ROI bridges
    matched each qpfile record's 0-based coded-frame ordinal against
    `AVFrame->pts` (stream `time_base` units), so the equality almost never
    held and the final record was silently applied to every frame —
    defeating per-frame saliency ROI. Both adapters now match against a
    per-encoder input-frame counter (`roi_input_frame`), aligning with
    `saliency.py`'s `range(duration_frames)` frame_index domain.
  - **R3-15** (`0005`, `0013`): `uninit_sycl` / `uninit_metal` logged an
    uninitialized `double vmaf_score` when `vmaf_score_pooled()` failed
    (CERT EXP33-C indeterminate-value read). Both now initialise to `0.0`.
  - **R3-16** (`0005`): the `--enable-libvmaf-sycl` configure probe used a
    stale `>= 2.0.0` version floor and the flat `libvmaf_sycl.h` header
    path; it now probes `libvmaf >= 3.0.0` + `libvmaf/libvmaf_sycl.h`,
    matching the CUDA/HIP/Metal selectors and the actual install layout.
    Cascading context lines in `0006`/`0010`/`0011`/`0013` were updated so
    the full series still replays cleanly against pristine `n8.1`.
