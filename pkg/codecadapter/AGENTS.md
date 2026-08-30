# AGENTS.md — the vmafx-tune ML-driven port

Go port of the predictor-, saliency- and search-driven half of `vmaf-tune`:
`recommend`, `predict`, `recommend-saliency` and `prefilter`. This note covers
`pkg/codecadapter` and its eight sibling packages, because the invariants below
span them.

| Package | Ports |
| --------- | ------- |
| `pkg/codecadapter` | `vmaftune/codec_adapters/` |
| `pkg/ffencode` | `vmaftune/encode.py` |
| `pkg/scorecli` | `vmaftune/score.py` |
| `pkg/pershot` | `vmaftune/per_shot.py` (detection half) |
| `pkg/predictor` | `vmaftune/predictor.py`, `predictor_features.py`, `predictor_validate.py` |
| `pkg/conformal` | `vmaftune/conformal.py` (split-conformal half) |
| `pkg/uncertainty` | `vmaftune/uncertainty.py` |
| `pkg/recommend` | `vmaftune/recommend.py` |
| `pkg/saliency` | `vmaftune/saliency.py` |
| `pkg/prefilter` | `vmaftune/prefilter.py`, `filter_adapters/pelorus_deband.py` |
| `pkg/corpusrow` | the `recommend`-scoped part of `vmaftune/corpus.py` |
| `internal/pyjson` | Python `json.dumps` output compatibility |

## Rebase-sensitive invariants

1. **The Python is the oracle, and the tests hold it.**
   `pkg/codecadapter/golden_python_test.go` and
   `pkg/saliency/golden_python_test.go` are machine-generated dumps of the
   Python implementations run on fixed inputs; `pkg/corpusrow`'s
   `pythonSchemaV3Keys` is `CORPUS_ROW_KEYS` verbatim. When a Python adapter,
   kernel or row key changes, regenerate those tables in the same change —
   do not edit the Go side to make a failing golden test pass.

2. **`Adapter.FFmpegCodecArgs` excludes `ExtraParams`; `ResolveCodecArgs`
   includes them** (`codecadapter.go`). The split mirrors the Python's
   `ffmpeg_codec_args` / `extra_params` layering, which
   `encode._resolve_codec_args` composes. Folding the extras into
   `FFmpegCodecArgs` would double libvpx-vp9's `-row-mt 1` once the encode
   driver appends them again. Note that libvpx's `-b:v 0` is NOT an extra
   param: it is part of the codec argv proper (`qualityTail`), because the
   Python emits it from `ffmpeg_codec_args`.

3. **The AMF trio's argv is deliberately de-duplicated.** The Python's
   `extra_params(preset, qp)` on `h264_amf` / `hevc_amf` / `av1_amf` returns
   the same tokens `ffmpeg_codec_args` already produced, so the Python emits
   `-quality … -rc cqp -qp_i … -qp_p …` twice per AMF encode. The Go port
   emits each token once. If a future change makes the Python's duplication
   load-bearing (it is not today — FFmpeg takes the last occurrence), this
   deviation has to be revisited, not silently inherited.

4. **`recommend`'s CRF window is 10–50, not the adapter's quality range**
   (`cmd/vmafx-tune/cmd/recommend.go`). The Python CLI never overrides
   `coarse_to_fine_search`'s `crf_min` / `crf_max` defaults, so pinning the
   Go side to the adapter range would make the two binaries visit different
   cells and produce non-comparable corpora. Change both sides together.

5. **Corpus JSONL carries bare `NaN` / `Infinity` tokens.** An unmeasured
   feature aggregate is NaN by design (ADR-0366) and Python's `json.dumps`
   writes the bare token. `pkg/corpusrow` writes them via `internal/pyjson`
   and `pkg/recommend.SanitizeNonFiniteTokens` reads them back. Do NOT
   "fix" the writer to emit `null`: a Python reader would then get `None`
   where `float(None)` raises, instead of a NaN that propagates. The
   sanitiser is string-literal aware on purpose — a source path containing
   the text `NaN` must survive.

6. **`internal/pyjson` is the single implementation of Python-output
   compatibility.** It exists because `encoding/json` diverges in four
   places: it cannot marshal NaN at all, it renders `93.0` as `93`, it
   HTML-escapes `<`, `>` and `&`, and it emits non-ASCII raw where Python
   escapes to `\uXXXX`. Every payload the ML-driven subcommands emit goes
   through it. Do not reach for `encoding/json` for a user-facing payload.

7. **`predictor.FeatureVector`'s fourteen-element order is pinned by the
   model card** (`predictor.go`). `predictor_train.py` produces models
   against that exact layout; reordering it silently corrupts every learned
   prediction rather than failing. It lives in one function for that reason.

8. **The analytical curve must stay monotone in CRF** (`predictor.go`).
   `PickCRF` binary-searches the curve, which is only sound because
   `PredictAnalytical` is non-increasing across every codec's whole quality
   range. `TestPredictAnalytical_isMonotone` sweeps all fourteen codecs to
   hold that; if a coefficient set ever breaks it, fix the coefficients, not
   the test.

9. **NaN interval bounds must reach `uncertainty.Classify` intact**
   (`recommend.go`, `rowInterval`). A row with no `vmaf_interval` block gets
   NaN bounds, which classify as MIDDLE and defer to the point estimate.
   Substituting a zero-width `(point, point, point)` interval would classify
   as TIGHT and short-circuit the search on a "lower bound" that is really
   just the point estimate. `TestPickTargetVMAFWithUncertainty_zeroWidthIsNotTight`
   is the guard.

10. **Exit code 2 is load-bearing.** The Python CLI uses it for a
    requested-but-unavailable feature (no Pelorus filter, no ROI dispatch for
    the encoder, a bad CRF range) and for `predict`'s `fall_back` verdict;
    scripts branch on it. `cmd/vmafx-tune/cmd/root.go` honours an error's
    `ExitCode()` for that reason. Returning a plain error from those paths
    silently downgrades them to exit 1.

11. **Saliency inference is absent for a specific reason, recorded in code.**
    `cmd/vmafx-tune/cmd/saliencysession.go`'s
    `ErrSaliencyInferenceUnavailable` documents why (the `vmafx-ort-runner`
    bridge passes tensors through argv, which cannot carry 3×H×W) and what
    would unblock it. `TestSaliencySession_reportsWhyInferenceIsUnavailable`
    fails the moment an implementation lands, forcing the doc comment, the
    CLI help and `docs/usage/vmafx-tune-go.md` to be updated together.

12. **The TPE sampler is native and its trajectory is NOT Optuna's.**
    `pkg/prefilter/tpe.go` implements the published construction rather than
    depending on a Go Optuna port that would pull gorm plus the MySQL,
    Postgres and cgo-SQLite drivers into a one-shot CLI. Per-seed
    reproducibility and the emitted schema are contractual; the
    trial-by-trial sequence is not, and no test should assert on it. The
    contract tests assert convergence against a uniform baseline instead.

13. **The Pelorus knob table is a frozen two-repo contract** (ADR-0110,
    `pkg/prefilter/knobs.go`). Renaming, retyping or re-ranging a knob is a
    coordinated Pelorus + vmafx change. `TestKnobTable_isTheFrozenContract`
    pins all ten. The out-of-contract options (`sample`, `blur`, `planes`,
    `meta`) are excluded on purpose and must stay rejected.
