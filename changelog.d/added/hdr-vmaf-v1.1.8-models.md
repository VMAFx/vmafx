- **Netflix HDR-VMAF v1.1.8 models ship and are built in (upstream
  `74f930d17d`).** `hdr_vmaf_v1.1.8_3d0h`, `_3d0h_2160`, `_5d0h`,
  `_1d5h_2160` and their high frame rate variants load from
  `model/hdr_vmaf_v1.1.8*/` or by version. The `vmaf` CLI scores two PQ
  inputs with `hdr_vmaf_v1.1.8_3d0h` when no `--model` is given, a model's
  `conversion_target` with an unknown key is refused, and `vmaf-tune`
  resolves PQ sources to the shipped model (HLG still falls back to SDR)
  ([v1 models](docs/models/v1.md),
  [vmaf-tune HDR scoring](docs/usage/vmaf-tune-hdr-and-sampling.md#hdr-vmaf-scoring)).
