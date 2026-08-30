- Enable the full `modernize-*` clang-tidy family in `.clang-tidy`, with
  four explicit per-check disables for high-noise or C-ABI-hostile checks:
  `-modernize-use-trailing-return-type`, `-modernize-use-auto`,
  `-modernize-avoid-c-arrays`, `-modernize-use-nodiscard`. Discharge the
  top 15 findings on fork-added C++ translation units in the same change:
  8 `modernize-use-nullptr` (raw `NULL` → `nullptr`),
  6 `modernize-deprecated-headers` (e.g. `<stdlib.h>` → `<cstdlib>`,
  drop redundant `<stdbool.h>`), and 1 `modernize-use-auto` (cast-init
  type duplication). Touched files:
  `core/src/feature/feature_collector.cpp`,
  `core/src/metadata_handler.cpp`. ADR-0915.
