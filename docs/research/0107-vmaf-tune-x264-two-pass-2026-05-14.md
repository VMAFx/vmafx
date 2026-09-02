<!-- markdownlint-disable MD013 -->
# Research 0107: `vmaf-tune` x264 two-pass gap

## Question

Can the Phase F two-pass seam close the documented `libx264` gap without
changing the encode driver or corpus schema?

## Findings

- `encode.run_two_pass_encode` already routes pass-specific argv through
  `adapter.two_pass_args(pass_number, stats_path)`.
- `X265Adapter` uses codec-private `-x265-params pass=N:stats=<path>`.
- FFmpeg exposes x264 two-pass state through the generic
  `-pass N -passlogfile <prefix>` flags, so the x264 adapter can opt in
  without a driver branch.
- The corpus schema already records pass count in the encode cache key,
  so enabling `supports_two_pass` on `libx264` does not require a schema
  bump.

## Decision

Enable `libx264` two-pass support in `X264Adapter` by declaring
`supports_two_pass = True` and adding a `two_pass_args()` method that
returns FFmpeg's native passlogfile flags. Keep the pass-1 null-muxer and
scratch cleanup behaviour in the shared encode driver.

## Validation

- `PYTHONPATH=tools/vmaf-tune/src .venv/bin/python -m pytest tools/vmaf-tune/tests/test_codec_adapter_x265_two_pass.py tools/vmaf-tune/tests/test_auto_phase_f1_f2.py tools/vmaf-tune/tests/test_encode_dispatcher_per_adapter.py -q`
- `.venv/bin/python -m ruff check tools/vmaf-tune/src/vmaftune/codec_adapters/x264.py tools/vmaf-tune/src/vmaftune/encode.py tools/vmaf-tune/src/vmaftune/corpus.py tools/vmaf-tune/src/vmaftune/cli.py tools/vmaf-tune/tests/test_codec_adapter_x265_two_pass.py tools/vmaf-tune/tests/test_auto_phase_f1_f2.py`

## References

- req: "find real backlogs/scaffolds and continue with real coding on a new branch"
- [ADR-0333](../adr/0333-vmaf-tune-phase-f-two-pass.md)
