- **Withdrawn 1.0.0 candidates and tester releases scheduled for deletion after 0.1.0 ([ADR-3398](docs/adr/3398-delete-withdrawn-rc-releases.md)).**
  Once `v0.1.0` is published and signed, GitHub releases and tags `v1.0.0-rc.1`..`rc.3`
  and the six `tester-*` prereleases (`tester-20261004-4d3792b3`, `tester-20261004-860050c3`,
  `tester-20261005-9bc68a10`, `tester-20261007-40e87b15`, `tester-windows-20261004-2889f963`,
  `tester-windows-20261007-40e87b15`), as well as GHCR package versions tagged
  `v1.0.0-rc.*` across `vmafx`, `vmafx-server`, `vmafx-operator`, `vmafx-controller`,
  and `vmafx-node`, will be deleted from the forge. Retraction blocks in `go.mod`,
  fixture tags, and the `v1.0.0-rc.4` candidate restart are preserved.
  Supersedes Decision 3 of [ADR-3292](docs/adr/3292-release-line-restart-0x.md).
