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
   (`python3 scripts/ci/ort_nuget_install.py --print-pin <version>`
   prints size + sha256 entry), then raise `ARG ORT_VERSION` and update
   this note.

   Guard `scripts/ci/check_ort_tarball_pins.py` performs bounded static checks;
   it is not a general shell or Docker parser. It checks the entire logical
   `RUN` instruction for helper tokens, including a bare token and module
   spelling. A helper token after the command-parser cap still yields a
   continuation-limit finding. Continuation joining treats blank and
   full-line comment lines consistently.

   A container helper `RUN` is accepted only when every simple command
   separated by unquoted shell `&&` is the helper, `export
   ORT_VERSION=<literal>`, `set -e`/`set -eux`/`set -o pipefail`, or
   `ldconfig`. Quoted `&&` is not a separator. Bare `ORT_VERSION=<literal>`,
   inline overrides, `.`/`source`, `--print-pin`, flags, exec form, heredoc,
   and other commands are rejected. Single-quoted `$ORT_VERSION` text does
   not expand. Helper version may be a literal or `$ORT_VERSION`/`${ORT_VERSION}`
   in an accepted form.

   Effective `ORT_VERSION` comes from the preceding export in that `RUN`, or
   from same-stage `ARG`/`ENV` bindings. Last assignment wins; `ENV` takes
   precedence over `ARG`. Only real `FROM` instructions start a new stage;
   `FROM` text in continuations or heredoc content does not. Unsupported
   dynamic values affecting version resolution fail closed. Each recognized
   NuGet URL resolves its referenced variable using the value effective at
   that stage and instruction. Unrelated variables carrying an ORT package
   name do not cause a finding.

   CBA checks associate validation with each target download: checksum must
   follow download, and tar extraction must follow checksum. Discovery uses
   Git-tracked workflow and container files named exactly `Dockerfile` or
   `Containerfile`, prefixed by `Dockerfile.` or `Containerfile.`, or suffixed
   by `.Dockerfile` or `.Containerfile`, including `docker/dev/*.Dockerfile`;
   `Containerfile.yaml` uses container parsing. Recognized Git discovery
   failures exit 2; glob fallback applies only to non-Git fixture directories.
   Untracked backups stay outside discovery.

   Limit: `COPY install.sh /install.sh` plus `RUN sh /install.sh` is not read;
   cross-file shell scripts remain outside guard scope. The installer still
   enforces package size and SHA-256 pins when such a script invokes it.

   `ARG ORT_SHA256` is not read by build; kept only for `renovate.json`
   dependency-update rule until rule moves (ADR-3561 follow-up).
