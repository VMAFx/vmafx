---
paths:
  - core/src/feature/adm_tools.c
  - core/src/feature/adm_tools.h
invariant: ADM contrast-masking edge policy is asymmetric across CPU and GPU twins.
area: adm
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# ADM Contrast-Masking Edge Policy Asymmetry

## ADM contrast-masking edge policy is asymmetric (ADR-1204)

`adm_cm_thresh3x3_s` in `adm_tools.c` is CPU reference for 3x3
contrast-masking neighbourhood, and its edge handling is deliberately **not**
symmetric:

```c
i_m1 = (i == 0)     ? 1     : i - 1;   /* near edge MIRRORS to index 1   */
i_p1 = (i == h - 1) ? h - 1 : i + 1;   /* far  edge CLAMPS to last index */
```

Every GPU twin (`cuda/float_adm/`, `hip/float_adm/`, `metal/float_adm.metal`,
`sycl/float_adm_sycl.cpp`) must reproduce **both** halves. symmetric mirror
(`2 * half_w - x - 2`) reads index `w - 2` where CPU reads `w - 1`. It is
tempting because it looks uniform. It survives casual testing because
two only differ when ADM border crop `(int)(dim * ADM_BORDER_FACTOR - 0.5)`
collapses to 0 — band dimensions `<= 14` — since only zero crop pulls
first and last row and column into summation region.

If upstream rewrites `ADM_CM_THRESH_S_*` macro family, re-derive twins
from closed form above, not from macros. `adm_tools.h` no longer carries
those macros (nothing expanded them since ADR-1141; 141 lint findings in
dead text): upstream hunk on them = conflict by design, port into
`adm_cm_thresh3x3_s()` + `adm_cm_thresh()` / `i4_adm_cm_thresh()`, never
re-add the macros.
