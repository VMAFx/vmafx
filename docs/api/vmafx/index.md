# VMAFx C API (preview)

The VMAFx API is the successor of the `libvmaf.h` API: headers under
`vmafx/`, functions named `vmafx_*`, and a model built around contexts,
devices, frames with fences, window scores and a provenance record behind
every score ([ADR-1852](../../adr/1852-vmafx-api-redesign.md)). It is an RC4
preview: this build ships the first slice, and its ABI (version 0.1) may
still change until `v1.0.0` freezes it. The existing
[`libvmaf.h` API](../index.md) keeps working and is now implemented on top of
this one for the functions the slice covers.

Every declaration, the Python binding and the
[reference page](reference.md) are generated from one definition,
`core/api/vmafx.toml`; the [API generation guide](../../development/api-generation.md)
explains how to change it.

## What the slice contains

| Call | Does |
| --- | --- |
| `vmafx_context_create` / `vmafx_context_destroy` | Create and destroy a scoring session. A destroy that fails leaves the context valid for a retry. |
| `vmafx_version_string`, `vmafx_abi_version` | Build version (git describe) and the ABI version of the loaded library. |
| `vmafx_context_provenance` | ABI version, build version, active backend and number of registered extractors. |
| `vmafx_context_extractor_info` | Name and backend of each registered extractor. |
| `vmafx_feature_score` | One feature's score at a frame, with the extractor and backend that produced it. |
| `vmafx_error_*` | Status, message, the named subject and the engine's errno of a failure. |
| `vmafx_context_libvmaf_handle`, `vmafx_context_from_libvmaf` (`vmafx/libvmaf_bridge.h`) | Use the `libvmaf.h` calls the slice does not cover yet on the same session. |

Frames still go in through `libvmaf.h` (`vmaf_use_feature`,
`vmaf_read_pictures`) on the bridged handle until the frame API lands.

## A complete program

```c
#include <stdio.h>

#include <libvmaf/libvmaf.h>
#include <vmafx/libvmaf_bridge.h>
#include <vmafx/vmafx.h>

static int report(VmafxStatus status, VmafxError *error)
{
    fprintf(stderr, "%s: %s [%s]\n", vmafx_status_name(status), vmafx_error_message(error),
            vmafx_error_subject(error));
    vmafx_error_free(error);
    return 1;
}

int main(void)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.n_threads = 4;
    VmafxContext *context = NULL;
    VmafxError *error = NULL;
    VmafxStatus status = vmafx_context_create(&config, &context, &error);
    if (status != VMAFX_OK)
        return report(status, error);

    VmafContext *vmaf = vmafx_context_libvmaf_handle(context);
    vmaf_use_feature(vmaf, "psnr", NULL);
    /* ... vmaf_read_pictures(vmaf, &ref, &dist, i) for each frame, then
     * vmaf_read_pictures(vmaf, NULL, NULL, 0) to flush ... */

    VmafxScore score = VMAFX_SCORE_INIT;
    status = vmafx_feature_score(context, "psnr_y", 0, &score, &error);
    if (status != VMAFX_OK)
        return report(status, error);
    printf("%s[%llu] = %.6f from %s\n", score.feature, (unsigned long long)score.index,
           score.value, score.extractor ? score.extractor : "(imported)");

    status = vmafx_context_destroy(context, &error);
    return status == VMAFX_OK ? 0 : report(status, error);
}
```

Build against an installed tree with `pkg-config --cflags --libs libvmaf`;
in this preview the slice lives in the same library as `libvmaf.h`. RC4 splits
it as ADR-1852 decides: `libvmafx.so.1` (pkg-config `libvmafx`) with
`libvmaf.so.3` as a thin compatibility library on top.

## Rules every call follows

- **Status codes** are stable on every platform: `VMAFX_OK` (0), `VMAFX_PENDING`
  (1, the frame is not final yet) and negative errors (`VMAFX_E_INVALID`,
  `VMAFX_E_NOTFOUND`, `VMAFX_E_RANGE`, ...; see the [reference](reference.md)).
- **Errors name what failed.** Pass a `VmafxError **` as the last argument to
  receive the status, a message, the subject (the parameter, feature or
  extractor) and the engine's errno; free it with `vmafx_error_free()`. Pass
  `NULL` to get the status only; the message then goes to the log at `ERROR`.
- **Structs carry their size.** Initialise every struct with its `*_INIT`
  macro (it sets `struct_size`). A newer library reads only the fields an
  older caller compiled in; an output struct receives what its `struct_size`
  holds, and `struct_size` is set to the bytes written.
- **Strings** the library returns live as long as the object they came from;
  `vmafx_version_string()` for the whole process.

## Python

The generated binding uses only the standard library (`ctypes`) and loads the
library you name; there is no search fallback:

```python
from vmafx import Library

lib = Library("/usr/local/lib/libvmaf.so.3")   # or set VMAFX_LIBRARY
with lib.context() as context:
    print(lib.version_string(), context.provenance())
```

A failed call raises `vmafx.VmafxError` with `status`, `message`, `subject`
and `errno`; `VMAFX_PENDING` raises its subclass `VmafxPending`. The module
checks its struct layouts against the definition when it is imported. The
package sits in `bindings/python/vmafx/` in the source tree.

## libvmaf functions implemented on this API

`vmaf_init`, `vmaf_close`, `vmaf_version` and `vmaf_feature_score_at_index`
are generated shims on the calls above. Their behaviour is unchanged: the
same `NULL` checks, `*vmaf` cleared on failure, the engine's own negative
errno on failure, and a failed `vmaf_close()` still leaves the context valid
for a retry. Every other `libvmaf.h` function is unchanged.
