<!-- markdownlint-disable MD013 MD041 MD060 -->

# Research digest: gosec sweep — per-finding fix rationale

- **Date**: 2026-06-01
- **Author**: lusoris (with Claude Code agent)
- **Companion ADR**: [ADR-0983](../adr/0983-gosec-findings-fix-sweep.md)
- **Tool**: `gosec` v2.21.4 (commit aabb)

## Scan command

```bash
# Source-truth scan (used by go-ci.yml + make lint-go)
gosec -exclude-generated ./...

# Audit scan (includes cgo + protoc-generated, surfaces context-only noise)
gosec ./...
```

## Raw counts

|              | Pre-sweep | Post-sweep |
|---           |---        |---         |
| Total        | 38        | 10\*       |
| G103 (LOW)   | 4         | 4\*        |
| G104 (LOW)   | 1         | 0          |
| G115 (HIGH)  | 6         | 6\*        |
| G202 (MED)   | 1         | 0          |
| G204 (MED)   | 19        | 0          |
| G301 (MED)   | 1         | 0          |
| G304 (MED)   | 5         | 0          |
| G306 (MED)   | 1         | 0          |

\* The post-sweep G103 (4) and G115 (6) findings live in `gen/go/*.pb.go`
(protoc-generated) and the cgo trampoline cache for `pkg/libvmaf`. Both
are excluded from the CI gate via `gosec -exclude-generated`. Source files
hand-authored on the fork carry zero findings.

## Per-finding triage

### G104 — unhandled error (1)

| File | Line | Verdict | Fix |
|---|---|---|---|
| `cmd/vmafx-mcp/impl.go` | 186 | Real | Check `outFile.Close()` error; `os.Remove(outPath)` errors logged to stderr via `errors.Is(rmErr, os.ErrNotExist)` guard |

### G202 — SQL string concatenation (1)

| File | Line | Verdict | Fix |
|---|---|---|---|
| `cmd/vmafx-controller/queue/queue.go` | 511 | False positive | The concatenated fragment is `repeatCommaQ(len(statuses)-1)` output (pure `,?,?,...` placeholders, no user data). Status values bind through `placeholders...`. Cited via `// #nosec G202 -- ...` |

### G204 — subprocess with variable (19)

All G204 findings are variants on "the subprocess command path / arg is a
named variable rather than a string literal." gosec cannot prove the
variable is safe without help. Every finding was triaged against the
fork's `libvmaf.ValidatePath` allowlist + the const-binary-name patterns.

| File | Line | Variable | Validating helper | Verdict |
|---|---|---|---|---|
| `pkg/storage/fuse_mount.go` | 94 | `argv[0]` = `s.rcloneBin` | Constructor sets `s.rcloneBin` from operator config (trusted) | False positive |
| `pkg/storage/fuse_mount.go` | 175 | `bin` = `{fusermount3, fusermount, umount}` | Const-string for-range | False positive |
| `pkg/storage/http_serve.go` | 97 | `argv[0]` = `s.rcloneBin` | Same as fuse_mount | False positive |
| `pkg/gpu/detect.go` | 87 | `cmd` arg | All callers pass hard-coded vendor strings (`nvidia-smi`, `rocm-smi`, etc.) | False positive |
| `pkg/encoder/encoder.go` | 100 | `probeBin` = `ffprobe` (joined with `ffmpegBin` dir) | `ffmpegBin` is operator-configured `EncodeParams` | False positive |
| `pkg/encoder/encoder.go` | 173 | `argv[0]` = `bin` (ffmpegBin) | Same | False positive |
| `pkg/ai/infer.go` | 111 | `runnerPath` | `exec.LookPath("vmafx-ort-runner")` — PATH-resolved fixed name | False positive |
| `pkg/bisect/bisect.go` | 160 | `argv[0]` = `vmafBin` | Operator-configured; bisect is dev-time, not RPC surface | False positive |
| `cmd/vmafx-mcp/impl.go` | 208 | `vmafBin` | `libvmaf.FindBinary()` (env-overridable, fixed candidate list); args `ref`/`dis` come from `libvmaf.ValidatePath` upstream | False positive |
| `cmd/vmafx-mcp/impl.go` | 397 | `bash` + `script` | `script` is `RepoRoot() + "testdata/bench_all.sh"` | False positive |
| `cmd/vmafx-mcp/impl.go` | 498 | `python3 -c <inline script>` | `script` is a constant; `modelPath` / `featuresPath` go through `ValidatePath` | False positive |
| `cmd/vmafx-mcp/impl.go` | 687 | `ffmpeg` + argv | argv mixes literals with `ValidatePath`-filtered YUV paths | False positive |
| `cmd/vmafx-mcp/impl.go` | 753 | `vmafBin` (probe) | Probe uses `os.MkdirTemp` paths + literal flags | False positive |
| `cmd/vmafx-mcp/impl.go` | 842 | `vmafBin --version` | `--version` is literal | False positive |
| `cmd/vmafx-mcp/impl.go` | 932 | `ffprobe path` | `path` is `ValidatePath`-filtered (set in `handleVmafScoreEncoded`) | False positive |
| `cmd/vmafx-mcp/impl.go` | 986 | `ffmpeg` decode | `src` is `ValidatePath`-filtered, `dst` is os.MkdirTemp | False positive |
| `cmd/vmafx-mcp/impl.go` | 1211/1249/1293 | `vmaftune` | Resolved via `findVmafTune()` (fixed candidate list); argv is schema-validated tool args + literal flags | False positive |

