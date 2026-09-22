- **`vmaf-perShot` and the VPL decode tool can no longer spin forever on an
  input that never ends.** The per-shot scan now stops at
  `VMAF_PER_SHOT_MAX_FRAMES` and reports `-EFBIG` instead of wrapping its
  `uint32_t` frame counter, and `vmaf_vpl`'s decode retry loop gives up after
  the same 60 s ceiling its sync operation already used rather than retrying a
  wedged device indefinitely. Both previously used an unbounded `for (;;)`.
- **Core CLI cleanup paths restructured to satisfy the HISS control-flow
  contract.** The YUV reader, the VPL tool, the per-shot scanner and the
  Windows `getopt` shim drop thirteen `goto` jumps and six oversized functions
  in favour of staged helpers. Scores, output text, exit codes and
  freed-resource ordering are unchanged — the golden 576x324 pair still scores
  76.667831 and `vmaf-perShot` still emits a byte-identical plan.
- **The two new CLI ceilings are documented with what they do not
  guarantee.** [ADR-1287](docs/adr/1287-cli-tool-unbounded-loop-ceilings.md)
  records why each bound was chosen and the alternatives rejected;
  `docs/state.md` carries the closed defect and the three items the change
  opens — the VPL ceiling has not run against real Intel hardware, the
  per-shot ceiling proves termination but is not a hang timeout, and
  `scripts/ci/tidy-baseline-sycl.json` is stale-high for `vmaf_vpl.c`
  (21 recorded, 12 measured) until it is re-recorded on a SYCL build.
- **Governance surfaces re-measured against the corrected standards engine.**
  The praetorctl build pinned earlier on this branch measured a function's
  length from its signature line rather than its opening brace, which inflated
  every wrapped-signature function. The HISS-04 negative fixture, the
  `core/tools` notes and `docs/rebase-notes.md` now record the brace-relative
  boundary the engine actually enforces, and `.standards-baseline.json` is
  re-recorded downward to 938. `docs/state.md` records which of the earlier
  splits were phantom findings, and opens
  `T-TIDY-RATCHET-CPU-UNTIGHTENED-2026-09-22` for the five files this branch
  cleans below `scripts/ci/tidy-baseline-cpu.json`, which only a
  gcc-15 + clang-tidy-22 measurement may tighten.
