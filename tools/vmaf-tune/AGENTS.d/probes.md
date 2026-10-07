---
paths:
  - tools/vmaf-tune/src/vmaftune/resolution.py
  - tools/vmaf-tune/src/vmaftune/hw_devices.py
  - tools/vmaf-tune/tests/test_resolution.py
  - tools/vmaf-tune/tests/test_hw_devices.py
invariant: Hardware probing is opt-in by codec; dummy-encode resolution floor is 320x240; resolution rule is height-only.
---
<!-- markdownlint-disable MD024 -->
# Hardware and resolution probes

- **Hardware-encoder availability probing is opt-in by codec, not
  by flag.** `probe_encoder_available()` only runs 1-frame lavfi
  dummy encode when codec is in `HARDWARE_ENCODERS`. Adding new
  hardware encoder family (e.g. VAAPI) means appending its names to
  that tuple; encoder will then automatically pay dummy-encode cost
  on every `compare` invocation. CPU encoders short-circuit after
  `ffmpeg -encoders` listing grep.
- **Probe dummy-encode resolution floor is 320×240 (ADR-0601).**
  `probe_encoder_available()` uses `nullsrc=size=320x240:rate=24:
  duration=0.5` for 1-frame dummy encode. Do not lower this
  resolution: NVENC requires at least ~145×49 and QSV requires
  ~128×96; 64×64 (pre-fix value) was below both minima and caused
  every hardware encoder to fail probe with EINVAL on otherwise
  fully-working GPU hosts.
- **Hardware encoders are probed before CLI's first encode, too.**
  `cli._require_hardware_encoder()` runs `probe_encoder_available()` for
  any encoder in `compare.HARDWARE_ENCODERS` (NVENC, QSV, AMF and, since
  2026-10-04, VideoToolbox) from `corpus`, live `recommend` and `ladder`,
  and stops with exit status 2 and probe's reason.
- **One VA-API resolution order (`hw_devices.resolve_vaapi_device`).**
  Explicit path, then node `session_vaapi_device()` set for
  command (`compare --vaapi-device`), then `$VMAFTUNE_VAAPI_DEVICE`,
  then first Intel render node, then `/dev/dri/renderD128`.
  session is context manager so test or command cannot leak it.
- **`resolution.py` decision rule is height-only.** `height >= 2160`
  picks `MODEL_4K` (`vmaf_v1.0.16_1d5h_2160`); everything else picks
  `MODEL_1080P` (`vmaf_v1.0.16_3d0h`). explicit `--vmaf-model` turns
  rule off (`CorpusOptions.resolution_aware=False`), and `neg` maps
  either result through `neg_model_for`. Width
  is accepted in API for symmetry but ignored in body. Do not add
  per-codec / per-pixel-count branches without ADR-0289 follow-up —
  rule mirrors Netflix's published guidance and is only defensible
  default until fork ships its own intermediate models.
