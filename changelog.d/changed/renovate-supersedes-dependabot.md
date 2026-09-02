- **Mend Renovate replaces Dependabot as the dependency-update bot (ADR-0363).**
  Adds [`renovate.json`](renovate.json) (self-hosted configuration) and
  [`.github/workflows/renovate.yml`](.github/workflows/renovate.yml) (weekly Monday
  cron at 04:17 UTC, `renovatebot/github-action` SHA-pinned to v46.1.13). Disables
  `.github/dependabot.yml` (renamed to `.disabled`; retained for rollback). Key
  capability over Dependabot: a `customManagers` regex rule tracks
  `FFMPEG_SHA:=n[0-9.]+` in `ffmpeg-patches/test/build-and-run.sh` and
  `FFMPEG_PATCHES_BRANCH` in `scripts/ci/ffmpeg-patches-check.sh` against
  `github-tags/FFmpeg/FFmpeg`, enabling automatic FFmpeg release-tag bump PRs — a
  surface Dependabot cannot reach. GitHub Actions minor+patch pins are grouped into
  a single weekly PR; pre-commit hook revisions are grouped separately. Operator
  setup requires provisioning a `RENOVATE_TOKEN` fine-grained PAT; see
  [`docs/development/dependency-bot.md`](docs/development/dependency-bot.md).
