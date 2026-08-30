- **Automated CI gate for the ffmpeg-patches surface-sync rule
  ([ADR-0356](../docs/adr/0356-ffmpeg-patches-surface-gate.md)).**
  CLAUDE.md §12 r14 (every PR that changes a libvmaf public-surface
  symbol consumed by `ffmpeg-patches/*.patch` must update at least
  one patch file in the same PR) is now mechanically enforced. New
  `ffmpeg-patches-surface-check` blocking job in
  [`.github/workflows/rule-enforcement.yml`](../.github/workflows/rule-enforcement.yml),
  backed by
  [`scripts/ci/ffmpeg-patches-surface-check.sh`](../scripts/ci/ffmpeg-patches-surface-check.sh).
  The script parses every patch under `ffmpeg-patches/` once,
  extracts a "consumed set" of `vmaf_*` / `Vmaf*` / `libvmaf_*` /
  `--enable-libvmaf-*` tokens, and intersects the set against the
  PR's diff over `core/include/libvmaf/*.h` and
  `core/meson_options.txt`; fails when the intersection is
  non-empty and no `ffmpeg-patches/*.patch` is in the diff. Per-PR
  opt-out is `no ffmpeg-patches update needed: REASON` in the PR
  body, matching the ADR-0108 family convention. Sub-second runtime
  on the live nine-patch stack; runnable locally via
  `BASE_SHA=… HEAD_SHA=… PR_BODY=… bash scripts/ci/ffmpeg-patches-surface-check.sh`.
  Documentation:
  [`docs/development/automated-rule-enforcement.md`](../docs/development/automated-rule-enforcement.md).
