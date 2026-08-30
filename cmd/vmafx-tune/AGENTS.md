# AGENTS.md — cmd/vmafx-tune

Parent: [../../AGENTS.md](../../AGENTS.md).

Go port of the vmaf-tune rate-quality tuning CLI. Installed as `vmafx-tune-go`
during the migration; see Stage roadmap in
[ADR-0705](../../docs/adr/0705-vmafx-tune-go-stage1.md).

## Rebase-sensitive invariants

1. **JSON schema compatibility** (`pkg/report/report.go`): the JSON output of
   `EmitJSON` must remain schema-compatible with the Python `compare.py` v1/v2
   payloads. The Python `report.py` renderer ingests this JSON directly. Any
   field rename or removal requires a coordinated Python-side change. Add new
   optional fields only; never remove existing ones without a schema-version bump.

2. **NaN coercion** (`pkg/report/report.go` `nanToNull`): `float64` fields that
   can be NaN (failed-row bitrate, VMAF, encode time) MUST be serialized as JSON
   `null`, not as bare `NaN` tokens. RFC 8259 strict parsers reject bare `NaN`.
   Mirror the Python `_nan_to_none` discipline.

3. **Bisect midpoint bias** (`pkg/bisect/bisect.go`): the midpoint rounds toward
   the *higher* CRF end `(lo + hi + 1) / 2` so the best-so-far record is never
   populated with an unvalidated CRF. Changing the rounding direction breaks the
   monotonicity invariant.

4. **ScoreFunc seam** (`pkg/bisect/bisect.go`): `ScoreFunc` is the subprocess
   boundary. Tests inject mock score functions. Never merge the score function
   inline into `Run`; the seam is load-bearing for unit testability.

5. **Stage-1 scope** (`pkg/encoder/encoder.go`): `encoder.New` accepts only
   `libx264` and `libx265`. Hardware encoders (NVENC, QSV, AMF) and SVT-AV1 are
   Stage-2 scope. Do not add hardware encoder support here without a new ADR and
   the associated hw-init flag plumbing from Python `compare.py`.

6. **Binary name** (`cmd/vmafx-tune/main.go`): the binary installs as
   `vmafx-tune-go`, not `vmaf-tune`, during Stage 1 to avoid collisions with the
   Python binary. Stage 3 (swap) will rename. Never install it as `vmaf-tune` in
   a PR that does not also remove the Python entry point.

7. **`errors.Join` for multi-step cleanup** (`pkg/bisect`, `pkg/encoder`,
   `pkg/storage`, `cmd/vmafx-controller/queue` — and any new sibling package
   that grows a similar pipeline): when a primary error and a cleanup error
   can both arise, return `errors.Join(primary, cleanup)` rather than
   silently dropping the cleanup error via `_ = X()` or
   `X() //nolint:errcheck`. Guard cleanup `os.Remove` calls with
   `errors.Is(rmErr, os.ErrNotExist)` so a not-yet-created file is not
   flagged as a cleanup failure. The `slog` error-attribute key is
   `"error"` everywhere (`"err"` is retired). See ADR-0935.

8. **`WireRow` vs `Row` duality** (`pkg/report/`): `Row` (report.go) is the
   emit-only shape used by `compare` to produce JSON output; `WireRow` (multi.go)
   is the unmarshal-only shape used by `report` to read that JSON back in.
   These are intentionally separate types. Do not merge them — `Row` uses bare
   `float64` + NaN convention for emit; `WireRow` uses `*float64` for nullable
   unmarshal. Merging the two would break either the emit path (RFC 8259 bare NaN)
   or the unmarshal path (null vs 0 ambiguity). ADR-0770.

9. **Schema auto-detection** (`cmd/vmafx-tune/cmd/report.go` `loadReportFile`):
   the `report` subcommand probes `renditions` vs `rows` top-level keys to
   determine whether a JSON file is a ladder or compare payload. This probe is
   the contract between the two schemas. Any future schema that omits both keys
   must be added to the probe before the `report` subcommand can read it.

