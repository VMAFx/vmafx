- **CI lints the protobuf contract and refuses wire breaks
  ([ADR-3329](docs/adr/3329-ci-buf-lint-breaking.md)).** The new `Buf Lint + Breaking` job of
  `lint-and-format.yml` runs `buf lint` and `buf breaking` (`WIRE_JSON`) on `proto/` against the
  pull request's base commit, with the `BUF_VERSION` that `build-config.env` pins.
