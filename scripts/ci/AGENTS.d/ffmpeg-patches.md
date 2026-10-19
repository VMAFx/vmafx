---
paths:
  - scripts/ci/ffmpeg_patch_stack.py
  - scripts/ci/checkout-annotated-tag.sh
  - scripts/ci/test_ffmpeg_patch_*.py
  - scripts/ci/test_git_fixture_isolation.py
  - ffmpeg-patches/series.txt
invariant: Replay `series.txt` cumulatively against `build-config.env` tag; fixtures discard inherited `GIT_*`.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# FFmpeg patch lifecycle (ADR-1240)

`ffmpeg_patch_stack.py`, local `ffmpeg-patches-apply-check` hook and
`ffmpeg-patch-stack.yml` share one release owner: `build-config.env`.
Replay `series.txt` cumulatively, fail on fetch/replay/configuration drift,
write only after whole candidate succeeds. Disposable Git must discard
inherited `GIT_*` repository variables and caller Git configuration. Discovery
scheduled, accepts only stable tags; ordinary checks use reviewed tag.
Required aggregator name = exactly `FFmpeg Patch Stack`.

`checkout-annotated-tag.sh` is warning-clean release checkout shared by
Docker, dev-container, hosted-integration, and smoke consumers. It must resolve
peeled commit for annotated tags, fetch that exact commit without inherited
`GIT_*` or caller configuration, verify `HEAD`, and recreate local tag for
version discovery. Never replace it with `git clone --depth=1 --branch`:
container Git version warns that annotated tag object is not commit.
`test_ffmpeg_patch_smoke_safety.py` uses annotated fixture tag and rejects
warning/error output.

Fixture setup and assertions obey same isolation rule as production
replayer. `test_ffmpeg_patch_stack.py`, `test_ffmpeg_patch_smoke_safety.py`
and dependency-classifier shell fixture discard inherited `GIT_*` before
their first Git command, disable caller global/system Git configuration.
Never rely on `git -C` alone. `test_git_fixture_isolation.py` runs those
fixtures plus agent-cleanup fixture with disposable caller variables,
checks byte-for-byte metadata/work preservation, remains registered in
pre-commit/pre-push and required Pre-Commit CI. Poison only fresh temporary
caller paths; never export real repository's Git paths into test.

Level Zero fixture setup and its checker subprocess use same Git isolation.
Preserve real linked-worktree hook regression: Git itself exports `GIT_DIR`,
so clean parent shell insufficient. Old-command control may mutate
only disposable caller; fixed helper must preserve every shared Git and
linked-worktree file, including both indexes and staged/unstaged work.
