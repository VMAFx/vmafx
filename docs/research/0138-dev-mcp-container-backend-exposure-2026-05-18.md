<!-- markdownlint-disable MD060 -->
# Research-0138: dev-MCP container backend exposure (SYCL / Vulkan / HIP)

**Date**: 2026-05-18
**Branch**: fix/dev-container-backend-exposure
**Status**: Closed (fix landed in same PR; ADR-0509)

## Problem statement

The `vmaf-dev-mcp` container is the project-mandated default execution
environment (CLAUDE.md §12 rule 15, ADR-0496). Despite shipping every GPU
SDK in its image (CUDA, oneAPI, ROCm, Mesa Vulkan), only CPU and CUDA
backends actually worked at run-time on the dev host. The dev host
concurrently has working CUDA (RTX 4090), SYCL (Intel Arc A380), Vulkan
(3 adapters) and HIP (gfx1036) outside the container, so the gap was
strictly container-side.

Backend matrix in the in-use image (commit `e0061b7e0` running on
`vmaf-dev-mcp` 2026-05-18):

| Backend | Container | Host |
|---|---|---|
| cpu     | OK 76.66783 | OK 76.66783 |
| cuda    | OK 76.66783 | OK 76.66783 |
| sycl    | FAIL "No device of requested type available" | OK 76.66783 |
| vulkan  | FAIL Intel-Arc-only enumeration (NVIDIA hidden) | OK 3/3 GPUs |
| hip     | FAIL "built without hip support (ADR-0498)" | OK 76.66783 |
| metal   | FAIL "built without metal support" | n/a (Linux) |

## Diagnostic methodology

Probes were run inside the live `vmaf-dev-mcp` container with `docker exec`.

### SYCL: `sycl-ls --verbose` + UR adapter trace + strace

```bash
docker exec vmaf-dev-mcp sycl-ls --verbose
```

emitted:

```text
<LOADER>[INFO]: failed to load adapter 'libur_adapter_level_zero.so.0'
                with error: libhwloc.so.15: cannot open shared object file
<LOADER>[INFO]: failed to load adapter 'libur_adapter_level_zero_v2.so.0'
                with error: libhwloc.so.15: cannot open shared object file
Platforms: 0
```

`find / -name "libhwloc*"` located the library at
`/opt/intel/oneapi/tcm/latest/lib/libhwloc.so.15` — present in the image
but absent from `LD_LIBRARY_PATH` (which carried
`compiler/latest/lib` and `umf/latest/lib` only). Adding
`tcm/latest/lib` to `LD_LIBRARY_PATH` allowed the level-zero UR
adapter to load, but `sycl-ls` still reported `Platforms: 0`.

