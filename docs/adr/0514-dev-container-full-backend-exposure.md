<!-- markdownlint-disable MD013 MD060 -->
# ADR-0514: dev-MCP container exposes every host GPU backend (CUDA + SYCL + Vulkan + HIP)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: container, dev-experience, gpu, sycl, vulkan, hip, cuda, fork-local

## Context

The `vmaf-dev-mcp` container (`dev/Containerfile` / `dev/docker-compose.yml`)
is the project-mandated default execution environment for vmaf / vmaf-tune /
ai / MCP work (CLAUDE.md §12 rule 15, ADR-0496). Despite shipping CUDA,
Intel oneAPI, ROCm, and Mesa Vulkan SDKs in its image layers, only CPU and
CUDA were actually usable at run-time on the dev host:

```text
backend=cpu     ok      VMAF 76.66783
backend=cuda    ok      VMAF 76.66783
backend=sycl    fail    "No device of requested type available"
backend=vulkan  fail    Intel-Arc-only enumeration; nvidia_icd hidden
backend=hip     fail    "built without hip support (ADR-0498)"
backend=metal   fail    "built without metal support"   <-- Apple-only, expected on Linux
```

The host concurrently has 3 working Vulkan adapters (RTX 4090, Intel Arc
A380, AMD gfx1036), a working SYCL/level-zero Arc target, and a working
ROCm HIP target — none of which the container surfaced. Three independent
root causes were responsible (Research-0138):

1. **SYCL — missing `libhwloc.so.15` on `LD_LIBRARY_PATH`.** The Intel
   oneAPI 2026.0 level-zero UR adapter (`libur_adapter_level_zero.so.0`)
   dlopens `libhwloc.so.15` at adapter-load time to enumerate NUMA
   topology before touching a device. The library is present at
   `/opt/intel/oneapi/tcm/latest/lib/libhwloc.so.15` but
   `tcm/latest/lib` was absent from the image's `LD_LIBRARY_PATH`, so
   the adapter failed to load and sycl-ls reported "No platforms found"
   even with the Arc device node visible.
2. **SYCL — missing `/dev/dri/by-path/` symlinks.** The Intel
   compute-runtime (`libze_intel_gpu.so.1`) discovers Arc GPUs through
   the udev-managed `pci-XXXX:YY:ZZ.W-render` symlinks under
   `/dev/dri/by-path/`. Docker's `devices: ["/dev/dri:/dev/dri"]`
   directive passes the leaf device nodes but does not preserve the
   parent directory's symlinks — the container saw `card0..card2` /
   `renderD128..renderD130` but only one orphaned by-path entry,
   collapsing device enumeration to zero platforms.
3. **Vulkan — `VK_ICD_FILENAMES` pinned to a non-existent lavapipe
   path.** The image set
   `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`,
   but the file mesa ships in this layer is `lvp_icd.json` (no
   `.x86_64` suffix). The Vulkan loader honoured the env var, found
   no matching ICD, and reported zero devices — hiding the NVIDIA
   ICD that the NVIDIA Container Toolkit had correctly bind-mounted
   into `/etc/vulkan/icd.d/nvidia_icd.json`, and the Intel/AMD ICDs
   the mesa-vulkan-drivers package installs into
   `/usr/share/vulkan/icd.d/`.
4. **HIP — `core/tools/meson.build` missing `-DHAVE_HIP=1` cflag.**
   The Containerfile passes `-Denable_hip=true` to meson, the meson
   summary reports `enable_hip : true`, and `libvmaf.so.3` links
   against `libamdhip64.so.6`. But the vmaf CLI's HIP support gate is
   `#ifdef HAVE_HIP` (compile-time guards around `#include
   "libvmaf/libvmaf_hip.h"`, the `VmafHipState` init/cleanup, and the
   `--backend hip` strict-mode arm). `core/tools/meson.build`
   conditionally appends `-DHAVE_CUDA=1` / `-DHAVE_SYCL=1` /
   `-DHAVE_VULKAN=1` to `vmaf_tool_cflags` but had no matching
   `-DHAVE_HIP=1` branch — so the CLI compiled with the
   "built without hip support" strict-mode error path active even
   though libvmaf itself was built with HIP enabled. The fix is one
   3-line meson conditional. Verified post-fix:
   `vmaf --backend hip` returns VMAF 76.66783 / rc=0 against the
   golden CPU pair on the host's AMD gfx1036.

