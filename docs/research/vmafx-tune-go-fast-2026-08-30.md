<!-- markdownlint-disable MD013 MD041 MD060 -->

# Research digest: porting `vmaf-tune fast` to Go

- **Date**: 2026-08-30
- **Scope**: `vmafx-tune-go fast` — the Phase A.5 proxy + TPE + GPU-verify
  recommend path (ADR-0276 design, ADR-0304 production wiring)
- **Source of truth**: `tools/vmaf-tune/src/vmaftune/fast.py` (712 lines),
  `conformal.py` (575), `proxy.py` (216), `score_backend.py` (selection half),
  and the `_add_fast_args` / `_run_fast` / `_build_fast_*` block of `cli.py`
- **Outcome**: smoke path ported and working; production path ported except the
  ONNX proxy-inference step, which is blocked

## 1. What `fast` actually is

Six steps, of which only one turned out to be un-portable:

| # | Step | Python | Go | Status |
|---|------|--------|----|--------|
| 1 | Validate flags, pick a scoring backend | `_run_fast`, `score_backend.select_backend` | `runFast`, `pkg/scorebackend.Select` | Ported |
| 2 | TPE search over the integer CRF axis | Optuna `TPESampler(seed=0)` | `pkg/fast.RunTPE` on goptuna | Ported (see §3) |
| 3 | Per trial: encode a short probe slice | `cli._build_fast_sample_extractor` → `encode.run_encode` | `pkg/fast.NewSamplePredictor` → `pkg/encoder` | Ported |
| 4 | Per trial: score the slice, extract canonical-6 | `score.build_vmaf_command` + `cli._parse_canonical6_means` | `pkg/fast.BuildVMAFCommand` + `ParseCanonical6Means` | Ported, with four corrections (§2) |
| 5 | Per trial: run `fr_regressor_v2` over features + codec block | `proxy.run_proxy` (onnxruntime) | `pkg/fast.ORTProxy` | **Blocked** (§4) |
| 6 | One real encode + libvmaf score to verify | `cli._build_fast_encode_runner` | `pkg/fast.NewVerifier` | Ported |

Steps 1, 3, 4 and 6 are subprocess orchestration around `ffmpeg` and the
`vmaf` CLI — nothing about them resists a Go port. Step 2 needed a TPE
implementation. Step 5 needs a multi-input ONNX session.

## 2. Four defects found in the Python probe path

Reading step 4 closely turned up four independent bugs. Each is enough on its
own to make the production recommendation meaningless; together they mean
`fr_regressor_v2` has probably never seen a real feature vector from this code
path.

### 2.1 The probe encode is never scored (silent zero)

`cli._build_fast_sample_extractor` encodes the probe to
`workdir/probe_<enc>_crf<n>.mp4` and hands that path straight to
`score.build_vmaf_command`. The libvmaf CLI reads **raw YUV only** — and
because `build_vmaf_command` always passes `--width` / `--height` /
`--pixel_format` / `--bitdepth`, both inputs are routed through
`raw_input_open` (`core/tools/cli_parse`), which rejects a container. The
score subprocess exits non-zero and the seam returns `([0.0] * 6, kbps)`.

`score.py` has the fix — `maybe_decode_distorted`, which ffmpeg-decodes a
container to raw YUV first — and `run_score` calls it. The fast path's probe
leg bypasses `run_score` (deliberately, to reach per-feature means rather than
the pooled score) and so also bypasses the decode. The *verify* leg goes
through `run_score` and is unaffected.

Reproduced directly:

```console
$ vmaf --reference ref.yuv --distorted probe_libx264_crf11.mp4 \
       --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
       --model version=vmaf_v0.6.1 --json --output score.json
Error reading YUV frame data.
problem reading pictures
```

### 2.2 Canonical-6 keys are looked up under the wrong names

`cli._CANONICAL_6_KEYS` is `("adm2", "vif_scale0", …, "motion2")` and
`_parse_canonical6_means` looks those bare names up in `pooled_metrics`, then
falls back to the same bare names in `frames[].metrics`. Modern libvmaf emits
the integer-pipeline names — `integer_adm2`, `integer_vif_scale0`, … —
under both. `score.parse_feature_aggregates` knows this and carries
`_CANONICAL_TO_POOLED_KEY` for exactly this translation; the fast path does
not use it. Result: every feature falls through to the `0.0` default.

So §2.1 and §2.2 independently produce `[0, 0, 0, 0, 0, 0]`.

### 2.3 The codec one-hot is mis-slotted

Three places define the encoder vocabulary, and they disagree:

