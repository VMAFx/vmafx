## Device runtime and driver version in provenance records (2026-10-27)

`feat/provenance-device-runtime`, issue #2142.

Additive public ABI 0.1.14 extension adding `const char *runtime` to `struct VmafxDeviceInfo`
and populating `device_runtime` in provenance records across CPU and GPU backends.
No upstream-mirrored files are modified; all changes are contained within fork-added
`core/src/vmafx/`, `core/src/{cuda,hip,sycl}/`, `core/api/vmafx.toml`, and generated API bindings.
