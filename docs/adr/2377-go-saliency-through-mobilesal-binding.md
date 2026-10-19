<!-- markdownlint-disable MD013 MD060 -->
# ADR-2377: Go saliency inference runs through the core's MobileSal extractor and the RC4 Go binding

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: maintainer
- **Tags**: saliency, vmaf-tune, go, onnx, rc5, tooling

## Context

The Go `vmafx-tune` cannot run the saliency model in process. The numeric
pipeline around the model is ported and tested against Python (`pkg/saliency`),
but the single forward pass is missing: Go has no ONNX Runtime in the module,
and `cmd/vmafx-ort-runner` passes tensors in the argument vector, which cannot
carry the 3xHxW saliency input (about 6.2 million floats for a 1080p frame).
`predict --use-saliency` returns an error and `recommend-saliency
--saliency-aware` degrades to a plain encode
(`ErrSaliencyInferenceUnavailable` in `cmd/vmafx-tune/cmd/saliencysession.go`).
The Python `vmaf-tune` is deleted in RC5 (#1249), so the gap has to close
before that. The earlier note of #1249 named two ways out: a cgo ONNX Runtime
binding in `cmd/vmafx-tune`, or a streaming tensor protocol for
`cmd/vmafx-ort-runner`. The core already ships a `mobilesal` feature extractor
that opens its model through the library's own DNN session code
(ADR-0218, ADR-0286).

## Decision

Go saliency inference uses the core's MobileSal extractor through the Go
binding that RC4 work package 7 generates from the VMAFx API definition
([#2434](https://github.com/VMAFx/vmafx/issues/2434)). `pkg/saliency` keeps its
pre- and post-processing. There is no second ONNX Runtime integration: no cgo
ONNX Runtime binding in `cmd/vmafx-tune`, and no tensor transport added to
`cmd/vmafx-ort-runner` for this purpose.

Not decided here, and left to the implementing change in RC5: how the
extractor hands its per-pixel map to the binding, and the model and weights it
loads.

## Alternatives considered

| Option | Pros | Cons | Outcome |
|---|---|---|---|
| Core MobileSal extractor through the RC4 Go binding (**chosen**) | One ONNX Runtime integration, the library's; `cmd/vmafx-tune` stays free of a second runtime; the binding exists anyway | Depends on WP7 and on the extractor exposing its map | Chosen |
| cgo ONNX Runtime binding in `cmd/vmafx-tune` | Self-contained forward pass | A second runtime integration; `cmd/vmafx-tune` becomes a cgo build | Not chosen |
| Streaming tensor protocol for `cmd/vmafx-ort-runner` | Keeps the tool pure Go | Extends a bridge built for 14 floats to 75 MB frames; still a second path to the same runtime | Not chosen |

## Consequences

- **Positive**: closes the last recommend-saliency gap of the Go port without a
  new dependency; HISS-19 holds (one inference path).
- **Negative**: `vmafx-tune` saliency needs the RC4 binding and a library with
  the extractor built in.
- **Neutral / follow-ups**: #1249's task text names this route; the
  `--use-saliency` and `--saliency-aware` flags are implemented on it in RC5
  before `tools/vmaf-tune` is deleted. #2414 (TensorRT and DirectML providers)
  is not affected: it extends the library's ONNX surface.

## References

- `Q-132`: "Use the core's MobileSal saliency extractor through the RC4 Go binding (WP7); no second ONNX Runtime integration; needs an ADR"
- [ADR-0218](0218-mobilesal-saliency-extractor.md), [ADR-0286](0286-saliency-student-fork-trained-on-duts.md)
- Issues [#1249](https://github.com/VMAFx/vmafx/issues/1249), [#2434](https://github.com/VMAFx/vmafx/issues/2434)
