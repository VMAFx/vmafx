# fix(core): emit -ENOSYS stubs for libvmaf_hip.h / libvmaf_metal.h when backend is OFF

Both `core/include/libvmaf/libvmaf_hip.h` and
`core/include/libvmaf/libvmaf_metal.h` document the contract that every
public entry point returns `-ENOSYS` when libvmaf is built without the
relevant backend. The real bodies in `core/src/hip/common.c`,
`core/src/metal/common.mm`, `core/src/metal/picture_import.mm`, and the
`vmaf_hip_import_state` / `vmaf_metal_import_state` /
`vmaf_metal_read_imported_pictures` definitions inside `core/src/libvmaf.c`
all sit behind `#ifdef HAVE_HIP` / `#ifdef HAVE_METAL`, so a default
`-Denable_hip=false -Denable_metal=disabled` build emitted none of those
symbols into `libvmaf.so` and any downstream link that referenced them
failed.

Fix: add `core/src/hip/stubs.c` + `core/src/metal/stubs.c` that mirror the
canonical `core/src/dnn/dnn_api.c` `VMAF_HAVE_DNN` stub pattern and wire
each TU into `libvmaf_feature_static_lib` via `hip_sources` /
`metal_sources` only when the backend is disabled. The stubs return
`-ENOSYS`, set out-params to NULL on the pointer-returning entry points,
and `vmaf_hip_available()` / `vmaf_metal_available()` correctly return 0.
