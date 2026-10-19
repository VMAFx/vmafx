---
paths:
  - .config/lefthook/engine.sh
  - .config/praetor/engine.mk
  - lefthook.yml
  - scripts/git-hooks/hiss-audit.sh
  - scripts/githooks/state-sync.sh
  - .config/lint-exceptions.d/shfmt.toml
  - scripts/ci/tests/test_shfmt_scope.py
invariant: Governance hooks reach engine only through `sh .config/lefthook/engine.sh`; launcher stays praetor's unedited text.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# Engine launcher in governance hooks

ADR-3272. Seven governance jobs of `lefthook.yml`,
`scripts/git-hooks/hiss-audit.sh`, `scripts/githooks/state-sync.sh` call
`sh .config/lefthook/engine.sh <arguments>`. Never bare `praetorctl` /
`standardsctl` there: bare call runs `PATH` engine, not pinned one.

Launcher reads `PRAETOR_REF` from workflows under `.github/workflows`,
installs engine once per pin with `go install` into
`${XDG_CACHE_HOME:-$HOME/.cache}/praetor/engine/<pin>`, runs that engine.
Unreadable pin, two different pins, failed install -> hook fails, no `PATH`
fallback. Missing launcher -> status 127. Guards:
`scripts/git-hooks/test-hiss-audit-offline.py`,
`scripts/githooks/tests/test_install.py`.

`.config/lefthook/engine.sh` and `.config/praetor/engine.mk` = praetor's
text. `praetorctl adopt` refreshes each only while unedited. Never edit,
never format. `praetorctl audit` skips both files. Conflict: take one
side or regenerate with `adopt --lock-source-root=<praetor clone at pin>`
in throwaway clone.

Launcher not shfmt-stable, no licence line (cordanaLLM/praetor issues
1282, 1281) -> dated exceptions in `.config/lint-exceptions.d/shfmt.toml`
and `spdx.toml`. Three places skip shfmt for listed files: `exclude` of
`shfmt-src` hook, `SHFMT_FILES` in `Makefile`, shell branch of
`scripts/githooks/pre-commit.sh`. `scripts/ci/tests/test_shfmt_scope.py`
holds all three to list; stale entry (file shfmt-clean again) fails.

Pin move: steps in `docs/development/praetor-gate.md`; `adopt` only in
throwaway clone, copy back stale managed files only. `PreToolUse` entries
in agent settings: own change, not here.
