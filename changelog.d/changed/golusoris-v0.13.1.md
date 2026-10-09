- **The Go services build on golusoris v0.13.1 and its core v0.10.1**
  (from v0.12.0 and v0.9.2). `VMAFX_HTTP_LIMITS_BODY=0` no longer removes the
  request-body cap of `vmafx-server`, `vmafx-controller` and `vmafx-node`: `0`
  now keeps the 10 MiB default, and the new `VMAFX_HTTP_LIMITS_UNLIMITED=true`
  removes the cap. The controller now reports a failed registration of its
  lease-sweep job at start instead of panicking. The variables are listed in
  `docs/usage/env-vars.md`.
