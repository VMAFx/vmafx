<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1210: The SYCL integer-ADM contrast-masking kernel mirrors its near edge

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: sycl, correctness, feature-extractor, adm

## Context

`test_sycl_adm_parity` was failing on the Arc A380 — on the shipped default
fixture, not a new one:

```text
adm model-opt parity FAIL (integer_adm_scale3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02):
cpu=0.58175555 sycl=0.58191226 delta=1.57e-04 tol=1.00e-04
```

The CPU contrast-masking neighbourhood rule is asymmetric
(`core/src/feature/integer_adm.c:1009-1012`):

```c
i_m1 = (i == 0)     ? 1     : i - 1;   /* near edge MIRRORS to index 1   */
i_p1 = (i == h - 1) ? h - 1 : i + 1;   /* far  edge CLAMPS to last index */
```

The SYCL twin clamped **both** edges:

```c
if (ny < 0) ny = 0;              /* wrong: reads the centre row twice   */
if (ny >= e_h) ny = e_h - 1;     /* right                               */
```

Clamping the near edge to 0 reads row/column 0 twice and drops the mirrored
sample entirely. It only diverges once a scale's ADM border crop
`(int)(dim * 0.1 - 0.5)` collapses to 0 — band dimensions `<= 14` — because
only then are row 0 and column 0 inside the CM summation region. At the
shipped 256x144 fixture, scale 3 is a 16x9 band, which is exactly that regime.

This is the same defect family as [ADR-1204](1204-adm-cm-edge-clamp-gpu-twins.md)
(which fixed the *far* edge in the float-ADM twins) and the same fix that
[ADR-1167](1167-adm-cm-row-level-rounding.md) / PR #1224 applied to the integer
GPU kernels. CUDA (`adm_cm.cu`) and HIP (`adm_cm.hip`) both carry it via an
`offset_i[0] = 1` table; Metal carries it via `iadm_clampx`, whose `x = -x`
branch mirrors correctly. **SYCL was the only twin that never received it.**

## Decision

We will mirror the near edge to index 1 in the SYCL integer-ADM CM kernel,
matching the CPU closed form and the three twins that already do.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Mirror the near edge to index 1 (chosen) | Two-character change; matches the CPU reference and all three other twins exactly | — | — |
| Adopt CUDA's `offset_i` / `offset_j` table form | Structurally identical to the twin that has the ADR-1167 fix | A larger rewrite of a kernel whose only defect is two indices; the SYCL loop shape is different by design | Rejected — bigger diff, same result |
| Relax `test_sycl_adm_parity`'s 1e-4 tolerance | No kernel change | The divergence is a discrete indexing error, not accumulation noise; loosening the gate would hide it | Rejected |

## Consequences

- **Positive**: `test_sycl_adm_parity` passes (6/6, was failing). The whole
  shipped SYCL parity suite goes 17/1 → **18/0** on the Arc A380, and the
  large-fixture suite is 16/0.
- **Negative**: `integer_adm` scores from the SYCL backend move at resolutions
  whose coarse bands are `<= 14` px — towards the CPU reference, which is the
  point. No fork-added snapshot covers SYCL integer ADM at those sizes.
- **Neutral / follow-ups**: the reason this survived is that the failure is
  hardware-gated — the test skips cleanly with no SYCL device, which is every
  hosted runner, so only a workstation run with an Intel GPU sees it. The
  `SYCL Parity (Arc A380)` lane does run on real hardware but builds
  `--buildtype=release`; this was found in a debug build, so the two
  configurations should be reconciled.

## References

- CPU reference: `core/src/feature/integer_adm.c:1009-1012`.
- Twins that already had the fix: `core/src/feature/cuda/integer_adm/adm_cm.cu`
  (`offset_i[0] = 1`), `core/src/feature/hip/integer_adm/adm_cm.hip`,
  `core/src/feature/metal/integer_adm.metal` (`iadm_clampx`).
- [ADR-1167](1167-adm-cm-row-level-rounding.md) / PR #1224 — the original
  integer-ADM GPU border-indexing fix that SYCL missed.
- [ADR-1204](1204-adm-cm-edge-clamp-gpu-twins.md) — the far-edge half of the
  same family, in the float-ADM twins.
- Reproducer: `meson test -C build --suite sycl test_sycl_adm_parity` on a host
  with an Intel GPU.
- Source: `req` — user direction to fix bugs found by the parity sweep.
