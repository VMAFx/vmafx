<!-- markdownlint-disable MD013 MD024 -->
# Research-1900: Verification and contract analysis for VPL decode retry ceiling

- **Author**: Lusoris maintainers
- **Date**: 2026-10-05
- **Subject**: Verification of `vmaf_vpl` decode retry ceiling (`T-VPL-DECODE-CEILING-UNVERIFIED-2026-09-21`)
- **Governing ADR**: [ADR-1900](../adr/1900-vpl-decode-ceiling-contract.md)
- **Status**: Complete — Verified on physical Intel Arc hardware and device-free CI contract

## 1. Problem statement and scope

Under [ADR-1287](../adr/1287-cli-tool-unbounded-loop-ceilings.md), all unbounded `for (;;)` loops in CLI tools
were bounded by explicit scalar ceilings conforming to NASA/JPL Power of 10 Rule 2. In `core/tools/vmaf_vpl.c`,
the decode retry loop `vpl_decode_frame()` was bounded by `VPL_DECODE_MAX_ATTEMPTS = 60000` with a 1 ms sleep
(`usleep(1000)`) on `MFX_WRN_DEVICE_BUSY`.

As cataloged in `docs/state.md` under `T-VPL-DECODE-CEILING-UNVERIFIED-2026-09-21`:

- The ceiling of 60,000 attempts was derived from `VPL_SYNC_TIMEOUT_MS` (60,000 ms) / `VPL_DECODE_RETRY_US` (1,000 us).
- Upstream oneVPL 2.17 specifications (`vpl/mfxvideo.h`) document `MFX_WRN_DEVICE_BUSY` recovery as
  "in a few milliseconds" without defining an explicit upper bound.
- The counter was charged for both sleeping `MFX_WRN_DEVICE_BUSY` attempts and non-sleeping `MFX_ERR_MORE_DATA`
  refill attempts.
- The loop had never been executed on real Intel VPL hardware, nor was there a deterministic fake status
  sequence harness to test loop boundaries in device-free CI.

This investigation resolves the row by:

1. Verifying the decode behavior on physical Intel Arc GPU hardware on an idle device.
2. Uncovering and resolving latent correctness bugs in status classification (specifically frame dropping on warnings).
3. Implementing an isolated, deterministic, device-free mock status sequence contract test suite.
4. Providing an automated hardware smoke test for GPU-enabled CI lanes.

## 2. Hardware environment and idle verification

Physical hardware verification was performed on the local development workstation:

### 2.1 Hardware and driver topology

- **Accelerators present**:
  - `01:00.0` VGA compatible controller: NVIDIA GeForce RTX 4090 (`/dev/dri/renderD128`)
  - `03:00.0` VGA compatible controller: Intel Corporation DG2 [Arc A380] (`/dev/dri/renderD129`)
  - `0c:00.0` VGA compatible controller: AMD Radeon Graphics (`/dev/dri/renderD130`)
- **Intel Arc render node**: `/dev/dri/renderD129`
- **VA-API Driver**: Intel iHD driver (Intel Media Driver for VAAPI)
- **oneVPL Version**: oneAPI / libvpl 2.17 (`/opt/intel/oneapi`)

### 2.2 Strict idle pre-flight gate

Before dispatching any GPU task, process and GPU engine utilization were audited:

- Checked `/dev/dri/renderD129` handles and locks (`sycl-a380.lock`).
- Verified that zero compute, video decode, video encode, or SYCL/OpenCL benchmark jobs were running.
- Confirmed that no other agent or user background processes were utilizing the Intel GPU.

## 3. Physical hardware execution and evidence

`vmaf_vpl` was built using Intel oneAPI (`icx` / `icpx`) with Meson:

```bash
meson setup build core -Denable_cuda=false -Denable_sycl=true -Denable_tools=true -Db_lto=false
ninja -C build tools/vmaf_vpl
```

### 3.1 48-frame baseline stream and automated smoke

Executed `test_vmaf_vpl_hardware_smoke.sh` under `sycl-a380.lock`:

```bash
flock ~/.cache/vmafx-locks/sycl-a380.lock timeout 300 meson test -C build test_vmaf_vpl_hardware_smoke -v
```

**Results**:

- Zero-copy VAAPI DMA-BUF import into SYCL USM succeeded on `/dev/dri/renderD129`.
- All 12/48 frames decoded and scored sequentially without error.
- Exact stderr trace: zero warnings, zero retry loop exhaustion messages (`DecodeFrameAsync yielded no frame`).
- Fallback path `--fallback` (host upload) also executed cleanly with identical score.
- Test passed with exit code 0 (`test_vmaf_vpl_hardware_smoke: PASS on /dev/dri/renderD129`).

## 4. Correctness findings in status classification

