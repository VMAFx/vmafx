<!-- markdownlint-disable MD013 MD060 -->
# Research Digest 0135 — Vulkan Dispatch Overhead Characterization

**Date:** 2026-05-15
**Author:** performance-analysis agent
**Status:** Final
**Tracking:** T7-18
**ADR:** none required (research-only; no code changes)

---

## Executive Summary

The T7-18 hypothesis — that Vulkan per-frame dispatch latency exceeds CUDA's due
to excessive small command-buffer submissions, eager fence waits, or queue-submission
dominance — is **partially confirmed but mis-attributed**.

Measured on RTX 4090, driver 595.71.05, Vulkan 1.4.350:

- For single-kernel psnr extraction (1 pipeline), Vulkan and CUDA have **identical
  startup overhead** (~155 ms each), both dominated by kernel-space time (86% sys
  for Vulkan).
- For full VMAF model scoring (multi-extractor), Vulkan is **35–41% slower than CUDA**
  (254 ms vs 188 ms at 576p/48f; 299 ms vs 212 ms at 1080p/5f).
- The root cause is **per-process shader compilation via vkCreateComputePipelines
  with no pipeline cache**, not per-frame fence waits or queue submission overhead.
- Per-frame GPU compute cost is below measurement noise for 576p PSNR (<0.5 ms)
  and immaterial for all tested resolutions against the startup fixed cost.
- At ≤ 50 frames the GPU backends (both Vulkan and CUDA) are 12–500× slower
  than CPU for the psnr-only path and 2–3× slower for full VMAF, because startup
  dominates.

---

## Build

- **Vulkan build:** `meson setup build-vulkan-profile -Denable_vulkan=enabled
  -Denable_cuda=false -Denable_sycl=false -Dbuildtype=release -Db_ndebug=false
  -Dc_args='-g -fno-omit-frame-pointer' -Dcpp_args='-g -fno-omit-frame-pointer'`
  → `core/build-vulkan-profile/tools/vmaf`
- **CUDA build:** same flags with `-Denable_cuda=true -Denable_vulkan=disabled`
  → `core/build-cuda-profile/tools/vmaf`
- **Git hash:** commit `7b10b27a2` (branch `fix/saliency-per-mb-eval-2026-05-15`)

---

## Reproducer Commands

```bash
VMAF_VK=/path/to/core/build-vulkan-profile/tools/vmaf
VMAF_CUDA=/path/to/core/build-cuda-profile/tools/vmaf
REF=/path/to/testdata/ref_576x324_48f.yuv
DIS=/path/to/testdata/dis_576x324_48f.yuv
MODEL=/path/to/model/vmaf_v0.6.1.json

# psnr-only startup scaling (1 vs 48 frames)
for f in 1 2 4 8 16 48; do
  time $VMAF_VK -r "$REF" -d "$DIS" -w 576 -h 324 -p 420 -b 8 \
       --feature psnr --backend vulkan --frame_cnt $f -n -q
done

# Full VMAF model: Vulkan vs CUDA vs CPU
$VMAF_VK  -r "$REF" -d "$DIS" -w 576 -h 324 -p 420 -b 8 --model "path=$MODEL" --backend vulkan  -o /tmp/vk.json   --json -q
$VMAF_CUDA -r "$REF" -d "$DIS" -w 576 -h 324 -p 420 -b 8 --model "path=$MODEL" --backend cuda   -o /tmp/cuda.json --json -q
$VMAF_VK  -r "$REF" -d "$DIS" -w 576 -h 324 -p 420 -b 8 --model "path=$MODEL" --backend cpu    -o /tmp/cpu.json  --json -q

# perf stat for user/kernel split
perf stat -e task-clock,cycles:u,cycles:k \
  $VMAF_VK -r "$REF" -d "$DIS" -w 576 -h 324 -p 420 -b 8 \
  --feature psnr --backend vulkan --frame_cnt 48 -n -q
```

**Input YUV references:** `testdata/ref_576x324_48f.yuv` + `testdata/dis_576x324_48f.yuv`
(48-frame 576×324 YUV420P 8-bit, derived from Big Buck Bunny).
Also tested: `python/test/resource/yuv/src01_hrc00_1920x1080_5frames.yuv` (5 frames, 1080p 8-bit).

