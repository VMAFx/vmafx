---
paths:
  - tools/vmaf-tune/src/vmaftune/codec_adapters/__init__.py
  - tools/vmaf-tune/tests/test_codec_adapters*.py
  - tools/vmaf-tune/src/vmaftune/codec_adapters/registry.py
  - api/vmafx-encoders.toml
  - pkg/encoderregistry/**
invariant: CodecAdapter Protocol declares all fields; ten-name preset vocabulary; two_pass_args implemented on all adapters.
---
<!-- markdownlint-disable MD024 -->
# Codec adapter protocol and registry

- **Adapter facts live in `api/vmafx-encoders.toml`
  ([ADR-3388](../../../docs/adr/3388-central-encoder-registry.md)).**
  Knob, windows, absolute range, default, presets, version, probe settings,
  capability flags: edit TOML, run `python3 scripts/codegen/vmafx-api.py --write`.
  Never hand-edit either `registry.gen.json` (Go copy in
  `pkg/encoderregistry/`, Python copy in `codec_adapters/`). Dataclass field
  values = row values; `tests/test_encoder_registry.py` fails otherwise;
  Go side `pkg/codecadapter/registry_test.go`. Bisect window = row
  `absolute_range`. New encoder or gap = new `[[encoders]]` row first.

- **Codec-adapter contract is multi-codec from day one.** Phase A
  wires `libx264` end-to-end; `libaom-av1`
  ([ADR-0279](../../../docs/adr/0279-vmaf-tune-codec-adapter-libaom.md))
  joins as metadata-and-argv-helper adapter (its argv shape uses
  `-cpu-used`, not `-preset`, so encode driver gains second argv
  path when codec-pluggable encode wiring lands).
  `codec_adapters/__init__.py` exposes registry search loop must
  use uniformly. Do not branch on codec name in `corpus.py` /
  `encode.py` / `score.py`; route via adapter. New codecs are
  one-file additions under `codec_adapters/`.
- **Adapter preset vocabulary is cross-codec sweep axis.** Ten-name
  preset tuple (`placebo, slowest, slower, slow, medium, fast,
  faster, veryfast, superfast, ultrafast`) is shared across
  AV1-family adapters. Single `--preset` axis covers x264 / x265
  / svtav1 / libaom-av1 / libvpx-vp9 in one sweep. Each adapter maps
  name onto its codec-specific knob (cpu-used, preset enum, ...).
  Do not introduce per-adapter preset names; if codec needs knob
  shared vocabulary cannot express, route it through `extra_params`
  rather than splitting preset axis.
- **Codec-adapter contract is multi-codec from day one.**
  `codec_adapters/__init__.py` exposes registry search loop must
  use uniformly. Do not branch on codec name in `corpus.py` /
  `encode.py` / `score.py`; route via adapter. New codecs are
  one-file additions under `codec_adapters/`. Wired today: `libx264`
  (Phase A scaffold) and `libx265` (ADR-0288). One narrow exception
  lives in `encode.parse_versions(stderr, encoder=…)` — per-codec
  banner regex (x264's `x264 - core <N>` vs x265's
  `x265 [info]: HEVC encoder version <V>`) cannot be expressed as
  single pattern, so function dispatches on encoder name. This
  branch is allowed; corpus emitter and search loop must still go
  through registry.
  wires `libx264` plus NVENC family (`h264_nvenc`, `hevc_nvenc`,
  `av1_nvenc` — see
  [ADR-0290](../../../docs/adr/0290-vmaf-tune-nvenc-adapters.md)).
  `codec_adapters/__init__.py` exposes registry search loop must use
  uniformly. Do not branch on codec name in `corpus.py` /
  `encode.py` / `score.py`; route via adapter. New codecs are
  one-file additions under `codec_adapters/`. Hardware-encoder
  families share private helpers (e.g. `_nvenc_common.py`) — keep
  mnemonic preset map and CQ window in one place per family so
  per-codec files stay thin.
  wires `libx264` and `libsvtav1` (ADR-0294); `codec_adapters/__init__.py`
  exposes registry search loop must use uniformly. Do not branch
  on codec name in `corpus.py` / `encode.py` / `score.py`; route via
  adapter. New codecs are one-file additions under
  `codec_adapters/`.
- **`ffmpeg_preset_token()` adapter hook is optional** —
  `corpus.iter_rows` falls back to forwarding preset name verbatim
  when adapter does not implement it (libx264 path). Adapters that
  need non-string preset translation (libsvtav1 today, libsvthevc /
  future codecs tomorrow) implement hook and return string for
  argv. Do not promote it to required protocol method without
  same-PR pass over every existing adapter.
- **`two_pass_args` is implemented on every adapter (ADR-0595).**
  No adapter inherits protocol-default `NotImplementedError` body.
  `libaom-av1` + `libvvenc` are now `supports_two_pass=True`
  (FFmpeg generic `-pass N -passlogfile <prefix>`). `libsvtav1`
  returns same VBR-mode argv but stays `supports_two_pass=False`
  because SVT-AV1 enforces "CRF does not support multi-pass" at
  runtime — harness default mode is CRF, so driver falls back to
  single-pass. NVENC / QSV / AMF return their single-invocation
  in-encoder analysis flags (`-multipass fullres` /
  `-extbrc 1 -look_ahead_depth 40` / `-preanalysis true`) for pass
  1 and `()` for pass 2; callers compose pass-1 argv into
  `EncodeRequest.extra_params` for quality-boosted single-pass
  encode. All four VideoToolbox adapters raise typed
  `VideoToolboxTwoPassUnsupportedError` from `_videotoolbox_common`
  documenting that `VTCompressionSession` has no multi-pass C API.
  Do not regress these adapters back to bare
  `NotImplementedError` — search loop assumes contract is
  uniformly implemented.
- **Adapter `quality_range` is search-space boundary, not
  user-input gate (ADR-0306).** Widening libx264's range from `(15,
  40)` to `(0, 51)` was deliberate: recommend / coarse-to-fine
  flow must be allowed to probe boundary CRFs to
  bracket answer. If future codec adapter wants to restrict
  *user-visible* range on `--crf NNN`, do that at CLI layer, not
  in `adapter.validate`.

## Phase scope (codec registry)

Phase A (original scaffold): grid sweep + JSONL emit, x264 only.
ADR-0281 added three QSV codec adapters as one-file extension off
registry; encode-pipeline widening that makes them functional is
itself separate Phase A follow-up. Phases B–F per ADR-0237
(bisect / predictor / ladder / MCP) remain explicitly out of
scope here; do not add that code into this tree without ADR-0237
follow-up promoting corresponding phase.
Phase A (corpus generation): grid sweep + JSONL emit, x264 only.
Phase D (per-shot CRF tuning, ADR-0392): orchestrates shot
detection (via C-side `vmaf-perShot` binary, ADR-0222), extracts
each shot to raw YUV, and binds pluggable per-shot CRF predicate
to Phase B's real bisect backend by default. CLI deliberately
stops before running final segment encodes — it emits FFmpeg
encoding plan as JSON plus optional shell script.
`--predicate-module` remains advanced custom/test escape hatch;
it is no longer production path.

- **Encoder-version probe is process-cached fallback (ADR-0498,
  follow-up #7).** `encode._probe_encoder_version_from_ffmpeg`
  runs at most once per `(ffmpeg_bin, encoder)` pair via
  `_PROBE_CACHE` (module-scope dict). Tests that exercise fallback
  must clear `_PROBE_CACHE` explicitly. Probe parses
  `ffmpeg -version`'s configuration line and returns
  `"<encoder>-enabled"` when encoder is compiled in; empty string
  lets caller keep its `"unknown"` placeholder so existing tests
  that pin that exact value still pass.
  `_VERSION_PROBE_PATTERNS` now covers `libx264`, `libsvtav1`,
  `libx265`, `libvpx-vp9`, `libaom-av1`, and `libvvenc` (ADR-1077).
  Tests for any of these codecs that use fake runner and don't
  return `--enable-*` text in stdout must capture only first
  subprocess call (encode argv), not last. Probe fires second
  `ffmpeg -version` call when encoder banner absent from encode
  stderr.
  `encode.probe_encoder_info(ffmpeg_bin, encoder)` returns
  `EncoderInfo(encoder, codec_detected, version_label)` — callers
  should use this rather than re-parsing version string.
- **`codec_adapters.parse_available_codecs(stdout, *, restrict_to_known)`
  (ADR-0498 follow-up #7).** Parses `ffmpeg -hide_banner -encoders`
  output into frozenset of codec names. Set
  `restrict_to_known=False` to get full ffmpeg encoder list;
  default restricts to adapter registry so callers can intersect
  with `known_codecs()`.
- **`CodecAdapter` Protocol must declare every field every
  concrete adapter relies on (ADR-0888).**
  `codec_adapters/__init__.py` declares `CodecAdapter`
  `typing.Protocol`; every concrete adapter (`X264Adapter`,
  `LibaomAdapter`, NVENC / AMF / QSV / VideoToolbox / VVenC /
  SvtAv1 / libvpx) implements contract. When adding new field to
  *every* concrete adapter — most recently `presets: tuple[str, ...]`
  field consumed by `ladder._default_sampler_preset` — promote field
  to Protocol in same change. Concrete-only field that callers
  reach via `getattr(adapter, "presets")` silently drops type
  safety and pyright flags every cross-adapter call site. Protocol
  also = spec for `_REGISTRY: dict[str, CodecAdapter]` table;
  pyright variance rules require Protocol fields `Final` /
  read-only iff every adapter uses frozen dataclass — track that
  audit separately if new mutable-field adapter ever lands.
