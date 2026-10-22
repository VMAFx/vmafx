## Praetor 4ae64cb2 and clang-tidy data paths (2026-10-19)

`.standards.yaml` declares `scripts/ci/exact_twins.d` under
`clang_tidy.data_paths`; `scripts/ci/praetor_tidy_coverage.py` writes no
exception for a unit below `DATA_PREFIX`. A new parity-gate fragment needs no
exception entry. The exceptions block between the generated markers is still
written only by `praetor_tidy_coverage.py --write` from
`.config/lint-exceptions.d/`; the `release-token` entry lives in
`.config/lint-exceptions.d/release-token.toml` and expires on 2026-10-29.
No score, C API or build change.
