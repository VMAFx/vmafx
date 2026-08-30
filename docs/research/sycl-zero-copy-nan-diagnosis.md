<!-- markdownlint-disable MD013 MD060 -->
# SYCL Zero-Copy NaN Diagnosis — Phase 1 Research Digest

**Diagnostic branch:** `fix/libvmaf-sycl-zerocopy-nan`  
**Verdict commit:** see `git log --oneline -- docs/research/sycl-zero-copy-nan-diagnosis.md`  
**Date:** 2026-06-30  
**Phase:** 1 (Diagnose) — Plan 01-02 (DIAG-02)  
**ADR-0108 role:** Phase 2 / Phase 4 research-digest stub (citable by commit hash from the fix PR)

---

## Summary

This digest records the decisive measurement that selects the Phase 2 fix direction for the
`libvmaf_sycl` zero-copy NaN bug in VMAFx. The bug causes the `libvmaf_sycl` FFmpeg filter
(QSV/VAAPI zero-copy path) to return `VMAF score: nan` on matched QSV-decoded 4K HDR input
while the equivalent `libvmaf` host-upload filter (same machine, same SYCL device) produces
correct scores at ~119 fps.

**Verdict:** hyp 1 confirmed — buffer content wrong in the VA-import path → Phase 2 = FIX-01
(double-buffer the VA-import path; converge onto the `shared_frame_upload` slot-toggling
pattern from `common.cpp:597-598`).

---

## Test Setup

**Machine:** NAS server, Intel Arc A380 (SYCL device 0), Fedora, Kernel 6.19  
**Container:** `vmafx` built from `Containerfile.vmafx` (oneAPI basekit + ffmpeg n8.1.1 + VMAFx)  
**Probe code:** `vmaf_sycl_checksum_y_slot` (commits b9c0088d + df8a382e on this branch) injected
via `probe-diag.patch` build-context patch (not pushed to remote).

**Input pair (Pair 1 — 4K HDR, PRIMARY):**

| Role | Codec | Pixel Format | Resolution |
| ------ | ------- | ------------- | ------------ |
| REF | HEVC Main10 | yuv420p10le | 3840x2160 |
| DIST | AV1 Main | yuv420p10le | 3840x2160 |

Both decoded with QSV (`hevc_qsv` / `av1_qsv`). Formats match — no conversion needed.

**Oracle:** `libvmaf` filter with `sycl_device=0` + `VMAF_SYCL_NO_GRAPH=1` (host-upload path,
proven working at ~119fps). Input converted via `hwdownload,format=p010le` before the filter.  
**Reproducer:** `libvmaf_sycl` filter (zero-copy VA-import path, broken).  
**Env gate:** `VMAF_SYCL_CHECKSUM=1` for both runs.  
**Frame cap:** 100 frames (NaN signature appears within first 10-15 frames).

---

## Key Measurements

### Aggregate VMAF

| Filter | VMAF score | NaN frames |
| -------- | ----------- | ----------- |
| Oracle (host-upload, NO_GRAPH=1) | **95.934579** | 0 / 100 |
| Reproducer (VA-import zero-copy) | **NaN** | reproduced |

**Zero-cost gate:** Reproducer without `VMAF_SYCL_CHECKSUM` also gives NaN — probe does not
perturb the result.

### Slot Behavior

The D2H probe reads `state->cur_compute` and logs it as `slot=N` for each frame.

| Path | Slots observed |
| -------- | --------------- |
| Oracle (host-upload) | 0, 1, 0, 1, 0, 1, … (correct double-buffer toggle) |
| Reproducer (VA-import) | 0, 0, 0, 0, 0, 0, … (permanently stuck at 0) |

The host-upload path correctly toggles `cur_compute` via `shared_frame_upload`
(`common.cpp:597-598`). The VA-import path calls `vmaf_sycl_advance_frame` which increments
only `frame_counter`, not `cur_compute` — `cur_compute` stays 0 forever.

### Per-Frame Checksum Diff

80 frames compared (oracle: 100 frames; sycl: 80 frames before VA surface error).

| Metric | Result |
| -------- | -------- |
| REF CRC mismatches | **80 / 80** (100%) |
| DIS CRC mismatches | **80 / 80** (100%) |

**Sample rows:**

```text
Frame  Host-slot  Host-ref-CRC  Host-dis-CRC  Sycl-slot  Sycl-ref-CRC  Sycl-dis-CRC  Match
    0          0    0x09271dc5    0x09271dc5          0    0xa3471dc5    0xa3471dc5    DIFF/DIFF
    8          0    0x09271dc5    0x09271dc5          0    0xbcf83fca    0xa3471dc5    DIFF/DIFF
    9          1    0x09271dc5    0x09271dc5          0    0x6800c257    0xa3471dc5    DIFF/DIFF
   10          0    0xcefe8929    0x28e13197          0    0xb92ea64f    0x6800c257    DIFF/DIFF
   11          1    0x045103d1    0xda0a893b          0    0x1d955aa7    0xbcf83fca    DIFF/DIFF
   12          0    0xd8482266    0x02a49da4          0    0x92638727    0xb92ea64f    DIFF/DIFF
   79          1    0xcd3a0f58    0x4bd3c4fb          0    0x4f204a63    0x0cf86c66    DIFF/DIFF
```

