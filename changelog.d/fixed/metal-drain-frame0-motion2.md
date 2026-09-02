- Metal backend no longer drops the final frame's score (and the
  motion / motion2 tail) at end-of-stream, and `float_motion_metal` now
  emits frame-0 `motion2`. Two darwin-only defects fixed: (1)
  `flush_context_serial` (`core/src/libvmaf.c`) drained only the
  CUDA / HIP / SYCL extractors' pending final-frame `collect()`; the eight
  Metal extractors were never drained, so the last submitted frame's
  `collect(N)` never ran and its score was silently lost. A new
  `VMAF_FEATURE_EXTRACTOR_METAL` flag (bit 7) is set on every registered
  `feature/metal/` extractor and a `HAVE_METAL` drain branch in flush
  mirrors the existing HIP / SYCL drain. (2) `float_motion_metal`
  (`core/src/feature/metal/float_motion_metal.mm`) never appended
  `motion2 = 0.0` at index 0 (frame-0 missing) and wrote `motion2` twice
  at index 1; the `collect()` indexing now appends `motion2 = 0.0` at
  index 0 and makes index 1 a no-op, matching `integer_motion_metal` and
  the HIP / CUDA twins. Darwin-only; not buildable on the Linux dev / CI
  lane — validated on Apple Silicon. Linux / CUDA / HIP / SYCL backends
  unaffected (the drain branch is `HAVE_METAL`-gated and the motion2
  change is confined to the Metal `.mm`).
