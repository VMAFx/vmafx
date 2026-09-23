- **Achieved 100% REUSE 3.3 specification compliance tree-wide across all 9,139 files in REUSE scope (BUG-003).**
  Baseline measurement on current master showed 6,806 files lacking copyright and 7,251 files lacking licensing
  information, 14 invalid SPDX expressions in prose across 13 files, and 1 unused license (`LicenseRef-Apache-2.0-u2netp`).
  Resolved cleanly without churn or noisy per-file headers:
  - Wrapped 14 prose and string mentions of license identifiers across 13 files with standard REUSE ignore comments
    (`<!-- REUSE-IgnoreStart -->` / `<!-- REUSE-IgnoreEnd -->`, `// ...`, and `# ...`), dropping invalid SPDX expressions to 0.
  - Activated `LicenseRef-Apache-2.0-u2netp` in `REUSE.toml` on `docs/ai/models/u2netp_mirror_card.md` and `docs/ai/u2netp-mirror.md`,
    and downloaded canonical ISC, LGPL-2.1-or-later, and GPL-2.0-or-later texts, leaving 0 unused and 0 missing licenses.
  - Defined root `REUSE.toml` (version 1) using a `closest` default plus exact provenance overrides. Strictly preserved upstream Netflix provenance
    (`2016-2020 Netflix, Inc.`, `BSD-2-Clause-Patent`) across `core/**`, `compat/python-vmaf/**`, `python/**`, `model/**`,
    `resource/**`, `Dockerfile*`, `Makefile`, and `OSSMETADATA`; preserved third-party provenance for `cJSON` (MIT), `xiph` (BSD-3-Clause),
    `iqa` (BSD-3-Clause), `cpuid.asm` (BSD-2-Clause), `x86inc.asm` (ISC), `pelorus` (BSD-2-Clause-Patent), and `fastdvdnet_pre` (MIT);
    kept inherited root and renamed upstream files under BSD-2-Clause-Patent; kept FFmpeg patches under LGPL-2.1-or-later (patch 0019 also
    spans GPL-2.0-or-later); retained every rename-aware no-CLA outside contribution on its existing BSD terms; assigned `model/tiny/**`
    models Lusoris `BSD-2-Clause-Patent`; and assigned Lusoris `EUPL-1.2` only to fork-authored code and docs.
  - Added fail-closed regression gates and tests: `reuse-lint` and `test-reuse-compliance` hooks in `.pre-commit-config.yaml`,
    a `lint-reuse` target in `Makefile` wired into `make lint`, dedicated unit tests in `scripts/ci/tests/test_reuse_compliance.py`,
    and CI verification with pre-commit 4.6.2 and REUSE 6.2.0 pinned in `.github/workflows/lint-and-format.yml`.
