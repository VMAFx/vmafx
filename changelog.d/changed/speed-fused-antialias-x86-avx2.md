- **SpEED filters only the decimated samples on x86 too, with an AVX2
  vertical pass (Netflix/vmaf `ad42c532`, `9cb9479f`).** `speed_chroma` and
  `speed_temporal` evaluate the anti-alias filter at the samples the 16x
  decimation keeps on every target, where x86 used to filter the whole plane
  and then decimate. The vertical pass runs on AVX2 where the host has it.
  Scores are byte-identical to before at every `--cpumask` setting
  ([SpEED CPU SIMD dispatch](docs/metrics/speed.md#cpu-simd-dispatch)).
