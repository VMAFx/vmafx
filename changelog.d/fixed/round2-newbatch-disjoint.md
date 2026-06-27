### Fixed

Round-2 bug-hunt new-batch (5 disjoint-file findings; fork-local, golden-safe).

- **R2-2** CUDA HOST_PINNED allocator dimension guard (`core/src/cuda/picture_cuda.c`): reject w/h ∉ [1,32768] before the 32-bit stride/pic_size compute (mirrors the host twin, CERT INT30-C).
- **R2-3** model-collection partial-failure leak (`core/src/read_json_model.cpp` + `.c`): destroy + NULL sub-model 0 and the partial collection on a later sub-model failure.
- **R2-5** `feature_vector_append` unbounded grow (`core/src/feature/feature_collector.c`/`.cpp`/`.h`): reject `index >= FEATURE_VECTOR_MAX_INDEX (1u<<28)` + overflow-guard the doubling (defensive only; upstream-parity preserved).
- **R2-10** `flush_context` skipped the SYCL flush on a CUDA-flush error (`core/src/libvmaf.c`): accumulate `err |=` across backends; set `flushed` once at the end iff `err==0`.
- **R2-9** removed dead `gpu_dispatch_env.c` (ADR-0858 orphan; live `.cpp` already correct) + added a contract test.
