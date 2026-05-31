# ADR-0339: `av1_videotoolbox` placeholder adapter + upstream watcher

- **Status**: Accepted
- **Status update 2026-05-15**: implemented;
  `Av1VideoToolboxAdapter` class in
  `tools/vmaf-tune/src/vmaftune/codec_adapters/`; placeholder adapter
  runtime probe in place.
- **Date**: 2026-05-09
- **Deciders**: @Lusoris
- **Tags**: tooling, ai, ffmpeg, codec, hardware-encoder, apple, fork-local, upstream-blocked

## Context

Apple M3 / M4 silicon ships with hardware AV1 encode capability, but
FFmpeg upstream has not yet exposed it. Verification by the prior
worktree agent (a7e303a8b58166616, 2026-05-08) against
`git.ffmpeg.org/ffmpeg.git` master tip `8518599cd1`:
`libavcodec/videotoolboxenc.c` registers H264, HEVC, and PRORES
encoders only. The `av1_videotoolbox` symbol that exists in the tree
is `ff_av1_videotoolbox_hwaccel` — a **decoder** hwaccel, not an
encoder.

We need a way to:

1. Give the codec-adapter registry a stable `"av1_videotoolbox"`
   entry today, so callers on Apple Silicon (per-title encoders, the
   `vmaf-tune` search loop, downstream tooling) can name the codec
   they intend to use without conditional logic that assumes Apple
   AV1 is unsupported forever.
2. Activate that adapter automatically the moment FFmpeg upstream
   ships the encoder, without coupling activation to the fork's
   manual sync cadence.

The fork's no-guessing rule (`feedback_no_guessing`) forbids
fabricating encoder option names, so the placeholder must refuse to
emit argv until it has positively confirmed the encoder exists on
the host.

## Decision

Ship `Av1VideoToolboxAdapter` as a placeholder paired with an
upstream watcher and a self-activating runtime probe:

- The adapter registers in `ADAPTER_REGISTRY` with
  `quality_range=(0, 100)` (inheriting the VideoToolbox-family
  convention from h264 / hevc), `supports_runtime: bool = False`,
  and `adapter_version="0-placeholder"`.
- On every `validate()` / `ffmpeg_codec_args()` call the adapter
  invokes `ffmpeg -hide_banner -h encoder=av1_videotoolbox` and
  inspects the output for two sentinels: `"is not recognized"`
  (FFmpeg doesn't know the encoder; raise
  `Av1VideoToolboxUnavailableError`) or
  `"Encoder av1_videotoolbox"` (FFmpeg knows the encoder; promote
  to active for this call).
- The argv emission shape (`-c:v av1_videotoolbox -realtime 0/1
  -q:v <int>`) is extrapolated from h264/hevc VT and only fires
  after the runtime probe has confirmed the encoder is live. The
  activation PR rewrites this method if upstream ships a different
  knob shape.
- A weekly GitHub Actions cron at
  `.github/workflows/upstream-watcher.yml` invokes
  `scripts/upstream-watcher/check_ffmpeg_av1_videotoolbox.sh`,
  which partial-clones FFmpeg master and greps
  `libavcodec/videotoolboxenc.c` for `AV_CODEC_ID_AV1`. On a hit,
  the workflow opens a `upstream-blocked` GitHub issue (de-duped
  by exact title) carrying the activation checklist.
- This is the **first** upstream-watcher in the fork. The pattern
  is documented in `docs/development/upstream-watchers.md` so
  later watchers (e.g. for other upstream-blocked encoders or
  feature extractors) can reuse it without redesign.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Skip until FFmpeg lands the encoder | Zero ship cost today | Apple users have no encoder name to target; every downstream tool needs a "AV1 not on Apple yet" branch; activation needs a brand-new PR with full design instead of a flip | Leaves Apple users with no encoder name. Forces every downstream consumer to grow conditional logic that will be unwound later. |
| Hard-coded activation tied to the fork's pinned FFmpeg version | Simpler probe (just compare a version string) | Couples the adapter's behaviour to the FFmpeg version-bump cadence, which is bursty and unreliable; misses cases where a custom user build of FFmpeg has the encoder before the fork bumps its pin | Activation lag is unbounded and tied to an unrelated cadence. Doesn't help users who build their own FFmpeg. |
| Chosen: runtime probe + auto-activation + weekly upstream watcher | Self-activates on any FFmpeg build that recognises the encoder; bounded one-week worst-case detection latency; reusable pattern for the next upstream-blocked surface | One subprocess call per `validate` (negligible — `ffmpeg -h` returns near-instantly); placeholder argv shape is a guess that may need rewriting on activation | The probe cost is rounding error against an actual encode; the argv-shape risk is bounded by the no-guessing gate (we only emit the shape after the probe passes, and the activation PR is forced to re-verify it). |

## Consequences

- **Positive**: `tools/vmaf-tune` callers can list
  `av1_videotoolbox` alongside the other codecs from day one; the
  registry stays one-file-per-codec; the moment FFmpeg upstream
  ships the encoder (or a user runs a custom FFmpeg build that
  has it), the adapter activates with zero code change here.
- **Positive**: The watcher pattern is documented and reusable.
  Future upstream-blocked surfaces (other Apple hardware
  encoders, ONNX op support, libvpx2 features, …) follow the
  same shape.
- **Negative**: Carries a small amount of placeholder code that
  the activation PR will rewrite — specifically the `quality_range`,
  the `_PROBE_*_NEEDLE` sentinels, and the argv emission. If
  upstream ships a radically different knob shape (e.g. a
  `-bitrate`-only encoder), the rewrite is non-trivial.
- **Negative**: The runtime probe runs `ffmpeg -h` on every
  `validate`. For the typical `vmaf-tune` search loop this fires
  once per (codec, preset, crf) combination — a few hundred
  times in the worst case, well under a second of accumulated
  cost.
- **Neutral / follow-ups**: The activation PR (whenever upstream
  lands the encoder) bumps `adapter_version` to `"1"` so the
  ADR-0298 cache treats pre-activation rows as stale, flips the
  `supports_runtime` default, drops the unavailable-error
  raises, replaces the placeholder tests with real argv
  assertions, and updates this ADR's status to Superseded.

## References

- Source: `req` (this implementation task: ship a placeholder
  adapter + upstream watcher because FFmpeg n8.1 doesn't ship the
  encoder).
- Verification finding from prior agent
  `a7e303a8b58166616` (2026-05-08): FFmpeg master tip
  `8518599cd1` registers H264 / HEVC / PRORES VideoToolbox
  encoders only. The `ff_av1_videotoolbox_hwaccel` symbol is a
  decoder hwaccel, not an encoder.
- Related ADRs:
  [ADR-0237](0237-quality-aware-encode-automation.md) (codec-adapter contract),
  [ADR-0283](0283-vmaf-tune-videotoolbox-adapters.md) (h264/hevc VideoToolbox adapters),
  [ADR-0298](0298-vmaf-tune-cache.md) (cache-key invalidation on `adapter_version`),
  [ADR-0294](0294-vmaf-tune-codec-adapter-svtav1.md) (codec-adapter dispatcher pattern).
