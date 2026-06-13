- **Metal backend runtime** (ADR-0420 / T8-1b). The Metal compute
  backend's runtime contract now works on Apple Silicon. Replaces the
  T8-1 scaffold's `-ENOSYS` stubs with Objective-C++ TUs (`common.mm`,
  `picture_metal.mm`, `kernel_template.mm`) that drive `Metal.framework`
  directly: `vmaf_metal_state_init` allocates a real `MTLDevice` +
  `MTLCommandQueue`, `vmaf_metal_picture_alloc` returns a
  shared-storage `MTLBuffer` (zero-copy unified memory on
  Apple-Family-7+), and the kernel-template lifecycle helpers create
  the per-consumer queue + event pair via `[id<MTLDevice>
  newSharedEvent]`. Apple-Family-7 gate; Intel Macs and non-Apple
  hosts surface as `-ENODEV`. Two internal accessors added to
  `metal/common.h` (`vmaf_metal_context_{device,queue}_handle`)
  keep consumer TUs pure-C. First real kernel (`motion_v2_metal`)
  arrives in T8-1c — tracked in issue #763.
