## Upstream reconcile: arm64 ADM port hazards, #1551 closed (2026-10-08)

`docs/upstream-reconcile-2026-10-08`, [ADR-1402](adr/1402-adm-cm-centre-tap-int32.md),
[ADR-1413](adr/1413-adm-gain-limit-truncated-double-product.md),
[ADR-1417](adr/1417-integer-aim-unclipped-upstream-parity.md),
[ADR-0155](adr/0155-adm-i4-rounding-deferred-netflix-955.md). Documentation
only; no fork code changes.

**arm64 ADM contrast masking and scales 1-3 (upstream 8bc5a5c6a, b41d2340a):
port hazards.** Port this code only if it is bit-exact with the fork's scalar
kernels (ADR-1402, ADR-1413, ADR-1417), checked under `qemu-aarch64` with GCC
and clang. Three places to check before taking it:

1. `adm_cm_threshold_neon()` (`arm64/adm_neon.c:364-380`) narrows the scale-0
   centre tap with `vmovn_s32`, and `adm_cm_accum_neon()` shifts the threshold
   in 32 bits (`:401`). The fork's scalar keeps the tap in 32 bits and forms the
   excess in 64 bits with a clamp (ADR-1402); a port must not copy these two
   lines.
2. `i4_adm_cm_threshold_neon()` (`:760-761`) uses `vdupq_n_s64(INT32_MIN)` as
   the rounding term to match upstream's scalar (Netflix/vmaf#955). The fork's
   scalar keeps that behaviour (ADR-0155), so this one is correct to keep.
3. `adm_decouple_neon()` falls back to the scalar kernel for non-integral gain
   limits (`:273`), which agrees with ADR-1413.

`adm_cm_neon()` only runs for widths of 32 and above and falls back to the
scalar kernel below.

**Update to the `core/src/feature/compat_builtin.h` entry (Netflix/vmaf#1551,
retracting #1422) further down this page.** Do not adopt Netflix/vmaf#1422's
`__lzcnt` form. Upstream's #1551 retracted it and was closed on 2026-10-02
without merging; its algorithm is now upstream's own: 7388bd6fc (in the MSVC
compat header) and 7437f3d9a.
