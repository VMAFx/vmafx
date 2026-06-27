- `pkg/gpu` GPU detection: bound each vendor probe with a real timeout.
  `runProbe` set `cmd.WaitDelay` but never gave the command a context, so
  `WaitDelay` alone did not cap a probe that produces no output — a wedged
  `nvidia-smi` / driver-blocked `rocm-smi` could stall `gpu.Detect()` (and
  node startup) indefinitely. Now uses `exec.CommandContext` with
  `context.WithTimeout(probeTimeout)` plus a short `WaitDelay` grace, matching
  the `pkg/libvmaf` subprocess pattern; the timeout fires and `Detect()`
  falls back to CPU.
- `pkg/ai` `Registry.Infer`: add a `context.Context` first parameter and run
  `vmafx-ort-runner` via `exec.CommandContext` (with an `inferTimeout` upper
  bound when the caller supplies no deadline, mirroring `pkg/encoder` /
  `pkg/bisect`). A cancelled job or elapsed deadline now tears down the ORT
  subprocess instead of hanging the worker.
- Rust `vmafx-sys` (`safe.rs`): `VmafContext::read_pictures` now **consumes**
  both `VmafPicture` values by move instead of borrowing `&mut`. Ownership of
  the plane buffers transfers to libvmaf, so the caller can no longer call the
  public `unref_picture` on a transferred picture and double-free it — the
  type system rejects use-after-move. The error path does **not** manually
  unref (the libvmaf contract takes ownership for the call's duration; a second
  unref is a use-after-free against a CUDA-enabled libvmaf), matching the
  higher-level `vmafx` crate's `Context::read_pictures` contract (PR #1056,
  round-3 R3-2). The `vmafx` crate's separate `read_pictures` double-free was
  already fixed by PR #1056 and is not re-touched here.
- `core/src/meson.build`: correct the stale comment claiming
  `enable_rust_features` defaults to `true`; `core/meson_options.txt` sets
  `value: false`. The comment now matches the single source of truth.
