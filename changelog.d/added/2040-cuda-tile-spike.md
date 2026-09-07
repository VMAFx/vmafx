- **Research digest: CUDA Tile C++ has no additive reduction**
  (`docs/research/2040-cuda-tile-additive-reduction-gap.md`). A timeboxed spike
  against CUDA 13.3 on an RTX 4090 settles the Tile-adoption question on a
  mechanism rather than a judgement call: the Tile API ships `reduce_max`,
  `reduce_min` and the bitwise reductions as builtins but has no `reduce_add`,
  while every hot kernel in this fork — `float_vif_compute`, the `integer_adm`
  accumulators, `float_moment`, the SpEED covariance pass — is built from
  additive reductions. Expressing one from `partial_sum` scans measures at 47
  registers and 4 KB of shared memory against 24 registers and zero shared for
  the natively supported `reduce_max`, to produce a scalar the current SIMT
  path gets from a register-only warp shuffle. The digest also records that
  Tile kernels need a separate `__tile_global__` entry point (a `__global__`
  function cannot call `__tile__` code) and a separate `--tilefatbin` artifact.
