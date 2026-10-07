- **`vmaf` reads raw files in the layouts of the import table (RC4 WP13, ADR-2145, ADR-2146).**
  `--pixel_format` takes `400` and the layout names (`nv12`, `p010`, `yuyv422`, `uyvy422`, `v210`,
  `ayuv`, `xv30`, `rgba` ...), `--bitdepth` any depth from 8 to 16, and the `--rgb_matrix`,
  `--rgb_range`, `--rgb_transfer` and `--rgb_out_range` flags state an RGB input (each missing one is
  named; nothing is assumed). The y4m reader takes `C420p9` ... `C444p16` and `Cmono9` ... `Cmono16`.
  See [Raw input layouts](docs/usage/cli.md#raw-input-layouts).