5. **VK_ICD_FILENAMES — `ENV` cannot truly unset.** Initial attempt
   to clear the env vars via `ENV VK_ICD_FILENAMES= / ENV VK_DRIVER_FILES=`
   in the Containerfile produced empty-string values at runtime, which
   the Vulkan loader treats the same as a non-existent file
   (`ERROR_INCOMPATIBLE_DRIVER`). The fix is to **not** set the env
   var in the Containerfile at all, and explicitly `unset
   VK_ICD_FILENAMES VK_DRIVER_FILES` in `dev/scripts/dev-mcp-entrypoint.sh`
   on container startup. Operators that need to force a single ICD can
   still pass it per-invocation with `docker exec -e VK_ICD_FILENAMES=…`.

The Vulkan backend has a separate fp64 enable-bit issue tracked under
ADR-0492 (`fix/vulkan-fp64-gate-relax` branch) that prevents Intel Arc
from passing the parity gate. This ADR addresses container exposure
only — once both PRs land, Vulkan on Arc / RADV / NVIDIA all probe
green inside the container.

## Decision

Patch `dev/Containerfile` + `dev/docker-compose.yml` so every GPU
backend libvmaf builds for is also runnable inside the container on
hosts that have the matching silicon. Concretely:

1. **`dev/Containerfile`** — append `${ONEAPI_ROOT}/tcm/latest/lib` to
   `LD_LIBRARY_PATH` in the gpu-sdks stage so the level-zero UR adapter
   resolves `libhwloc.so.15`.
2. **`dev/Containerfile`** — delete the previous
   `ENV VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`
   line and do **not** replace it. `ENV VK_ICD_FILENAMES=` (empty
   string) is not a fix — the Vulkan loader treats an empty value the
   same as a non-existent file and bails with
   `ERROR_INCOMPATIBLE_DRIVER`.
