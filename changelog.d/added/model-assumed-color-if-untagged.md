- **A model's `conversion_target` can name a colour for untagged input (Netflix/vmaf `272c596770`).**
  With `assumed_color_if_untagged`, a reference and distorted pair without any
  colorimetry is scored as if both carried that colour, with one warning in
  the log; a partly tagged pair, or one tagged input against an untagged one,
  is still rejected
  ([Model-declared conversion target](docs/models/v1.md#model-declared-conversion-target)).