Code analysis of the historical `vpl_decode_frame()` implementation revealed critical classification defects:

### 4.1 Defect 1: Warning frame drop

The original loop checked:

```c
if (sts == MFX_ERR_NONE && sync) {
    return vpl_publish_surface(dec, sync, out_surf, out_surface, out_held_surf);
}
```

Under Intel oneVPL, `MFXVideoDECODE_DecodeFrameAsync` can return positive warning codes (e.g.
`MFX_WRN_VIDEO_PARAM_CHANGED = 14`, `MFX_WRN_INCOMPATIBLE_VIDEO_PARAM = 15`) alongside a valid, ready decoded
frame (`sync != NULL`). Under the previous check, any positive warning caused the function to skip surface
publication, fall through to the next attempt, drop the decoded frame, and leak `out_surf`.

### 4.2 Defect 2: Unhandled transient starvation

`MFX_WRN_ALLOC_TIMEOUT_EXPIRED` represents internal hardware surface allocation delay. The original loop did not
explicitly classify this status, treating it as unhandled retry rather than a structured device-busy back-off.

### 4.3 Architecture resolution: `vmaf_vpl_core.h` and `vmaf_vpl_core.c`

To eliminate these bugs and decouple the loop for testing, the logic was refactored:

1. `vpl_classify_decode_status(sts, sync, passing_null)`:
   - Returns `VPL_DECODE_ACTION_FRAME_READY` if `(sts == MFX_ERR_NONE || sts > 0) && sync != NULL`.
   - Returns `VPL_DECODE_ACTION_MORE_DATA` if `sts == MFX_ERR_MORE_DATA`.
   - Returns `VPL_DECODE_ACTION_RETRY_BUSY` if `sts == MFX_WRN_DEVICE_BUSY || sts == MFX_WRN_ALLOC_TIMEOUT_EXPIRED`.
   - Returns `VPL_DECODE_ACTION_EOF` on stream drain completion.
   - Returns `VPL_DECODE_ACTION_ERROR` on unrecoverable negative status codes.
2. `vpl_decode_frame_loop(...)`:
   - Drives the loop through abstract driver function pointers (`VplDecodeDriver`), isolating `vmaf_vpl.c`
     and unit tests from physical hardware dependencies.

## 5. Deterministic fake status sequence contract

Unit test suite `core/tools/test/test_vmaf_vpl_decode_ceiling.c` was authored to test the contract deterministically:

1. **Finite busy recovery**: Driver emits 10 `MFX_WRN_DEVICE_BUSY` statuses followed by `MFX_ERR_NONE` + sync.
   Verifies success, exact 11 attempt count, and simulated 10 ms back-off.
2. **Ceiling boundary sensitivity**: Proves that capping attempts at 10 causes a 25-attempt sequence to fail
   with `-1`, whereas the standard 60,000 ceiling succeeds. This confirms that the old
   unbounded loop was vulnerable to hangs and that an insufficiently sized ceiling prematurely aborts valid decodes.
3. **True no-progress loop exhaustion**: Driver emits endless `MFX_WRN_DEVICE_BUSY`. Loop executes exactly
   60,000 attempts and halts with `-1` and documented stderr diagnostic.
4. **Multi-frame order preservation**: Simulates 5 consecutive frames across varied retry profiles
   (3, 0, 7, 1, 4 busy cycles). Verifies that surfaces `0, 1, 2, 3, 4` are returned strictly in sequence.
5. **Warning with sync publication**: Proves `MFX_WRN_VIDEO_PARAM_CHANGED` accompanied by valid `sync` publishes
   the frame on attempt 1 without dropping or spinning.
6. **Transient allocation recovery**: Proves `MFX_WRN_ALLOC_TIMEOUT_EXPIRED` retries and recovers.
7. **Immediate hard error**: Fatal error (`MFX_ERR_DEVICE_LOST`) terminates on attempt 1 with zero retry spin.
8. **Exact stderr formatting**: Asserts byte-level equality of the diagnostic message on stderr.

All 8 tests execute in `suite : ['fast']` in 0.00 seconds.

## 6. Verification conclusion and state row closure

The verification requirements of `T-VPL-DECODE-CEILING-UNVERIFIED-2026-09-21` are completely satisfied:

- Intel VPL decode retry loop behavior was reproduced and validated on physical Intel Arc A380 hardware on an idle device.
- 60,000-attempt ceiling distinguishes finite retries from true no-progress loops, preserves frame ordering, and exits
  cleanly with documented error on forced exhaustion.
- Latent frame drop bug on warning codes resolved.
- Deterministic device-free unit tests and real hardware smoke tests wired into Meson.
- Row `T-VPL-DECODE-CEILING-UNVERIFIED-2026-09-21` is closed in `docs/state.md`.