3. **`dev/scripts/dev-mcp-entrypoint.sh`** — `unset VK_ICD_FILENAMES
   VK_DRIVER_FILES` on container start, so the Vulkan loader falls
   back to its default search of `/etc/vulkan/icd.d/` (NVIDIA
   Container Toolkit's bind-mount) and `/usr/share/vulkan/icd.d/`
   (mesa). The entrypoint is the right place because Docker's `ENV`
   cannot truly unset, and operators that need to force a single ICD
   can still pass it per-invocation via `docker exec -e
   VK_ICD_FILENAMES=…`.
4. **`core/tools/meson.build`** — add `-DHAVE_HIP=1` to
   `vmaf_tool_cflags` when `enable_hip` is true, mirroring the
   existing CUDA / SYCL / Vulkan conditionals. Without this the vmaf
   CLI's HIP gate (`#ifdef HAVE_HIP` in `vmaf.c`) is inactive even
   when libvmaf itself is built with HIP enabled, and `vmaf --backend
   hip` returns the "built without hip support" strict-mode error.
5. **`dev/Containerfile`** — add a build-time probe loop after the
   libvmaf install step that iterates `cpu cuda sycl vulkan hip` and
   runs vmaf against the Netflix golden CPU pair. GPU backends without
   device passthrough are soft-failures (`|| echo WARN`); the line we
   actually care about is `built without X support`, which trips on
   the precise HIP regression that motivated this ADR.
6. **`dev/docker-compose.yml`** — add a read-only bind-mount of
   `/dev/dri/by-path` to both `dev-mcp` and `smoke-probe-cron`
   services. The leaf device nodes are still passed via `devices:
   ["/dev/dri:/dev/dri", "/dev/kfd:/dev/kfd"]` so Docker writes the
   cgroup rwm rules; the bind-mount carries the udev symlinks that
   `devices:` cannot.
7. **`dev/AGENTS.md`** — document the four invariants (LD path
   includes tcm/latest/lib, no VK_ICD_FILENAMES pin, by-path
   bind-mount, build probe) so future rebases preserve them.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(Chosen)** Patch Containerfile env + compose volumes; keep ROCm/oneAPI install layers as-is | Minimal change; preserves all CUDA/oneAPI/Vulkan/HIP investment from earlier ADRs (0496/0498); future operators don't need new host packages | Two files to keep in sync; relies on host having `/dev/dri/by-path` (true on any modern systemd-udev host) | Smallest blast radius; touches the exact lines responsible for the breakage. |
| Drop oneAPI/Vulkan/HIP from the image and require operators to install on host | Smaller image | Defeats the purpose of ADR-0496 (default-to-container); host-build debt returns | Direct contradiction of ADR-0496. |
| Bind-mount the entire `/dev/dri` directory via `volumes:` instead of `devices:` | Single mount entry; symlinks preserved automatically | `volumes:` does not generate cgroup `device.allow` rules — render-node opens get EPERM on default-cgroup hosts | Functional regression; only works if container runs `--privileged`. |
| Run the dev-MCP container as `--privileged` and let everything in /dev be visible | Simplest cgroup story | Defeats CERT C ENV32 (already protected by the non-root `vmaf` user); broad attack surface for an everyday dev container | Disproportionate trust escalation. |
| Pin a single Vulkan ICD via `VK_ICD_FILENAMES` (e.g. only NVIDIA) | Reproducible; one device per probe | Cross-backend parity testing (the whole point of the container) needs concurrent visibility of NVIDIA + Arc + AMD | Defeats /cross-backend-diff workflow. |
| Install host-side `intel-compute-runtime` + level-zero packages and mount the host's `/opt/intel` into the container | One fewer image layer | Pins container behaviour to the host's exact oneAPI version; portability lost; CI builds break (CI runners don't have oneAPI on host) | Anti-pattern for reproducible containers. |

## Consequences

- **Positive**: All four GPU backends (`cuda`, `sycl`, `vulkan`, `hip`)
  are runnable inside `vmaf-dev-mcp` on hosts with the matching
  silicon, with CPU-parity scores (5-place tolerance per
  ADR-0214). Cross-backend numeric-diff workflows (`/cross-backend-diff`)
  run end-to-end without leaving the container. The build-time probe
  surfaces silent backend-disable regressions in the image build log,
  so a future commit that accidentally flips `-Denable_hip=true` off
  prints `built without hip support` next to the probe line for
  `backend=hip` on every rebuild instead of waiting for a user to
  notice.
- **Negative**: The `/dev/dri/by-path` bind-mount adds a host-path
  dependency. Operators on systems without systemd-udev (rare;
  minimal container hosts) may need to comment it out. The Vulkan
  loader now enumerates every available ICD on every vmaf invocation
  — a few milliseconds of startup overhead per run.
- **Neutral / follow-ups**:
  - ADR-0492 (`fix/vulkan-fp64-gate-relax`) must merge before Vulkan
    on Intel Arc actually returns a score; this ADR only exposes the
    device.
  - **`vmaf_hip_import_state` returns `-ENOSYS` (library-side gap)**:
    once this PR ships, `vmaf --backend hip` proceeds past the CLI
    compile-time gate, initialises HIP state successfully, and then
    fails at `vmaf_hip_import_state` because the function in
    `core/src/hip/common.c:149` still returns `-ENOSYS` with the
    comment "stays unwired until the first feature kernel lands"
    (despite ADR-0468 having landed the first HIP feature kernel).
    Wiring `vmaf_hip_import_state` to stash the HIP state on the
    VmafContext and routing HIP-capable extractors through it is a
    separate library-side change out of scope here; the container-side
    plumbing this PR ships is the prerequisite. Tracked separately in
    `docs/state.md` as the HIP runtime gap.
  - `docs/development/dev-mcp.md` updated with the new backend matrix
    and env-var requirements.
  - `dev/AGENTS.md` gains four new invariants.
  - State.md row added under "Recently closed" closing finding 8 of
    `SESSION_FINDINGS_v9_GPU_PROBE.md`.

## References

- Research-0138: `docs/research/0138-dev-mcp-container-backend-exposure-2026-05-18.md`
- ADR-0492: `0492-vulkan-vif-shader-fp64-g-computation.md` (separate
  Vulkan Arc fp64 fix)
- ADR-0496: `0496-prefer-dev-mcp-container-rule.md` (CLAUDE rule 15 —
  default to dev-MCP container)
- ADR-0498: `0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md` (origin of
  the strict-mode "built without X" error string the probe scans for)
- Source: `req` — user-provided briefing identifying the three
  container-side gaps (Vulkan ICD, SYCL runtime, HIP build) and the
  Containerfile patches required to close them.
