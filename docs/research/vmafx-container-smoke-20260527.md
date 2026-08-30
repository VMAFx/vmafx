<!-- markdownlint-disable MD013 MD036 MD060 -->
# Research: vmaf-dev-mcp Container Smoke Test — 2026-05-27

**Date:** 2026-05-27
**Operator:** lusoris
**Branch:** `docs/dev-container-smoke-20260527`
**Status:** COMPLETE — findings documented, no code fixes in this PR

---

## 1. Purpose

End-to-end smoke test of the `vmaf-dev-mcp` container per
`docs/development/dev-mcp.md`. Validates:

- Container builds from a fresh image rebuild (stale 7-day image, commits had
  landed in `libvmaf/`, `mcp-server/`, `ai/`, `tools/vmaf-tune/` since last
  build).
- `vmaf` binary is present, reports version, and all six backends are compiled
  in.
- Netflix golden-fixture score matches the expected value (~76.6).
- All GPU backends (CUDA, SYCL, Vulkan, HIP) return correct scores.
- MCP server starts, completes the JSON-RPC handshake, and responds to
  `tools/list` and `tools/call` (vmaf_score).
- FFmpeg codec matrix is complete.

---

## 2. Environment

| Component | Value |
|---|---|
| Host OS | CachyOS Linux 7.0.10-1 |
| Docker Engine | 28.x (Compose v2) |
| Container image rebuilt | yes — 7-day-old image replaced |
| NVIDIA GPU | RTX 4090 (driver 595.71.05, CUDA 13.2) |
| Intel GPU | Arc A380 (DG2, gfx1030 class) |
| AMD GPU | Ryzen 9 9950X3D iGPU (gfx1036 / Raphael RDNA2, exposed as gfx1030) |

---

## 3. Container Build

```text
docker compose --project-directory $(git rev-parse --show-toplevel) \
    -f dev/docker-compose.yml build dev-mcp 2>&1 | tail -30
```

**Outcome:** SUCCESS. All 53 BuildKit stages completed. The Python venv layer
(stage 4) pulled torch 2.12.0, onnxruntime 1.26.0, pytorch-lightning 2.6.5.
Build time was within the expected range; layer cache reused stages 1-3.

---

## 4. Container Start

```text
docker compose --project-directory $(git rev-parse --show-toplevel) \
    -f dev/docker-compose.yml up -d
```

**Outcome:** SUCCESS. `vmaf-dev-mcp` healthy within 20 s. `vmaf-smoke-probe-cron`
started and depends on the primary healthcheck passing.

---

## 5. vmaf Binary Smoke Test

```text
docker exec vmaf-dev-mcp /usr/local/bin/vmaf --version
```

**Output:** `3.0.0`

Backend flags compiled in (from `vmaf --help`):

```text
--no_cuda          --no_sycl          --sycl_device
--no_vulkan        --vulkan_device    --no_hip
--hip_device       --no_metal         --metal_device
--backend auto|cpu|cuda|sycl|vulkan|hip|metal
```

All six backends (CPU, CUDA, SYCL, Vulkan, HIP, Metal) are compiled in.

---

## 6. Golden-Fixture Scoring

Command:

```bash
docker exec vmaf-dev-mcp /usr/local/bin/vmaf \
  --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --output /tmp/golden-test.json --json
```

**Result:** PASS

| Field | Value |
|---|---|
| VMAF mean | 76.667767 |
| Frame count | 48 |
| Expected (Netflix golden) | ~76.6 |
| Delta | +0.07 (within expected tolerance) |

Note: the auto backend selected SYCL (Intel Arc A380) on this invocation. This is
correct — SYCL is the first backend the auto selector picks when an Intel GPU is
present and the GPU probe succeeds.

---

## 7. Per-Backend Scoring Results

All backends tested against the same Netflix golden pair
(`src01_hrc00_576x324.yuv` vs `src01_hrc01_576x324.yuv`).

| Backend | Score | Status | Notes |
|---|---|---|---|
| `cpu` | 76.668 | PASS | Reference score |
| `cuda` | 76.668 | PASS | RTX 4090, within ±0.001 of CPU |
| `sycl` | 76.668 | PASS | Intel Arc A380 (gfx1030/DG2) |
| `vulkan` | 76.668 | PASS | RTX 4090 (fp64 path selected automatically) |
| `hip` | 57.335 | **FAIL** | AMD Ryzen iGPU (gfx1036/Raphael) — 19 points off |

