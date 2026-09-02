<!-- markdownlint-disable MD060 -->
# ADR-0277: ffmpeg-patches refresh against n8.1 — 2026-05-04 (no drift)

- **Status**: Accepted
- **Date**: 2026-05-04
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ffmpeg, fork-local, maintenance, patches

## Context

Per [CLAUDE.md §12 r14](../../CLAUDE.md), the
[`ffmpeg-patches/`](../../ffmpeg-patches/) stack must replay cleanly
against pristine `n8.1`. The verification gate is **series replay**
(cumulative `git am --3way`), not per-patch standalone apply, because
patches `0002…0006` build on each other and only apply cleanly against
the cumulative state from earlier patches in the series — see
[ADR-0118](0118-ffmpeg-patch-series-application.md) and
[ADR-0186 §FFmpeg patch coupling](0186-vulkan-image-import-impl.md).

The previous refresh shipped with PR #331 on 2026-05-03 (docs-only:
`CHANGELOG.md` + `docs/rebase-notes.md`). PRs that have landed since
on `master` (#332-#341) did not touch `ffmpeg-patches/` or any
libvmaf C-API surface that the patches consume, so a content drift
was not expected. This ADR records the verification run that
confirms the stack is still good.

## Decision

We replay the six-patch series against a pristine `n8.1` checkout
(`git -C /tmp clone --depth 1 --branch n8.1 https://github.com/FFmpeg/FFmpeg.git`),
apply via `git am --3way` in `series.txt` order, regenerate via
`git format-patch n8.1..`, and diff the regenerated tree against the
in-tree patches. **No content drift detected.** All six patches replay
cleanly; the only diffs from `git format-patch` are cosmetic noise
(`Subject: [PATCH 1/6]` numbering, MIME headers, hunk-context
shorthand counts, hunk offsets that float against cumulative state).
We keep the in-tree patches unchanged to minimise churn — per the
`refresh-ffmpeg-patches` skill rule "if only timestamp drift, prefer
original to minimise churn."

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep originals (chosen) | Zero in-tree diff; no merge conflict surface for in-flight PRs; the regenerated diffs are pure stylistic noise | Slight inconsistency with what `format-patch` would emit fresh | The substantive content is byte-identical at the diff level; replacing with regenerated patches buys nothing and creates churn |
| Replace with regenerated patches | Patches match what `format-patch` produces from a clean replay | Adds noise to git history (PATCH numbering, MIME headers, hunk-count reformatting) without any functional change | Pure churn; CLAUDE.md §12 prefers stable in-tree files unless there's a real change |
| Skip the verification | No work | Drift could be silently accumulating; rule §12 r14 expects a periodic series-replay gate | Verification is the entire point of the periodic refresh cadence |

## Consequences

- **Positive**: Verifies the patch stack is rebase-clean against
  `n8.1`. Confirms PRs #332-#341 (HIP kernel-template consumers, OSSF
  Scorecard work, Vulkan 1.4 deferral, U-2-Net deferral) introduced
  zero drift on the ffmpeg-integration surface — they touched
  libvmaf-internal kernels, CI workflows, and docs only. Captures the
  replay procedure for the next refresh.
- **Negative**: None.
- **Neutral / follow-ups**: Next refresh trigger is either (a) a
  libvmaf C-API surface change that any patch consumes (per §12 r14
  this would land a refresh in the same PR), or (b) the next periodic
  replay (~monthly cadence). The `vf_libvmaf` smoke build is deferred
  to CI (`ffmpeg-integration.yml`) because the local
  `meson-uninstalled` `.pc` file's include layout doesn't satisfy
  FFmpeg's `#include <libvmaf.h>` probe (the headers live under
  `libvmaf/libvmaf.h` only, and the uninstalled `Cflags` doesn't add
  the bare-`libvmaf.h` shortcut that the system-installed `.pc`
  exposes via a separate `-I${includedir}/libvmaf` line). End-to-end
  validation runs in CI against an installed libvmaf prefix.

## References

- [ADR-0118](0118-ffmpeg-patch-series-application.md) — patches ship
  as ordered `series.txt`, not a single carry; per-patch standalone
  apply is the wrong gate.
- [ADR-0186](0186-vulkan-image-import-impl.md) — image-import contract
  the FFmpeg patch coupling rule (libvmaf C-API change ⇒ patch
  refresh in same PR).
- [ADR-0238](0238-vulkan-picture-preallocation.md) — picture
  preallocation surface; the most recent `ffmpeg-patches/` content
  refresh (PR #264, 2026-04-29).
- PR #331 — previous refresh (docs-only).
- Source: `req` — user-direction `Replay the ffmpeg-patches/ stack
  against the current n8.1 head of FFmpeg. Capture any drift,
  regenerate patches if needed, run vf_libvmaf smoke against a
  known-good libvmaf build.` (paraphrased).
