- **ffmpeg-patches replay against pristine `n8.1` — 2026-05-04
  (ADR-0277).** Periodic verification of the six-patch FFmpeg
  integration stack under
  [`ffmpeg-patches/`](ffmpeg-patches/). All six patches
  (`0001-libvmaf-add-tiny-model-option.patch` through
  `0006-libvmaf-add-libvmaf-vulkan-filter.patch`) replay cleanly
  cumulatively against a fresh `n8.1` checkout via
  `git am --3way`. **No content drift**: `git format-patch n8.1..`
  regeneration produces only cosmetic noise (`PATCH 1/6`-style
  numbering, MIME headers added by `format-patch`, hunk-context
  counts, hunk offset shifts that float against cumulative state).
  In-tree patches kept unchanged to minimise churn. Confirms PRs
  #332-#341 (HIP fifth/sixth kernel-template consumers, OSSF
  Scorecard remediation, Vulkan 1.4 bump deferral, U-2-Net
  saliency replacement deferral) did not touch the libvmaf C-API
  surfaces consumed by any patch — see
  [ADR-0277](docs/adr/0277-ffmpeg-patches-refresh-2026-05-04.md).
  `vf_libvmaf` end-to-end smoke deferred to CI
  ([`ffmpeg-integration.yml`](.github/workflows/ffmpeg-integration.yml)) —
  the meson-uninstalled `.pc` file's include layout does not
  satisfy FFmpeg's `#include <libvmaf.h>` probe locally; CI
  validates against an installed libvmaf prefix.
