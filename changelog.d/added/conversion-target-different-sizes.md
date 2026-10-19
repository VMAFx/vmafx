- **A model with a display resolution scores inputs of different sizes
  (Netflix/vmaf `cf9a6ec51b`).** When `conversion_target.resolution.display` is
  set, the reference and the distorted input may differ in size; both are scaled
  to the size the reference scales to, and inputs of a different orientation or
  aspect ratio are rejected
  ([Model-declared conversion target](docs/models/v1.md#model-declared-conversion-target)).