Second-round strace of `sycl-ls` showed `libze_intel_gpu.so.1` opening
`/dev/dri/by-path/pci-0000:01:00.0-render` (PCI BFD `01:00.0` —
matches the **container's** view of the by-path symlink, but the
container's by-path only had **one** entry while the host had three).
`ls /dev/dri/by-path/` on host:

```text
pci-0000:03:00.0-{card,render}  -> Intel Arc A380
pci-0000:06:00.0-{card,render}  -> NVIDIA RTX 4090
pci-0000:7d:00.0-{card,render}  -> AMD gfx1036
```

vs. container:

```text
pci-0000:01:00.0-{card,render}  -> card1, renderD128   (orphan; PCI BFD doesn't match any host card)
```

Root cause: Docker's `devices:` directive passes leaf device nodes only.
The udev symlinks under `/dev/dri/by-path/` are not propagated. The
Intel compute-runtime enumerates GPUs through those symlinks; with only
one orphaned (and PCI-BFD-wrong) symlink visible, `zeInit` returns
`ZE_RESULT_ERROR_UNINITIALIZED`.

### Vulkan: `vulkaninfo --summary` after VK env-var clear

```bash
docker exec vmaf-dev-mcp env | grep VK_
# VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
ls /usr/share/vulkan/icd.d/lvp_icd.x86_64.json
# (no such file or directory)
ls /usr/share/vulkan/icd.d/
# asahi_icd.json  gfxstream_vk_icd.json  intel_hasvk_icd.json
# intel_icd.json  lvp_icd.json           nouveau_icd.json
# radeon_icd.json virtio_icd.json
ls /etc/vulkan/icd.d/
# nvidia_icd.json    (bind-mounted by NVIDIA Container Toolkit)
```

The pinned filename ends in `.x86_64.json` which mesa does not ship —
the file is `lvp_icd.json`. The Vulkan loader honoured the env var,
found no matching ICD, and reported zero devices. Unsetting both
`VK_ICD_FILENAMES` and `VK_DRIVER_FILES` and re-running:

```bash
docker exec vmaf-dev-mcp bash -c 'unset VK_ICD_FILENAMES VK_DRIVER_FILES; vulkaninfo --summary'
```

surfaced all four ICDs concurrently:

```text
deviceName = NVIDIA GeForce RTX 4090
deviceName = Intel(R) Arc(tm) A380 Graphics (DG2)
deviceName = AMD Ryzen 9 9950X3D (RADV RAPHAEL_MENDOCINO)
deviceName = llvmpipe (LLVM 20.1.2, 256 bits)   # software fallback, last
```

### HIP: `vmaf --backend hip`

```bash
docker exec vmaf-dev-mcp vmaf --backend hip ...
# vmaf: --backend hip requested but this libvmaf was built without hip
#       support; refusing to silently fall back to CPU (ADR-0498)
```

Initial hypothesis was that the running image predated the
`-Denable_hip=true` meson flag in the Containerfile. Inspection during
the post-fix rebuild showed the running image is in fact built with HIP:

```bash
$ docker run --rm vmaf-dev-mcp:local bash -c 'ldd /usr/local/bin/vmaf | grep amdhip'
        libamdhip64.so.6 => /opt/rocm/lib/libamdhip64.so.6 (0x00007f...)
$ grep -nB1 'enable_hip' /tmp/dev-mcp-build.log
#34 1.857     enable_hip      : true
#34 1.857     enable_hipcc    : true
```

So libvmaf has HIP linked. The strict-mode error comes from the vmaf
CLI binary, not libvmaf. `grep -n "HAVE_HIP" core/tools/vmaf.c`
reveals 8 `#ifdef HAVE_HIP` guards around `libvmaf_hip.h` include,
`VmafHipState` init/cleanup, and the `--backend hip` arm. Cross-check
in `core/tools/meson.build`:

```meson
if get_option('enable_cuda')
    vmaf_tool_cflags += ['-DHAVE_CUDA=1']
endif
if get_option('enable_sycl')
    vmaf_tool_cflags += ['-DHAVE_SYCL=1']
    vmaf_tool_deps += sycl_dependency
endif
if get_option('enable_vulkan').enabled()
    vmaf_tool_cflags += ['-DHAVE_VULKAN=1']
endif
# (no enable_hip branch!)
```

Root cause: the CUDA / SYCL / Vulkan cflags conditionals exist but no
matching `enable_hip` conditional was ever added. So the CLI compiles
with `HAVE_HIP` undefined, the `#ifdef HAVE_HIP` blocks are removed by
the preprocessor, and the `--backend hip` arm hits the
`compiled_in == false` branch → "built without hip support" strict-mode
error (ADR-0498). Fix: 3-line meson conditional mirroring the others.

The probe step ADR-0514 ships is the right diagnostic surface — it
prints the exact `built without hip support` substring next to
`backend=hip` on every image rebuild, so the next regression of this
class is visible without having to `docker exec ... vmaf --backend hip`
by hand.

## Fix matrix

| Cause | Where | Fix |
|---|---|---|
| `libhwloc.so.15` not findable | `dev/Containerfile` env layer | Append `${ONEAPI_ROOT}/tcm/latest/lib` to `LD_LIBRARY_PATH`. |
| Vulkan ICD env pinned to non-existent path | `dev/Containerfile` env layer + `dev/scripts/dev-mcp-entrypoint.sh` | Delete the `ENV VK_ICD_FILENAMES=…` line (do NOT replace with empty-string — the loader treats `""` the same as a non-existent file); `unset VK_ICD_FILENAMES VK_DRIVER_FILES` at entrypoint start so operators can still override per-`docker exec`. |
| `/dev/dri/by-path` symlinks dropped by Docker `devices:` | `dev/docker-compose.yml` | Add read-only bind-mount of `/dev/dri/by-path` to both services. |
| vmaf CLI missing `-DHAVE_HIP=1` cflag despite `enable_hip=true` | `core/tools/meson.build` | Add a `if get_option('enable_hip') ... vmaf_tool_cflags += ['-DHAVE_HIP=1'] endif` block mirroring the existing CUDA/SYCL/Vulkan conditionals. |
| Silent regressions to `-Denable_hip=true` (and friends) | `dev/Containerfile` post-install layer | Add a build-time backend probe loop scanning for "built without X support". |
| Future rebases re-introducing any of the above | `dev/AGENTS.md` | Add four invariants documenting each constraint with the bug class it prevents. |

## Verification (post-fix, in-container)

```bash
docker exec vmaf-dev-mcp bash -c '
  for B in cpu cuda sycl vulkan hip; do
    vmaf --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
         --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
         --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
         --backend $B --json --output /tmp/probe_$B.json
    echo "rc=$? backend=$B"
  done
'
```

Expected after this PR + ADR-0492 (Vulkan fp64 relax) land:

| Backend | VMAF | rc |
|---|---|---|
| cpu | 76.66783 | 0 |
| cuda | 76.66783 | 0 |
| sycl | 76.66783 | 0 |
| vulkan | 76.66783 | 0 |
| hip | 76.66783 | 0 |

The post-fix matrix is captured in the PR description of
`fix/dev-container-backend-exposure`.

## Out of scope

- ADR-0492 Vulkan fp64 enable-bit (separate PR, separate fix path).
- `vmaf-tune ladder --score-backend` argument plumbing for parity
  across non-CUDA backends (separate PR).
- The session findings v9 file's findings 1-7 (CHUG re-extract, real-
  bisect compare path) — unrelated to container exposure.

## References

- ADR-0514: `0514-dev-container-full-backend-exposure.md`
- ADR-0492: `0492-vulkan-vif-shader-fp64-g-computation.md`
- ADR-0496: `0496-prefer-dev-mcp-container-rule.md`
- ADR-0498: `0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md` (strict-mode
  error string the probe scans for)
- Research-0135: `0135-dev-mcp-container-stage-3-fix-2026-05-16.md`
  (prior dev-MCP regression)
- Source: `req` — user briefing.
