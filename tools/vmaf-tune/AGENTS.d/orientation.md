---
paths:
  - tools/vmaf-tune/pyproject.toml
  - tools/vmaf-tune/vmaf-tune
  - tools/vmaf-tune/src/vmaftune/__init__.py
invariant: Quality-aware encode automation harness; usage docs describe shipped status; multi-phase architecture.
---
<!-- markdownlint-disable MD024 -->
# Orientation: scope, phases, and workflows

Quality-aware encode automation harness. See
[`docs/adr/0237-quality-aware-encode-automation.md`](../../../docs/adr/0237-quality-aware-encode-automation.md)
for umbrella spec and
[`docs/research/0044-quality-aware-encode-automation.md`](../../../docs/research/0044-quality-aware-encode-automation.md)
for option-space digest.

- **`dev` extra is everything `tests/` imports.**
  `pip install -e "tools/vmaf-tune[dev]"` then
  `python -m pytest tools/vmaf-tune/tests/` must give 0 failed, and no
  skip may be package suite imports but `pyproject.toml` does not
  declare (matplotlib and onnxruntime hid failing tests that way). new
  optional import in `src/` gets extra; one suite needs goes into
  `dev` too, and both hash locks that read this file
  (`tools/vmaf-tune/requirements-dev-lock.txt`,
  `dev/requirements-python-env-lock.txt`) are regenerated with
  existing pins kept. Tests read repository sources and find `vmaf`
  CLI through `tests/_vmaf_cli.py` (missing in-repo source fails, it
  never skips), and `tests/conftest.py` gives every test its own working
  directory. binary comes from `scripts/lib/vmaftest.py` alone
  (`VMAF_BIN`, `VMAF_BIN_FOR_TESTS`, repository's build directories):
  never add `PATH` lookup or host-install candidate to test, and keep
  capability checks (`--backend` support) as predicates on that binary.
- **Usage docs describe shipped implementation status.**
  Dedicated `docs/usage/vmaf-tune-*.md` pages and umbrella
  `docs/usage/vmaf-tune.md` page are user-discoverable contracts,
  not backlog scratch space. When tune surface leaves scaffold
  state, update both standalone page and umbrella page in same PR;
  do not leave `(stub)`, `scaffold-only`, or stale CLI names on
  paths backed by implementation and tests.

Phases B (target-VMAF bisect), C (per-title CRF predictor), E
(Pareto ABR ladder) and F (MCP tools) per ADR-0237 are explicitly
out of scope here; do not add bisect / predictor / ladder / MCP
code into this tree without ADR-0237 follow-up promoting
corresponding phase.
