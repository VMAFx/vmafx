# AGENTS.md — cmd/vmafx-tune

Parent: [../../AGENTS.md](../../AGENTS.md).

Go port of vmaf-tune rate-quality tuning CLI. Installed as `vmafx-tune-go`
during migration; see Stage roadmap in
[ADR-0705](../../docs/adr/0705-vmafx-tune-go-stage1.md).

## Rebase-sensitive invariants

1. **JSON schema compatibility** (`pkg/report/report.go`): JSON output of
   `EmitJSON` must remain schema-compatible with Python `compare.py` v1/v2
   payloads. Python `report.py` renderer ingests this JSON directly. Any
   field rename or removal requires coordinated Python-side change. Add new
   optional fields only; never remove existing ones without schema-version bump.

2. **NaN coercion** (`pkg/report/report.go` `nanToNull`): `float64` fields
   that can be NaN (failed-row bitrate, VMAF, encode time) MUST serialize as
   JSON `null`, not bare `NaN` tokens. RFC 8259 strict parsers reject bare
   `NaN`. Mirror Python `_nan_to_none` discipline.

3. **Bisect midpoint bias** (`pkg/bisect/bisect.go`): midpoint rounds toward
   *higher* CRF end `(lo + hi + 1) / 2` so best-so-far record never
   populated with unvalidated CRF. Changing rounding direction breaks
   monotonicity invariant.

4. **ScoreFunc seam** (`pkg/bisect/bisect.go`): `ScoreFunc` = subprocess
   boundary. Tests inject mock score functions. Never merge score function
   inline into `Run`; seam load-bearing for unit testability.

5. **Stage-1 scope** (`pkg/encoder/encoder.go`): `encoder.New` accepts only
   `libx264` and `libx265`. Hardware encoders (NVENC, QSV, AMF) and SVT-AV1 =
   Stage-2 scope. Do not add hardware encoder support here without new ADR and
   associated hw-init flag plumbing from Python `compare.py`.

6. **Binary name** (`cmd/vmafx-tune/main.go`): binary installs as
   `vmafx-tune-go`, not `vmaf-tune`, during Stage 1 to avoid collisions with
   Python binary. Stage 3 (swap) will rename. Never install it as `vmaf-tune`
   in PR that does not also remove Python entry point.

7. **`errors.Join` for multi-step cleanup** (`pkg/bisect`, `pkg/encoder`,
   `pkg/storage`, `cmd/vmafx-controller/queue` — and any new sibling package
   growing similar pipeline): when primary error and cleanup error
   can both arise, return `errors.Join(primary, cleanup)` rather than
   silently dropping cleanup error via `_ = X()` or
   `X() //nolint:errcheck`. Guard cleanup `os.Remove` calls with
   `errors.Is(rmErr, os.ErrNotExist)` so not-yet-created file is not
   flagged as cleanup failure. `slog` error-attribute key =
   `"error"` everywhere (`"err"` retired). See ADR-0935.

8. **`WireRow` vs `Row` duality** (`pkg/report/`): `Row` (report.go) =
   emit-only shape used by `compare` to produce JSON output; `WireRow`
   (multi.go) = unmarshal-only shape used by `report` to read that JSON back
   in. Intentionally separate types. Do not merge them — `Row` uses bare
   `float64` + NaN convention for emit; `WireRow` uses `*float64` for nullable
   unmarshal. Merging two would break either emit path (RFC 8259 bare NaN)
   or unmarshal path (null vs 0 ambiguity). ADR-0770.

9. **Schema auto-detection** (`cmd/vmafx-tune/cmd/report.go` `loadReportFile`):
   `report` subcommand probes `renditions` vs `rows` top-level keys to
   determine whether JSON file = ladder or compare payload. Probe =
   contract between two schemas. Any future schema omitting both keys
   must be added to probe before `report` subcommand can read it.

10. **clikit root + one-shot fx adapter** (`cmd/vmafx-tune/cmd/root.go`,
    `golusoris.go`): CLI root built with `clikit.New`, every
    subcommand with `clikit.Command` (ADR-1119 Phase-1). One-shot subcommands
    (compare, ladder, report, not-yet-ported stubs) wire handler
    via `clikit.WithRunE(withGolusoris(fn))`. `withGolusoris` boots `fx`
    graph from `bootstrap.Base`, `fx.Populate`s `*slog.Logger` + `*config.Config`
    into handler, runs `fn`, then `app.Stop`s — returns `fn`'s error so
    cobra sets process exit code. **Do not** swap these to
    `clikit.WithFx(golusoris.Core, fx.Invoke(fn))`: clikit's `WithFx` calls
    `app.Run()` (blocks until signal) and discards `fx.Invoke` error, so
    one-shot command would hang, lose exit code. New subcommands follow
    same `withGolusoris` shape; keep `run*` signature
    `func(ctx context.Context, d deps, ...) error` so logger/config injection
    flows through.

