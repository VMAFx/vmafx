## Praetor pin 32a35c8fb947, the HISS-08 signal guard (2026-10-18)

`chore/praetor-refresh-32a35c8f`. A rebase or sync keeps `PRAETOR_REF` at `32a35c8fb947...` in
`.github/workflows/standards-gate.yml` and praetor's text of `.config/lefthook/python.sh`, which
`.config/lint-exceptions.d/shfmt.toml` and `spdx.toml` list until a pin fixes cordanaLLM/praetor
issue 1441; never format the launcher or add a header to it by hand. `.config/lefthook/scripts/common.py`
keeps its guard before `os.killpg` (no pid or group of 1 or less, no bool, not its own group).
`scripts/ci/tests/test_codex_hook_config.py` lists praetor's `praetorctl hook codex pre-tool` entry
under `ENGINE_HOOKS`.
