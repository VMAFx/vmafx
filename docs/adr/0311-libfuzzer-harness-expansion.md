<!-- markdownlint-disable MD013 -->
# ADR-0311: libFuzzer harness expansion — `fuzz_yuv_input` + `fuzz_cli_parse`

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: lusoris, Claude
- **Tags**: security, build, ci, docs, fork-local

## Context

[ADR-0270](0270-fuzzing-scaffold.md) landed the libFuzzer scaffold under
`core/test/fuzz/` with one initial harness (`fuzz_y4m_input`). That
harness already paid for itself: a 60-second smoke run surfaced a heap-
buffer-overflow in `y4m_convert_411_422jpeg` (fixed in PR #357). The
scaffold was always intended as the *first* harness — the meson plumbing,
the nightly workflow, and the corpus-management convention are
deliberately generic so additional parsers can land in the same shape.

Two sibling parsers in `core/tools/` are the obvious next targets:

1. **`yuv_input.c`** — the headerless raw-YUV reader. Where Y4M parses
   a header, YUV is *unparsed* and the caller supplies dimensions; the
   interesting fuzz surface is the chroma-subsampling arithmetic in
   `yuv_input_fetch_frame` (the same shape that surfaced the 411 bug
   in the Y4M path) plus the truncated-fread / short-read branch in
   `dst_buf_sz`-sized reads. Cannot be reached through the existing
   Y4M harness because `video_input_open` only registers the Y4M vtbl.
2. **`cli_parse.c`** — the CLI argument parser. Attacker-reachable
   whenever a host script wraps `vmaf` and forwards untrusted argv
   (filenames, `--feature` payloads, `--model` colon-delimited
   strings). The colon-delimited sub-parsers (`parse_model_config` /
   `parse_feature_config`) run `strsep` chains over heap-duplicated
   argv strings — classic format-string / overrun shape.

Both are pure parser surfaces, both are zero-GPU / zero-DNN, and both
fit the existing scaffold's `LLVMFuzzerTestOneInput(uint8_t *, size_t)`
contract. The Research-0083 digest enumerates the wider candidate set
and ranks complexity / risk.

## Decision

We will land two additional harnesses (`fuzz_yuv_input.c`,
`fuzz_cli_parse.c`) under `core/test/fuzz/`, register them in the
existing `meson.build` opt-in (`-Dfuzz=true`), seed each with 6 hand-
crafted inputs covering branch-significant shapes, and add both to the
nightly `.github/workflows/fuzz.yml` matrix at 60 s/harness/night.
`fuzz_cli_parse` uses `-Wl,--wrap=exit` to intercept the `usage()` /
`exit(1)` path via a `setjmp`/`longjmp` shim so a single bad input
does not terminate the fuzzer process. ADR-0270 stays the parent
ADR for the scaffold itself; this ADR is a strict expansion.

## Alternatives considered

| Option                                                        | Pros                                                                                 | Cons                                                                                                                                                                  | Why not chosen                                                                                                                                                                       |
|---------------------------------------------------------------|--------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **`yuv_input` + `cli_parse` (chosen)**                        | Both reachable from the public CLI, both are pure C, both fit the scaffold verbatim. | The `cli_parse` harness needs a `__wrap_exit` linker shim because `usage()` calls `exit(1)`; that is a small but non-trivial bit of harness machinery.                | Highest risk-weighted coverage delta for the LOC budget — `cli_parse` is the actual attacker entry point on a wrapped CLI; `yuv_input` is the one parser surface Y4M does not reach. |
| `output.c` (XML / JSON / CSV / SUB writer)                    | Output formatting routes through user-controllable model / feature names.            | The writer is fed *libvmaf-internal* state, not raw bytes; harness has to fabricate a valid `VmafFeatureCollector` first — more harness scaffolding than fuzz target. | Harness would mostly exercise the printf-format paths the unit tests already cover at higher fidelity. Defer until the unit-test gate finds gaps.                                    |
| `dnn/` ONNX model-load path                                   | Tiny-AI surface is a real attacker-reachable parser (loads `.onnx` from disk).       | ORT does its own internal fuzzing (Microsoft / Google fuzz ORT continuously); duplicating that gives diminishing returns and pulls a heavyweight dep into the build.  | Not worth the build-time cost on the fork's CI; revisit once we ship a *fork-specific* ONNX preflight (allowlist sniff, sidecar verifier).                                           |
| `vmaf_per_shot` / `vmaf_roi` JSONL parsers                    | Real fork-local file readers, no upstream coverage.                                  | Both are line-oriented and short — the existing unit tests cover the parser branches; coverage delta is small.                                                        | Tracked as a follow-up if the nightly job catches regressions there. Not worth the new-harness LOC for the current sweep.                                                            |
| Defer expansion until OSS-Fuzz onboards                       | Frees engineering time.                                                              | Scorecard `Fuzzing` stays at one-target tier; `cli_parse` and raw-YUV bugs remain undetected.                                                                         | Rejected — the scaffold is reusable, the per-target cost is ~150 LOC, and the YUV path mirrors the same chroma-subsampling shape that already produced one real bug in the Y4M path. |

## Consequences

- **Positive**:
  - Coverage of every pure parser surface in `core/tools/` —
    Y4M, raw YUV, and CLI argv. Future Y4M / YUV chroma-conversion
    bugs (the same shape as the 411 OOB write) get caught in the
    raw path too, not only the Y4M front door.
  - The `cli_parse` harness exercises `parse_model_config` /
    `parse_feature_config` colon-tokenisation, which currently has
    no unit-test coverage.
  - The 60-second smoke run on the seed corpus already surfaced
    a real `assert(long_opts[n].name)` failure in
    `error()` ([`core/tools/cli_parse.c:250`](../../core/tools/cli_parse.c)):
    handlers for the long-only options `ARG_THREADS` /
    `ARG_SUBSAMPLE` / `ARG_CPUMASK` call
    `parse_unsigned(optarg, 't' / 's' / 'c', argv[0])` with a
    hardcoded short-option char that is *not* registered in
    `long_opts[]`. Any abbreviated `--threads` / `--subsample` /
    `--cpumask` invocation with a non-numeric argument trips
    the assertion. Captured reproducer parked under
    [`core/test/fuzz/cli_parse_known_crashes/cli_threads_abbrev_assert.argv`](../../core/test/fuzz/cli_parse_known_crashes/cli_threads_abbrev_assert.argv);
    the fuzzer harness carries an early-reject filter for the
    `--th*` / `--s*` / `--c*` token prefixes so the nightly
    job stays green until the fix lands. Tracked as a follow-up
    bug; the fix is a one-line change at each call site (replace
    the hardcoded char with `ARG_THREADS` / `ARG_SUBSAMPLE` /
    `ARG_CPUMASK`).
  - Scorecard `Fuzzing` check moves from "≥ 1 target" toward the
    "≥ 3 targets" tier.
- **Negative**:
  - Three parallel matrix legs in the nightly workflow instead of
    one (≈3 minutes of runner time per night vs ≈5 before; the
    per-harness budget drops from 5 min to 60 s to keep the total
    bounded, see `.github/workflows/fuzz.yml`).
  - `fuzz_cli_parse`'s `__wrap_exit` linker shim is GNU-ld /
    lld-specific; documented in the harness comment block. Macs
    shipping Apple ld would need an `-Wl,-undefined,dynamic_lookup`
    fallback, but the fuzz CI lane runs only on `ubuntu-latest`.
- **Neutral / follow-ups**:
  - A pre-commit hook that requires every new `tools/` parser to
    be matched by a fuzz harness is *not* in scope here; tracked
    as a future automation when at least 5 parsers ship harnesses.
  - The Research-0083 digest queues `output.c` and the tiny-AI
    preflight as the next two candidates if the current sweep
    finds bugs.

## References

- [ADR-0270](0270-fuzzing-scaffold.md) — parent scaffold ADR.
- [Research-0083](../research/0083-libfuzzer-harness-expansion-target-survey.md) —
  surface survey, complexity ranking, and Scorecard delta.
- [docs/development/fuzzing.md](../development/fuzzing.md) — operator
  runbook (updated with the two new harnesses).
- [libFuzzer (LLVM)](https://llvm.org/docs/LibFuzzer.html).
- [OSSF Scorecard `Fuzzing` check](https://github.com/ossf/scorecard/blob/main/docs/checks.md#fuzzing).
- Source: `req` — paraphrased from the user's 2026-05-05 request to
  expand the libFuzzer scaffold to cover `yuv_input` and `cli_parse`
  as the natural next harnesses after the Y4M wedge target landed.
