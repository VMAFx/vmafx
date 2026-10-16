- **CI lints OpenAPI specification with Spectral
  ([ADR-3331](docs/adr/3331-ci-spectral-openapi-lint.md)).** The new
  `Spectral OpenAPI Lint` job of `lint-and-format.yml` runs
  the checksum-verified Spectral release binary against `api/openapi/vmafx-server-v1.yaml` pinned by
  `SPECTRAL_VERSION` and `SPECTRAL_SHA256` in `build-config.env`.
