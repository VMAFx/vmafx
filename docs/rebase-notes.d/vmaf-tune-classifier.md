## Content-aware video classification in vmaf-tune (2026-10-25)

- `tools/vmaf-tune/src/vmaftune/classify.py`: implements `ContentTags` frozen
  dataclass, ITU-T P.910 SI/TI extraction via FFmpeg `siti`, geometry/duration
  probing, HDR detection via `hdr.detect_hdr`, and local VLM hook for genre tagging.
- `tools/vmaf-tune/src/vmaftune/cli.py`: wire `vmaf-tune classify` subcommand with
  `--genre`, `--vlm-url`, `--vlm-model`, and `--json` flags.
- `tools/vmaf-tune/tests/test_classify.py`: full 3D test suite covering positive,
  negative, boundary conditions, planted negative fail-by-name on missing VLM,
  security boundary against remote endpoints, and cache recomputation on corruption.
- `docs/adr/3374-vmaf-tune-classifier-vision-fails-by-name.md`: documents Phase P1/P2
  shipping, fail-by-name on missing vision backend (superseding ADR-0618 fallback),
  local-only frame transport, and adult genre inclusion.
- `docs/usage/vmaf-tune-classifier.md`: documentation for `vmaf-tune classify`.
- `tools/vmaf-tune/AGENTS.d/classifier.md`: agent invariant page documenting
  fail-by-name and local-only frames invariants.
- **On sync**: keep fork's content-aware classifier implementation.
