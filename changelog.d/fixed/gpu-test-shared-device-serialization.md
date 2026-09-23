- **GPU tests no longer exhaust a shared accelerator by running concurrently.**
  Every Meson test in the `gpu` suite now uses exclusive scheduling, and an
  introspection contract rejects future GPU registrations that omit it.
