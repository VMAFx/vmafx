- **CodeQL quality-alert cleanup (code-scanning backlog)**. After triaging the
  full master code-scanning backlog against `origin/master` (124 alerts → 109
  resolved: most were verified false-positives or intentional patterns —
  SIMD-vs-scalar bit-exact parity asserts, bounded `float*float` products that
  cannot overflow and whose accumulation must NOT be widened on golden
  extractors, `# noqa`-annotated probe-imports in MCP tests, allowlist-mitigated
  path handling, and the fork's `==== original/modification ====` upstream-diff
  documentation in golden math files — all dismissed with recorded reasons),
  this lands the small set of genuine, behaviour-neutral fixes: removed four
  unused Python imports (`Hashable` in `tools/decorator.py`; `pytest`/`tempfile`
  in two harness tests), converted a two-label `VmafModelCollectionScoreType`
  switch in the CLI to a plain `if` (`core/tools/vmaf.cpp`), and added missing
  include guards to `core/src/feature/moment.h` and `core/src/feature/alias.h`.
  No metric-path or golden-affecting change.
