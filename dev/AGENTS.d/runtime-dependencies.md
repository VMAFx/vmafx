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
3. **`ORT_VERSION` pinned on `1.x` (`<2.0`); `ai/pyproject.toml` Python
   floor `onnxruntime>=1.31.1,<2.0` owned independently.** Current pin:
   `1.30.0`. Source = `microsoft.ml.onnxruntime` NuGet package through
   `scripts/ci/ort_nuget_install.py --package cpu` (ADR-3561). C API
   stable across 1.x line; ROCm EP and CUDA EP available from ORT 1.26+
   (matching container's ROCm 10.1.0 + CUDA 13.x stack —
   ADR-0541/ADR-0542, ROCm bumped by ADR-1225); provider-enabled
   distributions need matching runtime dependencies (ADR-0113). Version
   raise -> add pin to `scripts/ci/ort-nuget-pins.json` first
   (`python3 scripts/ci/ort_nuget_install.py --package cpu --print-pin <version>`
   prints size + sha256 entry), then raise `ARG ORT_VERSION`, the matching
   `tracked.cpu` anchor, and this note; guard
   `scripts/ci/check_ort_tarball_pins.py` refuses `ORT_VERSION` without a
   pin in `scripts/ci/ort-nuget-pins.json`, a tracked version without a
   size/sha256 entry, inline `ORT_VERSION` overrides on helper calls, and
   old release sources split across `RUN` or `ADD` continuations with comments.
