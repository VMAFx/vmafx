# Status codes in Python, Go and Rust

VMAFx extends VMAF (Video Multi-Method Assessment Fusion). Its stable status
codes come from [`core/api/vmafx.toml`](../../../core/api/vmafx.toml).
Zero means success, a positive code carries information, and a negative code
means failure. `PENDING` means a score is not final yet; keep waiting or poll
again according to the operation's contract.

Import Python's `Status` from `vmafx`, Go's package from
`github.com/VMAFx/vmafx/pkg/vmafx`, or Rust's `vmafx::Status`.
The Go status package uses only the standard library and requires no cgo or
installed native library. Rust's status table adds no dependency; building the
complete Rust binding crate still requires its existing native dependencies.

## Shared table

The names below refer to the same integer in all three languages. The errno
column gives the compatibility errno name, whose numeric value depends on the
platform. It is separate from the stable status integer.

| Value | Python | Go | Rust | errno |
| --- | --- | --- | --- | --- |
| 0 | `Status.OK` | `StatusOK` | `Status::Ok` | `0` |
| 1 | `Status.PENDING` | `StatusPending` | `Status::Pending` | `EAGAIN` |
| -1 | `Status.E_INVALID` | `StatusEInvalid` | `Status::EInvalid` | `EINVAL` |
| -2 | `Status.E_NOMEM` | `StatusENoMem` | `Status::ENoMem` | `ENOMEM` |
| -3 | `Status.E_NOTSUP` | `StatusENotSup` | `Status::ENotSup` | `ENOTSUP` |
| -4 | `Status.E_NOTFOUND` | `StatusENotFound` | `Status::ENotFound` | `ENOENT` |
| -5 | `Status.E_BUSY` | `StatusEBusy` | `Status::EBusy` | `EBUSY` |
| -6 | `Status.E_DEVICE` | `StatusEDevice` | `Status::EDevice` | `EIO` |
| -7 | `Status.E_IO` | `StatusEIO` | `Status::EIo` | `EIO` |
| -8 | `Status.E_RANGE` | `StatusERange` | `Status::ERange` | `ERANGE` |
| -9 | `Status.E_INTERNAL` | `StatusEInternal` | `Status::EInternal` | `EIO` |
| -10 | `Status.E_TIMEOUT` | `StatusETimeout` | `Status::ETimeout` | `ETIMEDOUT` |
| -11 | `Status.E_ABI` | `StatusEABI` | `Status::EAbi` | `EINVAL` |
| -12 | `Status.E_MISMATCH` | `StatusEMismatch` | `Status::EMismatch` | `EBADMSG` |

See [Errors](errors.md) for the error object's additional diagnostic context.

## Go

`Status` has underlying type `int32`. `String()` returns the canonical TOML
name, for example `VMAFX_E_INVALID`. `Errno()` returns the corresponding
`syscall.Errno`; `StatusPending.Errno()` is `syscall.EAGAIN`.
Use `Err()` to test for failure: it returns `nil` for every nonnegative code,
including `StatusOK` and `StatusPending`.

```go
status := vmafx.StatusEInvalid
if err := status.Err(); err != nil {
    if errors.Is(err, os.ErrInvalid) {
        // Correct the input before trying again.
    }
}
```

Each known failure returns its own sentinel:
`ErrInvalid`, `ErrNoMem`, `ErrNotSup`, `ErrNotFound`, `ErrBusy`, `ErrDevice`,
`ErrIO`, `ErrRange`, `ErrInternal`, `ErrTimeout`, `ErrABI`, `ErrMismatch`.
`ErrInvalid` wraps `os.ErrInvalid`, `ErrNotFound` wraps `os.ErrNotExist`, and
`ErrNotSup` wraps `errors.ErrUnsupported`. The other sentinels are distinct
errors even when their errno names match.

Unknown codes print as `Status(n)` and return zero from `Errno()`. An unknown
negative code returns an error containing its integer from `Err()`; an unknown
nonnegative code returns `nil`.

## Rust

`Status` is a `#[repr(i32)]`, `#[non_exhaustive]` enum. Convert raw integers
with `Status::from_raw`; unknown values return `None`.
`as_str()` returns the canonical TOML name. `errno()` returns the symbolic
errno name as `&'static str`, including `"0"` for success and `"EAGAIN"` for
`Pending`. It does not assume one platform's numeric errno values.

```rust
use vmafx::Status;

let raw = -1;
if let Some(status) = Status::from_raw(raw) {
    assert_eq!(status, Status::EInvalid);
    assert_eq!(status.as_str(), "VMAFX_E_INVALID");
    assert_eq!(status.errno(), "EINVAL");
}
assert_eq!(Status::from_raw(i32::MIN), None);
```

Matches over `Status` need a wildcard arm because later definitions can add
variants. This table does not change the existing Rust `Error` conversion of
legacy libvmaf return codes or the Go `pkg/libvmaf` package.

## Regenerate and verify

Edit the definition, then regenerate from the repository root:

```sh
python3 scripts/codegen/vmafx-api.py --write
python3 scripts/codegen/vmafx-api.py --check
python3 -m pytest -q scripts/codegen/tests/test_vmafx_api_status_bindings.py
```

The generator writes the existing Python enum in
`bindings/python/vmafx/_api.py`, Go's `pkg/vmafx/status_gen.go`, and Rust's
`bindings/rust/vmafx/src/status_gen.rs`. Never edit those outputs by hand.
`--check` exits 1 on drift. The parity test compares all three rendered tables
with the loaded definition and plants a changed value to verify drift detection.