11. **`fx.NopLogger`, not `bootstrap.FxLogger()`** (`cmd/vmafx-tune/cmd/golusoris.go`):
    `withGolusoris` attaches `fx.NopLogger` so fx's own provide/invoke/lifecycle
    events never reach console. `bootstrap.FxLogger()` (routes fx
    events onto app `*slog.Logger`) = for long-running services; on
    one-shot CLI floods `stderr` with dependency-graph chatter on every
    invocation. Domain diagnostics still go through injected `*slog.Logger`.

12. **Log level comes natively from VMAFX_-prefixed config** (golusoris
    v0.5.0, #234): `golusoris.log` submodule reads `log.level` / `log.format`
    from shared config singleton, so root-scope
    `fx.Replace(config.Options{EnvPrefix:"VMAFX_"})` penetrates, and
    auto-built `*slog.Logger` honors `VMAFX_LOG_LEVEL` / `VMAFX_LOG_FORMAT` with
    **no decorator**. (Earlier `levelledLogger` v0.4.0 workaround removed
    when pin moved to v0.5.0.) `TestGolusorisInjection_ConfigDrivesLogLevel`
    guards behavior — builds graph without any decorator, matching
    `withGolusoris()` exactly.

13. **Per-shot plan JSON byte-compatible, not merely schema-compatible**
    (`pkg/pershot/plan_json.go`): Python emitter =
    `json.dumps(plan_doc, indent=2, sort_keys=True)`. Two consequences
    load-bearing, easy to break by accident. (a) Every wire struct
    (`planWire`, `shotWire`) declares fields in **alphabetical JSON-key
    order** — reproduces `sort_keys=True`. Reordering them for
    readability silently breaks byte-parity. (b) Floats go through `pyFloat`,
    restoring Python `repr()` form (`24.0`, not Go's `24`) and its
    fixed-vs-exponent threshold; `ensureASCII` reproduces
    `ensure_ascii=True`, `SetEscapeHTML(false)` stops Go escaping
    `< > &` where Python does not. Diff harness =
    `TestRenderPlanJSON_GoldenMatchesPython`; change failing it =
    regression, not formatting preference. ADR-0531 fixes
    NaN-to-`null` mapping for `bitrate_kbps`.

14. **Two quality windows per codec, are not interchangeable**
    (`pkg/encoder/adapter.go`): `Adapter.AbsoluteLo/Hi` = window CRF
    bisect **searches** (ADR-0538 — wide, so premium-archival VMAF targets
    stay reachable), while `Adapter.QualityLo/Hi` = informative window
    per-shot tuner **clamps final recommendation into**. Differ
    for `libx265` (0..51 vs 15..40) and `libsvtav1` (0..63 vs 20..50).
    `AdapterEncoder.CRFRange()` deliberately returns *absolute* pair
    because `bisect.Run` consumes it as search domain. Collapsing two
    changes which CRFs tuner can reach.

15. **`EncodeParams.InputArgs` vs `ExtraArgs` = placement contract**
    (`pkg/encoder/encoder.go`): `InputArgs` emitted **before** `-i`,
    `ExtraArgs` **after** `-c:v`. ffmpeg rejects demuxer options
    (`-f rawvideo -pix_fmt -s -r`) and device-init options
    (`-init_hw_device`, `-filter_hw_device`) anywhere but pre-input
    position — QSV chain failed with `-22 Invalid argument` for exactly
    this reason until split existed (ADR-0601). Filter options (`-vf`)
    must stay post-input. Do not "simplify" two fields into one.

16. **`YUVScoreFunc` is not interchangeable with `VMAFScoreFunc`**
    (`pkg/bisect/`): `VMAFScoreFunc` passes both paths to `vmaf` with no
    geometry flags, only works for Y4M pair. `YUVScoreFunc` = raw-YUV
    path: decodes containerised distorted file first, passes
    `--width/--height/--pixel_format/--bitdepth/--model`. Those flags flip
    libvmaf's `use_yuv` branch, why `.y4m` deliberately **absent**
    from `rawYUVSuffixes` — Y4M header then trips file-size guard in
    `raw_input_open` (ADR-0499).

17. **`--predicate-module` and `--fast-nr` fail fast, are not ignored**
    (`cmd/vmafx-tune/cmd/pershot.go` `rejectUnportedPerShotFlags`): both flags
    registered so CLI surface matches Python parser, but invoking
    either returns error naming Python fallback. Accepting, ignoring
    them would silently change run's semantics (custom predicate would be
    replaced by bisect; NR early-elimination would not happen). If
    ONNX Go binding lands, `--fast-nr` graduates here first.

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

23. **Python-compatible JSON is not `encoding/json`** (`pkg/pyjson`, one
    CPython-JSON encoder since ADR-1137): every payload a ported
    subcommand also emits from Python goes through `pyjson.Marshal` /
    `MarshalIndentSorted` / `MarshalStrict`, never
    `json.Marshal`/`MarshalIndent`. Go, CPython disagree on four:
    struct-field order vs sorted keys, HTML escaping, non-ASCII
    escaping, float rendering (`float64(92)` = `92` in Go, `92.0` in
    CPython; switch to exponent notation at 1e6 vs 1e16).
    `pyjson.FloatRepr` pinned against CPython `repr()` by
    `pkg/pyjson/testdata/float_repr.txt` corpus. Reaching for `encoding/json`
    in ported emit path silently breaks
    byte parity. (`compare`'s `emitSweepJSON` predates this package, still
    uses `MarshalIndent` with declaration-ordered struct fields; known
    gap, not pattern to copy.)

24. **`encode-profile` emits no `-init_hw_device` chain — deliberately**
    (`pkg/encodeprofile/encode.go` `BuildFFmpegCommand`): FFmpeg's QSV bridge
    needs VA-API device flags before first `-i` (ADR-0601), and
    `vmaftune.compare` injects them via its own pre-input argv. But
    `vmaftune.encode.build_ffmpeg_command` — function this ports, and
    one `encode-profile` calls — never has. Adding chain here would make
    Go `--dry-run` argv differ from Python's for every QSV row. Fix it in
    both implementations at once, or not at all.

25. **AMF adapters emit constant-QP block twice** (`pkg/codecadapter`
    `amfExtraParams`): CPython's `encode._resolve_codec_args` inspects each
    adapter's `extra_params` signature, for two-parameter AMF variant,
    appends return value after codec slice — so
    `-quality/-rc/-qp_i/-qp_p` appears twice with identical values. FFmpeg takes
    last-wins so duplicate inert, but IS in argv Python prints
    under `--dry-run`, records in corpus rows. Go port reproduces it on
    purpose; de-duplicating it = parity break, not cleanup.

26. **`sidecar` group's exit statuses and fixtures = Python
    contract** (`cmd/vmafx-tune/cmd/sidecar.go`, `sidecar_parity_test.go`,
    `testdata/sidecar/`): every validation failure exits 2:
    `useUsageExitCode` for flag-layer errors; `requireFlags` instead of
    `MarkFlagRequired` for missing flags; `asUsageError` on codec / model /
    feature-file / capture-file paths. Cache-dir and `state.json` write
    failures stay exit 1, because Python `OSError` there uncaught. Do not
    wrap whole run function in `asUsageError`; split = contract.
    `TestSidecarPythonParity` diffs every stdout payload and every `state.json`
    snapshot byte-for-byte against fixtures dumped from Python CLI by
    `testdata/sidecar/regen.sh` (pinned host UUID, relative `--cache-dir`);
    regenerate them only with coordinated change on both sides. Stderr
    wording is not pinned, but `batch-record` skip-line numbers are:
    `splitLinesUniversal` reproduces CPython's `newline=None` iteration so
    they match, must not be swapped back for `bufio.Scanner` (caps
    line length, cannot split on lone `\r`).
27. **`vmafx-ort-runner` is a repository artefact, not an environment
    assumption** (`cmd/vmafx-ort-runner`, ADR-1134): `pkg/predictor.ORTSession`
    (one ORT-session adapter since ADR-1137; `pkg/tune/predictor` = its
    transitional alias) execs it through `pkg/ai.Registry.Infer`; dev
    container plus Go CI job build it from `./cmd/...`, smoke-run
    it against `model/predictor_libx264.onnx`. When `predict --model`
    degrades to analytical curve, log line carries runner's
    stderr — `exit status 3` means linked libvmaf has no ONNX Runtime,
    `not found on PATH` means it is not installed. Do not reintroduce
    comments or docs describing runner as external or "bundled by the
    image": that claim hid missing binary for three months.
    Runner still takes one flat `[1, N]` vector (invariant 20 stands);
    named inputs and stdin transport = protocol extensions of
    in-tree runner, not new dependencies.
28. **Saliency moments reach `predict` via shared session factory
    (PR for #1272 gaps).** `predict --use-saliency` opens session via
    `saliencySessionFactory` (same path `recommend --saliency` uses), writes
    per-shot population moments into `ShotFeatures.SaliencyMean` /
    `SaliencyVar`, landing at `predictor.FeatureVector` indices 5-6.
    Nonexistent `--saliency-model` = usage error (exit 2, naming file);
    unavailable runtime degrades to `(0, 0)` with log line. Never bypass
    factory with direct `newSaliencySession`; any change to
    feature-vector layout updates `predictor.FeatureVector` and
    `saliency_honesty_test.go` in same commit. Retired Python CLI
    redirects in `compare`/`root`/`main` gone; do not reintroduce
    "use vmaf-tune (Python)" hints for subcommands Go binary implements.

29. **One `vmafx.tune.command` span per invocation, from `withGolusoris`**
    (`cmd/vmafx-tune/cmd/golusoris.go`, ADR-0782 / ADR-1119): shared
    one-shot fx adapter = CLI's only OTel seam. Builds
    `bootstrap.Base` (golusoris `otel.Module`, no-op without OTLP endpoint,
    flushed by `app.Stop`), starts span with `AttrTuneCommand` =
    `cmd.CommandPath()`, ends it **before** `app.Stop` so OnStop flush
    exports it. Subcommands must not start own root span or call
    `InitOTel`; `deps.OTel` exists so `otel_test.go` can prove wiring.
    `TestWithGolusoris_CommandSpanWrapsRun` locks span, its attribute and
    its error status.
