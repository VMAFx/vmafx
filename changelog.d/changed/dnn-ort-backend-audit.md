### DNN ORT backend audit (Research-0775 / ADR-0775)

Audited `core/src/dnn/` for memory safety, thread-safety, provider-selection
correctness, model cache lifetime, and ORT error-path coverage. No code changes;
findings documented in `docs/research/research-0775-dnn-ort-backend-audit.md`.
Three follow-up items filed: (1) document per-session thread-safety contract in
`dnn.h`, (2) fix `VMAF_DNN_DEVICE_AUTO` chain to include OpenVINO:CPU fallback,
(3) propagate `GetTensorElementType` failure instead of silent UNDEFINED default.
