# FFmpeg patch automation

The FFmpeg integration follows stable upstream release tags. The root
`build-config.env` owns `FFMPEG_REMOTE` and `FFMPEG_TAG`; the patch tooling reads
both values from that file. Development branches, snapshots and prerelease tags
are outside this release channel.

## Check a change locally

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --check \
  --output-dir .workingdir2/cache/ffmpeg-patch-stack/manual-check
```

The check fetches the configured release and replays the complete patch series
in order. It fails on a replay conflict or when patches need a canonical refresh.
It leaves the tracked patch files and release setting unchanged. Inspect the
command output and the diagnostics under the selected output directory when it
fails.

The `ffmpeg-patches-apply-check` hook runs `--refresh` against the configured
release at both pre-commit and pre-push. It regenerates canonical patches after
a successful replay; the hook framework stops the commit or push if tracked
files changed, so review and stage those changes before retrying. Its inputs
include the patch directory, CI helpers, the shared build
configuration, public headers, Meson build/options files and the patch workflow.
An ordinary documentation edit does not select the hook. The existing hook
installation procedure is described in
[Automated rule enforcement](automated-rule-enforcement.md).

## Refresh the configured release

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --refresh \
  --output-dir .workingdir2/cache/ffmpeg-patch-stack/manual-refresh
```

The refresh regenerates the series only after every patch replays successfully.
Review the resulting diff and diagnostics, then run the check again before
committing. A conflict requires a code change and another complete replay; a
failed refresh is not a valid replacement series. If filesystem failure or a
second interruption also prevents rollback, the tool preserves adjacent
`.ffmpeg-refresh-<filename>-original-*` backups for recovery. Keep these until
the original files have been restored; the failure receipt identifies failed
replacements when recovery can finish reporting.

## Continuous checks and release discovery

The **FFmpeg Patch Stack** check registers on every ready pull request to
`master` and every push to `master`. Script and workflow contract tests run on
each of these events. The ADR-1140 impact planner selects the expensive fetch
and replay for core, patch, workflow or CI-authority changes. Unknown inputs
take the full validation path. Documentation-only edits finish after the
contracts without fetching FFmpeg.

Pull requests and manual workflow dispatches check the configured release. They
do not discover or switch to a newer upstream release. The broad CI pre-commit
job skips this one hook because the independent required check owns its replay
and impact routing; local pre-commit and pre-push runs keep it enabled.

The scheduled updater owns upstream discovery. Renovate no longer opens
separate FFmpeg pin-only updates that omit the series rebase.

At **05:43 UTC daily**, the separate **FFmpeg Release Refresh** job runs:

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --refresh --latest \
  --output-dir "$RUNNER_TEMP/ffmpeg-patch-refresh"
```

`--latest` is accepted only with `--refresh`. It discovers the highest stable
`nX.Y` or `nX.Y.Z` upstream release tag, replays the complete series and updates
the shared tag only after successful refresh. Development, RC and snapshot
refs are excluded.

The scheduled job uploads a `ffmpeg-patch-refresh-<run-id>-<attempt>` artifact,
retained for 14 days. It contains the tool's diagnostics, the command log,
`source-revision.txt`, `worktree-status.txt` and `proposed-refresh.patch`. The
diff includes tracked generated configuration mirrors as well as the patch
series. Review it against the recorded source revision before applying it to
an isolated branch and running the configured-release check.

A replay conflict leaves the scheduled job failed and retains its diagnostics.
The workflow has read-only repository permissions and creates no pull request,
merge or release tag. The scheduled result is a proposal for maintainer review.

## Test the automation contract

```bash
python3 -m unittest discover -s scripts/ci -p 'test_ffmpeg_patch*.py' -v
```

These tests cover replay behavior, hook triggers, PR-versus-schedule separation,
impact routing and propagation of command failures through diagnostic capture.
