---
paths:
  - core/src/feature/feature_extractor.h
  - core/src/feature/feature_collector.h
invariant: Shared C/C++ headers must declare one enum definition, never C++-only narrow enum.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Shared C / C++ Headers Enum Definition Invariant

## Rebase-sensitive invariants

- **Shared C / C++ headers: one enum definition, never C++-only narrow
  underlying type
  ([ADR-1470](../../../../docs/adr/1470-c-cxx-shared-enum-one-definition.md)).**
  `#ifdef __cplusplus enum X : unsigned char {` + plain C head = 1 byte in
  C++ TUs (SYCL, Metal twins), 4 in C TUs -> wrong bytes moment value
  crosses (by value over `extern "C"`, struct member, pointer). Was
  `VmafVifNameSet` in `nonfinite_score.h` (never crossed: only user
  `vmaf_vif_emit_scores()` is `static inline`); now plain `typedef enum` +
  cited NOLINT (`performance-enum-size`, `modernize-use-using`). Allowed
  dual head: `: unsigned int` WITH `..._ABI_UINT_MAX = UINT_MAX`
  enumerator (`model.h`, `luminance_tools.h`; size asserted in
  `core/test/test_flush_context_ordering.c`). On rebase: lint cleanup
  that "fixes" `performance-enum-size` with C++-only type -> revert to
  plain form. Guard: `core/test/test_c_cxx_enum_definition_contract.py`
  (every header `.c` under `core/` includes).
