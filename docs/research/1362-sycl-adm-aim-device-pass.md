<!-- markdownlint-disable MD013 MD060 -->
# Research-1362: The AIM pass on the SYCL integer ADM twin — what it takes to match the CPU bit for bit

- **Status**: Active
- **Workstream**: [ADR-1362](../adr/1362-sycl-integer-adm-aim-device-pass.md), [ADR-0746](../adr/0746-cuda-integer-adm3-aim-parity.md), [ADR-1167](../adr/1167-adm-cm-row-level-rounding.md)
- **Last updated**: 2026-09-29

## Question

`integer_adm_sycl` did not emit `aim` or `adm3`, so the default model
`vmaf_v1.0.16_3d0h` ran its ADM on the CPU under `--backend sycl`
(`T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05`). Can the twin compute
AIM inside its existing command graph, without a host wait, and give the CPU
`integer_adm` values bit for bit — and what does that cost?

## Sources

- `core/src/feature/integer_adm.c`: `adm_csf()` / `i4_adm_csf()` and
  `adm_cm()` / `i4_adm_cm()` with `measure_aim`, `integer_adm_scale0()` /
  `integer_adm_scale_s123()` (AIM passes `noise_weight = 0.0`),
  `integer_compute_adm()` and `adm_result_finalise()`.
- `core/src/feature/cuda/integer_adm/adm_cm.cu` (ADR-0746 kernels),
  `core/src/feature/metal/integer_adm.metal` (`integer_adm_aim_cm_s0` /
  `_s123`, stored `csf_f_aim`).
- icpx 2026.1 in `vmaf-dev-mcp:ocloc`, AOT for `bmg-g21` and `adl-s`; Arc B580
  and UHD 770 through WSL2 Level Zero. Fixtures: Netflix `src01` 576x324 pair
  (48 frames, and its 3-frame 10-bit version), BBB 3840x2160 (first 50
  frames), 853x480 and 17x17 luma crops (4:4:4, since the CLI rejects odd
  4:2:0 widths), and 10-bit copies of the 8-bit sets with a deterministic
  2-bit dither.

## Findings

1. **AIM needs one more stored band, not a second pipeline.** In the CPU, AIM
   rewrites the CSF buffers from `decouple_r` and runs the same contrast
   masking with `decouple_a` as the measured signal. The existing reduction
   kernel already recomputes `r` and `t - r` per sample, so the only new
   input is the 3x3 neighbourhood of `|csf(r)| / 30`, which the decouple
   kernel can store beside the DLM band it already writes.
2. **The old reduction decoupled every sample three times.** It launched one
   work-group per (band, row), and each work-group recomputed the decouple of
   all three bands because the masking threshold sums over all three. One
   work-group per row carrying the three bands' sums (nine with AIM) does the
   decouple once; adm2 and the scale outputs are integer sums, so the change
   of shape cannot move them.
3. **The old kernel wrapped the decouple quotient at scales 1-3.** It
   narrowed `(div_lookup * t) >> shift` to int32 before clamping to
   [0, 32768]; the CPU clamps the int64 `tmp_k`. Once `|t / o| > 2^16` the
   quotient passes INT32_MAX and wraps. Planting the old narrowing into the
   new kernel reproduces the old twin's adm2 and scale outputs on BBB 4K
   exactly (50/50 frames, every key), and breaks aim / adm3 on 2-3 frames of
   50. The old twin's `integer_adm_scale2` was up to 1.40e-6 off the CPU at
   4K (measured directly); at 576x324 two frames of 48 moved by at most
   3.7e-12.
4. **The device accumulators are exact; only the host finalisation differs.**
   The CPU finalises each scale in float (`adm_cm_restore_accum()`,
   `adm_num_scale()`, `adm_den_scale_finalise()`, `(float)(int64 / float)` at
   scales 1-3) and sums the float scale terms in double. Repeating exactly
   that on the host makes every ADM output bit-exact everywhere tested. The
   twin's historical double finalisation had left adm2 and the scales 1.4e-7
   to 2.9e-7 from the CPU; it is removed (maintainer decision, 2026-09-29).
5. **Sub-group size matters on Xe-LP.** Nine int64 sums per work-item spill at
   32 lanes on the UHD 770: `adm_sycl` alone at 4K took 59.5 ms per frame at
   sub-group 32 and 45.6 at 16 (DLM only on the old kernel: 41.0). The B580 is
   input-bound at this size and showed no difference.
6. **Recomputing `r` beats storing it on the iGPU.** Writing `r` from the
   decouple kernel and reading it back in the reduction (three more
   full-band buffers) took 49.3 ms per 4K frame on the UHD 770 against 45.6
   recomputing; the iGPU is bandwidth-bound.
