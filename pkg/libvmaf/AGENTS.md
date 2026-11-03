# AGENTS.md — pkg/libvmaf

Go wrapper around libvmaf. Provides three scoring surfaces plus tiny-AI session:

- `Scorer.Score` / `Scorer.Run` — subprocess delegation to the `vmaf` CLI binary
  (`libvmaf.go`, `run.go`, `readers_unix.go`); stays subprocess.
- `ScoreDirect` — in-process scoring of file pair (ADR-0931 Phase 1+), on the
  generated `pkg/vmafx` binding (`direct.go`).
- `StreamScorer` — stateful context for in-memory per-frame scoring of raw-byte
  stream, backing gRPC `ScoreStream` RPC (`stream.go`, ADR-0933 Phase 2), on the
  same binding.
- `DNNSession` — standalone ONNX Runtime session (`dnn.go`); only file of the
  package that still imports `"C"`.

Issue 2434: `ScoreDirect` and `StreamScorer` share one session (`engine.go`:
context, model, host frames, submit, flush, pooled score) and one validation
(`validate.go`); both call `pkg/vmafx` only. C call `pkg/vmafx` lacks is added
to `scripts/codegen/vmafx_api/emit_go.py`, never as second cgo copy here.

## Rebase-sensitive invariants

1. **Locale pin** (`pkg/vmafx/api_gen.go::init`, emitted by `emit_go.py`):
   `setlocale(LC_NUMERIC, "C")` runs once on package import. ADR-0137 makes
   this mandatory before any libvmaf function reading or writing
   floating-point string. Removing pin re-introduces locale-leak bug
   class (German `","` decimal separator parsed as `0`). This package no
   longer carries its own pin; generated init is only one.

2. **Frame ownership transfer** (`engine.go::scoringSession.submitPair`,
   `direct.go::readAndQueueDirectFrame`, `stream.go::PushFrame`): every
   `vmafx.NewHostFrame` pair is either consumed by `Context.Submit`
   (`vmafx_submit` takes both references on every path, success or failure;
   Go `Frame` handle is cleared by `takeHandle`) or released by a
   deferred `Frame.Close`. After submit deferred `Close` is no-op.

   ```text
   NewHostFrame(ref); NewHostFrame(dis); defer ref.Close(); defer dis.Close()
   // on error before submit: the deferred Close releases both
   ctx.Submit(ref, dis, idx)  // ownership transferred, Close is now a no-op
   ```

   Adding path that hands frame to C without `takeHandle`, or closes it
   twice through another route, leaks or double-frees plane storage. Add unit
   test for any new error path.

3. **Errno mapping = contract-frozen** (`errors.go::mapErrno`,
   `vmafx_errors.go::mapVmafxError`): four typed sentinels
   (`ErrInvalidArgument`, `ErrOutOfMemory`, `ErrModelNotFound`,
   `ErrPictureRead`) + stdlib wrapping (`os.ErrInvalid`, `os.ErrNotExist`)
   pinned by ADR-0931. Callers branch via `errors.Is`; renaming or unwrapping
   breaks contract. Extensions require ADR amendment. #2434 migration note:
   `pkg/vmafx` fails with `*vmafx.Error` (stable `Status`), not bare errno,
   so status selects errno (`StatusEInvalid` EINVAL, `StatusENoMem`
   ENOMEM, `StatusENotFound` ENOENT, `StatusEIO` EIO) and sentinel;
   any other status keeps engine errno in numeric fallback;
   `StatusEDevice` is never read as errno. The `*vmafx.Error` stays in the
   chain (`errors.As`). One difference is deliberate: vmafx reports missing
   model file as `StatusEIO`, so `engine.go::loadScoringModel` stats path
   after failed load and returns `ErrModelNotFound`. Pinned by
   `TestMapVmafxErrorKeepsTheErrnoContract`, `TestLoadScoringModelErrors`.

4. **`ScoreDirect` = CPU-only in Phase 1**: `cpumask=0, gpumask=0` in
   the `vmafx.ContextConfig` (`engine.go::newScoringConfig`). GPU backends land
   in Phase 2 via matching backend-init calls — never flip gpumask without wiring
   runtime first.

5. **Phase 1 model scope = SVM only**: `ScoreDirect` calls
   `vmafx_model_load_file`, routes by file extension. `.onnx`
   models hit different code path inside libvmaf needing ONNX
   Runtime session — Phase 3 scope. MCP dispatcher
   (`cmd/vmafx-mcp/impl_direct.go`) routes `.onnx`
   to subprocess fallback; never silently accept `.onnx` here.

6. **CGO linking fails closed**: `pkg/vmafx`, `dnn.go` and `libvmaf.go`
   deliberately have no `#cgo LDFLAGS` fallback (package `-I` flag lives in
   `dnn.go`; `libvmaf.go` has no cgo). Every build caller must set
   `CGO_LDFLAGS` to verified fork library: local Make/CI use
   `core/build-cpu/src`; production container stages use `/usr/local/lib`.
   This prevents missing build directory from
   silently resolving unrelated distro libvmaf. Keep Make targets,
   `go-ci.yml`, `Dockerfile.go-server`, `docker/Dockerfile.node`, dev
   container, `scripts/ci/test_go_workflow_contract.py`, and documented
   direct-command examples aligned.

