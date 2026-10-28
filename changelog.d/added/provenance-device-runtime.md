- **Device runtime and driver version in provenance records (#2142).**
  `VmafxDeviceInfo` appends `const char *runtime` (ABI 0.1.14), reporting the
  underlying runtime and driver version per backend rather than placeholder
  values: the host architecture on CPU (`x86_64`, `aarch64`), `cuda driver X.Y runtime X.Y`
  on CUDA, `<platform-name> <driver-version>` on SYCL, `hip driver X.Y runtime X.Y` on HIP,
  and Darwin kernel version or `metal` on Metal. Provenance records copy `info.runtime`
  into `device_runtime` and feature extractor records, falling back to `unknown` only
  when the backend runtime is unavailable.
