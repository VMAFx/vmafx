---
paths:
  - core/src/libvmaf.c
  - core/src/dnn/model_loader.c
  - core/test/dnn/test_vmaf_use_tiny_model.c
  - core/test/dnn/test_cli.sh
invariant: Feature-vector tiny models request their inputs, score at flush, and fail on missing input, never reading 0.0.
---
<!-- markdownlint-disable MD013 -->
# Feature-vector tiny-model inputs (ADR-1520)

- **Attach registers inputs.** `dnn_attach_feature_vector()` in
  `core/src/libvmaf.c` resolves every input slot (sidecar `feature_order` /
  `features`, canonical-6 only for six-wide model without list) to
  extractor through `dnn_slot_extractor()` and registers it with
  `vmaf_use_feature()` and default options. name no extractor writes is
  `-EINVAL`, list of another length than input is `-ENOTSUP`; all names
  are resolved before first registration.
- **Scoring runs at flush.** `dnn_flush_feature_vector()` is called from
  `flush_context()` after every backend flush, because motion extractor
  writes `motion2` only in its own `flush()` and GPU twins collect their last
  frame there. Do not move rank-2 scoring back into `vmaf_read_pictures()`:
  every frame would read missing `motion2`. Rank-4 image models stay on
  per-frame path.
- **missing input fails.** `dnn_lookup_feature()` returns `-ENOENT`, never
  `0.0`. frame with some but not all inputs fails flush and names
  features; frame with none (index caller skipped) and frame
  `n_subsample` drops are not scored. `dnn.next_index` keeps retried flush
  from appending frame twice.
- **codec block is caller's.** second input is accepted only when
  sidecar's `encoder_vocab` plus two equals its width
  (`dnn_check_codec_layout()`). block starts zero and `codec_ready` stays
  false until `vmaf_dnn_set_codec_context()` succeeds; until then
  `vmaf_read_pictures()` returns `-EINVAL`. `vmaf_dnn_codec_block_fill()`
  finds `"unknown"` by name and returns `-ENOENT` for vocabulary without
  it; never default to fixed slot such as last or third-from-last.
- `test_vmaf_use_tiny_model.c` (`test_feature_vector_*`,
  `test_codec_*`) and codec cases of `test_cli.sh` guard all four.