| Source | Index 3 | Index 5 | Last slot |
|---|---|---|---|
| `ai/scripts/train_fr_regressor_v2.py::ENCODER_VOCAB` (trained the model) | `libvvenc` | `h264_nvenc` | `unknown` |
| `model/tiny/fr_regressor_v2.json::encoder_vocab` (ships with the model) | `libvvenc` | `h264_nvenc` | `unknown` |
| `vmaftune/proxy.py::ENCODER_VOCAB_V2` (used at inference) | `libaom-av1` | `libvvenc` | `av1_qsv` |

The trainer and the sidecar agree; `proxy.py` does not. The two lists match
only on the first three entries, so the one-hot lands in the wrong slot for
every codec past `libsvtav1`. `proxy.py` also accepts `libaom-av1`, which the
model never saw, and rejects `unknown`, which is the model's own catch-all
slot.

### 2.4 The StandardScaler is never applied

`proxy.run_proxy`'s docstring says: *"Caller is responsible for StandardScaler
normalisation; this helper does NOT re-normalise."* No caller on the fast path
does it. `cli._build_fast_sample_extractor` returns raw libvmaf pooled means,
`fast._build_prod_predictor` forwards them unchanged to `_proxy_score`, and
`run_proxy` reshapes them straight into the graph. The sidecar ships
`feature_mean` / `feature_std` precisely so this step can happen.

### 2.5 How the Go port handles each

| Defect | Go behaviour |
|---|---|
| 2.1 | `runVMAF` decodes container-shaped distorted files to raw YUV (`decodeToRawYUV`, a port of `score._decode_to_raw_yuv`) on **both** legs, clamped with `-t` to the probe window so a short probe does not materialise the whole source. |
| 2.2 | `ParseCanonical6Means` tries `pooled_metrics["integer_<name>"]`, then the bare key, then a per-frame average of either. Works against both shapes. |
| 2.3 | `ProxyModel.CodecBlock` builds the one-hot from the sidecar's own `encoder_vocab`, so it cannot drift from the installed checkpoint. Out-of-vocabulary codecs map to `unknown` when the model has that slot; otherwise it is a hard error, never a silent zero-vector. |
| 2.4 | `ProxyModel.NormaliseFeatures` applies `(x - feature_mean) / feature_std` from the sidecar before inference. A zero or absent std leaves the feature unscaled rather than producing ±Inf. |

Verified end to end against the real `ffmpeg` 8.x + `vmaf` 3.2.0 on the
`testdata/ref_576x324_48f.yuv` fixture
(`TestProbePipelineExtractsRealFeatures`):

```text
canonical-6 at CRF 28: [0.98923 0.898144 0.987491 0.993269 0.995706 6.510201] (422.0 kbps)
verify VMAF at CRF 30: 95.8820
production recommendation: CRF 11, proxy 98.902, verify 97.677, gap 1.225
```

The Python implementation is **not** modified by this change; the divergences
are recorded in `docs/state.md` (T-VMAFTUNE-FAST-PY-PROBE-BROKEN-2026-08-30)
and in `cmd/vmafx-tune/AGENTS.md` invariants 15–16 so a future "restore
parity" rebase does not undo them.

## 3. TPE: goptuna instead of Optuna

### 3.1 Selection

`github.com/c-bata/goptuna` v0.9.0 (MIT) is a Go implementation of Optuna,
including a TPE sampler with the same Parzen-estimator defaults Optuna uses
(`consider_prior`, `prior_weight = 1.0`, magic clip, 24 EI candidates, 10
startup trials). Only the root package and `goptuna/tpe` are imported, so the
gorm / MySQL / PostgreSQL requirements in goptuna's own `go.mod` are pruned by
module-graph pruning:

```console
$ go list -deps github.com/c-bata/goptuna github.com/c-bata/goptuna/tpe | grep -v '^[a-z]*$'
github.com/c-bata/goptuna
gonum.org/v1/gonum/floats/scalar
gonum.org/v1/gonum/internal/asm/f64
gonum.org/v1/gonum/floats
github.com/c-bata/goptuna/internal/random
github.com/c-bata/goptuna/tpe
```

`go mod tidy` adds exactly two lines: `goptuna v0.9.0` (direct) and
`gonum.org/v1/gonum v0.17.0` (indirect).

### 3.2 Search-quality validation

The objective is unchanged: minimise `|predicted_vmaf - target| + 1e-4 *
predicted_kbps`. On the ADR-0276 smoke curve the global optimum is computable
by brute force, which gives an exact yardstick. Reference optima from the
*Python* `_smoke_predictor`:

| Target | Optimal CRF | Predicted VMAF | Predicted kbps | Objective |
|---|---|---|---|---|
| 95.0 | 15 | 95.23711249329942 | 5300.59966363287 | 0.7671724596627076 |
| 90.0 | 20 | 90.35515462202830 | 3486.83260599045 | 0.7038378826273399 |
| 88.0 | 22 | 88.24093019754717 | 2952.12248362229 | 0.5361424459094013 |
| 75.0 | 33 | 75.51283314339115 | 1203.02608958883 | 0.6331357523500318 |