10. **clikit root + one-shot fx adapter** (`cmd/vmafx-tune/cmd/root.go`,
    `golusoris.go`): the CLI root is built with `clikit.New` and every
    subcommand with `clikit.Command` (ADR-1119 Phase-1). One-shot subcommands
    (compare, ladder, report, and the not-yet-ported stubs) wire their handler
    via `clikit.WithRunE(withGolusoris(fn))`. `withGolusoris` boots an `fx`
    graph from `bootstrap.Base`, `fx.Populate`s a `*slog.Logger` + `*config.Config`
    into the handler, runs `fn`, then `app.Stop`s — and returns `fn`'s error so
    cobra sets the process exit code. **Do not** swap these to
    `clikit.WithFx(golusoris.Core, fx.Invoke(fn))`: clikit's `WithFx` calls
    `app.Run()` (blocks until a signal) and discards the `fx.Invoke` error, so a
    one-shot command would hang and lose its exit code. New subcommands follow
    the same `withGolusoris` shape; keep the `run*` signature
    `func(ctx context.Context, d deps, ...) error` so logger/config injection
    flows through.

11. **`fx.NopLogger`, not `bootstrap.FxLogger()`** (`cmd/vmafx-tune/cmd/golusoris.go`):
    `withGolusoris` attaches `fx.NopLogger` so fx's own provide/invoke/lifecycle
    events never reach the console. `bootstrap.FxLogger()` (which routes fx
    events onto the app `*slog.Logger`) is for long-running services; on a
    one-shot CLI it floods `stderr` with dependency-graph chatter on every
    invocation. Domain diagnostics still go through the injected `*slog.Logger`.

12. **Log level comes natively from the VMAFX_-prefixed config** (golusoris
    v0.5.0, #234): the `golusoris.log` submodule reads `log.level` / `log.format`
    from the shared config singleton, so a root-scope
    `fx.Replace(config.Options{EnvPrefix:"VMAFX_"})` penetrates and the
    auto-built `*slog.Logger` honors `VMAFX_LOG_LEVEL` / `VMAFX_LOG_FORMAT` with
    **no decorator**. (The earlier `levelledLogger` v0.4.0 workaround was removed
    when the pin moved to v0.5.0.) `TestGolusorisInjection_ConfigDrivesLogLevel`
    guards the behavior — it builds the graph without any decorator, matching
    `withGolusoris()` exactly.

13. **Per-shot plan JSON is byte-compatible, not merely schema-compatible**
    (`pkg/pershot/plan_json.go`): the Python emitter is
    `json.dumps(plan_doc, indent=2, sort_keys=True)`. Two consequences are
    load-bearing and easy to break by accident. (a) Every wire struct
    (`planWire`, `shotWire`) declares its fields in **alphabetical JSON-key
    order** — that is what reproduces `sort_keys=True`. Reordering them for
    readability silently breaks byte-parity. (b) Floats go through `pyFloat`,
    which restores Python `repr()` form (`24.0`, not Go's `24`) and its
    fixed-vs-exponent threshold; `ensureASCII` reproduces
    `ensure_ascii=True`, and `SetEscapeHTML(false)` stops Go escaping
    `< > &` where Python does not. The diff harness is
    `TestRenderPlanJSON_GoldenMatchesPython`; a change that fails it is a
    regression, not a formatting preference. ADR-0531 fixes the
    NaN-to-`null` mapping for `bitrate_kbps`.

14. **Two quality windows per codec, and they are not interchangeable**
    (`pkg/encoder/adapter.go`): `Adapter.AbsoluteLo/Hi` is the window the CRF
    bisect **searches** (ADR-0538 — wide, so premium-archival VMAF targets
    stay reachable), while `Adapter.QualityLo/Hi` is the informative window
    the per-shot tuner **clamps the final recommendation into**. They differ
    for `libx265` (0..51 vs 15..40) and `libsvtav1` (0..63 vs 20..50).
    `AdapterEncoder.CRFRange()` deliberately returns the *absolute* pair
    because `bisect.Run` consumes it as the search domain. Collapsing the two
    changes which CRFs the tuner can reach.

15. **`EncodeParams.InputArgs` vs `ExtraArgs` is a placement contract**
    (`pkg/encoder/encoder.go`): `InputArgs` are emitted **before** `-i`,
    `ExtraArgs` **after** `-c:v`. ffmpeg rejects demuxer options
    (`-f rawvideo -pix_fmt -s -r`) and device-init options
    (`-init_hw_device`, `-filter_hw_device`) anywhere but the pre-input
    position — the QSV chain failed with `-22 Invalid argument` for exactly
    this reason until the split existed (ADR-0601). Filter options (`-vf`)
    must stay post-input. Do not "simplify" the two fields into one.

16. **`YUVScoreFunc` is not interchangeable with `VMAFScoreFunc`**
    (`pkg/bisect/`): `VMAFScoreFunc` passes both paths to `vmaf` with no
    geometry flags, which only works for a Y4M pair. `YUVScoreFunc` is the
    raw-YUV path: it decodes a containerised distorted file first and passes
    `--width/--height/--pixel_format/--bitdepth/--model`. Those flags flip
    libvmaf's `use_yuv` branch, which is why `.y4m` is deliberately **absent**
    from `rawYUVSuffixes` — a Y4M header then trips the file-size guard in
    `raw_input_open` (ADR-0499).

17. **`--predicate-module` and `--fast-nr` fail fast, they are not ignored**
    (`cmd/vmafx-tune/cmd/pershot.go` `rejectUnportedPerShotFlags`): both flags
    are registered so the CLI surface matches the Python parser, but invoking
    either returns an error naming the Python fallback. Accepting and ignoring
    them would silently change a run's semantics (a custom predicate would be
    replaced by the bisect; NR early-elimination would just not happen). If
    an ONNX Go binding lands, `--fast-nr` graduates here first.

13. **`fast` exit codes travel on the error** (`cmd/vmafx-tune/cmd/fast.go`,
    `root.go` `Execute`): cobra maps any `RunE` error to exit 1, but
    `vmaf-tune fast` has a three-way contract — 0 (agreed), 2 (usage /
    environment), 3 (proxy/verify gap beyond tolerance; fall back to the slow
    grid). `runFast` therefore wraps its errors in `*fastExitError`, and
    `Execute` consults `fastExitCode(err)` before `os.Exit`. The exit-3 path
    still writes the payload first — callers parse it *and* branch on the
    status. Any new subcommand with its own exit contract follows the same
    shape rather than adding a second exit switch.

14. **`RecommendResult` field order IS the schema** (`pkg/fast/fast.go`,
    `jsonfloat.go`): the Python CLI emits
    `json.dumps(result, indent=2, sort_keys=True)`, so the Go struct declares
    its fields in alphabetical order of their JSON tag and `recommendWire`
    mirrors that order. Reordering the fields silently breaks byte
    compatibility. The float fields go through `pythonFloatRepr`, which
    reproduces CPython's `float.__repr__` (an integral `target_vmaf` must print
    as `90.0`, and `1e6` as `1000000.0` — Go's default encoder prints `90` and
    `1e+06`). Non-finite floats are coerced to JSON `null`, following the
    `compare` sweep emitter rather than Python's non-standard `NaN` token.

