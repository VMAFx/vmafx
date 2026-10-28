- **Errors and versioning guides for the VMAFx C API (issue 2436).** Two new
  pages explain how a call reports failure (status code, the optional
  `VmafxError`, its subject and errno, the message that reaches the log when
  no error object is passed) and how the ABI evolves (`since`, symbol version
  nodes, `struct_size`, deprecation and the runtime version check)
  ([Errors](docs/api/vmafx/errors.md),
  [Versioning and the ABI](docs/api/vmafx/versioning.md)).
