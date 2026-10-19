- **The route for Go saliency inference is decided.** [ADR-2377](docs/adr/2377-go-saliency-through-mobilesal-binding.md)
  records that `vmafx-tune` runs the saliency model through the core's MobileSal extractor and the
  generated Go binding, with no second ONNX Runtime integration. No code changes yet; RC5 implements
  `--use-saliency` and `--saliency-aware` on it before the Python `vmaf-tune` is deleted.