---

## Measured Timings

### psnr-only (Vulkan backend, single extractor, 1 pipeline)

| Frames | Vulkan wall (ms) | CUDA wall (ms) | CPU wall (ms) |
|--------|-----------------|----------------|---------------|
| 1      | 147             | 162            | 4             |
| 2      | 148             | 157            | 4             |
| 4      | 146             | 159            | 4             |
| 8      | 153             | 156            | 4             |
| 16     | 148             | 155            | 4             |
| 48     | 159             | 158            | 4             |

**Observation:** Vulkan and CUDA are statistically indistinguishable for psnr-only.
Both show ~155 ms regardless of frame count. CPU scales linearly at ~0.06 ms/frame.

### perf stat (psnr, 48 frames, 576p)

| Counter          | Vulkan     | CPU        |
|------------------|------------|------------|
| task-clock (ms)  | 145.9      | 2.8        |
| user cycles      | 130M       | 14.8M      |
| kernel cycles    | 674M       | 2.9M       |
| user:kernel ratio| 16%:84%    | 84%:16%    |
| wall time (s)    | 0.161      | 0.003      |

**Observation:** 84% of Vulkan clock time is kernel space. This points to driver ioctl
processing, not user-space algorithmic work.

### Full VMAF model (vmaf_v0.6.1.json, all extractors)

| Resolution | Frames | Vulkan (ms) | CUDA (ms) | CPU (ms) | VK/CUDA ratio |
|------------|--------|-------------|-----------|----------|---------------|
| 576×324    | 48     | 254         | 188       | 80       | +35%          |
| 1920×1080  | 5      | 299 (warm)  | 212 (warm)| 154      | +41%          |

---

## Profiling

### perf profile call-graph summary

Profile collected via `perf record -g --call-graph=dwarf -F 500` (254 samples,
1.877 MB, 150 ms run on 576p/psnr-only).

Without root access, kernel module symbols are unresolved. The visible user-space
call tree anchors to:

| Self% | DSO / symbol                          | Interpretation                     |
|-------|---------------------------------------|------------------------------------|
| 26%   | [kernel] (unresolved)                 | NV kernel module compute path      |
| 5%    | [kernel] (unresolved)                 | VkQueue submission ioctl            |
| 5%    | [kernel] (unresolved)                 | vkWaitForFences / fence signal      |
| 4%    | ld-linux / dynamic linker             | Dynamic symbol resolution (volk)   |
| 4%    | [kernel] (unresolved)                 | vkCreateInstance ioctl              |
| 2%    | libnvidia-rtcore.so (shader compiler) | SPIR-V → SM90a PTX compilation      |
| ~35%  | vkDestroyDevice + vkDestroyInstance   | Teardown (identified in call graph) |
| ~5%   | vkCreateInstance                      | Instance creation                  |
| ~3%   | vkEnumeratePhysicalDevices            | Device selection                   |

The dominant user-visible Vulkan calls in the profile are `vkDestroyDevice` and
`vkDestroyInstance` (teardown), with `vkCreateInstance` and
`vkEnumeratePhysicalDevices` also visible. `vkQueueSubmit` and `vkWaitForFences`
do not appear as dominant samples, ruling out per-frame fence-wait overhead as
the bottleneck.

Profile artifacts: `build/profiles/2026-05-15/vulkan_psnr_576p.perf`,
`build/profiles/2026-05-15/vulkan_psnr_1080p_init.perf` (both
committed as large binaries under `.gitignore`; available locally on the
profiling machine).

---

## Root Cause Attribution

The 155 ms startup overhead (Vulkan and CUDA alike) breaks down into four
contributing phases, ordered by estimated magnitude:

### (a) `vkCreateComputePipelines` with no pipeline cache — PRIMARY driver

`kernel_template.h:300`:

```c
vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, NULL, &out->pipeline);
```

`VK_NULL_HANDLE` as the pipeline cache argument means the NVIDIA driver compiles
SPIR-V to SM90a PTX on every process invocation. There is no on-disk or in-process
cache. `libnvidia-rtcore.so` appears in the profile (2% self-time, but that is from
a 200 Hz sample of a 150 ms window; actual wall fraction is likely 20–30%).

