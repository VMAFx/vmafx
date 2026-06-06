# AGENTS.md — pkg/libvmaf

Go wrapper around the libvmaf C ABI. Provides two scoring surfaces:

- `Scorer.Score` — subprocess delegation to the `vmaf` CLI binary (legacy).
- `ScoreDirect` — direct cgo into `libvmaf.so` (ADR-0931 Phase 1+).

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
