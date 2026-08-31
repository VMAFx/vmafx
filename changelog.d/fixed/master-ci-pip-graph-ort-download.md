- Restored automatic pip dependency-graph generation by keeping the tool-only
  root Python requirement at 3.14-series granularity and preventing Renovate
  from raising it past Dependabot's bundled interpreter patch.
- Made the Linux all-backends build download ONNX Runtime into a retry-managed
  file and verify its release SHA-256 before privileged extraction, so transient
  TLS failures cannot feed a partial or corrupt archive to `tar`.
