<!-- markdownlint-disable MD013 MD060 -->
# ADR-0660: Tiny-AI extractors check DNN availability before model paths

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: lusoris
- **Tags**: `tiny-ai`, `dnn`, `feature-extractors`, `api`, `docs`

## Context

The modernization audit flagged `lpips`, `dists_sq`, `fastdvdnet_pre`,
`mobilesal`, and `transnet_v2` as possible `-ENOSYS` scaffolds. They are not
missing implementations: each is an ONNX-backed feature extractor that needs
the build-time optional DNN runtime.

The old init order made that contract less obvious. A disabled-DNN build could
return `-EINVAL` for a missing model path before it ever reached the DNN stub
that returns `-ENOSYS`, even though a valid model path still could not run.
That confused humans and text audits because the optional-runtime failure was
not the first visible runtime gate.

## Decision

All tiny-AI feature extractors call a shared
`vmaf_tiny_ai_require_runtime()` helper after pixel-format / bit-depth
validation and before model-path probing. The helper returns `-ENOSYS` with a
single standard log line when libvmaf was built without DNN support. DNN-enabled
builds keep the existing path validation and still return `-EINVAL` when
neither `model_path` nor the extractor-specific environment variable is set.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave each extractor to discover disabled DNN through `vmaf_dnn_session_open()` | No code churn | Missing model paths mask the optional-runtime contract on disabled builds; future extractors can drift again | Rejected |
| Register DNN-backed extractors only when `-Denable_dnn=true` | Avoids init-time `-ENOSYS` | Breaks discoverability and the fork's ADR-0374 disabled-build public-surface pattern | Rejected |
| Central runtime preflight in the tiny-AI template | One contract, one log line, one test pattern | Slightly changes disabled-build error ordering for missing-path invocations | Chosen |

## Consequences

- **Positive**: disabled-DNN builds now report a consistent `-ENOSYS` optional-runtime error before model-path checks for every tiny-AI extractor.
- **Positive**: new tiny-AI extractors inherit the same init order through the shared template and test macro.
- **Negative**: callers that relied on disabled-DNN builds returning `-EINVAL` for missing model paths must now treat `-ENOSYS` as the higher-priority runtime-availability signal.
- **Neutral / follow-ups**: the committed ONNX models, registry entries, feature names, and DNN-enabled execution paths are unchanged.

## References

- [ADR-0250](0250-tiny-ai-extractor-template.md) — tiny-AI extractor helper pattern.
- [ADR-0374](0374-disabled-build-enosys-contract.md) — build-time optional public APIs return `-ENOSYS` when disabled.
- [ADR-0658](0658-project-modernization-audit.md) — scanner that surfaced the optional-runtime ambiguity.
- Source: req "well go on i guess we have enough backlog..."
