## Encoder facts come from api/vmafx-encoders.toml (2026-10-29)

- [ADR-3388](../adr/3388-central-encoder-registry.md): the quality windows,
  presets, versions and flags of `pkg/encoder`, `pkg/codecadapter` and the
  Python `codec_adapters` come from one registry; `_ABSOLUTE_CRF_RANGE_BY_NAME`
  in `tools/vmaf-tune/src/vmaftune/bisect.py` is gone (the bisect reads
  `absolute_range` from the registry).
- **On sync**: an upstream or branch change to an adapter's window, presets or
  version lands in `api/vmafx-encoders.toml`, then
  `python3 scripts/codegen/vmafx-api.py --write`; never in the generated
  `registry.gen.json` copies. A conflict in a generated copy takes either side
  and is regenerated at the tip. `tests/test_encoder_registry.py` and
  `pkg/codecadapter/registry_test.go` fail when an adapter and its row disagree.