7. **The AIM threshold costs about 4.6 ms per 4K frame on the UHD 770.** A
   timing-only variant with the AIM threshold removed ran in 41.0 ms against
   45.6.
8. **Non-integer gain limits are not exact, as before.** With
   `adm_enhn_gain_limit=1.2` aim and adm3 are 1.4e-7 from the CPU and adm2
   1.4e-6 (unchanged from the old twin): the Q31 emulation floors, the CPU
   truncates a rounded double product. Integer gains (1.0, 100.0, every
   shipped model) are exact.

## Results

Every per-frame `aim` and `adm3` value equals `--backend cpu` at
`--precision max` on both GPUs, with the default options and with the default
model's (`adm_csf_mode=2`, `adm_dlm_weight=0.7`, `adm_enhn_gain_limit=1.0`,
`adm_min_val=0.5`, `adm_noise_weight=0.02`):

Final state, every ADM output against the CPU (the Netflix pair and BBB 4K
re-measured on both GPUs after the float finaliser took over adm2 and the
scales):

| Fixture | Frames | aim / adm3 | adm2, scale0..3 | old twin's adm2 / scale* vs CPU |
| --- | --- | --- | --- | --- |
| Netflix 576x324, 8-bit | 48 | 0 | 0 | 2.3e-7 |
| Netflix 576x324, default-model options | 48 | 0 | 0 | 2.4e-7 |
| BBB 3840x2160, 8-bit | 50 | 0 | 0 | 1.34e-6 |
| BBB 3840x2160, default-model options | 50 | 0 | 0 | 1.40e-6 |

Measured with aim / adm3 on the float finaliser and adm2 / scales still on the
double one (so the DLM column shows that finaliser's residual; the device
accumulators are the same in both builds):

| Fixture | Frames | aim / adm3 | adm2 / scale* (double finaliser) |
| --- | --- | --- | --- |
| 853x480 crop, 8-bit | 50 | 0 | 2.6e-7 |
| 17x17 crop, 8-bit | 48 | 0 | 1.9e-7 |
| Netflix 576x324, 10-bit (real, dithered) | 3, 48 | 0 | 2.4e-7 |
| 853x480 / 17x17 crops, 10-bit | 50, 48 | 0 | 2.6e-7 |
| `adm_skip_aim`, `adm_skip_scale0` | 48 | 0 | 2.3e-7 |
| `adm_enhn_gain_limit=1.2`, `adm_p_norm=2` | 48 | 1.4e-7 | 1.4e-6 |

`test_sycl_adm_tiny_frames` now compares every key bit for bit on tiny
frames, noise, 16-bit input and CSF modes 0-3 on both GPUs.

Default model (no `--model`), ms per frame, (t(N) - t(2)) / (N - 2) with
N = 48 at 576x324 (median of 7 runs) and N = 22 at 4K (median of 3); "before"
is master with the CPU ADM fallback. Every GPU interval is timed inside the
shared GPU lock, so waiting for other jobs' GPU work is not counted, and
`--threads` is explicit (0 is the CLI default, under which the CPU fallback
runs on the main thread):

| Size | Device | `--threads` | Before | After |
| --- | --- | --- | --- | --- |
| 576x324 | Arc B580 | 0 | 2.80 | 2.83 |
| 576x324 | Arc B580 | 16 | 2.14 | 3.01 |
| 576x324 | UHD 770 | 0 | 9.32 | 10.78 |
| 576x324 | UHD 770 | 16 | 8.51 | 10.97 |
| 576x324 | CPU backend | 16 | 0.73 | 0.62 |
| 3840x2160 | Arc B580 | 0 | 48.42 | 9.19 |
| 3840x2160 | Arc B580 | 16 | 24.29 | 9.66 |
| 3840x2160 | UHD 770 | 0 | 75.65 | 87.52 |
| 3840x2160 | UHD 770 | 16 | 40.53 | 79.43 |
| 3840x2160 | CPU backend | 16 | 32.07 | 26.54 |

The CPU-backend rows run identical code before and after; their spread is
load from other jobs on the workstation, which also moves the UHD 770 (it
shares the CPU's power budget). The 576x324 B580 rows differ by less than
their run-to-run spread (single runs from 0.25 to 4.2 ms). With `--threads 16`
the old configuration let the CPU compute ADM for several frames beside the
iGPU; that is why the UHD 770 gets slower after this change, and why the
B580's "before" halves from `--threads 0` to `--threads 16`. The UHD 770 shares its power budget with the
CPU cores, so its rows move with that load as well.

## Open questions

- Port the design to the HIP twin (`core/src/feature/hip/integer_adm/adm_cm.hip`);
  the row stays open for it.
- `T-SYCL-ADM-FRACTIONAL-GAIN-LIMIT-2026-09-29`: exact non-integer gain
  limits.