15. **The proxy port guard is load-bearing** (`pkg/fast/proxy.go`):
    `ORTProxy.Score` refuses to run when the resolved model declares more than
    one ONNX input port. `fr_regressor_v2` has two (`features` `[N, 6]` and
    `codec` `[N, 14]`) and the Go seam (`pkg/ai.Registry.Infer` →
    `vmafx-ort-runner`) takes one flat vector. Do **not** "fix" this by
    concatenating the ports into a 20-D vector: `vmaftune/proxy.py` documents
    that the graph's first dense layer reads the 6-D `features` port only, so
    the codec dims become batch padding and `codec` receives nothing. Lift the
    guard only once the seam grows named inputs (or the model is re-exported
    single-port). `TestShippedModelIsTwoPort` fails loudly if the shipped
    artefact changes shape and the guard's documentation goes stale.

16. **The proxy vocabulary and scaler come from the sidecar, never a constant**
    (`pkg/fast/proxy.go` `CodecBlock` / `NormaliseFeatures`):
    `vmaftune/proxy.py` hardcodes an `ENCODER_VOCAB_V2` tuple that has drifted
    out of sync with `ai/scripts/train_fr_regressor_v2.py` and with
    `model/tiny/fr_regressor_v2.json` from index 3 onward, so its codec one-hot
    lands in the wrong slot for most codecs. The Go port reads `encoder_vocab`,
    `feature_mean` and `feature_std` from the model's own sidecar so they cannot
    drift from the installed checkpoint. Do not reintroduce a hardcoded
    vocabulary or drop the StandardScaler step.

17. **TPE tests are not `t.Parallel()`** (`pkg/fast/tpe_test.go`,
    `fast_test.go`): goptuna v0.9.0 draws part of its randomness from the
    process-global `math/rand` source, so concurrent studies perturb each
    other's trial sequences. Any test that asserts on a TPE outcome runs
    sequentially, and the tolerances are stated against a measured
    distribution (recorded in the test comments). Adding `t.Parallel()` to
    those tests reintroduces flakes; tests that never start a study may
    stay parallel.
