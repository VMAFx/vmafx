### fix(core): complete integer_motion v2 rename — add missing prev_prev_ref field

The PR #532 port of Netflix upstream `a4a1492d3` ("replace integer_motion with
pipelined v2 variant, rename") partially propagated: `integer_motion.c` line 324
was updated to reference `fex->prev_prev_ref` (for `motion_five_frame_window=true`
mode), but the field was never added to `VmafFeatureExtractor` in
`feature_extractor.h`, nor was the framework wiring in `libvmaf.c` updated to
populate it. This caused a hard build failure:

```
core/src/feature/integer_motion.c:324:49: error: 'VmafFeatureExtractor' has no
member named 'prev_prev_ref'; did you mean 'prev_ref'?
```

Fix: add `VmafPicture prev_prev_ref` to `VmafFeatureExtractor` (mirrors upstream
Netflix/vmaf master `feature_extractor.h`) and wire all four dispatch paths in
`libvmaf.c` — serial, threaded-one, batch-thread, and CUDA batch — to track and
propagate both `prev_ref` (n-1) and `prev_prev_ref` (n-2) using the same rolling
refcount pattern upstream uses.
