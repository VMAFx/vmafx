- **SYCL: no kernel uses scratch memory on Xe2 (Arc B580, Arc Pro B60).** On an Arc B580
  the term kernel of `float_adm_sycl` spilled 128 bytes, and
  `test_sycl_kernel_scratch` failed; its scores were still exact there, but a
  kernel in scratch memory returns wrong values on an Arc A-series GPU under
  the xe driver. The kernel now takes the 256-entry register file with the
  sub-group size left to the compiler, and spills on no target of the default
  ahead-of-time list. `test_sycl_float_adm_math` no longer fails at random on
  the B580: its probe ran three dependent kernels on an out-of-order queue.
  With both fixes the whole SYCL device suite and the parity gate pass on the
  B580 and the Arc Pro B60 (ADR-1501, `T-SYCL-FLOAT-ADM-TERMS-XE2-SPILL-2026-10-03`,
  `T-SYCL-FLOAT-ADM-PROBE-OUT-OF-ORDER-QUEUE-2026-10-03`).