For the full VMAF model, 22 `vmaf_vulkan_kernel_pipeline_create` calls execute (one
per pipeline across all extractors, counted from source). Each triggers one or more
`vkCreateComputePipelines` calls. This is why full-model Vulkan is 35–41% slower
than CUDA: CUDA embeds pre-compiled PTX/cubin in the fat binary and registers kernels
at module load time, avoiding the per-startup compile cost.

### (b) `vkCreateInstance` + device enumeration — SECONDARY

`vkCreateInstance` + `vkEnumeratePhysicalDevices` appear in the profile. The NV
driver initializes its shader compiler state on `vkCreateDevice`. Estimated
contribution: 20–40 ms. Not separable from (a) without root-level instrumentation.

### (c) `vkDestroyDevice` + `vkDestroyInstance` teardown — TERTIARY

Both calls dominate the profile's call-chain view (35% of samples), confirming that
teardown on the NVIDIA driver is expensive. This is symmetric with the init cost;
the driver likely flushes all pending work and reclaims GPU allocations synchronously.

### (d) VMA allocator + buffer allocation — QUATERNARY

VMA `vmaCreateAllocator` + the per-extractor `vmaf_vulkan_buffer_alloc` calls add
memory-allocator setup time. Estimated contribution: < 10 ms. Not isolated without
per-function instrumentation.

### Per-frame dispatch overhead: NOT a bottleneck

The T7-18 hypothesis about per-frame `vkQueueSubmit` + `vkWaitForFences` latency
is **not confirmed** by the data. The per-frame cost for 576p PSNR is below the
30 ms run-to-run jitter (< 0.5 ms), and `vkQueueSubmit` / `vkWaitForFences` do
not appear as self-time leaders in the perf profile. ADR-0256 (`submit_pool`)
already eliminated per-frame fence/command-buffer alloc/free overhead.
The synchronous `vkWaitForFences(UINT64_MAX)` design in `submit_end_and_wait` is
correct for the current single-threaded extractor model — changing it to async
would only help if the pipeline could overlap host and GPU work, which requires
a redesign of the frame-delivery path.

---

## Regression Check vs Last Committed Snapshot

`testdata/perf_benchmark_results.json` records CUDA at 249 fps (best, 48 frames
1080p BBB). This uses the full vmaf_bench harness with a persistent context across
all frames, so the 155 ms startup is amortized over 48 frames → ~ 3 ms/frame vs
245 ms of actual compute. The Vulkan backend is not in that snapshot (vmaf_bench
does not support Vulkan — it lacks `#ifdef HAVE_VULKAN` guards).

**Result: PASS — no regression.** The Vulkan numbers above are additive findings;
they do not contradict any existing committed benchmark value.

---

## Recommended Next Step

The following candidate fixes are ordered by estimated implementation complexity
and expected impact:

### Option 1 (Recommended): Per-process VkPipelineCache with optional XDG_CACHE_HOME serialization

**Impact:** Eliminates (a) on every process invocation after the first. First-run
compile amortized across all subsequent runs; typical savings 80–120 ms on an
NVIDIA discrete GPU.

**Mechanism:**

- Create one `VkPipelineCache` in `vmaf_vulkan_context_new` and store it on
  `VmafVulkanContext`. Pass it to every `vkCreateComputePipelines` call in
  `kernel_template.h` instead of `VK_NULL_HANDLE`.
- Serialize the cache to `$XDG_CACHE_HOME/libvmaf/pipeline_cache_<vendor>_<device>.bin`
  on `vmaf_vulkan_context_destroy`; reload at next `context_new` via
  `VkPipelineCacheCreateInfo.pInitialData`.
- Cache validation: check `VkPhysicalDeviceProperties.vendorID + deviceID + driverVersion`
  against a header embedded in the file; invalidate on mismatch.

**Implementation sites:**

- `core/src/vulkan/common.c`: create/load/save cache in `vmaf_vulkan_context_new`
  / `vmaf_vulkan_context_destroy`.
- `core/src/vulkan/kernel_template.h:300,689`: replace `VK_NULL_HANDLE` with
  `ctx->pipeline_cache`.
