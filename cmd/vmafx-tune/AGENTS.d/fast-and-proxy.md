---
paths:
  - cmd/vmafx-tune/cmd/fast*.go
  - cmd/vmafx-tune/cmd/root.go
  - pkg/fast/*.go
invariant: Fast exit codes 0/2/3; sorted RecommendResult + pythonFloatRepr; 1-port ORT guard; sequential TPE.
---
<!-- markdownlint-disable MD013 MD029 -->
# `fast` subcommand, ORT proxy guard, sidecar scaler and TPE tests

## Rebase-sensitive invariants

18. **`fast` exit codes travel on error** (`cmd/vmafx-tune/cmd/fast.go`,
    `root.go` `Execute`): cobra maps any `RunE` error to exit 1, but
    `vmaf-tune fast` has three-way contract — 0 (agreed), 2 (usage /
    environment), 3 (proxy/verify gap beyond tolerance; fall back to slow
    grid). `runFast` wraps errors in `*fastExitError`,
    `Execute` consults `fastExitCode(err)` before `os.Exit`. Exit-3 path
    still writes payload first — callers parse it *and* branch on
    status. Any new subcommand with own exit contract follows same
    shape rather than adding second exit switch.

19. **`RecommendResult` field order IS schema** (`pkg/fast/fast.go`,
    `jsonfloat.go`): Python CLI emits
    `json.dumps(result, indent=2, sort_keys=True)`, so Go struct declares
    fields in alphabetical order of JSON tag, `recommendWire`
    mirrors that order. Reordering fields silently breaks byte
    compatibility. Float fields go through `pythonFloatRepr`,
    reproducing CPython's `float.__repr__` (integral `target_vmaf` must print
    as `90.0`, `1e6` as `1000000.0` — Go's default encoder prints `90` and
    `1e+06`). Non-finite floats coerced to JSON `null`, following
    `compare` sweep emitter rather than Python's non-standard `NaN` token.

20. **Proxy port guard load-bearing** (`pkg/fast/proxy.go`):
    `ORTProxy.Score` refuses to run when resolved model declares more than
    one ONNX input port. `fr_regressor_v2` has two (`features` `[N, 6]` and
    `codec` `[N, 14]`), Go seam (`pkg/ai.Registry.Infer` →
    `vmafx-ort-runner`) takes one flat vector. Do **not** "fix" this by
    concatenating ports into 20-D vector: `vmaftune/proxy.py` documents
    graph's first dense layer reads 6-D `features` port only, so
    codec dims become batch padding, `codec` receives nothing. Lift
    guard only once seam grows named inputs (or model re-exported
    single-port). `TestShippedModelIsTwoPort` fails loudly if shipped
    artefact changes shape and guard's documentation goes stale.

21. **Proxy vocabulary and scaler come from sidecar, never constant**
    (`pkg/fast/proxy.go` `CodecBlock` / `NormaliseFeatures`):
    `vmaftune/proxy.py` hardcodes `ENCODER_VOCAB_V2` tuple that has drifted
    out of sync with `ai/scripts/train_fr_regressor_v2.py` and
    `model/tiny/fr_regressor_v2.json` from index 3 onward, so codec one-hot
    lands in wrong slot for most codecs. Go port reads `encoder_vocab`,
    `feature_mean` and `feature_std` from model's own sidecar so they cannot
    drift from installed checkpoint. Do not reintroduce hardcoded
    vocabulary or drop StandardScaler step.

22. **TPE tests are not `t.Parallel()`** (`pkg/fast/tpe_test.go`,
    `fast_test.go`): goptuna v0.9.0 draws part of its randomness from
    process-global `math/rand` source, so concurrent studies perturb each
    other's trial sequences. Any test asserting on TPE outcome runs
    sequentially, tolerances stated against measured
    distribution (recorded in test comments). Adding `t.Parallel()` to
    those tests reintroduces flakes; tests that never start study may
    stay parallel.
