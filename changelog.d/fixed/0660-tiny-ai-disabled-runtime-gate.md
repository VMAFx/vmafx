Tiny-AI feature extractors now return the shared `-ENOSYS` disabled-DNN
runtime error before probing `model_path`, instead of masking unavailable
ONNX Runtime builds as missing-model configuration errors.
