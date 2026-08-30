- **Round-4 audit bug-fix bundle (C/CUDA/DNN/feature-CPU)**. Ten verified
  defects from the adversarial audit of the admin-merged #1043–1062 batch,
  all golden-safe (error/close/comment/DNN paths, none on a CPU golden score
  path): (1) `speed_chroma_cuda` / (2) `speed_temporal_cuda` — a failing
  `cuCtxPopCurrent` jumped straight to the error return, leaking an unbalanced
  CUDA context-stack entry per frame; both now retry the pop and propagate the
  CUDA error. (3) `read_json_model` `model_collection_parse_loop` — a malformed
  (non-string) key after sub-model 0 leaked the partial model + collection;
  teardown added. (4) `model.c` `vmaf_model_collection_append` — the
  short-model-name path freed `mc` without first freeing the already-allocated
  `mc->model` (now `goto fail_model`). (5) `ort_backend` `build_input_tensor` —
  guarded the `n * sizeof(elem)` byte-count multiply against `size_t` overflow
  (the element-count guard alone left the byte count able to wrap). (6)
  `ort_backend` `fp32_to_fp16` — switched the normal case from truncation to
  round-to-nearest, matching `tensor_io.c`'s `f32_to_f16_one` (values in
  (65504, 65520) now correctly encode to +inf, not max-finite). (7) `ciede`
  init returned `-ENOMEM` for an unsupported bitdepth (now `-EINVAL`). (8)
  `ciede` close used a conjunctive guard that leaked `s->ref` when only `ref`
  was allocated; split into independent guards. (9) `cambi` close called
  `vmaf_picture_unref` on never-allocated (zero-init) slots, poisoning `err`
  with `-EINVAL` during partial-init close; now guarded. (10) corrected a
  misleading `integer_ssim` comment about the GPU twins (HIP/SYCL pin a
  hardcoded `L=255.0f`, only CUDA widens to int64_t/double). Also removed a
  dead `op_h` local in `speed_chroma_cuda`.
