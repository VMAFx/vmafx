# Research-2050: SpEED test native lint cleanup

## Diagnosis and implementation

At parent `2143174c7bf933f97aec610f423c559a885b8e9a`, a fresh configured CPU
build reproduces eight `constVariablePointer` findings in `test_speed.c` and
five in `test_speed_qa.c`. The extractor descriptors are only inspected or
passed to `vmaf_feature_extractor_context_create`, whose live declaration
already accepts `const VmafFeatureExtractor *`. Const qualification therefore
requires no production API or descriptor ownership change.

Clang-tidy 22.1.8 also diagnoses six and nineteen C `NULL` uses respectively,
plus 26 branches in `test_speed_qa_temporal_component_positive` against the
configured limit of 15. Both C tests receive the existing ADR-1138 bracket
for Windows C support and upstream C compatibility. The temporal test's
four picture allocations and four extractor/collector setup assertions move
into two small private helpers. Immediate failure returns preserve the
original runner behavior. No assertion is removed or weakened, and neither
helper is registered as another test.

No alternatives: the read-only declarations implement the existing API
contract, and grouping implements the existing touched-file branch rule.
There is no new policy or architectural decision and no new ADR.

## Preservation controls

The retained token control compares the exact ordered lists of all 67
assertion expressions/messages, 10 `mu_run_test` registrations, 47 `vmaf_*`
call sites and 170 numeric/string input literals against the parent files.
It permits only the three explicit equivalent pointer/address substitutions
in the moved extractor setup calls. The four grey-picture allocation calls
retain their arguments through the helper parameters. Apart from comments
and the eight const insertions, `test_speed.c` is token-identical. Meson
registration is byte-identical to the parent.

Both actual original/current Meson binaries exit zero with byte-identical
stdout/stderr, retaining five passing cases each. A disposable linker wrapper
then makes each of the eight moved setup assertions fail through its real
API boundary: picture allocations 7–10, descriptor lookup 6, and context
creation, context initialization and collector initialization 4. Original
and current binaries return one with identical messages and stdout/stderr
for all eight cases. Only the disposable test/wrapper objects disable LTO
so wrapping cannot be hidden by cross-TU inlining; the real production
objects and repository inputs are unchanged. This checks propagation, not
an expansion of allocation-failure cleanup coverage.

## Native measurement and limits

The private GCC 15.2 CPU release build enables float and assembly, disables
optional GPU/DNN/MCP/docs components, and uses CPUs 28–29. Both complete
touched files pass strict clang-tidy with zero warnings and zero uncited
exceptions. Exhaustive Cppcheck 2.21.1 with the official POSIX model reports
zero findings in either test. Its 133-command reduced context also contains
`test.c` variants and retains one `unusedFunction` finding in the untouched
`feature_collector.h`; that invocation exits one and is not a full lint pass.
No category is suppressed for that contextual diagnostic.

The real two-TU scoped writer records `test_speed.c` 6→0 warnings and
`test_speed_qa.c` 20→0, preserving every unmeasured entry and original full
report metadata. Total recorded debt changes 1359→1333 warnings with 54
uncited exceptions unchanged. This CPU-only test cleanup does not claim a
fresh whole-tree `make lint`, `make test`, Windows/Darwin, GPU, sanitizer or
Netflix Python golden pass. Production kernels, public headers, fixture
values and golden assertions are untouched.

## Reproduce and retain

In the configured CPU build:

```sh
meson test -C build --print-errorlogs test_speed test_speed_qa
python3 scripts/ci/tidy-ratchet.py --lane cpu --build-dir build \
  --only core/test/test_speed.c --only core/test/test_speed_qa.c
```

Canonical source hashes, tool and container identities, full configure/analyzer
commands, before/after outputs, token and failure controls, scoped writer
receipt and normal hook results are retained under
`.workingdir2/evidence/speed-tests-native-lint-20260908/`. Build objects remain
in the matching `.workingdir2/cache/` directory. This is a component for
root-owned integration; standalone publication is not requested.
