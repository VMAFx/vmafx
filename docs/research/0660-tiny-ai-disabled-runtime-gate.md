# Research-0660: Tiny-AI disabled-runtime gate

## Question

Are the modernization-audit `-ENOSYS` hits for LPIPS, DISTS-Sq,
FastDVDnet pre, MobileSal, and TransNet V2 real missing implementations or
intentional optional-runtime contracts?

## Findings

- The five extractors are registered unconditionally so users can discover the
  feature names and option tables in every libvmaf build.
- Their actual inference path is not scaffolded: DNN-enabled builds open ONNX
  sessions through `vmaf_dnn_session_open()` and bind named tensors through
  `vmaf_dnn_session_run()`.
- DNN-disabled builds intentionally provide public DNN symbols that return
  `-ENOSYS`; ADR-0374 defines that as the fork-wide contract for optional
  build surfaces.
- Before this change, extractor `init()` resolved model paths before reaching
  `vmaf_dnn_session_open()`. That made a disabled-DNN build return `-EINVAL`
  for missing model paths even though no path could have run.
- The clean fix is not another per-extractor comment. The tiny-AI helper layer
  already owns shared path and session-open plumbing, so it should also own the
  runtime-availability preflight.

## Decision Pressure

The user-visible behavior should prioritize the highest-level actionable
failure. A build without DNN support is not a model-path configuration problem;
it is an unavailable optional runtime. Returning `-ENOSYS` before path probing
therefore gives operators the right remediation: rebuild libvmaf with DNN
enabled, or omit the tiny-AI extractor.

## Validation Targets

- `test_lpips`
- `test_dists`
- `test_fastdvdnet_pre`
- `test_mobilesal`
- `test_transnet_v2`
- `make format-check`
- `mkdocs build --strict`

## References

- [ADR-0374](../adr/0374-disabled-build-enosys-contract.md)
- [ADR-0250](../adr/0250-tiny-ai-extractor-template.md)
- [ADR-0658](../adr/0658-project-modernization-audit.md)
