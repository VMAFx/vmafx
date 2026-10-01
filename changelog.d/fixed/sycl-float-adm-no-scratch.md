- **`float_adm_sycl` works on Arc A-series GPUs under the xe driver.** Its two
  contrast-masking kernels indexed a small private array with the band number,
  which the Intel graphics compiler keeps in scratch memory, and kernels that
  use scratch memory return wrong values on these GPUs under xe (ADR-1395). On
  an Arc A380 `--backend sycl --feature float_adm` returned NaN and stopped
  with `problem reading pictures`. The kernels now pick the band by value.
  The twin is within 2.5e-6 of the CPU on the Netflix 576x324 pair, 3.5e-7 on
  1080p checkerboards and 1.3e-5 at 3840x2160, as on other devices. This was
  the last entry of the scratch ratchet list: no libvmaf SYCL kernel uses
  scratch memory any more (110 kernels audited on the A380), the list stays
  empty, and the start-up warning on an affected device now says that no
  extractor's scores are affected.
