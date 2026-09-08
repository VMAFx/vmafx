# Research-2054: Preserve motion, SSIM and PSNR coverage during lint cleanup

At `fb79390160fdfc3f8316dd5ab6cae4c05de2b58d`, exhaustive Cppcheck reports
seventeen read-only descriptor views across `test_integer_motion_v2_coverage.c`,
`test_ssim_coverage.c` and `test_integer_psnr_coverage.c`. Strict clang-tidy
also reports thirteen oversized test bodies and one missing loop brace.
The drivers already meet the branch limit; their registrations remain intact.

Descriptor views become const, including PSNR's private descriptor-return
helper. Thirteen test-specific setup helpers retain lookup, option insertion,
context creation/init and collector creation in their original order. Each
caller immediately returns the original setup failure. The remaining picture,
extract, score and teardown statements are unchanged. SSIM's outer pixel loop
gains braces. Existing C NULL portability brackets remain intact.

## Alternatives considered

| Choice | Result |
| --- | --- |
| Const views and small test-specific setup stages | Selected: preserves exact assertion text, options, order and failure behavior. |
| Generic parameterized test setup | Rejected: would obscure distinct messages, options and ownership paths. |
| Suppress the size findings or change production APIs | Rejected: unnecessary and outside this observation-only cleanup. |

This applies existing test/lint policy; no new architectural decision or public
surface is introduced. A user guide and FFmpeg patch changes are not required.

## Validation and limits

All nineteen existing cases pass before and after in a private GCC 15.2 CPU
release build with assembly and default LTO. Their stdout/stderr match exactly.
Source checks preserve all 133 assertions, 183 API calls, nineteen ordered
registrations and every string/numeric literal. Expanding each helper back to
its original setup leaves identical whole-file tokens except const qualifiers
and the single loop-brace pair.

A retained C probe includes the original or changed test source and intercepts
only its five setup APIs, forwarding successful calls to the actual configured
library objects. Across fifteen affected scenarios, each of sixty-six moved
setup stages is failed independently. Original/current API traces, failure
messages and immediate stopping points match. The fifteen normal probe paths
also pass. The probes are local evidence, not new repository tests.

The actual release executables and diagnostic non-LTO objects differ after
helper extraction. They are retained with section hashes for inspection;
there is no binary-identity or performance claim.

Strict clang-tidy 22.1.8 measures all three TUs at zero warnings, uncited
annotations and compile failures. The guarded scoped writer removes only their
five/seven/two warning allowances and retains full-lane metadata. Exhaustive
Cppcheck on all six configured test/driver commands reports no owned-TU
findings. It exits one solely for the unchanged unused inline
`vmaf_feature_vector_get_score` in `feature_collector.h`; this reduced-project
header finding is retained, not suppressed or described as a whole-tree pass.

Reproduce the existing tests:

```bash
meson test -C build --print-errorlogs \
  test_integer_motion_v2_coverage test_ssim_coverage test_integer_psnr_coverage
```

Locally retained commands, source hashes, analysis databases, old/new binaries,
original-source strict diagnostics, all failure traces and comparison scripts:
`.workingdir2/evidence/metric-coverage-const-2026-09-08/`.
Production sources/headers, Meson registration and the common test driver are
unchanged. No golden assertions changed. Full combined RC1, Windows/GPU,
Netflix Python golden and sanitizer acceptance remain separate.

## References

- [C test invariants](../../core/test/AGENTS.md)
- [Engineering principles](../principles.md)
- req: "get the codebase clean" while preserving meaningful coverage.
