<!-- markdownlint-disable MD013 MD060 -->
# ADR-0931: MCP server — replace subprocess delegation with direct cgo (Phase 1)

- **Status**: Proposed
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `mcp`, `go`, `cgo`, `libvmaf`, `performance`, `vmafx`, `modernization`

## Context

`cmd/vmafx-mcp/impl.go` is 1306 lines of `exec.Command(vmaf-bin, ...)` plus
parse-stdout. Every MCP tool handler shells out to the `vmaf` CLI binary,
mirroring the Python server's pre-Go behaviour. The pattern was deliberate at
the time the Go server was introduced (see [ADR-0703](0703-vmafx-server-go-grpc.md),
"Direct cgo API calls (bypass vmaf binary)" alternative): it kept the first
Go cut tractable while the Python parity matrix was still in flight.

The subprocess model now has three measurable costs:

1. **Per-call latency floor.** Every `vmaf_score` invocation forks/execs a
   ~10 MB binary, parses `--help`-style flag plumbing inside `vmaf.c`,
   opens the model JSON, and re-allocates the entire scorer state. The
   floor is 80–200 ms even on a noop pair — material for IDE/MCP clients
   that issue many small scoring calls (per-shot bisects, ladder sweeps).
2. **Error surface.** Errors are recovered by parsing `stderr` and exit codes.
   The signal-to-noise is low: a missing model file and a malformed YUV emit
   nearly-identical strings. Programmatic clients (the gRPC server in
   `cmd/vmafx-server/`, the MCP IDE clients) get strictly less detail than
   what `libvmaf` already returns via negative errno.
3. **Operational coupling.** The Go binary must locate the `vmaf` binary at
   runtime (`pkg/libvmaf/paths.go::FindBinary`). Deployments need both the Go
   binary AND the C binary on disk, plus a search-path heuristic that breaks
   under distroless / read-only-root k8s pods unless `VMAF_BIN` is pinned by
   the operator.

`pkg/libvmaf/libvmaf.go` already advertises a `ScoreDirect` extension point
(comments only, no implementation). The wrapper links `libvmaf.so` at build
time (cgo) for symbol resolution but only ever invokes it indirectly via
`exec.Command`.

## Decision

We will implement a **direct-cgo scoring path** in `pkg/libvmaf/ScoreDirect`,
gated by `VMAFX_MCP_DIRECT=1`, and migrate the two simplest MCP tools
(`vmaf_score`, `describe_model`) to use it as Phase 1. The existing
subprocess path remains the default and is **not** removed in this PR.

### C-function contract

The direct path calls libvmaf in the following order, mirroring
`core/tools/vmaf.c`:

1. `setlocale(LC_NUMERIC, "C")` — ADR-0137 (locale-leaked decimal parsing).
2. `vmaf_init(&ctx, cfg)` — `cfg = {LOG_LEVEL_WARNING, n_threads=0,
   n_subsample=1, cpumask=0, gpumask=0}` (auto-thread, all SIMD, CPU only
   in Phase 1).
3. `vmaf_model_load_from_path(&model, &mcfg, model_path)` — `mcfg = {name,
   flags=DEFAULT}`.
4. `vmaf_use_features_from_model(ctx, model)`.
5. For each frame index `i`:
   - `vmaf_picture_alloc(&ref_pic, pix_fmt, bpc, w, h)`
   - `vmaf_picture_alloc(&dis_pic, pix_fmt, bpc, w, h)`
   - read `w*h*1.5` (yuv420p) bytes into each picture's `data[0..2]` planes
   - `vmaf_read_pictures(ctx, &ref_pic, &dis_pic, i)` — **transfers
     ownership** of both pics; libvmaf calls `vmaf_picture_unref` internally.
6. Flush: `vmaf_read_pictures(ctx, NULL, NULL, 0)`.
7. `vmaf_score_pooled(ctx, model, VMAF_POOL_METHOD_MEAN, &score, 0,
   frame_count-1)`.
8. `vmaf_model_destroy(model)` + `vmaf_close(ctx)`.

### Ownership semantics

- `VmafContext *` — owned by Go; freed in `defer vmaf_close()`.
- `VmafModel *` — owned by Go; freed in `defer vmaf_model_destroy()`.
- `VmafPicture` — allocated by Go (stack struct, heap planes via
  `vmaf_picture_alloc`); ownership transferred to libvmaf on
  `vmaf_read_pictures`. On error before the read call, Go calls
  `vmaf_picture_unref` to free the planes.
- All `C.CString` allocations are paired with `C.free` via `defer`.

### Error mapping

libvmaf returns negative errno (`-EINVAL`, `-ENOMEM`, `-ENOENT`, ...). The Go
wrapper maps these to typed errors:

| C return | Go error |
|---|---|
| `-EINVAL` | `ErrInvalidArgument` (wraps `os.ErrInvalid`) |
| `-ENOMEM` | `ErrOutOfMemory` |
| `-ENOENT` | `ErrModelNotFound` (wraps `os.ErrNotExist`) |
| `-EIO` | `ErrPictureRead` |
| other < 0 | `fmt.Errorf("libvmaf %s returned %d", call, rc)` |

The mapping lives in `pkg/libvmaf/errors.go` (new file). Callers can branch
via `errors.Is(err, libvmaf.ErrModelNotFound)`.

