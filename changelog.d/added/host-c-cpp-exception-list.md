- **The ADR-2478 host C exception list exists
  ([ADR-2478](docs/adr/2478-rust-core-migration.md)).** `.config/lint-exceptions.d/host-c-cpp.toml`
  names the host C files that stay C on the way to 3.0, each with owner, re-evaluation
  trigger and category; its first entries are the ten pelorus interop mirror files.
