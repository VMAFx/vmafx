Fix `vmaf-tune predict --use-saliency` so saliency mean/variance are
actually populated from the saliency ONNX path, and preserve row-provided
saliency / signalstats columns during predictor training.
