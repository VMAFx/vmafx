Ported the ML-driven half of `vmaf-tune` to `vmafx-tune-go`: `recommend`,
`predict`, `recommend-saliency` and `prefilter` now have real Go implementations
instead of redirecting to the Python binary. Their JSON payloads are
byte-identical to the Python originals — verified by diffing live runs — because
`internal/pyjson` reproduces `json.dumps`'s key order, separators, `NaN` /
`Infinity` tokens and float rendering, which `encoding/json` cannot.

Twelve supporting packages land with them: `pkg/codecadapter` (the nineteen-codec
registry, parity-tested against a dump of the Python adapters), `pkg/ffencode`
and `pkg/scorecli` (the ffmpeg / libvmaf drivers), `pkg/predictor` (analytical
curve, CRF inversion, feature extraction, validation harness), `pkg/pershot`,
`pkg/saliency` (the complete ROI pipeline including all five encoder sidecar
formats), `pkg/prefilter` (the frozen Pelorus knob contract plus a native TPE
sampler that needs no optional Optuna extra), `pkg/recommend`, `pkg/conformal`,
`pkg/uncertainty` and `pkg/corpusrow`.

Two gaps are documented rather than hidden: `recommend-saliency --saliency-aware`
falls back to a plain encode because the `vmafx-ort-runner` bridge passes tensors
through argv and cannot carry a 3xHxW frame, and the `prefilter` TPE trajectory
for a given seed differs from Optuna's while the search space, objective, schema
and reproducibility match. See `docs/usage/vmafx-tune-go.md` §Known gaps.