### G301 / G306 — file permissions (2)

| File | Line | Verdict | Fix |
|---|---|---|---|
| `cmd/vmafx-tune/cmd/compare.go` | 431 | Real (defence-in-depth) | Tighten MkdirAll mode 0o755 → 0o750 |
| `cmd/vmafx-tune/cmd/compare.go` | 434 | Real (defence-in-depth) | Tighten WriteFile mode 0o644 → 0o600. Reports can include dataset path identifiers |

### G304 — file inclusion via variable (5)

| File | Line | Verdict | Fix |
|---|---|---|---|
| `cmd/vmafx-mcp/impl.go` | 215 | False positive | `outPath` from `os.CreateTemp` |
| `cmd/vmafx-mcp/impl.go` | 770 | False positive | `outJSON` is `tmpDir / "out.json"` where tmpDir is `os.MkdirTemp` |
| `cmd/vmafx-mcp/impl.go` | 1162 | **Real bug** | `path` originated from caller-supplied `nameOrPath` joined onto repo root with no allowlist check. Path traversal vector via `../../../etc/passwd`. **Fixed by routing through `libvmaf.ValidatePath` before opening** |
| `pkg/ai/infer.go` | 183 | False positive | Sidecar path is `r.modelDir + modelName + ".json"`, both registry-controlled |
| `pkg/bisect/bisect.go` | 172 | False positive | XML path is `os.CreateTemp` output |

### G115 — integer overflow (6, all cgo cache)

All six G115 findings live in the cgo cache for `pkg/libvmaf/direct.go`
(`_cgo*` generated trampoline). The casts are:

- `C.enum_VmafPixelFormat(req.PixFmt)` — `req.PixFmt` is an exported enum
  constrained to `{PixFmtYUV420P, PixFmtYUV422P, PixFmtYUV444P}` by the
  `ParsePixFmt` helper; cannot overflow uint32.
- `C.uint(req.BitDepth)` — `BitDepth` validated to `{8, 10, 12}` by
  `frameBytes`.
- `C.uint(req.Width)` / `C.uint(req.Height)` — bounded to
  `libvmaf`-supported geometry (≤ 16384) at the schema layer.
- `C.uint(frameIdx-1)` — `frameIdx` was just incremented from zero in
  the outer loop; the `frameIdx == 0` guard above the call ensures the
  subtraction is safe.

These are not maintainable by hand (cgo regenerates the trampoline on
every build). Excluded via `-exclude-generated`.

### G103 — `unsafe.Slice` (4, all protoc-generated)

Lines 848 and 910 of `gen/go/vmafx.pb.go` are the standard protoc-gen-go
output for compressing the embedded FileDescriptorProto. Mechanically
identical across every protobuf-generated Go file in the ecosystem.
Excluded via `-exclude-generated`.

## CI integration

```yaml
# .github/workflows/go-ci.yml (post-sweep)
- name: Install gosec
  run: go install github.com/securego/gosec/v2/cmd/gosec@v2.21.4

- name: gosec (exclude generated)
  run: gosec -exclude-generated -quiet ./...
```

Plus the matching Makefile target:

```make
lint-go:
    @command -v gosec >/dev/null || { ... skip ... ; }
    @gosec -exclude-generated -quiet ./...
```

`lint-go` is now a phase of the top-level `make lint`, alongside `lint-c`,
`lint-py`, `lint-sh`, `lint-md`.

## Regression test

`cmd/vmafx-mcp/impl_gosec_test.go::TestDescribeModelRejectsTraversal`
asserts that `describeModel("../../../etc/passwd")` and three sibling
traversal attempts each return an error (allowlist gate or
not-found-in-model-tree), never a successful `Stat`. The test passes the
post-fix tree and would fail against the pre-fix
`os.Stat(filepath.Join(root, nameOrPath))` direct path.
