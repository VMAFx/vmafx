<!-- markdownlint-disable MD060 -->
# Research-0083: libFuzzer harness expansion — target survey

- **Date**: 2026-05-05
- **Author**: lusoris, Claude
- **Companion ADR**: [ADR-0311](../adr/0311-libfuzzer-harness-expansion.md)
- **Parent**: [ADR-0270](../adr/0270-fuzzing-scaffold.md), [Research-0059](0059-libfuzzer-scaffold-y4m.md)

## Problem statement

ADR-0270 landed `fuzz_y4m_input` and the generic libFuzzer scaffold.
That harness already surfaced one real heap-buffer-overflow on its
60-second smoke run. The scaffold is generic; the question is which
parsers in the libvmaf tree are worth fuzzing next, ranked by
expected bug density vs harness-authoring cost.

## Surface inventory

The fuzz-amenable surfaces in libvmaf split into four buckets:

### Bucket A — pure-C parsers in `core/tools/` (text + binary input)

| Surface                                                | Shape                                                                 | Reachability                                       | Harness LOC | Risk class           |
|--------------------------------------------------------|-----------------------------------------------------------------------|----------------------------------------------------|-------------|----------------------|
| `tools/y4m_input.c` — Y4M parser                       | sscanf header + memcpy chroma_type + size-derived malloc + fread body | Direct (CLI takes filesystem path)                 | ~140 (done) | High (already found 1 bug) |
| `tools/yuv_input.c` — raw-YUV reader                   | fread fixed-size frame body, chroma-subsampling arithmetic            | Direct (CLI takes filesystem path)                 | ~120        | Medium-high          |
| `tools/cli_parse.c` — argv tokeniser + colon parsers   | getopt_long + strsep over heap-dup'd `--feature` / `--model` strings  | Direct (host scripts forward untrusted argv)       | ~160        | Medium-high          |
| `tools/vmaf_per_shot.c` — per-shot JSONL writer/parser | Line-at-a-time fgets + sscanf                                         | Indirect (only consumes its own emitted JSONL)     | ~110        | Low (closed-loop)    |
| `tools/vmaf_roi.c` — per-CTU QP offset writer/parser   | Line-at-a-time fgets + atoi                                           | Indirect (consumes well-formed input from sidecar) | ~110        | Low                  |

### Bucket B — `core/src/` library-internal parsers

| Surface                                                          | Shape                                              | Reachability                          | Harness LOC | Risk class                |
|------------------------------------------------------------------|----------------------------------------------------|---------------------------------------|-------------|---------------------------|
| `src/output.c` — XML / JSON / CSV / SUB writers                  | printf-driven format string formatting             | Indirect (writes libvmaf-internal state) | ~250        | Low (writers, not parsers) |
| `src/model.c` — `.json` / `.pkl` model loader                    | parson JSON parser + libsvm `.pkl` reader          | Direct (`-m path=...` from CLI)       | ~180        | Medium                    |
| `src/dnn/` — ONNX model-load preflight                           | ORT-fronted .onnx parse + custom op-allowlist sniff | Direct (`--tiny-model path=...`)      | ~200        | Low (ORT itself fuzzed)   |
| `src/feature/cambi.c` — TVI LUT parser                           | sscanf over per-line LUT files                     | Indirect (built-in LUTs only by default) | ~130        | Low                       |

### Bucket C — Fork-local extensions

| Surface                                                              | Shape                                          | Reachability                          | Harness LOC | Risk class      |
|----------------------------------------------------------------------|------------------------------------------------|---------------------------------------|-------------|-----------------|
| `mcp-server/vmaf-mcp/` — MCP JSON-RPC server                         | jsonrpc-2.0 line-delimited parser              | Direct (network, when run as daemon)  | Python harness — out of scope for libFuzzer | High            |
| `tools/vmaf-tune/` JSONL corpus reader                               | Python — out of scope for libFuzzer            | n/a                                   | n/a         | n/a             |
| `src/dnn/sidecar.c` — model sidecar JSON loader                      | parson JSON, schema-validated                  | Direct (`--tiny-model` runtime sidecar) | ~140       | Medium          |

### Bucket D — Upstream-mirror code excluded from fork-local sweep

`src/feature/*.c` math kernels, SIMD paths, GPU kernels: not
parsers, not in scope.

## Ranking

| # | Target           | Bucket | Cost | Risk | Coverage delta vs Y4M | Decision                   |
|---|------------------|--------|------|------|-----------------------|----------------------------|
| 1 | `fuzz_yuv_input` | A      | ~120 LOC | Medium-high | High — chroma-subsampling arithmetic shape, not reachable through Y4M vtbl | **Land in this PR**         |
| 2 | `fuzz_cli_parse` | A      | ~160 LOC | Medium-high | High — argv tokeniser + colon parsers, no overlap with parser bytes        | **Land in this PR**         |
| 3 | `fuzz_model_load` | B     | ~180 LOC | Medium | Medium — parson is mature, but `.pkl` libsvm path is unaudited                | **Defer to follow-up PR**   |
| 4 | `fuzz_sidecar`    | C     | ~140 LOC | Medium | Medium — fork-local, ships in the tiny-AI flow                                | **Defer to follow-up PR**   |
| 5 | `fuzz_per_shot`   | A     | ~110 LOC | Low    | Low — closed-loop, low attacker leverage                                      | **Backlog**                 |
| 6 | `fuzz_output`     | B     | ~250 LOC | Low    | Low — writers, not parsers                                                    | **Backlog**                 |
| 7 | `fuzz_dnn_load`   | B     | ~200 LOC | Low    | Low — ORT is fuzzed continuously upstream                                     | **Backlog (revisit on PTQ)** |

