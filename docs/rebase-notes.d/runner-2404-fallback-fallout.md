## Three hosted jobs back on ubuntu-24.04 under a dated fallback (2026-10-11)

`ci/runner-2404-fallback-fallout`, [ADR-3251](adr/3251-hosted-runner-label-explicit.md). A rebase
or sync keeps `ubuntu-24.04` on `dev-container-build-work` (`dev-container-build.yml`) and on
`ffmpeg-work` (the `FFmpeg Ubuntu gcc` matrix row) and `ffmpeg-sycl-work`
(`ffmpeg-integration.yml`) for as long as `.config/lint-exceptions.d/hosted-runner-label.toml`
holds their entries (end date 2026-10-25): taking `ubuntu-26.04` on one of them without removing
its entry fails `scripts/ci/check_hosted_runner_labels.py` on a stale entry, and removing an
entry without the label fails it on an undeclared older image. The file exists now and goes with
its last entry. `scripts/ci/lint_exceptions.py` keys the rule `hosted-runner-label` by file and
job (`PART_KEYS`); every other rule keeps one entry per file, so do not add a part key for a
rule whose check does not read one.
