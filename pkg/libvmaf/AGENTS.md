# AGENTS.md — pkg/libvmaf

Go wrapper around the libvmaf C ABI. Provides three scoring surfaces:

- `Scorer.Score` — subprocess delegation to the `vmaf` CLI binary (legacy).
- `ScoreDirect` — direct cgo into `libvmaf.so` for a file pair (ADR-0931 Phase 1+).
- `StreamScorer` — stateful cgo context for in-memory per-frame scoring of a
  raw-byte stream, backing the gRPC `ScoreStream` RPC (`stream.go`, ADR-0933
  Phase 2).

## Rebase-sensitive invariants

1. **Locale pin** (`direct.go::init`): `setlocale(LC_NUMERIC, "C")` runs
   once on package import via the `score_direct_set_locale_c` cgo helper.
   ADR-0137 makes this mandatory before any libvmaf function that reads
   or writes a floating-point string. Removing the init() pin
   re-introduces the locale-leak class of bugs (German `","` decimal
   separator parsed as `0`).

2. **Picture ownership transfer** (`direct.go::ScoreDirect`): every
   `vmaf_picture_alloc` MUST be paired with either a subsequent
   `vmaf_read_pictures` call (which transfers ownership to libvmaf) OR
   an explicit `vmaf_picture_unref` on the Go side. The current loop
   uses the structure:

   ```text
   vmaf_picture_alloc(ref); vmaf_picture_alloc(dis)
   // on error before read: unref both, return
   vmaf_read_pictures(ctx, &ref, &dis, idx)  // ownership transferred
   // MUST NOT call unref after this point
   ```

   Adding an early-return between alloc and read without an unref leaks
   `posix_memalign`-backed plane storage. Add a unit test for any new
   error path.

3. **Errno mapping is contract-frozen** (`errors.go::mapErrno`): the
   four typed sentinels (`ErrInvalidArgument`, `ErrOutOfMemory`,
   `ErrModelNotFound`, `ErrPictureRead`) and their stdlib wrapping
   (`os.ErrInvalid`, `os.ErrNotExist`) are pinned by ADR-0931. Callers
   branch via `errors.Is`; renaming or unwrapping breaks the contract.
   Extensions require an ADR amendment.

4. **`ScoreDirect` is CPU-only in Phase 1**: `cpumask=0, gpumask=0` in
   the `vmaf_init` configuration. GPU backends land in Phase 2 via the
   matching backend-init calls (`vmaf_cuda_init` etc.) — do not flip
   the gpumask without wiring the runtime first.

5. **Phase 1 model scope = SVM only**: `ScoreDirect` calls
   `vmaf_model_load_from_path` which routes by file extension. `.onnx`
   models hit a different code path inside libvmaf that needs an ONNX
   Runtime session — Phase 3 scope. The MCP dispatcher
   (`cmd/vmafx-mcp/impl_direct.go`) is responsible for routing `.onnx`
   to the subprocess fallback; do not silently accept `.onnx` here.

6. **CGO_LDFLAGS coupling**: the cgo `LDFLAGS` directive in `libvmaf.go`
   points at `core/build-cpu/src` for local dev. Production builds rely
   on `libvmaf.so` at `/usr/local/lib` via `LD_LIBRARY_PATH` or
   `ldconfig`. Changing the local path requires updating the
   `LD_LIBRARY_PATH=$(pwd)/core/build-cpu/src` invocation in
   `docs/architecture/mcp-cgo-direct-migration.md`'s reproducer block
   and in any agent skill that builds the Go tree.

7. **`VMAF_MCP_ALLOW` uses `filepath.SplitList`** (`paths.go::AllowedRoots`,
   ADR-1084): the env-var path list is split with `filepath.SplitList` so
   both Unix (`":"` separator) and Windows (`";"` separator) are handled
   correctly. Do not replace this with `strings.Split(extra, ":")` — that
   silently mis-splits Windows drive-letter paths.

8. **`StreamScorer` harvests per-frame scores AFTER flush** (`stream.go`,
   ADR-0933): temporal VMAF features (notably motion) only finalise once
   `vmaf_read_pictures(NULL, NULL, 0)` has flushed the engine. `Finish`
   therefore calls `vmaf_score_at_index` / `vmaf_feature_score_at_index`
   only after the flush — never inside `PushFrame`. Do not "optimise" by
   trying to emit a per-frame score the moment a frame is pushed; the
   motion feature for frame N is not available until the sequence is
   flushed, and any such change would silently corrupt motion-dependent
   scores. The same picture-ownership-transfer rule as invariant 2 applies
   to every `PushFrame` alloc/read pair.

9. **`StreamScorer.streamFeatures` are the model's registered feature names**
   (`stream.go`): the per-feature lookup keys passed to
   `vmaf_feature_score_at_index` are the exact strings from the model JSON's
   `feature_names` (e.g. `VMAF_integer_feature_adm2_score`), NOT the alias
   names (`integer_adm2`) or the CLI/JSON pooled-metric keys (`adm2`). A
   wrong name does not error loudly — `vmaf_feature_score_at_index` returns
   a non-zero rc that the code silently skips, producing an empty feature
   map. If you change models or the feature set, re-verify the names against
   `model/<name>.json` `model_dict.feature_names`.

10. **Subprocess model selection uses the CLI parameter grammar**
    (`libvmaf.go::Scorer.Score`): the model argument is
    `-m path=/absolute/model.json`, not `-m /absolute/model.json`. The latter
    is a syntactically invalid model option and makes every file-backed server
    score fail before libvmaf runs. Keep the argv regression test when changing
    the scorer or CLI parser.
