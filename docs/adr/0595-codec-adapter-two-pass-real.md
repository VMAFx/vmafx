<!-- markdownlint-disable MD060 -->
# ADR-0595: Real two-pass argv for all 14 codec adapters

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: vmaf-tune, codec, encode, ffmpeg

## Context

Phase F (ADR-0333) added the `two_pass_args(pass_number, stats_path)` method
to the codec-adapter contract and wired the `run_two_pass_encode` driver to
invoke it. Only three adapters (`libx264`, `libx265`, `libvpx-vp9`) supplied
real implementations; the remaining 14 codec adapters (`libsvtav1`,
`libaom-av1`, `libvvenc`, the three NVENC, three QSV, three AMF, three
VideoToolbox encoders, plus the ProRes VideoToolbox adapter) inherited the
protocol-default body that raised `NotImplementedError`. Calls to
`two_pass_args` against any of those 14 codecs therefore crashed at the
contract surface rather than returning meaningful argv or a documented
refusal.

Two-pass encoding maps to four distinct vendor models that the harness must
preserve without per-codec branches in the search loop:

1. **Software encoders with a true two-invocation 2-pass pipeline.**
   `libx264`, `libx265`, `libvpx-vp9`, `libaom-av1`, `libvvenc` all run the
   same `ffmpeg <pass1> && ffmpeg <pass2>` shape with an on-disk stats
   sidecar. The harness's existing two-invocation driver applies as-is.
2. **Software encoder with a CRF-mode multi-pass prohibition.**
   `libsvtav1` enforces "CRF does not support multi-pass" at runtime
   (verified against SVT-AV1 v4.1.0); the adapter must surface that
   constraint instead of silently returning argv ffmpeg will reject.
3. **Hardware encoders with single-invocation in-encoder analysis.**
   NVENC (`-multipass fullres`), QSV (`-extbrc 1 -look_ahead_depth 40`),
   and AMF (`-preanalysis true`) all expose 2-pass-equivalent quality
   through a single ffmpeg invocation rather than two. Running the
   two-invocation driver against them would either fail outright or just
   re-encode from scratch twice.
4. **API-limited refusal.** Apple VideoToolbox's `VTCompressionSession`
   C API has no multi-pass interface; the only honest behaviour is to
   raise a clearly-typed error pointing users at the software fallback.

## Decision

Implement `two_pass_args` for every adapter so the contract is met:

- **Software true 2-pass** (`libaom-av1`, `libvvenc`): set
  `supports_two_pass = True` and emit the generic ffmpeg `-pass N
  -passlogfile <prefix>` pair the existing driver already handles for
  `libx264` / `libvpx-vp9`.
- **SVT-AV1**: set `supports_two_pass = False` (the harness pins CRF mode,
  which SvtAv1 forbids for multi-pass), but still return the VBR-mode
  argv (`-pass N -passlogfile <prefix>`) from `two_pass_args` so callers
  that explicitly override into VBR mode via `extra_params` can use it.
- **NVENC / QSV / AMF**: set `supports_two_pass = False` and return the
  vendor's single-invocation analysis flag set from
  `two_pass_args(1, _)`, `()` from `two_pass_args(2, _)`. The driver
  falls back to single-pass; callers can splice the pass-1 return value
  into `extra_params` for a quality-boosted single-pass encode.
- **VideoToolbox** (all four adapters): raise
  `VideoToolboxTwoPassUnsupportedError` (a `NotImplementedError`
  subclass) with a message that names the encoder and points users at
  `libx264 / libx265 / libsvtav1 / libaom-av1 / libvvenc` as the
  software fallback set.

The `supports_two_pass` flag remains the single source of truth for whether
the two-invocation driver runs; the new `two_pass_args` implementations are
the source of truth for *what argv* a 2-pass invocation would emit, even
when the driver chooses to short-circuit to single-pass.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Leave `NotImplementedError` for hardware encoders | No new code | Crashes the contract surface; callers can't introspect what argv 2-pass would emit; no path for hardware quality-boost flags to reach `extra_params` | Fails the acceptance gate ("`two_pass_args(1, p)` returns a real list of ffmpeg args") |
| Set `supports_two_pass=True` for hardware encoders and run two invocations | Reuses the existing driver | Wastes one full encode cycle (the "second pass" is just a separate single-pass run), confuses cache keys (ADR-0298), and contradicts vendor documentation | Hardware analysis is in-encoder; two invocations don't accumulate state |
| Use SVT-AV1 `-svtav1-params passes=2:pass=N:stats=` for SvtAv1 | Mirrors the standalone SvtAv1EncApp invocation | The FFmpeg `libsvtav1` wrapper doesn't expose those keys (verified against FFmpeg n8.1.1 + SVT-AV1 v4.1.0); the encoder also forbids multi-pass in CRF mode regardless | Falls foul of both the wrapper surface and the encoder's CRF prohibition |
| Raise `NotImplementedError` for VideoToolbox without a custom subclass | Smaller diff | Callers can't disambiguate "API limitation" from "we forgot to implement" without parsing the message | The custom subclass adds one symbol and removes ambiguity |

## Consequences

- **Positive**: every adapter now returns either real argv or a documented
  refusal. The `run_two_pass_encode` driver's fallback path (warn +
  single-pass) activates uniformly across hardware encoders. Callers that
  introspect the adapter to compose `extra_params` get the hardware
  quality-boost flags without per-codec branches.
- **Positive**: the SVT-AV1 CRF-mode multi-pass prohibition is documented
  in code (with the verbatim encoder error message) instead of being
  rediscovered each time a user enables `--two-pass`.
- **Negative**: the contract is now slightly asymmetric — software adapters
  with `supports_two_pass=True` return per-pass argv, while hardware
  adapters with `supports_two_pass=False` return pass-1 argv that lives
  in a single ffmpeg invocation. The asymmetry is documented per-adapter.
- **Neutral / follow-ups**: a future PR can add a `hardware_quality_boost`
  method on the protocol that surfaces the same NVENC / QSV / AMF flags
  more directly; today's `two_pass_args(1, _)` doubles as that surface.

## References

- ADR-0237 — codec-adapter contract.
- ADR-0294 — codec-agnostic dispatcher.
- ADR-0298 — adapter-version cache key.
- ADR-0333 — Phase F: 2-pass encoding for libx264 / libx265.
- SVT-AV1 v4.1.0 runtime error verified against FFmpeg n8.1.1 +
  SVT-AV1 v4.1.0 (`Svt[error]: CRF does not support multi-pass. Use
  single pass.`).
- FFmpeg `libavcodec/nvenc.c` `-multipass` AVOption (single-invocation,
  encoder-internal full-resolution analysis).
- FFmpeg `libavcodec/qsvenc.c` `-extbrc` + `-look_ahead_depth` AVOptions
  (single-invocation, in-encoder look-ahead window).
- FFmpeg `libavcodec/amfenc_*.c` `-preanalysis` AVOption.
- Apple `VTCompressionSession` C API — no multi-pass interface.
- Source: `req` ("Implement 2-pass encoding for the 14 codec adapters in
  VMAFx/vmafx that currently raise `NotImplementedError`").
