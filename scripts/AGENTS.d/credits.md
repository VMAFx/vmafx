---
paths:
  - scripts/docs/*credits*.py
  - docs/credits.yaml
  - docs/credits.md
invariant: docs/credits.yaml = curated source; tables rendered between markers; never hand-edit.
---
<!-- markdownlint-disable MD013 MD038 -->
# Credits page generator and gate (ADR-2485)

## Credits page (ADR-2485)

`docs/credits.yaml` is the curated list; `docs/credits.md` keeps hand-written prose
and renders its tables between `<!-- credits:table ID -->` markers.
`scripts/docs/credits_lib.py` loads, validates and renders (one implementation);
`generate-credits.py` writes / checks the page; `check-credits.py` holds the list
to the tree through `credits_checks.py`: page drift, uncredited third-party path
(REUSE.toml non-project licences, `third_party` / `3rdparty` / `vendor` dirs, notice
and font files, foreign copyright headers), unused `LICENSES/*.txt`, skill or agent
file with `derived_from` / "Adapted from" and no entry, entry path missing. An
`exceptions:` item in the list names one file, one rule, a reason and an expiry;
expired or idle items fail. Add an entry, never an ignore. Tests:
`scripts/docs/tests/test_credits.py` (planted defect per check). Wired into
`make docs-fragments-check` / `-write`. Do not hand-edit tables.
