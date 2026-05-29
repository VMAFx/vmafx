# installed version.h header generation
version_h_data = configuration_data()
version_h_data.set('VMAF_API_VERSION_MAJOR', vmaf_api_version_major)
version_h_data.set('VMAF_API_VERSION_MINOR', vmaf_api_version_minor)
version_h_data.set('VMAF_API_VERSION_PATCH', vmaf_api_version_revision)
version_h_target = configure_file(input: 'version.h.in',
                                  output: 'version.h',
                                  configuration: version_h_data)

is_cuda_enabled = get_option('enable_cuda') == true
is_sycl_enabled = get_option('enable_sycl') == true
# ADR-0209 / T7-10: HIP scaffold — install the public header whenever the
# scaffold is opted in so downstream consumers can compile against it.
is_hip_enabled = get_option('enable_hip') == true
# `enable_vulkan` is a feature option (`disabled`/`enabled`/`auto`), not a
# boolean — handle accordingly. Treat both `enabled` and `auto` as "install
# the public header"; otherwise consumers who built with `-Denable_vulkan=
# enabled` then ran `meson install` end up without `libvmaf_vulkan.h` under
# their prefix and downstream pkg-config probes (e.g. FFmpeg's
# `--enable-libvmaf-vulkan` configure check) silently fail. Symptom
# reported by lawrence 2026-04-28: FFmpeg `configure` accepts
# `--enable-libvmaf-vulkan` and reports it as enabled, but only `vmaf_pre`
# and the regular `libvmaf` filter end up built — `libvmaf_vulkan` is
# silently dropped because `check_pkg_config libvmaf_vulkan ...
# libvmaf/libvmaf_vulkan.h vmaf_vulkan_state_init_external` can't find
# the header.
is_vulkan_enabled = get_option('enable_vulkan').enabled() \
                    or get_option('enable_vulkan').auto()

platform_specific_headers = []

if is_cuda_enabled
  platform_specific_headers += 'libvmaf_cuda.h'
endif

if is_sycl_enabled
  platform_specific_headers += 'libvmaf_sycl.h'
endif

if is_vulkan_enabled
  platform_specific_headers += 'libvmaf_vulkan.h'
endif

# Embedded MCP server header (ADR-0209 / T5-2 scaffold). Install
# whenever `enable_mcp=true` so downstream consumers can compile
# against the public surface even before the runtime PR (T5-2b)
# wires the bodies — every entry point returns -ENOSYS until then.
if get_option('enable_mcp')
  platform_specific_headers += 'libvmaf_mcp.h'
endif

if is_hip_enabled
  platform_specific_headers += 'libvmaf_hip.h'
endif

# `enable_metal` is a feature option (`disabled`/`enabled`/`auto`). Install the
# public header whenever Metal is opted in (or auto-resolved on macOS) so that
# downstream FFmpeg `--enable-libvmaf-metal` configure probes can find
# `libvmaf/libvmaf_metal.h` in the install tree. Without this entry the header
# was silently omitted from `meson install` output, breaking the
# `check_pkg_config libvmaf_metal ... libvmaf/libvmaf_metal.h
# vmaf_metal_state_init` configure check. Mirrors the Vulkan guard above —
# same feature-option type, same treat-auto-as-install rationale (ADR-0437).
is_metal_enabled = get_option('enable_metal').enabled() \
                   or get_option('enable_metal').auto()
if is_metal_enabled
  platform_specific_headers += 'libvmaf_metal.h'
endif

if get_option('enable_dnn').enabled() or get_option('enable_dnn').auto()
  platform_specific_headers += 'dnn.h'
endif

# install headers
# macros.h is always installed — it defines VMAF_EXPORT which every public
# header includes, so downstream consumers building against the installed
# tree must be able to find it. ADR-0377.
install_headers(['libvmaf.h',
                'feature.h',
                'macros.h',
                'model.h',
                'picture.h',
                'vmaf_assert.h'],
                version_h_target,
                platform_specific_headers,
                subdir : 'libvmaf')
