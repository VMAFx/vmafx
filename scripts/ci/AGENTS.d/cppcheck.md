---
paths:
  - scripts/ci/write_cppcheck_posix_model.py
  - scripts/ci/cppcheck-public-entrypoints.cfg
  - scripts/ci/tests/test_cppcheck_posix_model.py
invariant: Both cppcheck paths load generated POSIX model, `--check-level=exhaustive` and public-entrypoint cfg.
area: tidy
---
<!-- markdownlint-disable MD013 MD060 -->
# Cppcheck models and analysis level

Both local and required CI cppcheck invocations load installed analyzer's
full `posix` model through `write_cppcheck_posix_model.py`. Fork pthread types on
POSIX and in Windows compatibility shim are C aggregates, not unknown C++
classes with implicit constructors. For pre-2.22 models, generator inserts
missing `pthread_cond_init` contract. For newer defective shape it
removes only argument 2's `not-null` marker: POSIX permits `NULL` to select
default condition attributes. Preserve argument 1's marker and every other
installed model node; leave already-correct entry unchanged. Resolve
paired model through `--filesdir` or pre-2.18 install-relative layout; do
not copy or hand-maintain second POSIX model. Unknown/duplicate model shape,
missing source, or analyzer validation failure must fail closed and leave any
last-valid generated file intact.

Keep generated model separate from target selection: preserve database
defines, include paths and language settings, with no forced platform or
language. `tests/test_cppcheck_posix_model.py` runs actual cppcheck on shared
headers, nullable/default attributes, still-non-null condition object, and
uninitialized-member/constructor negative controls in Cppcheck job after
installation. Missing tools/models fail that test; no diagnostic category is
disabled. See [model investigation](../../../docs/research/cppcheck-pthread-model-2026-09-08.md).

Both paths select `--check-level=exhaustive` (ADR-1245). Preserve this value-flow
policy alongside existing severity sets and all command variants; never
suppress `normalCheckLevelMaxBranches` to hide incomplete analysis. Real-tool
suite includes normal/exhaustive branch-budget controls and defect controls.

Both paths also load `cppcheck-public-entrypoints.cfg` (ADR-1246). Keep its exact
public names shared; adding private helper to remove unused warning is not
export contract. Configured-driver hook validates model against
`VMAF_EXPORT` declarations and Meson's explicit installed-header lists, including
option-conditional headers. Preserve its cfg/header/workflow/test trigger paths.
Real-tool Cppcheck suite must reject missing/invalid models and still find
unlisted unused helpers and defects inside listed bodies. Entry names are
scope/linkage-blind: do not reuse them for private/static functions. Keep
measured collision control and both existing severity selections unchanged.
