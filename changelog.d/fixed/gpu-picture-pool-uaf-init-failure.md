- `vmaf_gpu_picture_pool_init()` use-after-free on failure paths.
  The combined assignment `*const p = *pool = malloc(...)` published
  the pool pointer to the caller's `*pool` argument *before* any later
  failure path ran. On `goto free_p` (pic-array malloc or
  pthread_mutex_init failure) the function freed `p` but left `*pool`
  dangling — the natural `vmaf_close()` teardown then called
  `vmaf_gpu_picture_pool_close()` on freed memory (UAF + potential
  double-free, since the caller stores the handle in the long-lived
  `VmafContext.cuda.ring_buffer`). Fix: clear `*pool = NULL` at every
  failure label so a non-zero return reliably signals "pool not
  constructed". Adds a CPU-only `test_gpu_picture_pool_uaf` regression
  in `suite=fast` that exercises the `goto free_p` arm via an
  oversized `pic_cnt` malloc-fail.
