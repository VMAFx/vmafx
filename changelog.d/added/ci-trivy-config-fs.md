- **CI scans Dockerfiles and filesystem dependencies with Trivy
  ([ADR-3330](docs/adr/3330-ci-trivy-config-fs.md)).** The new `Trivy Config + FS` job of
  `lint-and-format.yml` runs `trivy config` (IaC Dockerfile checks, `.trivyignore.yaml`) and
  `trivy fs` (vulnerability and secret scanning) using the `TRIVY_VERSION` release binary
  pinned in `build-config.env`.
