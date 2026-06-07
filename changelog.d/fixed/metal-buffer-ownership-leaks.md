### Metal backend: fix MTLBuffer retain-count leaks on init failure and external-handle import

Three Objective-C++ ARC ownership bugs in the Metal backend are corrected:

1. `float_ms_ssim_metal.mm` `init_fex_metal`: when any of the 26 bridge-retained
   pyramid/hbuf/partials MTLBuffers could not be allocated, the function jumped to
   `fail_lc` and returned without releasing the buffers already stored in the struct
   fields.  Each such failure leaked between 1 and 25 retained MTLBuffer objects.
   A new `fail_bufs` label releases all partially-allocated buffers before falling
   into `fail_lc`.

2. `float_ms_ssim_metal.mm` `init_fex_metal` (secondary): when `build_pipelines`
   succeeded but `feature_name_dict` allocation failed, the `fail_pso` chain fell
   through to `fail_lc` without releasing the 26 buffers.  The same `fail_bufs`
   label now sits between `fail_pso` and `fail_lc`.

3. `picture_import.mm` `vmaf_metal_state_init_external`: externally-provided device
   and queue handles received an extra `CFRetain` before the `__bridge_retained`
   call, producing an unbalanced +2 retain that `vmaf_metal_state_free`'s single
   `__bridge_transfer` could not fully release.  The redundant `CFRetain` calls and
   now-unused `device_owned_externally` / `queue_owned_externally` flags are removed;
   `__bridge_retained` alone provides the correct +1 that `__bridge_transfer` balances.
