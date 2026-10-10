# FFmpeg patch automation

The FFmpeg integration follows stable upstream release tags. The root
`build-config.env` owns `FFMPEG_REMOTE`, `FFMPEG_TAG` and `FFMPEG_COMMIT`
(the commit the tag names); the patch tooling reads all three from that
file. Container ARG defaults are generated mirrors, and publication uses
those defaults without a separate release override.
Development branches, snapshots and prerelease tags are outside this channel.

A change to a libvmaf public surface that affects the integration updates the
patch series in the same pull request (hard rule 11 in
[agent-hard-rules.md](agent-hard-rules.md)). The commands below are how to
check and refresh it.

## Shared FFmpeg fix series

Fixes to FFmpeg itself are not kept in `ffmpeg-patches/`. They live in
[VMAFx/ffmpeg-patches](https://github.com/VMAFx/ffmpeg-patches), which
publishes a signed release tarball per FFmpeg base, and every VMAFx FFmpeg
build applies that series first and `ffmpeg-patches/series.txt` second
([ADR-3143](../adr/3143-ffmpeg-shared-fix-series.md)). `build-config.env`
pins it:

| Key | Meaning |
| --- | --- |
| `FFMPEG_FIX_SERIES_REPO` | URL of the repository that publishes the series |
| `FFMPEG_FIX_SERIES_TAG` | the release tag |
| `FFMPEG_FIX_SERIES_SHA256` | sha256 of `ffmpeg-patches-<tag>.tar.gz` |

`scripts/ci/ffmpeg-shared-series.sh` is the only code that fetches and applies
it. The patch-stack replay below, `ffmpeg-patches/test/build-and-run.sh`, the
hosted FFmpeg jobs and the four container builds all call it:

```bash
# Apply to a checkout of FFMPEG_TAG (commits each patch with git am --3way):
scripts/ci/ffmpeg-shared-series.sh apply /path/to/ffmpeg
# Apply to an unpacked release archive that is not a Git checkout:
scripts/ci/ffmpeg-shared-series.sh apply --method apply /path/to/FFmpeg-n9.0.2
# Only download, verify and unpack; prints the directory:
scripts/ci/ffmpeg-shared-series.sh fetch
```

The script stops, without applying anything, when:

- the tarball's sha256 is not the pinned one;
- the tarball's `base.env` names another FFmpeg tag or commit than
  `FFMPEG_TAG` / `FFMPEG_COMMIT`;
- a member of the tarball would be written outside its directory;
- `FFMPEG_FIX_SERIES_VERIFY=cosign` is set and cosign is missing, or cosign
  refuses the release's signed `SHA256SUMS`.

With the default `FFMPEG_FIX_SERIES_VERIFY=auto` the signature is checked when
cosign is installed; the script's one-line summary says whether it was. The
`FFmpeg Patch Stack` gate installs cosign and requires it. Downloads are cached
under `<cache root>/vmafx/ffmpeg-fix-series/`; the container builds use a
temporary directory (`FFMPEG_FIX_SERIES_CACHE=none`).

Series `v0.1.0-rc.1` holds four patches. Three change FFmpeg's behaviour in
every VMAFx build:

| Patch | File | Behaviour |
| --- | --- | --- |
| 0001 | `libavutil/vulkan.c` | On a device with one queue family, a frame barrier no longer releases the frame to `VK_QUEUE_FAMILY_IGNORED`. |
| 0002 | 50 files | The GCC 14/16 diagnostics hardening that was `ffmpeg-patches/0019`; no change for VMAFx builds. |
| 0003 | `libavcodec/nvenc.c`, `nvenc.h` | `hevc_nvenc` with `udu_sei=1` drops a user data unregistered SEI that exceeds NVENC's 1024-byte limit for non-VCL NAL units, with a warning, instead of failing the encode. |
| 0004 | `libavcodec/nvenc.c`, `nvenc.h` | `h264_nvenc` and `hevc_nvenc` do not write a user data unregistered SEI that NVENC would write truncated; the first one is a warning. |

A fix to FFmpeg belongs in that repository, not in `ffmpeg-patches/`. To take
a new series release, change the three keys together and run the refresh and
the check below.

## Check a change locally

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --check \
  --output-dir .workingdir/cache/ffmpeg-patch-stack/manual-check
```

The check fetches the configured release, applies the shared fix series and
replays the complete patch series in order. It fails on a replay conflict or
when patches need a canonical refresh.
It leaves the tracked patch files and release setting unchanged. Inspect the
command output and the diagnostics under the selected output directory when it
fails.

### The local hook

The `ffmpeg-patches-apply-check` hook runs `--refresh` against the configured
release at both pre-commit and pre-push. It regenerates canonical patches after
a successful replay. The hook framework stops the commit or push if tracked
files changed, so review and stage those changes before retrying.

The hook runs when a change touches one of these inputs:

- the patch directory and the patch workflow;
- the CI helpers under `scripts/ci/`;
- the shared build configuration (`build-config.env`);
- public headers under `core/include/`;
- Meson build and options files.

An ordinary documentation edit does not select the hook. Install the hooks
with `make install-hooks`; see
[Automated rule enforcement](automated-rule-enforcement.md) for what the
installed hooks do.

## The local FFmpeg source cache

The tool does not download FFmpeg on every run. It keeps one bare Git
repository per remote and stores each fetched release commit in it under the
tag's name, so the hook at every commit and push reads the commit from disk.

- **Location:** `$XDG_CACHE_HOME/vmafx/ffmpeg-patch-stack/<remote hash>/source.git`.
  Without `XDG_CACHE_HOME` it is `~/.cache/...`, on macOS
  `~/Library/Caches/...` and on Windows `%LOCALAPPDATA%\...`.
  `--cache-dir <dir>` selects another root.
- **What is verified:** the commit of the configured tag must equal
  `FFMPEG_COMMIT`. A cache entry or a network fetch that names another commit
  fails with both commits in the message; the wrong commit is never stored or
  used. After a deliberate pin bump the first run fetches the new tag, and the
  old entry stays unused.
- **Which path ran:** the command output and `receipt.json` (`source`,
  `target_source`, `cache`) say `cache hit`, `fetched into cache` or, for a
  newer release found by `--latest`, `fetched (unpinned)`. A newer release has
  no pin yet: it is always fetched, and `--refresh --latest` writes its commit
  to `FFMPEG_COMMIT` together with the tag.
- **Concurrency:** writers hold a lock file next to the repository (waiting at
  most ten minutes), and a ref moves only after its commit was fetched and
  verified, so parallel hooks share one entry.
- **Self-repair:** an empty or corrupt cache directory, or a ref whose objects
  are gone, is dropped and fetched again. A cached commit that differs from the
  pin is not repaired silently; delete the directory below to discard it.

To clear the cache, remove `<cache root>/vmafx/ffmpeg-patch-stack/`.

## Refresh the configured release

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --refresh \
  --output-dir .workingdir/cache/ffmpeg-patch-stack/manual-refresh
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
`master` and every push to `master`. Two workflow jobs split the work:

| Trigger | What runs | Which release |
| --- | --- | --- |
| Pull request, push to `master`, manual dispatch | Contract tests on every event; fetch and replay of the complete series when the ADR-1140 impact planner selects it | The configured release (`FFMPEG_TAG`); never discovers a newer one |
| Schedule, 05:43 UTC daily (**FFmpeg Release Refresh** job) | `--refresh --latest`, see below | The newest stable upstream release |

The impact planner selects the expensive fetch and replay for core, patch,
workflow or CI-authority changes. Unknown inputs take the full validation
path. Documentation-only edits finish after the contracts without fetching
FFmpeg.

The broad CI pre-commit job skips the `ffmpeg-patches-apply-check` hook
because the independent required check owns its replay and impact routing.
Local pre-commit and pre-push runs keep it enabled.

### Scheduled release discovery

The scheduled updater owns upstream discovery. Renovate no longer opens
separate FFmpeg pin-only updates that omit the series rebase. The job runs:

```bash
python3 scripts/ci/ffmpeg_patch_stack.py --refresh --latest \
  --output-dir "$RUNNER_TEMP/ffmpeg-patch-refresh"
```

`--latest` is accepted only with `--refresh`. It discovers the highest stable
`nX.Y` or `nX.Y.Z` upstream release tag and replays the complete series onto
it. Development, RC and snapshot refs are excluded.

The pinned shared fix series targets one FFmpeg release. When a newer release
exists, the refresh therefore stops with `FFmpeg n9.1 is released, but the
pinned shared fix series v0.1.0 targets n9.0.2` (with the tags of the day),
writes nothing to the tree, and
keeps the rebased candidate patches under `<output-dir>/patches`. Move
`FFMPEG_FIX_SERIES_TAG` and `FFMPEG_FIX_SERIES_SHA256` to a series release for
the new FFmpeg together with `FFMPEG_TAG` and `FFMPEG_COMMIT`, then run
`--refresh`.

The job uploads a `ffmpeg-patch-refresh-<run-id>-<attempt>` artifact, retained
for 14 days, containing:

- the tool's diagnostics;
- the command log;
- `source-revision.txt`;
- `worktree-status.txt`;
- `proposed-refresh.patch`.

The diff includes tracked generated configuration mirrors as well as the patch
series. Review it against the recorded source revision before applying it to
an isolated branch and running the configured-release check.

A replay conflict leaves the scheduled job failed and retains its diagnostics.
The workflow has read-only repository permissions and creates no pull request,
merge or release tag. The scheduled result is a proposal for maintainer
review.

## Test the automation contract

```bash
python3 -m unittest discover -s scripts/ci -p 'test_ffmpeg_patch*.py' -v
```

These tests cover replay behavior, hook triggers, PR-versus-schedule separation,
impact routing and propagation of command failures through diagnostic capture.
