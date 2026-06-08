<!-- markdownlint-disable MD013 MD060 -->
# Artifact publishing policy (Phase 4b.9)

All canonical build artifacts for the VMAFx fork are produced inside the
`vmaf-dev-mcp` container. Host-side meson/ninja builds are available for
diagnostic purposes (IDE integration, debugger sessions, sanitizer sweeps)
but are **not** the authoritative source for any published artifact.

The policy is recorded in
[ADR-1102](../adr/1102-phase4b9-container-only-publishing.md).

---

## What "canonical artifact" means

A canonical artifact is any file that is:

- Tagged in a GitHub release (`libvmaf.so`, Python wheels, CLI binaries).
- Published to a container registry (`ghcr.io/vmafx/vmafx:*`).
- Attached to a CI run as a downloadable artifact and used downstream
  (benchmark result JSON, snapshot score files).

Intermediate build objects (`*.o`, `*.a`, build directories) and developer
tooling outputs (flamegraphs, profile data, local benchmark runs) are
**not** canonical artifacts and are not covered by this policy.

---

## Container-first rule

Before building an artifact for publication, verify that the container image
is up to date with `master`:

```bash
# Check image age vs. last master commit that touched a relevant path
git log --oneline -1 -- core/ mcp-server/ ai/ tools/vmaf-tune/ dev/

# Rebuild if the image predates that commit
docker compose --project-directory "$(git rev-parse --show-toplevel)" \
  -f dev/docker-compose.yml build dev-mcp
docker compose -f dev/docker-compose.yml up -d
```

Then run the artifact build inside the container:

```bash
docker exec vmaf-dev-mcp bash -c "
  cd /workspace && \
  meson setup build -Denable_cuda=true -Denable_sycl=true && \
  ninja -C build
"
```

The resulting binaries at `/workspace/build/` and `/usr/local/bin/vmaf`
inside the container are the artifacts that CI clones via the same
Containerfile to produce the release binaries.

---

## Rebuild trigger conditions

Rebuild the container image (not just `ninja`) when any of the following
change on `master`:

| Trigger | Why |
|---------|-----|
| `dev/Containerfile` or `dev/docker-compose.yml` | Base image or layer set changed |
| `core/` (any C source, header, or meson file) | Library ABI or build output changed |
| `mcp-server/vmaf-mcp/` | MCP server entry point or dependencies changed |
| `ai/` | ONNX Runtime integration or model interface changed |
| `tools/vmaf-tune/` | vmaf-tune CLI changed |
| `ffmpeg-patches/` | Downstream FFmpeg integration changed |
| Python dependency files (`requirements*.txt`, `pyproject.toml`) | Runtime environment changed |

A single `git log --oneline -1 -- <paths>` against the above list
is sufficient to decide. If the most recent commit touching any of those
paths post-dates the container image's build timestamp, rebuild.

---

## Host-side builds: when they are appropriate

| Use case | Appropriate? |
|----------|-------------|
| Running clang-tidy / clangd (IDE integration) | Yes — use `build/` configured with CPU backend |
| gdb / lldb crash investigation | Yes — use the sanitizer-enabled host build |
| ASan / UBSan / TSan sweep | Yes — `meson setup build-asan -Db_sanitize=address` |
| Producing a release binary | **No** — must use container |
| Running the Netflix golden gate in CI | **No** — CI uses the container environment |
| Quick local smoke test during development | Yes — acceptable, but results should not be published |

If a backend fails to reproduce in the container, diagnose the container first.
Fix `dev/Containerfile` rather than chasing host toolchain drift. The host's
toolchain versions (system icpx, system CUDA, host Python) are intentionally
not pinned and will diverge over time.

---

## CI integration

The release workflow (`release.yml`) and the backend-parity gate
(`cross-backend.yml`) both run inside the container image defined at
`dev/Containerfile`. They do **not** use the runner's host toolchain.
This means a local container build that passes is a reliable predictor of
whether the CI gate will pass.

See [docs/development/ci.md](ci.md) for the full CI gate list and
[docs/development/dev-mcp.md](dev-mcp.md) for the container operator guide.

---

## Related documents

- [ADR-1102](../adr/1102-phase4b9-container-only-publishing.md) — policy decision and rationale
- [ADR-0496](../adr/0496-prefer-dev-mcp-container-rule.md) — default-to-container project rule (CLAUDE.md §15)
- [ADR-0451](../adr/0451-local-dev-mcp-container.md) — initial dev-MCP container decision
- [docs/development/dev-mcp.md](dev-mcp.md) — container operator guide
- [docs/development/docker-production.md](docker-production.md) — production image reference
- [docs/development/release.md](release.md) — full release automation flow
