---
paths:
  - scripts/sync-pelorus-interop.sh
  - scripts/ci/pelorus*
  - scripts/ci/tests/test-sync-pelorus-interop.sh
  - scripts/ci/tests/test_pelorus_mirror.py
invariant: Mirror guard fails closed without exact 40-character pin; CI checks out that object, never branch or tag.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# Pelorus mirror provenance gate (ADR-1113, ADR-1276)

`tests/test-sync-pelorus-interop.sh` proves top-level mirror guard fails
closed for plain directory and for Git checkout lacking exact pin. It
reconstructs source fixtures in disposable repositories, clears inherited
`GIT_*`, disables caller Git configuration; proves canonical fixture
prefix, tracked-path allowlist, and final-newline comparisons fail closed while
synthetic re-pin/update refreshes every banner. fixture uses portable
Python byte rewrite, not platform-specific `sed -i`. Keep it wired into
required Pre-Commit through `.pre-commit-config.yaml`.

real CI check belongs in existing `Pre-Commit` job in
`lint-and-format.yml`: derive 40-character pin from
`scripts/sync-pelorus-interop.sh`, check out `VMAFx/pelorus` at that object,
then run script's default mode. Never replace object with branch/tag
or restore its working-tree fallback; green check must bind every complete
rendered mirror and exact tracked lint-exemption set to reviewed source
bytes.

Licence (ADR-2817): `PELORUS_MIRROR_LICENSE` in sync script names one SPDX
identifier (`EUPL-1.2`). Every pinned source header and Pelorus fixture must
carry exactly that one `SPDX-License-Identifier` line; both modes refuse
otherwise. `--update` renders all ten files to temp dir, moves only after all
rendered, so refusal leaves mirror bytes unchanged. Changing identifier =
same PR updates `REUSE.toml` and `pelorus` entry of `docs/credits.yaml`.
Mirror paths get no `REUSE.toml` annotation and no
`.config/lint-exceptions.d/spdx.toml` entry. Keep SPDX tag split in script and
test strings, else REUSE reads it as file's own licence.
