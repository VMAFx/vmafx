<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1768: The FFmpeg libvmaf and libvmaf_cuda filters print no pooled score after a mid-run error

- **Status**: Accepted
- **Date**: 2026-10-05
- **Deciders**: Lusoris
- **Tags**: ffmpeg, cuda, correctness, fork-local

## Context

[ADR-1761](1761-sycl-filter-import-retry-then-fail.md) made `libvmaf_sycl`
and `libvmaf_metal` print no pooled score once they stop on a frame, because
that score covers fewer frames than were decoded. The generic `libvmaf`
filter and FFmpeg's `libvmaf_cuda` filter share upstream's `uninit()`, which
did not follow that rule:

- When `vmaf_read_pictures()` or a picture copy failed, `do_vmaf()` and
  `do_vmaf_cuda()` logged `problem during vmaf_read_pictures.` or `problem
  during copy_picture_data_cuda.` without the frame or the error, and returned
  an error. FFmpeg exited non-zero.
- `uninit()` then pooled the frames read before the error and printed
  `VMAF score:`. `frame_cnt` had already advanced past the failed frame, so
  the pooling usually failed and the line printed an uninitialised `double`:
  `VMAF score: 0.000000` in the dev container, for both filters.
- A failed end-of-stream flush and a failed pooled score were logged, and the
  score line was printed anyway.
- The error paths leaked the distorted `AVFrame`.

This is upstream FFmpeg code, and the fix stays in this fork's series.

## Decision

Following the maintainer's choice ("Patch our series"), patch `0021`
(`libvmaf: print no pooled score after a mid-run error`) changes
`libavfilter/vf_libvmaf.c`:

- `stop_on_frame()` logs one error naming the filter, the step, the frame and
  the error (`libvmaf: vmaf_read_pictures of frame 4 failed (Input/output
  error); the filter stops`), sets `stopped` (the field ADR-1761 added), frees
  the frame, and returns upstream's error code, so FFmpeg still exits
  non-zero. `do_vmaf()` and `do_vmaf_cuda()` call it on every failed copy and
  read. A call after the stop returns the error again without logging.
- `frame_cnt` advances only after a successful read, so the frame number in
  the message is the frame that failed.
- `uninit()` prints `no pooled score: the filter stopped on the error above`
  instead of pooling once `stopped` is set. It also prints no score and writes
  no report after a failed flush, and no score line for a model whose pooled
  score failed. A failed model also means no report is written.
- The divergence from upstream is deliberate. A series refresh keeps it, and
  nothing is sent upstream.

A failed flush happens while the graph is torn down. `uninit()` returns
`void`, so the run exits 0 there; the log and the missing score line show it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Patch the series: stop, name the frame once, no pooled score (chosen) | Every `libvmaf_*` filter follows one rule; no score covers fewer frames than were decoded | One more fork patch over upstream's filter code | Maintainer's choice |
| Leave upstream's behaviour | No divergence from upstream | A score line after an error, often an uninitialised value | Not chosen |
| Send the change upstream as well | Divergence could end later | Upstream review and timing are outside this project's control | Maintainer chose to keep it in the series |
| Pool the frames before the error and label the score partial | Keeps a number | A partial score still reads as the run's score in scripts that grep the line | Not chosen |

## Consequences

- **Positive**: a `libvmaf` or `libvmaf_cuda` score line and report always
  cover every decoded frame; the error names the frame and the cause.
- **Negative**: a script that read the score after a failed run gets nothing.
  That score was wrong. A failed flush leaves exit status 0.
- **Neutral / follow-ups**:
  - `core/test/test_ffmpeg_libvmaf_stop_contract.py` checks the series
    without a device;
  - `ffmpeg-patches/test/check-libvmaf-no-score-after-error.sh` injects the
    errors with `LD_PRELOAD` (`fault_inject_libvmaf.c`, shared with the
    ADR-1761 check) and runs the CPU filter, and the CUDA filter where it can
    run.

## References

- [ADR-1761](1761-sycl-filter-import-retry-then-fail.md) (the same rule for
  `libvmaf_sycl` and `libvmaf_metal`), [ADR-1240](1240-ffmpeg-release-patch-lifecycle.md)
  (the series lifecycle).
- Found while closing `T-FFMPEG-SYCL-FILTER-IMPORT-FAILURE-SKIPS-FRAME-2026-10-05`
  (#2110); state row `T-FFMPEG-LIBVMAF-SCORE-AFTER-MIDRUN-ERROR-2026-10-05`.
- Source: maintainer popup answer of 2026-10-05 on the shared `uninit()` of
  the `libvmaf` and `libvmaf_cuda` filters: "Patch our series (Recommended)".