**CUDA, SYCL, and Vulkan all return scores within ±0.001 of CPU.** HIP is the
only failing backend.

---

## 8. Bugs Found

### Bug 1: HIP backend produces wrong VMAF scores on Raphael iGPU (gfx1036)

**Severity:** CRITICAL
**Reproducer:**

```bash
docker exec vmaf-dev-mcp vmaf \
  --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --backend hip --json --output /tmp/hip_score.json
# Expected: ~76.668. Actual: ~57.335 (−19 points)
```

**Observed:** All 48 frames are in the range 55.5–60.7. This is not precision
drift; it is a computation error of ~19 VMAF points. The issue is reproducible
across multiple runs.

**Device context:** The host AMD GPU is a Ryzen 9950X3D integrated GPU
(gfx1036, Raphael/RDNA2 IP rev 10.3.6). `HSA_OVERRIDE_GFX_VERSION=10.3.0` is
set in `docker-compose.yml` `common-env` to map the unsupported gfx1036 onto
the supported gfx1030 target. `rocminfo` reports agent as `gfx1030`.

**Hypothesis:** The fat binary embedded at build time via `hipcc` targets
`gfx1030` (desktop RDNA2 dGPU). The iGPU silicon (gfx1036) is microarch-
compatible but may differ in LDS bank geometry, wave size, or cache topology
in a way that causes numerical error in the VIF/ADM feature kernels. A
secondary hypothesis is that the `HSA_OVERRIDE_GFX_VERSION` lie causes the
runtime to select a wrong dispatch geometry (warp size mismatch) for the
kernel launched against the iGPU.

**Known interaction:** The research doc
`docs/research/hip-integer-moment-registration.md` documents an earlier HIP
register-pressure bug on this same iGPU. This may be a separate root cause or
a related variant.

**Follow-up action:** Dedicated investigation PR required. Candidate fixes:
(a) run the per-frame feature extractor against CPU on gfx1036 and compare
intermediate VIF/ADM tensors against CUDA/SYCL ground truth to isolate the
divergence to a specific kernel; (b) try `HSA_OVERRIDE_GFX_VERSION=10.3.6`
(experimental; not on the supported list but forces the correct target);
(c) add a `gfx1036` → `gfx1035` (Rembrandt iGPU, also RDNA2 and on the list)
override as an alternative.

---

### Bug 2: Entrypoint GPU visibility probe falsely reports SYCL and HIP as NOT DETECTED

**Severity:** MEDIUM (operational noise; does not block scoring)
**Reproducer:**

```bash
docker logs vmaf-dev-mcp 2>&1 | grep WARN
# Output:
# [dev-mcp-entrypoint]   WARN: SYCL level_zero:gpu NOT detected after 10 attempts (~30 s)
# [dev-mcp-entrypoint]   WARN: HIP HSA agent NOT detected after 10 attempts (~30 s)
```

**Observed:** The entrypoint probe reports SYCL and HIP as undetected. However,
`vmaf --backend sycl` and `vmaf --backend hip` both successfully dispatch to
GPU hardware at scoring time. This means the probe is a false negative: the
`sycl-ls | grep level_zero.*gpu` and `rocminfo | grep Agent.*GPU` checks fail
even though the devices are accessible.

**Likely cause (SYCL):** The `sycl-ls` probe inside the entrypoint may run
before the Level Zero ICD registry has settled (the entrypoint calls
`sleep 3` between retries; 10 × 3 s = 30 s window). Alternatively, `sycl-ls`
and `vmaf`'s internal Level Zero init use different initialization paths, and
the probe command returns "Platforms: 0" while the library-level init in vmaf
succeeds.

**Likely cause (HIP):** `rocminfo` grep pattern `Agent.*GPU|gfx[0-9]+` may
not match `rocminfo`'s output format inside the container (observed: `Name: gfx1030`
is present, but not immediately after "Agent"). The `VMaf --backend hip` path
does successfully open the HSA agent.

