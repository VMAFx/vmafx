<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1036: Correct SPDX license identifiers and add missing libsvm copyright

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `license`, `security`, `supply-chain`

## Context

Two classes of licensing defect were identified in the r7 review:

**SPDX identifier mismatch.** The `LICENSE` file (line 2) declares
`SPDX short identifier: BSD-2-Clause-Patent`. All eight package manifests
(`Cargo.toml` workspace, `bindings/rust/vmafx/Cargo.toml`, and six Python
`pyproject.toml` files) instead carry `BSD-3-Clause-Plus-Patent`, which is not
a recognised SPDX identifier. Crates.io would advertise an unknown license
expression, PyPI rejects PEP 639 uploads with unrecognised identifiers, and
pip-audit tools flag such packages as having an unknown license.

**Missing upstream copyright in `svm.cpp`.** The vendored libsvm implementation
in `core/src/svm.cpp` carries only the Netflix copyright header. The upstream
libsvm BSD-3-Clause copyright (`Copyright (c) 2000-2019 Chih-Chung Chang and
Chih-Jen Lin`) is present in the paired `svm.h` but absent from `svm.cpp`. BSD-3-
Clause clause 1 requires that source redistributions retain the copyright notice.
`svm.h` was correct; `svm.cpp` was not.

## Decision

1. Replace all `BSD-3-Clause-Plus-Patent` `license =` values in the eight package
   manifests with `BSD-2-Clause-Patent` to match the actual `LICENSE` file SPDX
   identifier. Comments in those files that reference `BSD-3-Clause-Plus-Patent` as
   prose are updated too to avoid future confusion.
2. Prepend the verbatim libsvm BSD-3-Clause copyright block to `core/src/svm.cpp`,
   ahead of the Netflix header, matching the format in `svm.h`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Update LICENSE to say BSD-3-Clause-Plus-Patent | Manifests stay unchanged | Non-SPDX identifier propagates upstream; crates.io/PyPI still reject | Wrong direction |
| Use `Apache-2.0 WITH LLVM-exception` | SPDX-valid | Does not match the actual license | Wrong license |
| NOLINT the crates.io publish flag | Silences warning | Does not fix the legal defect | Unacceptable |

## Consequences

- **Positive**: Package manifests are now SPDX-valid and match the actual `LICENSE`
  file; crates.io and PyPI packaging no longer advertised an unknown license identifier;
  `svm.cpp` now satisfies BSD-3-Clause clause 1 for source redistributions.
- **Negative**: The `deny.toml` `[licenses]` allow-list entry for `BSD-3-Clause-Plus-Patent`
  should be reviewed, but leaving it in does not break the build.
- **Neutral / follow-ups**: The SPDX header comments in `.c`/`.h` files that say
  `BSD-3-Clause-Plus-Patent` are out-of-scope for this PR (they are source-file prose
  comments, not machine-parsed license declarations). A follow-up sweep can normalise
  them.

## References

- r7 review findings: [r7-licensing] Cargo.toml wrong SPDX; ai/pyproject.toml wrong SPDX; svm.cpp missing libsvm copyright
- `LICENSE` SPDX short identifier: `BSD-2-Clause-Patent` (line 2)
