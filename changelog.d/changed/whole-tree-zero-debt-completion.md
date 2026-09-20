# Changelog fragment

## AI

- Kept direct `ai/scripts/*.py` invocation while making their repository
  imports ordinary static imports: the shared bootstrap now installs its fixed
  repository roots during helper import, eliminating the suppressed `E402`
  queue without dynamic-import evasion.

- **Warning and standards baselines are migration inventories, not allowances.**
  HISS-10 now has one unambiguous whole-tree completion state: zero actionable
  findings across Netflix-mirror, vendored, fork-local, GPU, SIMD, tests, and
  tools. Cleanup may land in reviewable waves, but no wave or green ratchet run
  closes the work while any pinned HISS, clang-tidy, cppcheck, compiler,
  formatter, or invariant finding remains. See
  [ADR-1267](docs/adr/1267-whole-tree-zero-debt-completion.md).

## Core tools

- `vmaf_bench` now rejects unknown or incomplete options, returns failure when
  any benchmark target fails, distinguishes numeric validation failures (exit
  `1`) from aborted validation (exit `2`), and propagates backend, flush, score,
  allocation, and input errors instead of reporting them as successful skips.
  Its MCP wrappers preserve the same distinction.
- Y4M header integers and ratios are parsed with bounded, overflow-checked
  conversion. Trailing junk, duplicate separators, malformed interlace tags,
  and out-of-range values now fail at the header boundary.
- C translation units use the portable `VMAF_NULLPTR` token instead of raw
  `NULL` plus disabled `modernize-use-nullptr` diagnostics (ADR-1269).
