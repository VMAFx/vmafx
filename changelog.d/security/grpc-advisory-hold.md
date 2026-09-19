- **`google.golang.org/grpc` stays on the patched 1.83.x line.** A dependency
  bump had raised it to 1.84.0, which carries GHSA-2v4p-qf9q-27wj: a gRPC xDS
  server panics on a request that arrives with neither an `:authority` nor a
  `Host` header, which is a denial of service. The only fix for the 1.84 line is
  an unreleased development pseudo-version, so there is no stable release past
  1.84.0 to move to. A Renovate rule holds the package below 1.84.0 until
  grpc-go tags one.
