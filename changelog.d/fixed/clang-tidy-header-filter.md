- **The clang-tidy ratchet now sees headers.** `.clang-tidy`'s `HeaderFilterRegex` was anchored
  at `^core/`, but clang-tidy matches the absolute path of each header, so every in-repo header
  had been silently filtered as "non-user code" — 480 suppressed diagnostics on a single
  translation unit, 313 findings across 60 headers tree-wide that the ADR-1142 whole-tree gate
  had never counted. The regex accepts absolute paths and the baselines are re-recorded to
  include the headers ([ADR-1265](docs/adr/1265-clang-tidy-header-filter-absolute-paths.md)).