The chosen pair (#1, #2) gives the highest risk-weighted coverage
delta for the smallest harness LOC; the deferred pair (#3, #4) is
fork-local enough to be worth its own follow-up but does not unblock
this PR.

## OSSF Scorecard delta

The Scorecard `Fuzzing` check has three buckets:

- **0/10**: no fuzz target in repo, no OSS-Fuzz integration.
- **Partial**: at least one harness present (where we are post-ADR-0270).
- **Full**: comprehensive coverage *or* OSS-Fuzz onboarded.

Going from one harness to three does **not** automatically flip the
score to "full" — Scorecard counts presence, not breadth — but it
demonstrates a maintained fuzz-test discipline that reviewers can
verify by reading the matrix. Onboarding to OSS-Fuzz remains the
canonical "full" path; that is tracked as a separate follow-up
(see ADR-0270 §Alternatives).

## Smoke-run command (ADR-0311 reproducer)

```bash
CC=clang CXX=clang++ \
  meson setup build-fuzz libvmaf \
    --buildtype=debug \
    -Db_sanitize=address \
    -Db_lundef=false \
    -Dfuzz=true \
    -Denable_cuda=false -Denable_sycl=false -Denable_vulkan=disabled

ninja -C build-fuzz \
    test/fuzz/fuzz_y4m_input \
    test/fuzz/fuzz_yuv_input \
    test/fuzz/fuzz_cli_parse

mkdir -p /tmp/fuzz-smoke-yuv /tmp/fuzz-smoke-cli

./build-fuzz/test/fuzz/fuzz_yuv_input \
    -seed=0 -runs=1000 \
    /tmp/fuzz-smoke-yuv core/test/fuzz/yuv_input_corpus/

./build-fuzz/test/fuzz/fuzz_cli_parse \
    -seed=0 -runs=1000 \
    /tmp/fuzz-smoke-cli core/test/fuzz/cli_parse_corpus/
```

A clean smoke prints `Done 1000 runs in <N> second(s)` with no
crash artefacts. The 60-second nightly run replaces `-runs=1000`
with `-max_total_time=60`.

## Findings (preliminary)

- The 1000-run dry smoke on the seed corpora produces no crashes
  on either new harness. Real bug-finding requires the nightly
  60-second run (≈ 1M+ iterations) to drive past the seed
  shapes.
- The `__wrap_exit` longjmp shim in `fuzz_cli_parse.c` is the
  least-conventional bit; it is documented inline and isolated to
  the harness file so future maintainers reading the scaffold
  have a single grep-target for the pattern.

## Status update — 2026-05-30 (ADR-0882)

Deferred targets **#3 `fuzz_model_load`** and **#4 `fuzz_sidecar`**
landed under [ADR-0882](../adr/0882-fuzz-target-audit-json-model-sidecar.md)
as `fuzz_json_model` (libvmaf SVM model JSON parser) and
`fuzz_dnn_sidecar` (tiny-AI sidecar JSON loader). First-run
findings:

- `fuzz_json_model`: surfaced a heap-buffer-overflow in
  `vmaf_model_destroy` (`core/src/model.c:210`) — `parse_slopes`
  grows `feature_cap` via `ensure_feature_capacity` without
  updating `n_features`, so destruction walks past the
  initialised region. Reproducer committed under
  `core/test/fuzz/json_model_known_crashes/slopes_oob_destroy.bin`.
  Tracked as T-JSON-MODEL-SLOPES-FEATURE-CAP-OOB-2026-05-30 in
  [`docs/state.md`](../state.md); per
  [ADR-0404](../adr/0404-nightly-fuzz-triage-keep-gates.md) the
  nightly leg stays on until the fix lands.
- `fuzz_dnn_sidecar`: 3.95 M iterations clean in a 30-second
  local smoke run; no crashes surfaced.

Rows **#5 `fuzz_per_shot`**, **#6 `fuzz_output`**, and **#7
`fuzz_dnn_load`** remain Backlog per the original ranking.

## References

- [ADR-0270](../adr/0270-fuzzing-scaffold.md) — parent scaffold.
- [ADR-0311](../adr/0311-libfuzzer-harness-expansion.md) —
  this expansion's decision record.
- [Research-0059](0059-libfuzzer-scaffold-y4m.md) — original
  Y4M-target surface survey.
- [libFuzzer (LLVM)](https://llvm.org/docs/LibFuzzer.html).
- [OSSF Scorecard `Fuzzing` check](https://github.com/ossf/scorecard/blob/main/docs/checks.md#fuzzing).