**Secondary observation:** `sycl_dis[N]` frequently matches `sycl_ref[N-k]` for small k (1–6),
with irregular offsets. Example: `sycl_dis[10]=0x6800c257 = sycl_ref[9]`,
`sycl_dis[11]=0xbcf83fca = sycl_ref[8]`, `sycl_dis[12]=0xb92ea64f = sycl_ref[10]`.
This pattern (dis buffer reading stale ref data from earlier frames) is consistent with either
(a) the dis VA-import writing to `shared_ref_buf[0]` instead of `shared_dis_buf[0]` due to a
pointer confusion, or (b) a race where the single shared slot 0 is read during an in-flight
import. Phase 2 should instrument `vmaf_sycl_get_shared_dis` to confirm the pointer target.

### Per-Frame VMAF

| Frame | Host VMAF | Host int_motion | Sycl VMAF | Sycl int_motion |
| ------- | ----------- | ----------------- | ------------- | ----------------- |
| 0 | 97.428027 | 0.000000 | 97.428027 | 0.000000 |
| 1 | 97.428027 | 0.000000 | 100.000000 | 1024.000000 |
| 8 | 97.428027 | 0.000000 | 100.000000 | 4131.519855 |
| 10 | 94.389818 | 48.582123 | 100.000000 | 4131.657814 |
| 17 | 94.590208 | 0.201475 | 100.000000 | 4384.052737 |

Sycl VMAF=100.000000 (ref==dist, impossible) from frame 1 onwards. Sycl integer_motion mean
4157.5, max 4903.3 (vs oracle normal range 0–48). Both signatures match HANDOFF.md predictions.

---

## Frame-0 Caveat

Frame-0 checksums DIFFER between oracle and sycl (`0x09271dc5` vs `0xa3471dc5`) because the
two code paths allocate USM buffers with different initial states. However, frame-0 VMAF matches
(97.428027 on both paths) because frame 0 has no temporal predecessor — `integer_motion[0]=0`
and `adm[0]=vif[0]=1.0` from initialization. Frame-0 VMAF agreement is NOT evidence of buffer
correctness. Hyp-2 determination uses frames >= 1.

---

## Verdict

### hyp 1: buffer content wrong (slot/overwrite race) → Phase 2 = FIX-01

ANY frame having `crc_host != crc_sycl` for ref or dis selects hyp 1. We have 80/80 such
frames — an unambiguous result.

The sycl path puts the WRONG bytes into the compute slots. With `cur_compute` permanently 0,
both imports (ref and dis) and all compute operations share a single slot-0 buffer, creating
races where import for frame N+1 overwrites the slot that compute is reading for frame N.
Additionally, the dis buffer pattern suggests the dis import may be targeting the wrong USM
buffer pointer entirely.

**Fix direction (Phase 2 — FIX-01):**

1. Make `vmaf_read_pictures_sycl` write to the UPLOAD slot (not compute slot), then advance
   `cur_compute` — mirroring `shared_frame_upload` (`common.cpp:597-598`).
2. Verify that `vmaf_sycl_import_va_surface` for `is_ref=0` calls `vmaf_sycl_get_shared_dis`
   (not `vmaf_sycl_get_shared_ref`) to target the correct USM buffer.
3. Ensure the slot-1 combined command graph (recorded at `common.cpp:834-835`) is replayed for
   frame N while frame N+1 is imported to slot 0 — proper double-buffer separation.
4. Re-run this probe after the fix: verify checksums match oracle for all frames.

---

## Research Predictions vs Observations

| Prediction (01-RESEARCH.md) | Observation |
| ----------------------------- | ------------- |
| `cur_compute` = 0 permanently | Confirmed: sycl always slot=0 |
| Host-upload toggles correctly | Confirmed: oracle slot=0,1,0,1,... |
| `integer_motion` extreme (~3109) | Confirmed: sycl max 4903.3, mean 4157.5 |
| adm=vif=1.0 → VMAF ~100 on early frames | Confirmed: sycl VMAF=100 frames 1-79 |
| Oracle correct (~119fps) | Confirmed: VMAF 95.93, 0 NaN |
| Checksums differ → hyp 1 | Confirmed: 80/80 mismatches |

---

## FFmpeg-Patch Parity

`vmaf_sycl_checksum_y_slot` is internal to `core/src/sycl/common.h` — not in any public header
or consumed by `ffmpeg-patches/0005-*`. No patch update required (CLAUDE.md rule 14).

---

## Files Referenced

| File | Location | Contents |
| ------ | ---------- | ---------- |
| `host_checksums.csv` | `/tmp/vmafx-diag-results/` (gitignored) | 200 oracle checksum lines |
| `sycl_checksums.csv` | `/tmp/vmafx-diag-results/` (gitignored) | 160 sycl checksum lines |
| `host_vmaf.csv` | `/tmp/vmafx-diag-results/` (gitignored) | 101-row per-frame VMAF (oracle) |
| `sycl_vmaf.csv` | `/tmp/vmafx-diag-results/` (gitignored) | 81-row per-frame VMAF (sycl) |
| `VERDICT.md` | `.planning/phases/01-diagnose/VERDICT.md` (gitignored) | Full verdict with all evidence |

The salient evidence rows are embedded above. The CSV files are scratch-space diagnostic
artifacts; the embedded rows in this document are the durable record.