**Follow-up action:** Update probe commands and grep patterns in
`dev/scripts/dev-mcp-entrypoint.sh` to match actual `sycl-ls`/`rocminfo` output
format. Add the actual output of both commands to the log at INFO level (not
just the WARN) so operators can distinguish "probe grep mismatch" from "device
truly absent".

---

### Bug 3: Spurious "could not open file" message when running multiple backends in a batch loop

**Severity:** LOW (cosmetic; rc=0, file is written correctly)
**Reproducer:**

```bash
docker exec vmaf-dev-mcp bash -c '
for B in cpu cuda sycl vulkan hip; do
  vmaf --reference ... --distorted ... --backend $B --json --output /tmp/probe_${B}.json 2>&1
  echo "rc=$?"
done'
# First iteration of the loop prints: "could not open file: /tmp/probe_cpu.json"
# but rc=0 and the file IS written (31 KB, correct JSON).
```

**Observed only on the first invocation of `vmaf` in a new shell session (fresh
`docker exec`)**. Subsequent runs in the same session do not reproduce it.
Isolated runs (`vmaf ... --output /tmp/out.json` without a loop) do not
reproduce it in later tests.

**Hypothesis:** Race condition between file open and the terminal's first
write of the progress line. The message may originate from a logging path that
attempts to open the output file for a pre-flight check before the scoring
pipeline has created it, then silently retries and succeeds. The message is a
false error on stderr; the actual file write is correct and complete.

**Follow-up action:** Bisect the vmaf CLI output path to find where the
"could not open file" string is emitted and whether it is gated on a first-
open failure. Fix to suppress the message when the file is successfully created
by the scoring run.

---

### Bug 4: VK_DRIVER_FILES set in entrypoint does not propagate to docker exec sessions

**Severity:** LOW (Vulkan still works by default; risk only on unusual ICD
ordering hosts)
**Reproducer:**

```bash
docker exec vmaf-dev-mcp printenv VK_DRIVER_FILES
# (empty)
```

**Observed:** The entrypoint sets `export VK_DRIVER_FILES=...` and then does
`exec tail -F "$LOG_FILE"`, replacing the entrypoint process with `tail`. The
`export` is only visible in the environment of the replaced process; `docker exec`
sessions spawn new child processes from PID 1 (`tail`), which inherits from the
`tail` process's environment — but `tail` does not export environment variables
to its children because `tail` is not a shell.

**Impact on this host:** Vulkan correctly selects the NVIDIA RTX 4090 as
`vulkan_device=0` even without `VK_DRIVER_FILES` because the NVIDIA ICD
(`/etc/vulkan/icd.d/nvidia_icd.json`, bind-mounted by the NVIDIA Container
Toolkit) sorts before lavapipe. On a host where an Intel/AMD/mesa ICD sorts
before NVIDIA's ICD *and* before lavapipe, the device assignment could differ
from the intended behaviour.

**Fix:** Write `VK_DRIVER_FILES` to `/etc/environment` or
`/proc/1/environ`-compatible mechanism, or embed it in a profile file (e.g.
`/etc/profile.d/vmaf-vk.sh`) sourced by `bash --login` shells inside `docker
exec`. Alternatively, document that `docker exec -e VK_DRIVER_FILES=...` is
required for strict device pinning.

---

### Bug 5: First CUDA invocation after container cold start produced EINVAL error (transient)

**Severity:** LOW (transient; subsequent runs succeed)
**First observed** during the batch loop probe that was run immediately after
`docker compose up`. The probe_cuda.json written at that time contained:

```json
{
  "error": "vmaf_cuda_state_init failed",
  "backend_requested": "cuda",
  "errno": -22,
  "adr": "ADR-0498",
  "exit_code": 100,
  "backend_used": "cuda"
}
```

**All subsequent CUDA invocations** (including a fresh `docker exec` run 90 s
later) returned the correct score (76.668). `errno -22` is `EINVAL`.

**Hypothesis:** Cold-start race condition. When `docker compose up` brings the
container up and reports "healthy" (healthcheck is `vmaf --version`, which does
not touch CUDA), the NVIDIA Container Toolkit OCI hook that installs the CUDA
shared libraries into the container's view may not have completed fully. The
first CUDA library `dlopen` attempt races the hook's bind-mount and fails with
EINVAL. By the time `vmaf --backend cuda` is called again, the bind-mounts are
stable. The `start_period: 20s` in the healthcheck is insufficient for CUDA
state to stabilize on this host.

