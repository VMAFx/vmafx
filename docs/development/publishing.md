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
inside the container are the ones this policy calls canonical. Stamp the
staged tree before it leaves the container so its provenance travels with it
(see [Enforcement](#enforcement)):

```bash
docker exec vmaf-dev-mcp \
  bash /workspace/scripts/ci/check-container-build.sh --stamp /workspace/artifacts
```

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
| Running the Netflix golden gate | Yes — it is a verification job, not an artifact producer, and is out of scope for this policy. The CI job `netflix-golden` in `tests-and-quality-gates.yml` builds with host meson/ninja on `ubuntu-latest` and runs pytest there |
| Quick local smoke test during development | Yes — acceptable, but results should not be published |

If a backend fails to reproduce in the container, diagnose the container first.
Fix `dev/Containerfile` rather than chasing host toolchain drift. The host's
toolchain versions (system icpx, system CUDA, host Python) are intentionally
not pinned and will diverge over time.

---

## CI integration

There is no `release.yml` and no `cross-backend.yml`. The publishing pipeline
is three workflows plus one job:

| Workflow / job | Trigger | Produces | Builds in a container? |
|---|---|---|---|
| `.github/workflows/release-please.yml` | push to `master` | the release PR and, on merge, the tag + GitHub release | n/a — no build |
| `.github/workflows/supply-chain.yml` | `release: published` | `libvmaf.so` chain, the `vmaf` CLI, `models.tar.gz`, SBOMs, cosign signatures, SLSA provenance, the `vmaf-mcp` wheel | **No** — `build-artifacts` runs `meson`/`ninja` directly on `ubuntu-latest` |
| `.github/workflows/docker-publish-production.yml` | `release: published` | `ghcr.io/vmafx/vmafx:*` (cpu / cuda13 / rocm7 / oneapi2025 / server) | Yes, inherently — `docker buildx` against `docker/Dockerfile.production*` |
| `publish-builder-image` job in `.github/workflows/dev-container-build.yml` | push to `master` touching `dev/Containerfile`, `dev/scripts/**`, `dev/docker-compose.yml` | `ghcr.io/vmafx/vmafx-dev-builder:master` and `:sha-<short>` — the `libvmaf-build` stage, i.e. the toolchain a release build runs inside | Yes, inherently — `docker buildx` against `dev/Containerfile` |
| `cross-backend` job in `.github/workflows/tests-and-quality-gates.yml` | Disabled (`if: false`, awaits self-hosted GPU runner) | backend-parity report (a gate, not an artifact) | **No** — `ubuntu-latest` host toolchain |

Two consequences worth stating plainly, because the previous version of this
page claimed the opposite:

- A local container build is **not** a reliable predictor of the CI gates.
  Every CI job that compiles libvmaf does so with the runner's host toolchain;
  the only `container:` job key anywhere in `.github/workflows/` is in
  `security-scans.yml`, and it names a scanner image. When a container run and
  a CI run disagree, the toolchain difference is a live hypothesis, not
  something to rule out.
- `supply-chain.yml` currently builds the native release artifacts on the
  runner host, which is a live violation of the policy on this page. The
  enforcement mechanism below exists; wiring it into that job is tracked as
  `T-PUBLISH-NATIVE-RELEASE-NOT-CONTAINERISED-2026-09-03` in
  [docs/state.md](../state.md). The first half of that work has landed — the
  build toolchain is now published as an image a release job can pull (see
  [ADR-1186](../adr/1186-publish-dev-builder-image.md)) — but `build-artifacts`
  itself still compiles on the host, so the violation stands until it is
  pointed at that image.

Note that `docker/Dockerfile.production` and `docker/Dockerfile.production-gpu`
are *not* `dev/Containerfile`. The published images are built from their own
Dockerfiles; `dev/Containerfile` is the development and artifact-build
container.

See [docs/development/ci.md](ci.md) for the full CI gate list and
[docs/development/dev-mcp.md](dev-mcp.md) for the container operator guide.

---

## Enforcement

The policy is checked by `scripts/ci/check-container-build.sh`. Without it the
policy was documentation only: nothing anywhere could tell a container build
from a host build, so a host-built binary could be attached to a release with
no signal at all.

`dev/Containerfile` writes a marker at `/etc/vmafx-dev-container` in its first
(`build-deps`) stage, so every downstream stage inherits it. The marker is the
gate's single source of truth:

```text
vmafx_dev_container=1
image_title=vmaf-dev-mcp
containerfile=dev/Containerfile
source=https://github.com/VMAFx/vmafx
```

The gate has three modes, and fails closed in all three — a missing marker, a
foreign container's marker, a truncated marker, a missing stamp, an empty
stamp, or a stamp of an unknown schema all exit non-zero:

```bash
# 1. Am I in the container? Exit 0 only inside vmaf-dev-mcp.
scripts/ci/check-container-build.sh

# 2. Record provenance into a staged artifact tree. Asserts (1) first, so a
#    host build cannot produce the stamp.
scripts/ci/check-container-build.sh --stamp artifacts

# 3. Verify a stamped tree. Runs anywhere — the verifying job does not itself
#    need to be containerised.
scripts/ci/check-container-build.sh --verify artifacts
```

Mode 2 writes `artifacts/container-build-provenance.txt`:

```text
schema=vmafx-container-build-provenance/1
vmafx_dev_container=1
image_title=vmaf-dev-mcp
containerfile=dev/Containerfile
source=https://github.com/VMAFx/vmafx
git_commit=<GITHUB_SHA or git rev-parse HEAD>
stamped_at=<SOURCE_DATE_EPOCH or now, UTC>
```

This is an **accident gate, not a security boundary**. It catches "the release
job silently ran meson on the runner host", which is the failure this policy
exists to prevent. It does not defend against someone who deliberately forges
the marker; the cryptographic story for published bytes is cosign signing plus
SLSA provenance in `supply-chain.yml`.

**Known limitation:** the `VMAFX_CONTAINER_MARKER` environment variable
overrides the marker path (introduced so the offline unit suite
`scripts/ci/tests/test-check-container-build.sh` can test container and host
behaviours without modifying `/etc`). A CI job or runner setting this variable
to point to a valid marker file will bypass the gate. This is consistent with
the accident-gate threat model: the gate prevents inadvertent host builds in
the release pipeline, not malicious evasion.

### Where the gate runs today

| Job | What it asserts |
|---|---|
| `Dev Container Build` in `dev-container-build.yml` (pull requests) | the gate rejects the bare runner, accepts the built image, and a stamp made inside the image verifies outside it |
| `Publish builder image` in `dev-container-build.yml` (pushes to `master`) | the image actually pushed to GHCR, addressed by digest, is recognised as container-built |
| `Release Script Contract (ADR-1128)` in `rule-enforcement.yml` | the gate's hermetic unit suite (`scripts/ci/tests/test-check-container-build.sh`, no Docker needed) |

It is **not** yet wired into `supply-chain.yml`, and wiring it in before
`build-artifacts` is containerised would fail every release, because the gate
fails closed.

The registry half of that blocker is now resolved. `dev/Containerfile`'s
`libvmaf-build` stage is published to `ghcr.io/vmafx/vmafx-dev-builder` on every
`master` push that touches the container inputs, tagged `:master` (moving) and
`:sha-<short>` (immutable), per
[ADR-1186](../adr/1186-publish-dev-builder-image.md). A release job therefore has
an image to pull, and it is the same image the PR gate smoke-tests. The package
is private: its only reader is a `GITHUB_TOKEN`-authenticated job in this
repository, so publishing it creates no public distribution surface.

What remains is the conversion itself — running `build-artifacts` with
`container: ghcr.io/vmafx/vmafx-dev-builder:master` and stamping its output tree
with `check-container-build.sh --stamp`. That change cannot be exercised by any
pull request, since `supply-chain.yml` triggers only on `release: published` and
on a `workflow_dispatch` naming an existing tag; it must therefore verify that
the tag resolves rather than assume it. The state.md row cited above stays open
until it lands.

Run the unit suite locally with:

```bash
bash scripts/ci/tests/test-check-container-build.sh
```

---

## Related documents

- [ADR-1102](../adr/1102-phase4b9-container-only-publishing.md) — policy
  decision and rationale (note: its claim that `release.yml`/`cross-backend.yml`
  run inside the container making local container builds reliable CI predictors
  is superseded by observed CI reality, where neither workflow exists and CI
  builds libvmaf on bare `ubuntu-latest` runners)
- [ADR-0496](../adr/0496-prefer-dev-mcp-container-rule.md) — default-to-container project rule (CLAUDE.md §15)
- [ADR-0451](../adr/0451-local-dev-mcp-container.md) — initial dev-MCP container decision
- [docs/development/dev-mcp.md](dev-mcp.md) — container operator guide
- [docs/development/docker-production.md](docker-production.md) — production image reference
- [docs/development/release.md](release.md) — full release automation flow
