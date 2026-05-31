<!-- markdownlint-disable MD013 MD060 -->
# ADR-0496: Default to the `vmaf-dev-mcp` container for all vmaf / vmaf-tune / ai / MCP work

- **Status**: Accepted
- **Date**: 2026-05-17
- **Deciders**: lusoris
- **Tags**: tooling, container, dev-experience, project-rule, fork-local

## Context

The fork has accumulated five backends (CUDA, SYCL, Vulkan, HIP,
Metal) and a tiny-AI ONNX surface on top of upstream's CPU-only
libvmaf. Each backend pulls in its own toolchain (`nvcc`, `icpx`,
Vulkan SDK + GLSL compiler, hipcc, Apple SDKs), and the Python harness
adds its own layer (numpy 2.x, libsvm 3.32, sureal, scikit-image,
matplotlib, jinja2). Keeping the host machine green across every
combination has been a perennial timesink — six PRs in a single merge
train on 2026-05-17 (#1243…#1248) traced back to host-only environment
drift (libsvm enum migration, numpy 2.x scalar `repr`, locale-leaked
subprocess output, Cython-extension build gaps) that the
`vmaf-dev-mcp` container had already pinned through.

The container at [`dev/Containerfile`](../../dev/Containerfile) and
the compose stack at [`dev/docker-compose.yml`](../../dev/docker-compose.yml)
bake in:

- every backend's toolchain + runtime libraries
- the NVIDIA Container Toolkit runtime + device passthrough
- ffmpeg built with libvmaf and the codec stack we care about
- the MCP server with the canonical entry point on `/sockets/vmaf-mcp.sock`
- the workspace mount so `.corpus/`, `python/test/resource/`, and
  source tree are all visible at `/workspace/`

It also pins the Python interpreter, numpy, libsvm, scikit-image, etc.
to versions known to play nicely with each other and with the fork's
test suite — the host machine is on Python 3.14 / numpy 2.4 / libsvm
3.32 / German locale, which the merge train spent ~3 hours patching
around.

The pattern of "host first, container as fallback" has therefore
inverted. The container should be the default, with the host as the
fallback for the narrow set of work that genuinely doesn't need it.

## Decision

We will treat the `vmaf-dev-mcp` container as the **default execution
environment** for vmaf / vmaf-tune / ai / MCP-probing work and add a
new CLAUDE.md / AGENTS.md hard rule (rule 15 in CLAUDE.md, rule 12 in
AGENTS.md's renumbered list) codifying the workflow:

1. Rebuild the container before any non-trivial run if its image
   predates the last `master` sync that touched `libvmaf/`,
   `mcp-server/`, `ai/`, `tools/vmaf-tune/`, or `dev/`.
2. Exec into it (`docker exec vmaf-dev-mcp <cmd>`) for the actual
   work; the workspace lives at `/workspace/`, the vmaf binary at
   `/usr/local/bin/vmaf` with every backend live.
3. Skip the container only for: pure Python harness edits that don't
   touch the C surface, doc / changelog / ADR work, or pure git / gh
   operations.
4. Don't reinvent host builds when a backend isn't reproducing in the
   container — diagnose the container first, fix the Containerfile if
   it's a real container gap, rather than chasing the host build-flag
   soup.
5. Don't multiplex the same device across parallel jobs — pin
   long-running jobs (CHUG re-extract, BVI-DVC sweep) to one device
   (e.g. CUDA) and schedule sibling work on a different device (Intel
   Arc via SYCL, AMD via HIP, Vulkan on non-NVIDIA, or CPU). Use
   `--backend $name` (exclusive) or `--no_<backend>` (negative) to
   pin each parallel run to its own silicon.

Host-side builds (`build/`, `core/build-cuda`, `core/build-all`)
remain available and are still the right call for: clang-tidy
end-to-end runs, integration with the IDE's clangd, gdb on a
crash, sanitizer suites. They are no longer the default mental
model.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Status quo (host-first, container optional) | No new rule; existing fallback works | Each toolchain drift hits everyone repeatedly; today's merge train spent ~3 h on env-only fixes | The cost of host drift compounds across sessions |
| Container-only (deprecate host builds) | Simplest mental model; one canonical environment | Loses IDE integration (clangd / debugger), loses host-side sanitizer tooling, and adds a Docker-must-be-running prerequisite for trivial git-only tasks | Too restrictive; some workflows need host access |
| Host-only but pin the toolchain via Nix / Conda | Reproducible without Docker | Adds a third "preferred environment" on top of host + container; doesn't subsume container's GPU passthrough story; doesn't ship MCP server inline | Strictly more moving pieces than what we already have |

## Consequences

- **Positive**: the perennial "wait, why is X failing only on host?"
  hour-loss collapses. The container's reproducibility extends to
  every contributor.
- **Positive**: parallel-device discipline (rule 5) lets us land
  CHUG re-extract on CUDA while still running BBB e2e on Arc and a
  CPU baseline at the same time — three lanes instead of one queue.
- **Negative**: contributors now need Docker + NVIDIA Container
  Toolkit installed before they can run the "default" workflow. The
  host-build fallback is documented but adds a second-path cognitive
  cost.
- **Neutral**: the Containerfile becomes a high-value artefact that
  must stay in lockstep with `libvmaf/` and `mcp-server/`. Future
  drift in the Containerfile becomes a deferred-cost item we already
  track today (`dev/Containerfile` changes ship under
  `changelog.d/changed/`).

## References

- Triggering merge train: PRs #1243…#1248 (2026-05-17) all traced to
  host-only environment drift.
- Operator guide: [docs/development/dev-mcp.md](../development/dev-mcp.md).
- Related ADRs:
  [ADR-0024](0024-netflix-golden-tests.md) (Netflix golden gate),
  [ADR-0493](0493-test-yuv-fixture-md5-verification.md) (canonical
  YUVs in container path).
- Source: req (user direction 2026-05-17 — "fucking make this a
  project rule to rebuild and use the container … and as soon as
  chug is running we can always test other things on the arc or
  agpu... so just dont multi use the same device").