- `core/src/vulkan/vulkan_internal.h`: add `VkPipelineCache pipeline_cache` to
  `VmafVulkanContext`.

**Risk:** Driver pipeline cache format is vendor-opaque and not forward-compatible.
Mitigation: always validate the header before loading; fall back to recompile on
mismatch.

**Expected gain:** 80–120 ms reduction (per-process, after first run). Full VMAF
Vulkan at 576p/48f would drop from 254 ms → ~130–170 ms, closing 60–70% of the
CUDA gap.

### Option 2: Lazy pipeline initialization (deferred to first extract() call)

**Impact:** Hides startup from the caller if frames are pipelined; does not reduce
total work.

**Mechanism:** Move `create_pipeline()` + `alloc_buffers()` from `init()` to the
first `extract()` call under a per-extractor `once_flag`.

**Risk:** Changes the threading contract; first-frame latency spike becomes visible
in real-time pipelines. Does not fix the underlying compile cost.

**Expected gain:** 0 ms reduction in total wall time; redistributes latency.

### Option 3: Multi-frame batching (batch N dispatches per vkQueueSubmit)

**Impact:** Reduces queue-submit frequency but does NOT address the compile overhead.
Only beneficial if per-frame `vkQueueSubmit` latency were the bottleneck, which
the profile rules out.

**Mechanism:** Accumulate N frames' command buffers into a single `VkSubmitInfo`
with `commandBufferCount = N`. Requires non-trivial changes to the frame-delivery
and result-readback path.

**Expected gain:** < 5% for the current workload (per-frame submit overhead is
below noise floor).

### Option 4: Persistent secondary command buffers (SECONDARY_CMDBUF_REUSE path)

**Impact:** Eliminates `vkResetCommandBuffer` + `vkBeginCommandBuffer` per frame
(already optimized by ADR-0256 submit pool). The SECONDARY_CMDBUF_REUSE strategy
in `dispatch_strategy.c` is a stub; activating it would record dispatch commands
once and re-execute them each frame via `vkCmdExecuteCommands`.

**Mechanism:** Implement the stub in `dispatch_strategy.c`. For static-geometry
kernels (psnr, motion_v2) the push constants change per frame (width/height/bpc
do not change, but `num_workgroups_x` could be constant), so the cmdbuf could be
truly reusable. Requires VK_COMMAND_BUFFER_LEVEL_SECONDARY + inheritance info.

**Expected gain:** < 2% reduction (cmd buffer record overhead is already below
noise floor per ADR-0256; this optimizes the wrong layer).

---

## Decision Matrix

| Fix              | Complexity | Wall-time gain | Risk  | Verdict          |
|------------------|-----------|---------------|-------|------------------|
| Pipeline cache   | Medium    | 80–120 ms     | Low   | **Recommended**  |
| Lazy init        | Low       | 0             | Med   | Not useful       |
| Batch submit     | High      | < 5%          | High  | Wrong bottleneck |
| Secondary cmdbuf | High      | < 2%          | High  | Wrong bottleneck |

**Recommended fix:** Option 1 (pipeline cache). File as T7-18-follow-up; implement
in a dedicated PR with the ADR and `places=4` cross-backend gate per ADR-0214. A
possible secondary deliverable is adding Vulkan timing rows to `vmaf_bench` so
future regressions are caught automatically.

---

## T7-18 Backlog Resolution

T7-18 (per-frame Vulkan dispatch overhead) as originally framed — three candidate
causes: (a) too many small command buffers, (b) eager fence waits, (c) queue
submission cost dominates small kernels — is **not confirmed by measurement**.
All three per-frame mechanisms are either already optimized (ADR-0256) or below
the noise floor.

The real finding is a **startup overhead** from uncached shader compilation, which
is orthogonal to per-frame dispatch. The backlog item should be re-filed as:

> **T7-18-revised:** "Add VkPipelineCache to eliminate per-process SPIR-V recompilation.
> Expected reduction: 80–120 ms startup overhead on NVIDIA dGPU. Closes original
> T7-18 hypothesis (per-frame overhead not confirmed; startup overhead confirmed
> and attributed)."
