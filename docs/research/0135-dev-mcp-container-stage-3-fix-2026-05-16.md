<!-- markdownlint-disable MD013 MD060 -->
# Research-0135: dev-MCP container stage-3 EACCES diagnosis

**Date**: 2026-05-16
**Branch**: fix/dev-mcp-stage3-and-bundled-fixes-2026-05-16
**Status**: Closed (fix landed in same PR)

## Problem statement

Build attempts 5 and 6 (logged at `/tmp/dev-mcp-build6.log`) reached stage 2
(oneAPI install) successfully but failed at stage 3 (`libvmaf-build`) with:

```text
PermissionError: [Errno 13] Permission denied: '/build/vmaf/build'
ERROR: Unhandled python OSError. This is probably not a Meson bug, but an issue
with your build environment.
  File "mesonbuild/msetup.py", line 121, in validate_core_dirs
    os.makedirs(ndir1, exist_ok=True)
```

## Root cause

Docker's `WORKDIR` instruction creates the target directory as **root** (uid 0,
gid 0, mode 0755). The subsequent `COPY --chown=vmaf:vmaf . /build/vmaf/`
copies file and subdirectory contents into that directory and chowns each
transferred node — but `--chown` does **not** retroactively change the ownership
of the destination directory itself (`/build/vmaf`). It is equivalent to:

```text
mkdir -p /build/vmaf          # root:root, mode 0755
rsync --chown=vmaf:vmaf src/ /build/vmaf/
```

After the COPY, every file inside `/build/vmaf` is owned by `vmaf`, but the
directory `/build/vmaf` is still `root:root 0755`. When the subsequent
`USER vmaf` invocation runs `meson setup build`, meson calls
`os.makedirs('/build/vmaf/build', exist_ok=True)`. Since `/build/vmaf` is
owned by root, the vmaf user cannot create the `build` subdirectory inside it
→ EACCES → meson surfaces as OSError.

This is a well-known Docker multi-stage / non-root-user pitfall: `WORKDIR`
ownership is invisible at a glance but silently blocks non-root writes into
the directory.

## Verification

The log confirms exit code 13 (EACCES) at exactly
`os.makedirs('/build/vmaf/build', exist_ok=True)` within meson's
`validate_core_dirs`, 0.754 s into the RUN step — consistent with an
immediate permission check rather than a toolchain or prefix issue.

## Fix options considered

| Option | Notes | Selected |
|---|---|---|
| `RUN chown vmaf:vmaf /build /build/vmaf` (as root, before `USER vmaf`) | Minimal blast radius; fixes only the directory entry that blocks meson; preserves COPY --chown on file contents | **Yes** |
| `--prefix=/home/vmaf/install` instead of `/usr/local` | Would fix the `install` step but not the build-dir creation step; meson tries to mkdir the build dir first | No — wrong fix plane |
| Remove `USER vmaf` for the build step | Builds and installs as root; violates CERT C ENV32 (do not run builds as root); creates root-owned installed files | No — security regression |
| `RUN mkdir -p /build/vmaf && chown vmaf:vmaf /build/vmaf` before COPY | Equivalent to chosen fix but more verbose; WORKDIR already creates the dir | No — redundant mkdir |
| Switch from `WORKDIR` to explicit `RUN mkdir + chown` | Works, but drops the semantic clarity of `WORKDIR` | No — over-engineered |

## Bundled fixes (same PR)

These fixes were independently diagnosed (builds 1–4) but lost in the PR #845
squash merge. They are re-applied in the same PR to avoid accumulating a backlog
of container-only fix PRs:

| Fix | Symptom | Root cause |
|---|---|---|
| Drop `cuda-compiler` + `libcuda1` | `E: Package 'libcuda1' has no installation candidate` | `libcuda1` is the runtime driver; it must come from nvidia-container-runtime at runtime, not be baked into the image. `cuda-compiler` is a legacy alias no longer in current CUDA channels. `cuda-toolkit` already provides nvcc. |
| `intel-basekit-2025.3` → `intel-basekit` | `E: Unable to locate package intel-basekit-2025.3` | Intel does not publish year-quarter-versioned meta-package names in apt.repos.intel.com/oneapi. The correct unversioned name is `intel-basekit`. |
| `rocm-hip-sdk` → `rocm-hip-runtime-dev` | `rccl: Depends: libdrm-amdgpu-amdgpu1 but it is not installable` | `rocm-hip-sdk` pulls in `rccl` (multi-GPU collectives) which depends on `libdrm-amdgpu-amdgpu1` + `libdrm2-amdgpu` — packages absent from the ROCm 6.4 noble repo. libvmaf HIP feature kernels use one GPU per worker; rccl is not needed. |
| `hadolint ignore=DL4006` on patch-apply RUN | False-positive warning | hadolint DL4006 does not track cross-stage SHELL inheritance; `SHELL ["/bin/bash", "-o", "pipefail"]` is set in `gpu-sdks` and inherited by `libvmaf-build`. |

## References

- Build log: `/tmp/dev-mcp-build6.log`
- ADR-0451: `docs/adr/0451-local-dev-mcp-container.md`
- Docker documentation: [WORKDIR](https://docs.docker.com/reference/dockerfile/#workdir) — "The WORKDIR instruction creates the directory if it does not exist."
- CERT C ENV32-C: "All exit handlers must return normally."
