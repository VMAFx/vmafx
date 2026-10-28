# Errors

Calls in the VMAFx C API report status directly through integer return values
and provide structured error diagnostics through an optional error handle.
This guide describes how to detect failures, inspect error details, handle
resource lifetimes, and translate codes when interacting with compatibility
layers.

## Reporting failure

Every VMAFx API function that can fail returns a `VmafxStatus` code and accepts
a pointer to a `VmafxError *` as its final argument:

- `VMAFX_OK` (0) indicates success.
- `VMAFX_PENDING` (1) indicates that an asynchronous operation is in flight,
  such as an unready score or an unsignalled fence.
- Negative return values indicate errors (for example, `VMAFX_E_INVALID`,
  `VMAFX_E_NOMEM`, or `VMAFX_E_ABI`).

The full list of status codes, integer values, and semantics is defined in
`core/include/vmafx/types.h` and documented in the status table in
[`vmafx/types.h`](types.md#status-codes).

### The error handle

To obtain detailed failure diagnostics, pass the address of a `VmafxError *`
variable as the last parameter of an API call:

```c
VmafxError *err = NULL;
VmafxStatus status = vmafx_context_create(&cfg, &ctx, &err);
```

When the call succeeds (`VMAFX_OK`), `err` remains untouched (`NULL`). If the
call fails, the library allocates an opaque `VmafxError` object and writes its
pointer to `*err`.

The caller owns the allocated error object and must release it with
`vmafx_error_free()`:

```c
if (status != VMAFX_OK) {
    /* Inspect error details */
    vmafx_error_free(err);
}
```

Passing `NULL` to `vmafx_error_free(NULL)` is a safe no-op.

### Passing NULL for the error handle

If you only need the status code and do not require structured error metadata,
pass `NULL` as the last parameter:

```c
VmafxStatus status = vmafx_context_create(&cfg, &ctx, NULL);
```

When `NULL` is passed and a call fails, the library formats the diagnostic
message and writes it to the context's log callback at `VMAFX_LOG_LEVEL_ERROR`.
If the failure occurs before a context exists, or if no log callback is
configured, the message is written to `stderr`. The same happens when the
library cannot allocate the error object itself. No failure is silently
ignored.

## Inspecting an error

Functions for inspecting a `VmafxError` are declared in
`core/include/vmafx/error.h` and documented in
[`vmafx/error.h`](error.md):

- `vmafx_error_status(error)`: Returns the `VmafxStatus` code associated with
  the failure. Returns `VMAFX_E_INVALID` if `error` is `NULL`.
- `vmafx_status_name(status)`: Returns the literal constant name (such as
  `"VMAFX_E_INVALID"`), or `"VMAFX_UNKNOWN_STATUS"` if unrecognized.
- `vmafx_error_message(error)`: Returns a human-readable diagnostic message
  formatted at the failure site. Returns an empty string `""` if `error` is
  `NULL`.
- `vmafx_error_function(error)`: Returns the name of the library function that
  originated the error (for example, `"vmafx_context_create"`). Returns `""` if
  `error` is `NULL`.
- `vmafx_error_subject(error)`: Returns the name of the specific entity that
  caused the failure (such as an invalid parameter name, unsupported option, or
  unrecognized extractor). Returns `""` if no subject is attached or `error` is
  `NULL`.
- `vmafx_error_subject_kind(error)`: Returns a `uint32_t` representing the kind
  of entity named by the subject (a `VmafxSubjectKind` enum value). Returns
  `VMAFX_SUBJECT_NONE` (0) if `error` is `NULL`. The full list of subject
  kinds is documented in the subject table in
  [`vmafx/error.h`](error.md#vmafxsubjectkind).
- `vmafx_error_errno(error)`: Returns the negative errno reported by the
  underlying engine, or `0` if the failure did not originate from the engine.

## The errno map for compatibility callers

The legacy `libvmaf.h` API (`libvmaf.so.3`) wraps `libvmafx.so.1` and returns
negative errno values to preserve compatibility with existing callers.

The compatibility library translates in both directions with tables the
generator writes from `core/api/vmafx.toml`; they are internal, not public
API:

- A `VmafxStatus` becomes its negative errno (for example, `VMAFX_OK` becomes
  `0`, `VMAFX_PENDING` `-EAGAIN`, `VMAFX_E_INVALID` `-EINVAL` and
  `VMAFX_E_NOMEM` `-ENOMEM`). A status the table does not know becomes
  `-EIO`.
- A negative errno from the engine becomes a `VmafxStatus` only for the rows
  the definition marks as reversible; any other errno becomes
  `VMAFX_E_INTERNAL`.

The exact correspondence between status codes and compatibility errnos is
listed in the "libvmaf errno" column of the status table in
[`vmafx/types.h`](types.md#status-codes).

## Complete C example

The following example initializes a context configuration, handles potential
allocation failures, inspects the error structure, and cleans up all allocated
resources.

<!-- compile -->
```c
#include <stdint.h>
#include <stdio.h>
#include <vmafx/vmafx.h>

int main(void)
{
    VmafxContext *ctx = NULL;
    VmafxContextConfig cfg = VMAFX_CONTEXT_CONFIG_INIT;
    VmafxError *err = NULL;

    VmafxStatus status = vmafx_context_create(&cfg, &ctx, &err);
    if (status != VMAFX_OK) {
        if (err != NULL) {
            fprintf(stderr,
                    "Failure in %s: %s (status %s, subject %s, kind %u, "
                    "errno %d)\n",
                    vmafx_error_function(err),
                    vmafx_error_message(err),
                    vmafx_status_name(vmafx_error_status(err)),
                    vmafx_error_subject(err),
                    (unsigned)vmafx_error_subject_kind(err),
                    (int)vmafx_error_errno(err));
            vmafx_error_free(err);
        }
        return 1;
    }

    status = vmafx_context_destroy(ctx, &err);
    if (status != VMAFX_OK) {
        if (err != NULL) {
            fprintf(stderr, "Destroy failed in %s: %s\n",
                    vmafx_error_function(err),
                    vmafx_error_message(err));
            vmafx_error_free(err);
        }
        return 1;
    }

    return 0;
}
```

## See also

- [Versioning and the ABI](versioning.md) for ABI stability and struct sizing
- [Status codes](types.md#status-codes) in `vmafx/types.h`
- [Error functions and subjects](error.md) in `vmafx/error.h`
- [VMAFx API preview](index.md) for context lifecycle and scoring operations
