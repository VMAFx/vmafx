**`qat_train.py` trains rank-4 image models for real** (Research-2029 gap 4)

- `ai/scripts/qat_train.py` now picks the training loader from the rank of
  `qat.input_shape`: rank 2 keeps going to
  `vmaf_train.datamodule.VmafTrainDataModule` (tabular canonical-6 rows), and
  rank 4 goes to a new NCHW image loader that reads an `.npz` carrying `x`
  (aliases `images` / `degraded` / `input`, shape `(N, C, H, W)`) and `y`
  (aliases `targets` / `clean` / `reference` / `output`). Any other rank
  downgrades to `--smoke` with a message on stderr instead of failing inside
  the first `Conv2d`.
- Previously every config was handed to the tabular datamodule, so 2D CNN
  configs such as `ai/configs/learned_filter_v1_qat.yaml` could not train — they
  only appeared to work because their uncommitted parquet cache tripped the
  missing-cache branch into smoke mode.
- The `.npz` is validated when the loader is built (extension, array names,
  4D-ness, length match, non-empty), so a malformed cache fails before the fp32
  warm-start burns an epoch. Documented in
  [`docs/ai/quantization.md`](../../docs/ai/quantization.md).
