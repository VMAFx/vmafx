docs(research): add cuDNN version audit for ORT/tiny-AI inference path (research-0734)

Tracks the cuDNN dependency of our ONNX Runtime 1.26.0 build and checks the
latest cuDNN 9.22.0 release notes for bugs affecting INT8/FP16 small-CNN
inference. Verdict: no cuDNN exposure in the current CPU-only ORT install;
documents upgrade triggers for the GPU path.
