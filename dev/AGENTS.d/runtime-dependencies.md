---
paths:
  - dev/Containerfile
  - dev/scripts/dev-mcp-entrypoint.sh
  - ai/pyproject.toml
invariant: LD_LIBRARY_PATH includes tbb; NEO/ROCm pinned to kernel UAPI; ORT_VERSION pinned and matches ai/pyproject.toml.
---
<!-- markdownlint-disable MD013 -->
# Runtime dependency invariants (ADR-0541 / ADR-0568)

1. **`LD_LIBRARY_PATH` must include `${ONEAPI_ROOT}/tbb/latest/lib`
   (ADR-0541).** Intel CPU OpenCL ICD
   (`/opt/intel/oneapi/compiler/latest/lib/libintelocl.so`) dlopens
   `libtbb.so.12` at OpenCL platform-enumeration time. Without
   `tbb/latest/lib`, Khronos ocl-icd loader silently drops Intel CPU
   OpenCL platform, leaving SYCL with no CPU fallback when GPU path
   also degraded. Full env line in Containerfile now
   `${DPCPP_ROOT}/lib:${ONEAPI_ROOT}/umf/latest/lib:${ONEAPI_ROOT}/tcm/latest/lib:${ONEAPI_ROOT}/tbb/latest/lib:${LD_LIBRARY_PATH}`.
2. **NEO + ROCm userspace version-pinned to host kernel's UAPI
   (ADR-0541).** See "Userspace ↔ host-kernel UAPI version pins"
   above. `dev-mcp-entrypoint.sh` banner emits `WARN: SYCL
   GPU NOT detected` / `WARN: HIP HSA GPU agent NOT detected`
   line at container start when pin no longer matches host. Bump
   ARG, rebuild instead of working around it on host (CLAUDE.md §12
   r15 sub-rule 4).
3. **`ORT_VERSION` must satisfy `ai/pyproject.toml` requirement
   `onnxruntime>=1.20,<2.0`.** Current pin: `1.26.0` (bumped from
   1.20.1 per ADR-0568 2026-05-18). Tarball naming pattern =
   `onnxruntime-linux-x64-${ORT_VERSION}.tgz` from
   microsoft/onnxruntime GitHub releases. C API stable across 1.x
   line; however, ROCm EP and CUDA EP only available from ORT 1.26+
   (matching container's ROCm 10.1.0 + CUDA 13.x stack —
   ADR-0541/ADR-0542, ROCm bumped by ADR-1225). Bumping ORT_VERSION
   -> verify new version's tarball exists at GitHub releases URL
   before updating ARG, update this note.
