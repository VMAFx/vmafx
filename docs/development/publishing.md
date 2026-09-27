<!-- markdownlint-disable MD013 MD060 -->
# Artifact publishing policy (Phase 4b.9)

All canonical build artifacts for the VMAFx fork are produced inside an
image built from `dev/Containerfile`: the `vmaf-dev-mcp` container locally, or
one of its stages in CI (`build-deps` for the native release bundle). Host-side
meson/ninja builds are available
for diagnostic purposes (IDE integration, debugger sessions, sanitizer sweeps)
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
is four workflows plus one job:

| Workflow / job | Trigger | Produces | Builds in a container? |
|---|---|---|---|
| `.github/workflows/dev-container-publish.yml` | push to `master` (`dev/Containerfile`, `dev/scripts/**`) | `ghcr.io/vmafx/vmafx-dev-mcp:sha-<commit>`, `:master` | Yes — builds `libvmaf-build` stage. Published for transparency; releases do not pull it |
| `.github/workflows/release-please.yml` | push to `master` | the release PR and, on merge, the tag + GitHub release | n/a — no build |
| `.github/workflows/supply-chain.yml` | `release: published` | `libvmaf.so` chain, the `vmaf` CLI, `models.tar.gz`, SBOMs, cosign signatures, SLSA provenance, the `vmaf-mcp` wheel | **Yes** — `build-artifacts` builds the `build-deps` stage of the release tag's `dev/Containerfile` on a GitHub-hosted runner and compiles inside it ([ADR-1346](../adr/1346-hosted-slim-container-release-build.md)) |
| `.github/workflows/docker-publish-production.yml` | `release: published` | `ghcr.io/vmafx/vmafx:*` (cpu / cuda13 / rocm7 / oneapi2025 / server) | Yes, inherently — `docker buildx` against `docker/Dockerfile.production*` |
| `cross-backend` job in `.github/workflows/tests-and-quality-gates.yml` | Disabled (`if: false`, awaits self-hosted GPU runner) | backend-parity report (a gate, not an artifact) | **No** — `ubuntu-latest` host toolchain |

Consequences:

- Release native binaries (`libvmaf.so` SONAME chain, `vmaf` CLI, `models.tar.gz`)
  are compiled inside the `build-deps` stage, built in the release job from
  the tagged commit's own `dev/Containerfile`. The only registry pulls are
  the digest-pinned base image and BuildKit frontend it names; no external
  layer cache is involved
  ([ADR-1346](../adr/1346-hosted-slim-container-release-build.md), which
  supersedes ADR-1178).
- Non-release PR CI gates (e.g. `tests-and-quality-gates.yml`) continue to run on
  host runners for fast unit testing. When a container run and a CI host run
  disagree, the toolchain difference remains a live hypothesis.

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

| Job / Script | What it asserts |
|---|---|
| `Dev Container Build (PR gate)` in `dev-container-build.yml` | the gate rejects the bare runner, accepts the built image, and a stamp made inside the image verifies outside it; the release rehearsal then runs the whole release build in `build-deps` and verifies its stamp |
| `Release Script Contract (ADR-1128)` in `rule-enforcement.yml` | the gate's hermetic unit suite (`scripts/ci/tests/test-check-container-build.sh`, no Docker needed) |
| `build-artifacts` in `supply-chain.yml` | runs `--assert`, then stamps `artifacts/` with `--stamp`, both inside the `build-deps` stage it builds from the release tag (ADR-1346) |
| `verify-native-artifacts` in `supply-chain.yml` | verifies downloaded `artifacts/` with `scripts/ci/check-container-build.sh --verify`, which rejects a missing, empty, malformed or symlinked stamp |
| `scripts/release/verify-native-release-artifacts.sh` | verifies staged release bundle contains valid, non-empty, non-symlink `container-build-provenance.txt` |
| `attach-to-release` in `supply-chain.yml` | requires `container-build-provenance.txt` as a required release asset and verifies cosign signature bundle |

### Release compilation environment (ADR-1346)

[ADR-1346](../adr/1346-hosted-slim-container-release-build.md) moved native
release compilation off the self-hosted Arc runner that ADR-1178 required. No
such runner was registered, so a published release would have waited 24 hours
and been cancelled. `build-artifacts` in `.github/workflows/supply-chain.yml`
now runs on `ubuntu-latest`:

