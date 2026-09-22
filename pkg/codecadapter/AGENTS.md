# AGENTS.md — the vmafx-tune ML-driven port

Go port of predictor-, saliency- and search-driven half of `vmaf-tune`:
`recommend`, `predict`, `recommend-saliency`, `prefilter`. Note covers
`pkg/codecadapter` + eight sibling packages — invariants below span
them.

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
| `pkg/pyjson` | Python `json.dumps` / `jsonio.dumps_strict` output compatibility (ADR-1137) |

## Rebase-sensitive invariants

1. **Python is oracle, tests hold it.**
   `pkg/codecadapter/golden_python_test.go` and
   `pkg/saliency/golden_python_test.go` are machine-generated dumps of
   Python implementations run on fixed inputs; `pkg/corpusrow`'s
   `pythonSchemaV3Keys` is `CORPUS_ROW_KEYS` verbatim. When Python
   adapter, kernel or row key changes, regenerate those tables in same
   change — do not edit Go side to make failing golden test pass.

2. **`Adapter.FFmpegCodecArgs` excludes `ExtraParams`;
   `ResolveCodecArgs` includes them** (`codecadapter.go`). Split
   mirrors Python's `ffmpeg_codec_args` / `extra_params` layering,
   which `encode._resolve_codec_args` composes. Folding extras into
   `FFmpegCodecArgs` would double libvpx-vp9's `-row-mt 1` once encode
   driver appends them again. libvpx's `-b:v 0` is NOT extra param: it
   is part of codec argv proper (`qualityTail`) — Python emits it from
   `ffmpeg_codec_args`.

3. **AMF trio's argv is deliberately de-duplicated.** Python's
   `extra_params(preset, qp)` on `h264_amf` / `hevc_amf` / `av1_amf`
   returns same tokens `ffmpeg_codec_args` already produced, so Python
   emits `-quality … -rc cqp -qp_i … -qp_p …` twice per AMF encode. Go
   port emits each token once; since ADR-1137 that holds for every
   encode driver (`pkg/ffencode`, `pkg/corpus`, `pkg/encodeprofile`,
   `pkg/tune/executor`) — all build argv through this registry.
   `TestCodecArgsMatchPythonAtDefaultQuality` pins exception
   precisely: Python fixture's `extra` must equal codec-args tail, so
   change in what Python duplicates still caught. If future change
   makes duplication load-bearing (not today — FFmpeg takes last
   occurrence), deviation has to be revisited, not silently inherited.

4. **`recommend`'s CRF window is 10–50, not adapter's quality range**
   (`cmd/vmafx-tune/cmd/recommend.go`). Python CLI never overrides
   `coarse_to_fine_search`'s `crf_min` / `crf_max` defaults, so pinning
   Go side to adapter range would make two binaries visit different
   cells, produce non-comparable corpora. Change both sides together.

5. **Corpus JSONL carries bare `NaN` / `Infinity` tokens.** Unmeasured
   feature aggregate is NaN by design (ADR-0366); Python's
   `json.dumps` writes bare token. `pkg/corpusrow` writes them via
   `pkg/pyjson`; `pkg/recommend.SanitizeNonFiniteTokens` reads them
   back. Do NOT "fix" writer to emit `null`: Python reader would then
   get `None` where `float(None)` raises, instead of NaN that
   propagates. Sanitiser is string-literal aware on purpose — source
   path containing text `NaN` must survive.

6. **`pkg/pyjson` is single implementation of Python-output
   compatibility (ADR-1137).** Exists because `encoding/json` diverges
   in four places: cannot marshal NaN at all, renders `93.0` as `93`,
   HTML-escapes `<`, `>` and `&`, emits non-ASCII raw where Python
   escapes to `\uXXXX`. Every payload ML-driven subcommands emit goes
   through it. Do not reach for `encoding/json` for user-facing
   payload.

7. **`predictor.FeatureVector`'s fourteen-element order is pinned by
   model card** (`predictor.go`). `predictor_train.py` produces models
   against that exact layout; reordering it silently corrupts every
   learned prediction rather than failing. Lives in one function for
   that reason.

8. **Analytical curve must stay monotone in CRF** (`predictor.go`).
   `PickCRF` binary-searches curve — only sound because
   `PredictAnalytical` is non-increasing across every codec's whole
   quality range. `TestPredictAnalytical_isMonotone` sweeps all
   fourteen codecs to hold that; if coefficient set ever breaks it,
   fix coefficients, not test.

9. **NaN interval bounds must reach `uncertainty.Classify` intact**
   (`recommend.go`, `rowInterval`). Row with no `vmaf_interval` block
   gets NaN bounds, classify as MIDDLE, defer to point estimate.
   Substituting zero-width `(point, point, point)` interval would
   classify as TIGHT, short-circuit search on "lower bound" that is
   only point estimate.
   `TestPickTargetVMAFWithUncertainty_zeroWidthIsNotTight` is guard.

10. **Exit code 2 is load-bearing.** Python CLI uses it for
    requested-but-unavailable feature (no Pelorus filter, no ROI
    dispatch for encoder, bad CRF range) and for `predict`'s
    `fall_back` verdict; scripts branch on it.
    `cmd/vmafx-tune/cmd/root.go` honours an error's `ExitCode()` for
    that reason. Returning plain error from those paths silently
    downgrades them to exit 1.

11. **Saliency inference is absent for specific reason, recorded in
    code.** `cmd/vmafx-tune/cmd/saliencysession.go`'s
    `ErrSaliencyInferenceUnavailable` documents why (`vmafx-ort-runner`
    bridge passes tensors through argv, cannot carry 3×H×W) and what
    would unblock it.
    `TestSaliencySession_reportsWhyInferenceIsUnavailable` fails
    moment implementation lands, forcing doc comment, CLI help and
    `docs/usage/vmafx-tune-go.md` to be updated together.

12. **TPE sampler is native; trajectory is NOT Optuna's.**
    `pkg/prefilter/tpe.go` implements published construction rather
    than depending on Go Optuna port that would pull gorm plus MySQL,
    Postgres and cgo-SQLite drivers into one-shot CLI. Per-seed
    reproducibility and emitted schema are contractual; trial-by-trial
    sequence is not — no test should assert on it. Contract tests
    assert convergence against uniform baseline instead.

13. **Pelorus knob table is frozen two-repo contract** (ADR-0110,
    `pkg/prefilter/knobs.go`). Renaming, retyping or re-ranging knob
    is coordinated Pelorus + vmafx change.
    `TestKnobTable_isTheFrozenContract` pins all ten. Out-of-contract
    options (`sample`, `blur`, `planes`, `meta`) excluded on purpose,
    must stay rejected.

14. **Registry names are sorted through the standard library.**
    `codecadapter.Known`, `prefilter.KnownFilters`, and the private knob-name
    diagnostic use `slices.Sorted(maps.Keys(...))`. Do not re-grow parallel
    key-collection/sort loops; CLI choice and diagnostic order stays
    deterministic.
