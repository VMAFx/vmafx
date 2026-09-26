# AGENTS.md — pkg/libvmaf

Go wrapper around libvmaf C ABI. Provides three scoring surfaces:

- `Scorer.Score` — subprocess delegation to `vmaf` CLI binary (legacy).
- `ScoreDirect` — direct cgo into `libvmaf.so` for file pair (ADR-0931 Phase 1+).
- `StreamScorer` — stateful cgo context for in-memory per-frame scoring of
  raw-byte stream, backing gRPC `ScoreStream` RPC (`stream.go`, ADR-0933
  Phase 2).

## Rebase-sensitive invariants

1. **Locale pin** (`direct.go::init`): `setlocale(LC_NUMERIC, "C")` runs
   once on package import via `score_direct_set_locale_c` cgo helper.
   ADR-0137 makes this mandatory before any libvmaf function reading
   or writing floating-point string. Removing init() pin
   re-introduces locale-leak bug class (German `","` decimal
   separator parsed as `0`).

2. **Picture ownership transfer** (`direct.go::ScoreDirect`): every
   `vmaf_picture_alloc` MUST pair with either subsequent
   `vmaf_read_pictures` call (transfers ownership to libvmaf) OR
   explicit `vmaf_picture_unref` on Go side. Current loop uses this
   structure:

   ```text
   vmaf_picture_alloc(ref); vmaf_picture_alloc(dis)
   // on error before read: unref both, return
   vmaf_read_pictures(ctx, &ref, &dis, idx)  // ownership transferred
   // MUST NOT call unref after this point
   ```

   Adding early-return between alloc and read without unref leaks
   `posix_memalign`-backed plane storage. Add unit test for any new
   error path.

3. **Errno mapping = contract-frozen** (`errors.go::mapErrno`): four
   typed sentinels (`ErrInvalidArgument`, `ErrOutOfMemory`,
   `ErrModelNotFound`, `ErrPictureRead`) + stdlib wrapping
   (`os.ErrInvalid`, `os.ErrNotExist`) pinned by ADR-0931. Callers
   branch via `errors.Is`; renaming or unwrapping breaks contract.
   Extensions require ADR amendment.

4. **`ScoreDirect` = CPU-only in Phase 1**: `cpumask=0, gpumask=0` in
   `vmaf_init` configuration. GPU backends land in Phase 2 via
   matching backend-init calls (`vmaf_cuda_init` etc.) — never flip
   gpumask without wiring runtime first.

5. **Phase 1 model scope = SVM only**: `ScoreDirect` calls
   `vmaf_model_load_from_path`, routes by file extension. `.onnx`
   models hit different code path inside libvmaf needing ONNX
   Runtime session — Phase 3 scope. MCP dispatcher
   (`cmd/vmafx-mcp/impl_direct.go`) routes `.onnx`
   to subprocess fallback; never silently accept `.onnx` here.

6. **CGO linking fails closed**: `libvmaf.go` deliberately has no `#cgo
   LDFLAGS` fallback. Every build caller must set `CGO_LDFLAGS` to a verified
   fork library: local Make/CI use `core/build-cpu/src`; production container
   stages use `/usr/local/lib`. This prevents a missing build directory from
   silently resolving an unrelated distro libvmaf. Keep the Make targets,
   `go-ci.yml`, `Dockerfile.go-server`, `docker/Dockerfile.node`, the dev
   container, `scripts/ci/test_go_workflow_contract.py`, and the documented
   direct-command examples aligned.

7. **`VMAF_MCP_ALLOW` uses `filepath.SplitList`** (`paths.go::AllowedRoots`,
   ADR-1084): env-var path list split with `filepath.SplitList` so
   both Unix (`":"` separator) and Windows (`";"` separator) handled
   correctly. Never replace with `strings.Split(extra, ":")` — that
   silently mis-splits Windows drive-letter paths.

8. **`StreamScorer` harvests per-frame scores AFTER flush** (`stream.go`,
   ADR-0933): temporal VMAF features (notably motion) only finalise once
   `vmaf_read_pictures(NULL, NULL, 0)` flushes engine. `Finish`
   therefore calls `vmaf_score_at_index` / `vmaf_feature_score_at_index`
   only after flush — never inside `PushFrame`. Never "optimise" by
   trying to emit per-frame score moment frame pushed; motion feature
   for frame N unavailable until sequence flushed — any such change
   silently corrupts motion-dependent scores. Same
   picture-ownership-transfer rule as invariant 2 applies
   to every `PushFrame` alloc/read pair.

9. **`StreamScorer.streamFeatures` = model's registered feature names**
   (`stream.go`): per-feature lookup keys passed to
   `vmaf_feature_score_at_index` = exact strings from model JSON's
   `feature_names` (e.g. `VMAF_integer_feature_adm2_score`), NOT alias
   names (`integer_adm2`) or CLI/JSON pooled-metric keys (`adm2`). Wrong
   name does not error loudly — `vmaf_feature_score_at_index` returns
   non-zero rc code silently skips, producing empty feature
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
    (`direct.go::cgoScoringOwner`, `stream.go::StreamScorer.Close`):
    `vmaf_close` returning anything other than zero retains a teardown-only
    context. Never destroy the registered model until a close attempt returns
    zero. Stateful `StreamScorer.Close` returns the error and may be retried;
    `PushFrame` and `Finish` stay disabled after teardown begins. Function-
    scoped `ScoreDirect` and constructor unwinds make one immediate retry and
    deliberately retain the C owners after persistent failure rather than
    create a use-after-free.
