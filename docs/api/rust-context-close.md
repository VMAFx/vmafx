# Rust context close and retry

The safe Rust wrappers treat context teardown as an ownership transition, not
as an infallible destructor. This matters for GPU-backed contexts: libvmaf can
return an error while it still owns streams, modules, and model references.

Both Rust crates expose the same close protocol:

| Crate | Active context | Retry-only error token |
| --- | --- | --- |
| `vmafx` | `Context<'a>` | `ContextCloseError<'a>` |
| `vmafx-sys::safe` | `VmafContext<'a>` | `VmafContextCloseError<'a>` |

Call `close(self)` when teardown errors need to be handled explicitly. Exact
zero is the only success value. Any nonzero result, including an unexpected
positive status, returns the retry-only token and preserves the native context:

```rust
match context.close() {
    Ok(()) => {
        // The native context and all registered-model borrows are released.
    }
    Err(pending) => {
        eprintln!("initial close failed: {}", pending.error());
        match pending.retry() {
            Ok(()) => {
                // The retry completed teardown.
            }
            Err(still_pending) => {
                eprintln!("close retry failed; first error: {}", still_pending.error());
                // The one-retry budget is exhausted. Dropping this token
                // aborts without making a third native close call.
                drop(still_pending);
            }
        }
    }
}
```

After `close` starts, scoring cannot resume. The error token deliberately
exposes only `error()` and the consuming `retry()` method. A failed retry
returns the token again, but the one-retry budget is then exhausted. `error()`
continues to report the first close error; the retry result never replaces it.
Calling `retry()` again does not call native code and returns the same token.

Registered models must remain alive until a close attempt succeeds. The
`'a` lifetime on the retry token preserves that borrow in safe Rust; model
destruction cannot race a teardown-pending native context.

## Drop behavior

Explicit `close` is recommended whenever the application can report or recover
from teardown failures. RAII remains fail-closed:

- Dropping an active context makes a close attempt and one bounded retry.
- Dropping a fresh retry token consumes its one permitted retry. If that close
  fails, the process aborts.
- Dropping a token after an explicit retry failed aborts immediately, without
  issuing a third native close call.

Returning from `Drop` after persistent failure would end the registered-model
borrow while libvmaf still retained native pointers to those models, so the
wrapper fails closed instead.

The low-level raw FFI (`use vmafx_sys::*`) does not provide this ownership
guard. Raw callers must keep the `VmafContext` pointer and every imported
backend/model dependency alive until `vmaf_close` returns exactly zero.

See the [Rust development guide](../development/rust.md) for crate setup and
the [`vmaf_close` C contract](index.md#core-lifecycle-api) for the underlying
native API.