Measured distribution of the Go search's recommendation (sequential runs):

| Target | Budget | Repeats | Observed CRFs |
|---|---|---|---|
| 95 | 30 | 200 | `15` ×200 |
| 90 | 50 | 200 | `19` ×1, `20` ×161, `21` ×38 |
| 88 | 50 | 200 | `21` ×2, `22` ×190, `23` ×8 |
| 75 | 50 | 200 | `33` ×180, `34` ×20 |
| 95 / 90 / 88 / 75 | 150 | 150 each | the optimum, every run |

The port reaches the exact brute-force optimum reliably at 150 trials and stays
within ±1 at the shipped budgets, with predicted VMAF / kbps matching the
Python curve to the last bit (the values above are asserted directly in
`pkg/fast/tpe_test.go`).

### 3.3 The reproducibility caveat

Optuna's `TPESampler(seed=0)` makes a run bit-reproducible. goptuna v0.9.0 does
**not**: `tpe.SamplerOptionSeed` seeds the sampler's own `*rand.Rand` and its
startup `RandomSampler`, but `tpe.Sampler.sampleFromGMM` picks the active
Parzen component through `goptuna/internal/random.ArgMaxMultinomial`, which is
a bare `rand.Float64()` on the **process-global** `math/rand` source:

```go
// goptuna@v0.9.0/internal/random/random.go
func ArgMaxMultinomial(pvals []float64) (int, error) {
    x := make([]float64, len(pvals))
    floats.CumSum(x, pvals)
    r := rand.Float64()   // <- global source, not the sampler's rng
    ...
}
```

Go 1.20+ seeds that source randomly at startup and `rand.Seed` has been a no-op
since Go 1.24, so the leak cannot be closed from outside the library. Two
consequences:

1. Repeat runs may return a neighbouring CRF where two candidates score within
   about a VMAF point of each other (quantified in §3.2).
2. Concurrent studies in one process perturb each other. This is why every test
   that asserts on a TPE outcome runs sequentially — with `t.Parallel()` the
   spread widened past ±2 and the suite flaked at roughly 1 run in 100.

Upstream fix: thread the sampler's `rng` into `internal/random`. Until then the
caveat is documented at the top of `pkg/fast/tpe.go`, in
`docs/usage/vmafx-tune-go.md`, and in `cmd/vmafx-tune/AGENTS.md` invariant 17.

Incidentally: Optuna is an optional dependency of the Python fast path
(`pip install 'vmaf-tune[fast]'`) and is **not** installed in this development
environment, so `vmaf-tune fast` cannot run here at all. goptuna is a hard
module dependency of the Go binary, so `vmafx-tune-go fast --smoke` works out
of the box.

## 4. The blocker: ONNX named inputs

`model/tiny/fr_regressor_v2.json` records the graph's contract:

```json
"input_names": ["features", "codec"],
"output_names": ["score"]
```

Two named ports — `features` `[N, 6]` and `codec` `[N, 14]` — matching
`FRRegressor.forward(features, codec_onehot)` in
`ai/src/vmaf_train/models/fr_regressor.py`.

The only ONNX inference seam in the Go tree is `pkg/ai.Registry.Infer`, which
serialises one flat `[]float64` to the `vmafx-ort-runner` subprocess:

```go
cmd := exec.CommandContext(ctx, runnerPath,
    "--model", modelPath,
    "--inputs", string(inputJSON),   // a single JSON array
)
```

There is no wire format for a second named port, and `vmafx-ort-runner` is an
external binary with **no source in this repo**, so the runner cannot be
extended here either. `Registry.InferDirect` — the CGO path via a real ONNX
Runtime binding — is an explicit stub:

```go
var ErrDirectInferNotImplemented = fmt.Errorf(
    "ai: direct ORT CGO path not yet implemented (Stage 2)")
```

Concatenating the ports into one 20-D vector is not a workaround.
`vmaftune/proxy.py` documents that exact mistake: *"the first linear layer of
the exported graph reads the 6-D 'features' port only, so the 14 codec dims
were silently interpreted as batch padding and the 'codec' port received
nothing, breaking fast-path production mode."* Producing a quietly-wrong score
would be worse than failing, so `ORTProxy.Score` returns
`ErrProxyPortsUnsupported` with a diagnostic naming both ports and the
flattened width.

### 4.1 Options considered

