- **Container**: bump `ORT_VERSION` from 1.20.1 to 1.26.0; adds ROCm 7.x EP
  and CUDA 13.x EP support in ONNX Runtime inference when the container is rebuilt
  (ADR-0569).
- **Container**: bump `AMF_VERSION` from 1.4.36 to 1.5.2; AV1 ROI map + quality
  improvements; `--disable-filter=amf_capture` in the FFmpeg configure neutralises
  the `DisplayCapture.h` C++ header change introduced in 1.5.x (ADR-0569).
- **Container**: bump `VVENC_VERSION` from 1.12.0 to 1.14.0; encoder quality
  improvements at medium presets; gcc-14 compatibility fixes (ADR-0569).
- **Pre-commit**: bump `mirrors-clang-format` from v22.1.3 to v22.1.5,
  `black` from 26.3.1 to 26.5.1, `ruff-pre-commit` from v0.15.10 to v0.15.13
  (ADR-0569).
- **CI**: re-pin `sigstore/cosign-installer` from v4.1.1 to v4.1.2 (SHA
  `6f9f17788090df1f26f669e9d70d6ae9567deba6`); latest cosign signing fixes
  (ADR-0569).
- **Python deps**: widen `libsvm-official` ceiling from `<=3.32` to `<=3.37`;
  unlocks LIBSVM 3.33–3.37 solver bug fixes; 3.x API remains stable (ADR-0569).
