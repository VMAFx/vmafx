<!-- markdownlint-disable MD060 -->
# Fuzzing libvmaf

Operator runbook for the libFuzzer harnesses under
[`core/test/fuzz/`](../../core/test/fuzz/). Tracked under
[ADR-0270](../adr/0270-fuzzing-scaffold.md) (initial scaffold),
[ADR-0311](../adr/0311-libfuzzer-harness-expansion.md) (`fuzz_yuv_input`
and `fuzz_cli_parse` expansion), and
[ADR-0882](../adr/0882-fuzz-target-audit-json-model-sidecar.md)
(`fuzz_json_model` + `fuzz_dnn_sidecar` audit). The harnesses
satisfy the OSSF Scorecard
[`Fuzzing`](https://github.com/ossf/scorecard/blob/main/docs/checks.md#fuzzing)
check.

## What is shipped

| Harness            | Surface                                                                                                       | Source                                                                         | Seed corpus                                                                  | Known crashes                                                                                  |
|--------------------|---------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------|------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------|
| `fuzz_y4m_input`   | YUV4MPEG2 parser exposed via `video_input_open` / `_fetch_frame` / `_close`                                   | [`core/test/fuzz/fuzz_y4m_input.c`](../../core/test/fuzz/fuzz_y4m_input.c)     | [`y4m_input_corpus/`](../../core/test/fuzz/y4m_input_corpus/) (6 seeds)      | 1 (411-chroma OOB write — see ADR-0270 §Consequences).                                         |
| `fuzz_yuv_input`   | Headerless raw-YUV reader exposed via `raw_input_open` / `_fetch_frame`                                       | [`core/test/fuzz/fuzz_yuv_input.c`](../../core/test/fuzz/fuzz_yuv_input.c)     | [`yuv_input_corpus/`](../../core/test/fuzz/yuv_input_corpus/) (6 seeds)      | 0                                                                                              |
| `fuzz_cli_parse`   | `cli_parse` argv tokeniser + colon-delimited `--feature` / `--model` parser                                   | [`core/test/fuzz/fuzz_cli_parse.c`](../../core/test/fuzz/fuzz_cli_parse.c)     | [`cli_parse_corpus/`](../../core/test/fuzz/cli_parse_corpus/) (6 seeds)      | 1 (`--threads=<garbage>` abbreviation tripping `error()` assert — see ADR-0311 §Consequences). |
| `fuzz_json_model`  | `vmaf_read_json_model_from_buffer` + collection variant (SVM model JSON parser, `core/src/read_json_model.c`) | [`core/test/fuzz/fuzz_json_model.c`](../../core/test/fuzz/fuzz_json_model.c)   | [`json_model_corpus/`](../../core/test/fuzz/json_model_corpus/) (4 seeds)    | 1 (`parse_slopes` outruns `feature_names` → `vmaf_model_destroy` OOB — see ADR-0882 + T-JSON-MODEL-SLOPES-FEATURE-CAP-OOB-2026-05-30). |
| `fuzz_dnn_sidecar` | `vmaf_dnn_sidecar_load` (tiny-AI sidecar JSON parser, `core/src/dnn/model_loader.c`)                          | [`core/test/fuzz/fuzz_dnn_sidecar.c`](../../core/test/fuzz/fuzz_dnn_sidecar.c) | [`dnn_sidecar_corpus/`](../../core/test/fuzz/dnn_sidecar_corpus/) (5 seeds) | 0                                                                                              |

New harnesses follow the README at
[`core/test/fuzz/README.md`](../../core/test/fuzz/README.md).

## Build the harness

The fuzz harnesses are opt-in and require **clang** (libFuzzer is
a clang-only feature). They pair best with AddressSanitizer.

```bash
CC=clang CXX=clang++ \
  meson setup build-fuzz core \
    --buildtype=debug \
    -Db_sanitize=address \
    -Db_lundef=false \
    -Db_lto=false \
    -Dfuzz=true \
    -Denable_cuda=false -Denable_sycl=false
ninja -C build-fuzz test/fuzz/fuzz_y4m_input \
                    test/fuzz/fuzz_yuv_input \
                    test/fuzz/fuzz_cli_parse \
                    test/fuzz/fuzz_json_model \
                    test/fuzz/fuzz_dnn_sidecar
```

Three non-default Meson flags are load-bearing:

- `-Dfuzz=true` — opts the `core/test/fuzz/` subdirectory into
  the build (default `false`).
- `-Db_lundef=false` — clang's libFuzzer runtime defines symbols
  that resolve at final-link time; the default `b_lundef=true`
  errors them out at setup. The harness `meson.build` would
  emit a clear warning at setup time if this is forgotten.
- `-Db_lto=false` — the json_model + dnn_sidecar harnesses
  (ADR-0882) compile parser sources directly into the harness
  binary. With LTO on, ASan's module-dtor sections are discarded
  at link time on the larger source set, producing a hard
  linker error.

## Run a 60-second smoke

Each harness is independent; pick one, or run all three back-to-back:

```bash
mkdir -p /tmp/fuzz-smoke-y4m /tmp/fuzz-smoke-yuv /tmp/fuzz-smoke-cli

./build-fuzz/test/fuzz/fuzz_y4m_input \
    -max_total_time=60 -rss_limit_mb=2048 -malloc_limit_mb=1024 -timeout=10 \
    /tmp/fuzz-smoke-y4m core/test/fuzz/y4m_input_corpus/

./build-fuzz/test/fuzz/fuzz_yuv_input \
    -max_total_time=60 -rss_limit_mb=2048 -malloc_limit_mb=1024 -timeout=10 \
    /tmp/fuzz-smoke-yuv core/test/fuzz/yuv_input_corpus/

./build-fuzz/test/fuzz/fuzz_cli_parse \
    -max_total_time=60 -rss_limit_mb=2048 -malloc_limit_mb=1024 -timeout=10 \
    /tmp/fuzz-smoke-cli core/test/fuzz/cli_parse_corpus/
```

Expected output on a clean run:

```text
INFO: Running with entropic power schedule (0xFF, 100).
…
Done <N> runs in 60 second(s)
```

If the harness aborts with `==<pid>==ERROR: AddressSanitizer …`
and writes a `crash-<sha>` / `oom-<sha>` / `timeout-<sha>` file
in the working directory, treat that as a real bug. Re-run the
single artefact for a clean stack trace:

```bash
./build-fuzz/test/fuzz/fuzz_y4m_input crash-<sha>
```

Then file the bug per the
[bug-tracking workflow in `docs/state.md`](../state.md), park the
reproducer under `core/test/fuzz/<target>_known_crashes/`
(see [`core/test/fuzz/README.md` § Known crashes](../../core/test/fuzz/README.md#known-crashes))
so the regression is caught the moment the fix lands.

## Continuous fuzzing in CI

The [`fuzz.yml` GitHub Actions workflow](../../.github/workflows/fuzz.yml)
runs each harness for 5 minutes per night against the committed
seed corpus and uploads any crash / oom / timeout artefacts. It is
the gate that satisfies the Scorecard `Fuzzing` check. Adjust the
nightly duration via the workflow's `MAX_TOTAL_TIME` env, not by
editing the harness invocations.

## Adding a new harness

See the step list in
[`core/test/fuzz/README.md` § Add a new harness](../../core/test/fuzz/README.md#add-a-new-harness).
The summary is: drop `fuzz_<target>.c` next to the existing
harnesses, add an `executable(...)` block in
[`core/test/fuzz/meson.build`](../../core/test/fuzz/meson.build),
ship a small seed corpus under `<target>_corpus/`, register the
target in the matrix in `.github/workflows/fuzz.yml`, and update
the table at the top of this file.

## Known limitations

- The fuzz build is x86_64 / aarch64 + clang only. gcc has no
  libFuzzer; the Meson option errors cleanly when `cc.get_id()`
  is not `clang`.
- The harness caps input size at 64 KiB and rejects header lines
  whose `W` / `H` tag has more than 6 consecutive digits. This
  is a *fuzzer-stability* bound to keep allocator-probe inputs
  from dominating the corpus, not a real-world cap on the
  parser. Real bugs reachable through unbounded dimensions are
  still in scope; we just don't waste fuzzer cycles probing
  malloc-fragmentation paths.
- Coverage feedback is libFuzzer's intrinsic edge counter; we
  do not produce an LCOV report from fuzz runs today. Coverage
  is exercised separately by the unit-test gate.

## References

- [ADR-0270](../adr/0270-fuzzing-scaffold.md) — decision matrix
  and rejected alternatives (OSS-Fuzz onboarding, AFL++,
  defer-until-OSS-Fuzz, driver-only psnr_y harness).
- [Research digest 0054](../research/0059-libfuzzer-scaffold-y4m.md)
  — surface survey, smoke-run command, and the 411-chroma OOB
  finding.
- [libFuzzer (LLVM)](https://llvm.org/docs/LibFuzzer.html).
- [OSSF Scorecard `Fuzzing` check](https://github.com/ossf/scorecard/blob/main/docs/checks.md#fuzzing).
