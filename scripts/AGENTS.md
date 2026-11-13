<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
<!-- markdownlint-disable MD013 MD038 -->
# AGENTS.md — scripts/

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../docs/development/agents-index.md)).

Orientation for agents on top-level scripts tree (excluding
`scripts/ci/`, own AGENTS.md). Parent: [../AGENTS.md](../AGENTS.md).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `docs/*adr*`, `docs/agents_*.py`, `docs/tests/test_agents_index.py`, `docs/tests/test_generators.py`, `adr/**`, `/mkdocs.yml` | [adr-and-docs-generators](AGENTS.d/adr-and-docs-generators.md) | Generated ADR index, tag pages, AGENTS.md never hand-edited; next-free lock cwd-safe. |
| `release/concat-changelog-fragments.sh`, `/changelog.d/**` | [changelog-fragments](AGENTS.d/changelog-fragments.md) | `CHANGELOG.md` rendered from `changelog.d/<section>/*.md`; keep `^## \[` boundary. |
| `docs/*credits*.py`, `docs/tests/test_credits.py`, `/docs/credits.yaml`, `/docs/credits.md` | [credits](AGENTS.d/credits.md) | docs/credits.yaml = curated source; tables rendered between markers; never hand-edit. |
| `dev/*cleanup-agent-state.sh`, `dev/hw_encoder_corpus.py`, `dev/project_modernization_audit.py` | [dev-tools](AGENTS.d/dev-tools.md) | Cleanup protects dirty worktrees + strips GIT_*; corpus rename in producer; audit read-only. |
| `gen_*_onnx.py`, `gen_ssimulacra2_eotf_lut.py` | [fixture-generators](AGENTS.d/fixture-generators.md) | ONNX generators pin metadata for deterministic bytes; EOTF LUT shifts regression gate. |
| `git-hooks/*`, `githooks/**`, `/lefthook.yml` | [git-hooks](AGENTS.d/git-hooks.md) | Regular-file hook copies; quote-free lefthook; worktree state sync; merge-base mypy. |
| `**` | [ground-rules](AGENTS.d/ground-rules.md) | Shell set -euo pipefail, mktemp trap, LC_ALL=C sort; workflow renames in lockstep. |
| `sync-pelorus-interop.sh` | [pelorus-sync](AGENTS.d/pelorus-sync.md) | Pin 40-char commit via git show; fail closed without object; manifest + test together. |
| `run_unittests.sh` | [run-unittests](AGENTS.d/run-unittests.md) | Upstream-mirror Netflix Python unittest runner; keep byte-identical on rebase. |
| `setup/*` | [setup-detect](AGENTS.d/setup-detect.md) | Per-OS setup dispatcher stays idempotent; never sudo-escalates without user input. |