**Follow-up action:** Increase `healthcheck.start_period` to 45 s, or update
the healthcheck command to `vmaf --backend cpu --version` and add a separate
CUDA probe in the entrypoint's retry loop (similar to the SYCL/HIP probes,
which already retry 10 × 3 s).

---

## 9. MCP Server Smoke Test

**Protocol:** JSON-RPC 2.0 over stdio (`docker exec -i vmaf-dev-mcp /opt/vmaf-venv/bin/vmaf-mcp`)

### 9.1 Initialize + tools/list

```bash
printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}\n{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}\n{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}\n' \
  | docker exec -i vmaf-dev-mcp /opt/vmaf-venv/bin/vmaf-mcp
```

**Result:** PASS — 16 tools listed:

- `vmaf_score`, `list_models`, `list_backends`, `run_benchmark`
- `eval_model_on_split`, `compare_models`, `describe_worst_frames`, `probe_backend`
- `vmaf_version`, `vmaf_score_encoded`, `list_extractors`, `describe_model`
- `run_compare`, `run_ladder`, `run_tune_per_shot`
- Plus 1 additional unlisted tool from SDK metadata

Server version: `vmaf-mcp 1.27.1`, protocolVersion `2024-11-05`.

### 9.2 tools/call: vmaf_score (CPU backend)

Sent after `notifications/initialized`, targeting the 576×324 golden pair with
`backend: "cpu"`.

**Result:** PASS — score 76.668 in response content, frame-level metrics
present, `backend_used: cpu`.

### 9.3 UDS socket

The entrypoint does NOT create a UDS socket. The `docker-compose.yml` sets
`VMAF_MCP_UDS_PATH=/sockets/vmaf-mcp.sock` but the current entrypoint uses
stdio transport only (per `dev/scripts/dev-mcp-entrypoint.sh` comments). The
`/sockets/vmaf-mcp.sock` path does not exist inside the container.

This is expected per the current design (stdio is the documented transport).
The `VMAF_MCP_UDS_PATH` env var is retained for future UDS transport
implementation per existing code comments.

---

## 10. FFmpeg Encoder Matrix

All expected encoders are compiled in:

| Encoder | Status |
|---|---|
| `libsvtav1` | compiled-in |
| `libvvenc` | compiled-in |
| `libvpx-vp9` | compiled-in |
| `h264_nvenc` / `hevc_nvenc` / `av1_nvenc` | compiled-in |
| `h264_qsv` / `hevc_qsv` / `av1_qsv` | compiled-in |
| `h264_amf` / `hevc_amf` / `av1_amf` | compiled-in |
| `libaom-av1` | intentionally omitted (ROI fields issue, see Containerfile) |

Note: `libx264` and `libx265` are present but not listed above as they are
always-available baseline encoders.

---

## 11. Summary

| Item | Status |
|---|---|
| Container build | SUCCESS (rebuilt from stale 7-day image) |
| Container start | SUCCESS (healthy) |
| vmaf binary present + version | SUCCESS (3.0.0) |
| Golden fixture score (CPU) | PASS (76.668) |
| CUDA backend score | PASS (76.668) |
| SYCL backend score | PASS (76.668) |
| Vulkan backend score | PASS (76.668) |
| HIP backend score | **FAIL** (57.335 — 19 points off) |
| MCP server handshake | PASS (16 tools, v1.27.1) |
| MCP vmaf_score tool call | PASS |
| FFmpeg encoder matrix | PASS (all expected encoders compiled-in) |

**Total bugs surfaced: 5**

| ID | Severity | Description |
|---|---|---|
| Bug 1 | CRITICAL | HIP backend wrong scores on Raphael iGPU (gfx1036) |
| Bug 2 | MEDIUM | Entrypoint probe false-negatives for SYCL + HIP |
| Bug 3 | LOW | Spurious "could not open file" message (first run, rc=0) |
| Bug 4 | LOW | VK_DRIVER_FILES not propagated to docker exec sessions |
| Bug 5 | LOW | Transient CUDA EINVAL on cold start (first invocation only) |

Code fixes for all five bugs go in follow-up PRs. This research document is the
only deliverable for this PR.