7. **`VMAF_MCP_ALLOW` uses `filepath.SplitList`** (`paths.go::AllowedRoots`,
   ADR-1084): env-var path list split with `filepath.SplitList` so
   both Unix (`":"` separator) and Windows (`";"` separator) handled
   correctly. Never replace with `strings.Split(extra, ":")` — that
   silently mis-splits Windows drive-letter paths.

8. **`StreamScorer` harvests per-frame scores AFTER flush** (`stream.go`,
   ADR-0933): temporal VMAF features (notably motion) only finalise once
   `vmafx_flush` flushes engine. `Finish`
   therefore calls `vmafx_score_frame` / `vmafx_feature_score`
   only after flush — never inside `PushFrame`. Never "optimise" by
   trying to emit per-frame score moment frame pushed; motion feature
   for frame N unavailable until sequence flushed — any such change
   silently corrupts motion-dependent scores. Same
   frame-ownership-transfer rule as invariant 2 applies
   to every `PushFrame` frame pair.

9. **`StreamScorer.streamFeatures` = model's registered feature names**
   (`stream.go`): per-feature lookup keys passed to
   `vmafx_feature_score` = exact strings from model JSON's
   `feature_names` (e.g. `VMAF_integer_feature_adm2_score`), NOT alias
   names (`integer_adm2`) or CLI/JSON pooled-metric keys (`adm2`). Wrong
   name does not error loudly — `Context.FeatureScore` returns an
   error that `featuresAtIndex` silently skips, producing empty feature
   map. On changing models or feature set, re-verify names against
   `model/<name>.json` `model_dict.feature_names`.

10. **Subprocess model selection uses CLI parameter grammar**
    (`libvmaf.go::Scorer.Score`): model argument =
    `-m path=/absolute/model.json`, not `-m /absolute/model.json`. Latter
    = syntactically invalid model option, makes every file-backed server
    score fail before libvmaf runs. Keep argv regression test when changing
    scorer or CLI parser.

11. **Empty input name means positional binding** (`dnn.go::run`,
    ADR-1134): `Predict(ctx, "", x, rows, cols)` passes NULL
    `VmafDnnInput.name`, which `dnn.h` binds at descriptor's index.
    `cmd/vmafx-ort-runner` relies on this to serve graphs whose input name
    it does not know (every shipped `model/predictor_*.onnx` names its
    input `input`; `modeleval` passes `features`). Unconditional
    `C.CString(inputName)` would bind to graph input literally named `""`
    and fail every runner call. Pinned by `TestDNNSessionPositionalBinding`,
    which only runs against ORT-enabled libvmaf — Go CI job builds one.

12. **Context teardown is exact-zero and dependency ordered**
    (`engine.go::cgoScoringOwner`, `stream.go::StreamScorer.Close`):
    `Context.Close` (`vmafx_context_destroy`) failing retains the
    teardown-only context (ADR-1336). Never drop registered model until the
    close attempt succeeds. Stateful `StreamScorer.Close` returns error and may
    be retried; `PushFrame` and `Finish` stay disabled after teardown begins.
    Function-scoped `ScoreDirect` and constructor unwinds make one immediate
    retry and deliberately retain owners after persistent failure rather
    than create use-after-free. `mapCloseRC` stays exact-zero: positive
    status fails closed.

13. **`ScoreReaders` streams through inherited pipes** (`readers_unix.go`,
    ADR-1526): read ends passed as `cmd.ExtraFiles` -> `/dev/fd/3`,
    `/dev/fd/4`; parent closes its read-end copies right after `Start`
    (else a CLI that exits early leaves copiers blocked). A source read error
    (not EOF, not caused by our own close after `Wait`) fails the score even
    when the CLI exited 0 and is reported first. Non-Unix build returns
    `ErrStreamInputsUnsupported`; never fall back to a temp file.

14. **Package doc names every path into libvmaf** (`doc.go`): subprocess
    `Scorer`, in-process `ScoreDirect`, `StreamScorer`, cgo `DNNSession`. New
    entry point -> add to doc + `PACKAGE_DOC_SURFACES` in
    `scripts/ci/tests/test_go_libvmaf_linkage_docs.py`. No Go package importing
    this one (or cgo with libvmaf) may claim it does not link libvmaf; same
    test fails.

15. **One implementation per call (#2434)**: no `import "C"` for scoring in
    this package (`git grep -n 'import "C"' -- pkg/libvmaf` lists `dnn.go`
    only). bit-exact test `TestPooledMeanBitExactAcrossBindings` compares
    pooled mean of `vmaf_v1.0.16_3d0h` through `ScoreDirect`, `pkg/vmafx`
    and C API (`testdata/c_api_pooled_mean.c`) by IEEE-754 bits; change
    to numeric path that moves one bit fails it. subprocess path shares
    `newCLICommand`, `cliFailure`, `prepareRun` and `parseReport` between
    `Score`/`Run` and `ScoreReaders`; add to these, not second copy. `DNNSession`
    is not collapsed yet: `pkg/vmafx` has no DNN session handle in its Go
    binding.