1. It checks out the release tag.
2. It builds the `build-deps` stage of that tag's `dev/Containerfile` with
   `scripts/ci/build-dev-container-stage.sh build-deps`. `build-deps` is the
   digest-pinned Ubuntu 26.04 base plus Ubuntu archive packages (gcc-13,
   Meson, Ninja, NASM) and downloads nothing from third parties, so the job
   needs no GitHub token. The build uses the default Docker builder with no
   external layer cache and no registry, so no state from another workflow
   run can enter a release. An uncached build of the stage took about two
   minutes on a workstation; the job allows 60.
3. It runs `scripts/release/build-native-release-artifacts.sh` in that image
   with `docker run --pull never --network none`, as the runner's user, with
   the checkout mounted. The script asserts the container marker, refuses to
   build unless the checkout is `GITHUB_SHA` (the commit the stamp records),
   builds with Meson, stages the bundle, writes
   `container-build-provenance.txt` and runs the clean-environment verifier.
4. It hashes and uploads `artifacts/` on the runner for SBOM, signing, SLSA
   provenance and attachment, exactly as before.

The stamp records `image_title=vmaf-dev-mcp`. `build-deps` writes that marker
and every later stage of `dev/Containerfile` inherits it; the gate accepts no
other identity. `verify-native-artifacts` then checks the stamp and the
runtime on `ubuntu-26.04`, because a bundle compiled in the Ubuntu 26.04 image
needs glibc 2.43 or newer (see
[the release guide](release.md#native-linux-release-layout)).

**What is pinned and what is not.** The base image is pinned by digest through
`build-config.env`. The Ubuntu archive packages in `build-deps` resolve when
the stage is built, so rebuilding an old tag later may install newer
compilers or Meson than the original release used. The release compile itself
runs with networking disabled.

**Release-track exception.** `build-config.env` says published artifacts are
built on the `RELEASE_*` track (`RELEASE_BUILDER_BASE`, Debian 13, glibc 2.41)
and ship on `RELEASE_RUNTIME_CC` (distroless `cc-debian13`). The native bundle
is the exception: it is built on the `DEV_*` track, so it needs glibc 2.43 and
does not load on Debian 13, on the distroless release runtime or on Ubuntu
24.04. ADR-1346 records this for 1.0.0-rc.1; `docs/state.md` row
`T-RELEASE-NATIVE-BUNDLE-RELEASE-TRACK-2026-09-27` tracks building the bundle
on the release track before the final 1.0.0.

**Rehearsal on every container-affecting pull request.** The Dev Container PR
gate (`dev-container-build.yml`) builds `build-deps` with the same script and
runs the same `docker run` invocation. A pull request checkout reaches no tag,
so the gate first creates a local lightweight tag named after
`.release-please-manifest.json`'s version on `HEAD`; `vmaf --version` then
reports the `v<version>-0-g<commit>` form a release reports, and the verifier
checks it against the manifest version.

To reproduce the release build locally from a clean checkout of the tag (use a
UID that owns the checkout; the image needs no passwd entry for it):

```bash
tag=vX.Y.Z
bash scripts/ci/build-dev-container-stage.sh build-deps vmafx-release-build:local
docker run --rm --pull never --network none --user "$(id -u):$(id -g)" \
  --volume "$PWD:/src" --workdir /src vmafx-release-build:local \
  bash scripts/release/build-native-release-artifacts.sh "${tag#v}"
bash scripts/ci/check-container-build.sh --verify artifacts
```

`.github/workflows/dev-container-publish.yml` still publishes
`ghcr.io/vmafx/vmafx-dev-mcp` (the `libvmaf-build` stage) for transparency and
contributor convenience. No release job pulls it.

Run the unit suite locally with:

```bash
bash scripts/ci/tests/test-check-container-build.sh
```

---

## Related documents

- [ADR-1346](../adr/1346-hosted-slim-container-release-build.md) — native release build on a hosted runner inside the `build-deps` stage
- [ADR-1178](../adr/1178-dev-container-image-publish.md) — dev container publication and the former self-hosted release build (superseded by ADR-1346)
- [ADR-1102](../adr/1102-phase4b9-container-only-publishing.md) — policy decision and rationale
- [ADR-0496](../adr/0496-prefer-dev-mcp-container-rule.md) — default-to-container project rule (CLAUDE.md §15)
- [ADR-0451](../adr/0451-local-dev-mcp-container.md) — initial dev-MCP container decision
- [docs/development/dev-mcp.md](dev-mcp.md) — container operator guide
- [docs/development/docker-production.md](docker-production.md) — production image reference
- [docs/development/release.md](release.md) — full release automation flow
