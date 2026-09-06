- **Percentile pooling methods in core library** (`core/include/libvmaf/libvmaf.h`, `core/src/libvmaf.c`):
  adds `VMAF_POOL_METHOD_MEDIAN`, `VMAF_POOL_METHOD_PERC5`, `VMAF_POOL_METHOD_PERC10`, and
  `VMAF_POOL_METHOD_PERC20` to `enum VmafPoolingMethod` (Netflix#818, ADR-1181).
  Evaluates linear-interpolated percentiles over frame score distributions with zero regression
  on existing golden-data assertions.
- **Go bindings & FFmpeg integration**:
  surfaces `PoolMethod` in `pkg/libvmaf` (`ScoreDirectRequest`, `StreamConfig`) and
  updates `pool_method_map` and filter options in FFmpeg patches (`0005`, `0006`, `0013`).
- **Unit and regression tests** (`core/test/test_pooling_percentile.c`):
  validates interpolation arithmetic and reproduces reference Python quality runner
  results on `src01_hrc00_576x324.yuv` vs `src01_hrc01_576x324.yuv`.