### Fallback flag

`VMAFX_MCP_DIRECT=1` opts into the direct path per-process. Unset / any other
value falls back to the subprocess path. Logs at INFO level which path was
taken on first call so operators can confirm. This is Phase 1 only — Phase 3
flips the default once parity is broad and CI gates pass.

### Tools migrated in this PR (Phase 1)

- **`vmaf_score`** (simplest: ref + dis + geometry → mean VMAF). Direct path
  reads raw YUV, calls libvmaf, returns the same JSON shape as the subprocess
  path (`pooled_metrics.vmaf.mean` + `backend_used` + `backend_requested`).
- **`describe_model`** (no scoring; metadata only). Direct path calls
  `vmaf_model_load_from_path` + parses the file's own JSON (existing code) to
  emit `name`, `path`, `format`, `size_bytes`, `model_type`, `feature_names`.
  The cgo call here is essentially a syntactic validation — it confirms the
  model parses through libvmaf, which catches schema drift the file-only
  parser silently ignores.

### Out of scope (deferred to Phase 2+)

- GPU backends (`cuda`, `sycl`, `vulkan`, `hip`, `metal`) — Phase 1 is CPU
  only. Backend selection via `cpumask` / `gpumask` lands in Phase 2.
- Encoded-input tools (`vmaf_score_encoded`) — Phase 2 adds an ffmpeg-decode
  pipe into the direct scorer.
- Bootstrap / model-collection scoring — Phase 3.
- Removing the subprocess path — Phase 3 closeout PR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep subprocess delegation indefinitely | Zero migration risk; mirrors Python parity matrix | 80–200 ms per-call floor; poor error surface; deploy-time binary-search heuristic | Phase-4 modernization explicitly targets in-process scoring (memory `project_vmafx_phase4_language_modernization`) |
| Migrate all 16 tools in one PR | One-shot transition | ~1300 LOC handler rewrite + 16-handler test matrix in one PR violates ADR PR-size guidance; high revert cost on regression | Phase 1 ships 2 simplest tools behind a flag; Phase 2/3 extend |
| Rewrite the C `vmaf.c` pipeline in Go without cgo | Pure Go binary; no `libvmaf.so` dependency at runtime | Reimplements the entire feature-extractor pipeline + SIMD dispatch + GPU runtime — multi-month rewrite, infeasible | The C library IS the source of truth; cgo is the right surface |
| Use Go-only ffmpeg bindings to score | Removes one subprocess (ffmpeg) too | Doesn't solve the libvmaf subprocess; pulls in `goav`/cgo for ffmpeg, doubling the FFI surface | Orthogonal — Phase 2 ffmpeg integration is a separate decision |
| Default-on `VMAFX_MCP_DIRECT` in Phase 1 | Single code path | New code path with no production miles; risk of regression on rare YUV layouts (yuv422p, 10/12-bit) | Phase 1 keeps it opt-in until CI parity sweep is broad |

## Consequences

- **Positive**: Per-call latency drops from 80–200 ms to <5 ms on the direct
  path (no fork/exec, no `vmaf` argv parsing, no temp-file JSON round-trip).
  Typed errors enable programmatic recovery (`errors.Is(err,
  ErrModelNotFound)`). Distroless deployments can drop `/usr/local/bin/vmaf`
  from the image once Phase 3 completes.
- **Negative**: cgo build is required for the MCP server (already true via
  `pkg/libvmaf`'s existing cgo import — no new constraint). Reference-counted
  picture ownership is a foot-gun: a panicked read between `vmaf_picture_alloc`
  and `vmaf_read_pictures` leaks heap. The Go wrapper uses `defer
  vmaf_picture_unref` until the read transfers ownership, with explicit
  nil-out to avoid double-free.
- **Neutral / follow-ups**: Phase 2 migrates `vmaf_score_encoded`,
  `probe_backend`, `run_benchmark`. Phase 3 default-flips
  `VMAFX_MCP_DIRECT=1` after a full parity sweep against Netflix golden
  scores. Phase 3 closeout PR removes the subprocess path and the
  `FindBinary` heuristic. The `docs/architecture/mcp-cgo-direct-migration.md`
  document tracks the rollout per-tool.

## References

- [ADR-0703](0703-vmafx-server-go-grpc.md) — `vmafx-server` Go gRPC + HTTP
  service. The "Direct cgo API calls (bypass vmaf binary)" row in that ADR's
  Alternatives table is this ADR's parent decision.
- [ADR-0137](0137-thread-local-locale-for-numeric-io.md) — `setlocale(LC_NUMERIC, "C")` is
  mandatory before any `strtod`-touching libvmaf call.
- [ADR-0700](0700-vmafx-repo-layout.md) — `core/` path layout, headers at
  `core/include/libvmaf/`.
- [ADR-0686](0686-vmafx-rebrand-aggressive-modernization.md) — VMAFX
  modernization umbrella; this is item 1 ("MCP cgo direct") under that
  rebrand.
- `req` — "Implement modernization #1: replace MCP subprocess delegation
  with direct cgo via `pkg/libvmaf`. Phase 1: ADR + extension-point design +
  first 1-2 tool handlers as proof-of-concept, behind `VMAFX_MCP_DIRECT=1`
  flag." (per user task specification, 2026-05-31).
