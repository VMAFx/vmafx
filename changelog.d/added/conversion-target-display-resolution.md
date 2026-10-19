- **A model's conversion target can name a display resolution (Netflix/vmaf
  `ca2a2a111d`).** `conversion_target.resolution.display` (`1080` or `2160`)
  scales both inputs so their short side equals the display resolution, keeping
  orientation and aspect ratio, before scoring
  ([Model-declared conversion target](docs/models/v1.md#model-declared-conversion-target)).