| Option | Verdict |
|---|---|
| Extend `vmafx-ort-runner` with a named-input protocol + `Registry.InferNamed` | **Preferred.** Smallest change, keeps the CGO-free build. Blocked here only because the runner's source is not in this repo. |
| Promote `Registry.InferDirect` onto `github.com/yalue/onnxruntime_go` | Works and supports named inputs, but it is a CGO binding requiring `libonnxruntime` at build and run time. `pkg/ai` defers this to "Stage 2" for exactly that reason; adding it would make every `go build ./...` depend on ORT headers. Out of scope for this change. |
| Re-export `fr_regressor_v2` single-port (concatenate inside the graph) | Viable and cheap on the training side; needs an `ai/scripts/` change plus a new checkpoint and sidecar, and a decision about which export is canonical. |
| Hand-write an ONNX interpreter in Go for this graph | The model is a 6→32→32→32→1 MLP with Gemm/Relu only, ~10 KB of weights — technically tractable, but it means writing an inference engine and an ONNX protobuf parser, and matching ORT's numerics. A redesign, not a port. Rejected. |

Until one of the first three lands, production mode fails at the first TPE
trial with an actionable message, and `vmaf-tune fast` remains the way to run
it. Everything up to that point — backend selection, probe encode, decode,
libvmaf score, canonical-6 extraction, TPE, verify pass, payload, exit codes —
is ported and exercised.

> **Addendum (2026-09-02, ADR-1134).** The runner's source is now in this
> repository (`cmd/vmafx-ort-runner`, a cgo shim over `pkg/libvmaf`'s DNN
> session API), so the "Preferred" row above is no longer blocked on an
> external binary — it is a protocol extension of the in-tree runner plus a
> `Registry.InferNamed`. Nothing about the two-port blocker itself changed:
> the runner binds one array to a single input, and libvmaf rejects
> `fr_regressor_v2`'s two-input graph with an arity error (exit 1).

## 5. Byte-compatibility of the payload

The Python CLI emits `json.dumps(result, indent=2, sort_keys=True)`. Two things
had to be reproduced:

1. **Key order.** `RecommendResult` declares its fields in alphabetical order of
   their JSON tag, and `recommendWire` mirrors it.
2. **Float formatting.** CPython renders floats through `float.__repr__`; Go's
   `encoding/json` does not agree:

   | Value | Python | Go default |
   |---|---|---|
   | `90.0` | `90.0` | `90` |
   | `1000000.0` | `1000000.0` | `1e+06` |

   Both show up in a real payload (`target_vmaf` is almost always integral;
   `predicted_kbps` reaches 1e6 on 4K sources), so `pythonFloatRepr`
   reimplements CPython's rule: shortest round-tripping digits, fixed notation
   when `-4 < decpt <= 16` and exponential otherwise, with `.0` appended when
   the fixed form would read as an integer. Pinned against 24 reference values
   in `pkg/fast/jsonfloat_test.go`.

One deliberate divergence: Python would emit the non-standard `NaN` /
`Infinity` tokens for a non-finite float. The Go port coerces those to JSON
`null`, following the precedent already set by the `compare` sweep emitter
(`emitSweepJSON`'s `nan2null`) so the document stays RFC 8259 parseable.

## 6. `pkg/conformal`

`conformal.py` is pure mathematics over the standard library — split-conformal
and CV+ / jackknife+ prediction intervals — and ports cleanly with no
dependency. It is included here because the group's scope named it and because
`docs/usage/vmafx-tune-go.md` claimed a `pkg/conformal` had shipped in Stage 3;
it had not (no such package existed on `master`). The roadmap row is corrected
in the same change.

Two things worth pinning:

- The JSON sidecar is byte-compatible with `SplitConformalCalibration.to_json()`
  (alphabetical keys, trailing newline), so a calibration written by either
  implementation loads in the other.
- Python signals an empty or stale calibration with a suppressible
  `MiscalibrationWarning`. Go has no warning channel, so those become
  *advisory* errors — `ErrEmptyCalibration` and `*StaleCalibrationError` —
  returned alongside a fully usable value. Callers that want the Python
  "suppress and continue" behaviour ignore the error; the numbers are identical
  either way.

The package is not yet wired to a CLI flag — that is Stage 5
(`--quality-confidence`).

## 7. Verification

All green, from the worktree root:

```bash
gofmt -l pkg/fast/ pkg/conformal/ pkg/scorebackend/ pkg/encoder/ cmd/vmafx-tune/
go build ./...
go vet ./...
go test ./... -count=1
gosec -exclude-generated -quiet ./...   # 0 findings across the whole module
go test ./pkg/fast/ -count=400          # flake soak for the TPE tolerances
```

Reproducer for the `fast` surface itself:

```bash
go run ./cmd/vmafx-tune fast --smoke --target-vmaf 90        # exits 0, emits the payload
go run ./cmd/vmafx-tune fast --smoke --target-vmaf 90 \
  --crf-min 40 --crf-max 20                                  # exits 2, invalid CRF range
go run ./cmd/vmafx-tune fast --target-vmaf 90                # exits 2, --src is required
```
