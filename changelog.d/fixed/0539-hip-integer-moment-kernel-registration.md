# HIP: register real `integer_moment` HSACO blob (ADR-0539)

Closes the last unresolved-symbol gap in the `enable_hipcc=true` HIP
build.  The `integer_moment_hip` host TU referenced
`integer_moment_score_hsaco` but no meson entry emitted that symbol —
the existing `moment_score` key resolved to
`hip/float_moment/moment_score.hip` (the float twin), and no weak stub
covered it.

This PR adds a new `integer_moment_score` entry to
`hip_kernel_sources` pointing at the existing real kernel source
`hip/integer_moment/moment_score.hip` (the file was already on disk;
it just wasn't being compiled).  After this change all three of
`psnr_hip` / `psnr_hvs_hip` / `integer_moment_hip` resolve via real
HSACO blobs — no weak stubs for any of them.

End-to-end verification on the Netflix `src01_hrc00 ↔ src01_hrc01`
576×324 pair (HIP vs CPU):

```
psnr_y mean        : HIP=30.755064 CPU=30.755064 delta=0.000000
psnr_cb mean       : HIP=38.449441 CPU=38.449441 delta=0.000000
psnr_cr mean       : HIP=40.991910 CPU=40.991910 delta=0.000000
psnr_hvs mean      : HIP=31.330446 CPU=31.330446 delta=0.000000
psnr_hvs_y mean    : HIP=30.578766 CPU=30.578766 delta=0.000000
psnr_hvs_cb mean   : HIP=37.258498 CPU=37.258498 delta=0.000000
psnr_hvs_cr mean   : HIP=38.200260 CPU=38.200260 delta=0.000000
float_moment_ref1st mean : HIP=59.788567 CPU=59.788567 delta=0.000000
float_moment_dis1st mean : HIP=61.332007 CPU=61.332007 delta=0.000000
float_moment_ref2nd mean : HIP=4696.668388 CPU=4696.668388 delta=0.000000
float_moment_dis2nd mean : HIP=4798.659574 CPU=4798.659574 delta=0.000000
```

All within places=4 (in fact bit-exact: delta=0.000000).

### ADRs

- [ADR-0539](../docs/adr/0539-hip-psnr-moment-kernels-real.md) — this change.
- [ADR-0533](../docs/adr/0533-hip-all-extractors-registration-sweep.md) — registered the HIP integer_moment extractor in the dispatch table.
- [ADR-0536](../docs/adr/0536-hip-weak-hsaco-stubs.md) — weak stubs for the four ADM kernels that still reference CUDA helper macros (out of scope here).
