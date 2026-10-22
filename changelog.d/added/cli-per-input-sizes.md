- **The CLI accepts per-input raw video dimensions via separate reference and
  distorted size flags (Netflix/vmaf `94f3ff0dd8`).** Flags `--width_ref`,
  `--height_ref`, `--width_dist`, and `--height_dist` allow scoring raw inputs
  with differing resolutions under models that declare a display conversion
  target ([CLI usage](docs/usage/cli.md)).
