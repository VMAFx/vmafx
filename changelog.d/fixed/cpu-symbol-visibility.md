Apply `-fvisibility=hidden` to `libvmaf_cpu_static_lib` in `src/meson.build`
so the internal CPU-dispatch helpers (`vmaf_get_cpu_flags`, `vmaf_init_cpu`,
`vmaf_set_cpu_flags_mask`, `vmaf_get_cpu_flags_x86`) are no longer exported
as T symbols in the final `libvmaf.so`. Previously they leaked into the ABI
despite having no public header declaration or `VMAF_EXPORT` annotation.
