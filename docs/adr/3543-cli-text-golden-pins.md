<!-- markdownlint-disable MD013 MD060 -->

# ADR-3543: User-facing CLI text is pinned against golden transcripts

- **Status**: Accepted
- **Date**: 2026-11-08
- **Deciders**: lusoris
- **Tags**: `testing`, `cli`, `tools`

## Context

The repository gap audit ([issue #3214](https://github.com/VMAFx/vmafx/issues/3214),
rows C15-01..C15-13, recorded 2026-10-13) found that the text a user actually
reads — the `vmaf`/`vmafx` help, the `--version` output, the usage blocks of
the `vmaf_roi`/`vmaf_vpl`/`vmaf-perShot` sidecars, and the option-error and
geometry-error transcripts — could change without any test noticing. The
closest guards miss it: the codegen unit tests
(`scripts/codegen/tests/test_vmafx_api_options.py`) shape synthetic fixtures,
not what a built binary serves; `dnn/test_cli.sh` greps three flag strings;
the error-path tests assert exit codes and single substrings.

Two families of text exist. The `vmaf` help and option tables are generated
(`core/tools/cli_options.gen.inc`, ADR-1852), so a codegen drift guard could
cover them — but it would still not verify the bytes the binary serves, nor
the argv[0] head, nor the alias behaviour. The sidecar usage blocks are
hand-written `fprintf` literals with no generation step at all; only an
end-to-end transcript pin can guard those.

## Decision

- The user-facing text of the CLI tools is pinned end to end by fast-suite
  tests that run the real binaries and diff their output against golden
  transcripts stored under `core/tools/test/golden/`:
  - `test_vmaf_cli_text_golden.sh` — `vmaf --help` (full transcript), the
    `--version` format on both binaries, the `vmafx` alias behaviour
    (byte-identical help modulo argv[0]; its own `VMAFX ... precision=max`
    version line), the option-error transcripts (`--width`, `--precision`,
    `--model`), and the raw-yuv geometry refusals.
  - `test_vmaf_roi_usage.sh` and `test_vmaf_vpl_usage.sh` — the hand-written
    `vmaf_roi` and `vmaf_vpl` usage blocks, as two separate tests so the roi
    golden is a real PASS in every build; the vpl test skips (exit 77)
    outside Linux SYCL builds and is verified by the Ubuntu SYCL lanes.
  - `test_vmaf_per_shot.sh` — the `vmaf-perShot` usage block, on both its
    `--help` (stdout, exit 0) and no-args (stderr, exit 1) paths. This one
    test sits in the `slow` suite, where the sidecar smoke it extends already
    runs; the other pins are in `fast`.
- An error transcript is pinned as its reason line plus the full usage dump
  (every `usage()` refusal ends with `print_usage`), so the help golden and
  the error cases share one source of truth.
- The first `Usage:` line embeds `argv[0]`, which varies across platforms
  and invocation paths; the goldens and the tests normalise it to
  `Usage: <app> ...`. The `--precision` refusal, which starts with
  `argv[0]:`, is normalised the same way. Captured output is stripped of
  `\r` before comparing, because the Windows CRT translates `\n` to CRLF
  through a pipe.
- A text change is made by consciously regenerating the golden in the same
  pull request; the diff then shows the exact user-visible change for
  review. The `vmaf_vpl` golden embeds the current
  `VMAF_DEFAULT_MODEL_VERSION` value on purpose: bumping the default model
  must regenerate it.
- The `vmaf --version` output is pinned as a format (semver optionally
  followed by the `-<N>-g<sha>` describe suffix), not a value — the value
  changes with every commit.

## Alternatives considered

- **Codegen idempotency check only** (regenerate `cli_options.gen.inc` in CI
  and diff). Rejected as the sole guard: it misses the hand-written sidecar
  texts, the argv[0]/alias behaviour, and everything the binary prints after
  codegen — the pin verifies what a user of the built binary actually sees.
- **Golden transcripts inside the test scripts** (heredocs). Rejected: the
  189-line help transcript would dominate the scripts and obscure the case
  logic; separate golden files keep each transcript diffable on its own.
- **Pin the full `Usage:` line including argv[0]** and run only from fixed
  paths. Rejected: the same transcript must hold on Linux CI (relative
  `./tools/vmaf`) and Windows local runs (absolute `.exe` paths), and a
  pin that only one platform can satisfy is a false failure generator.

## Consequences

- Every wording, flag-name, spacing or exit-code change to the pinned text
  surfaces as a named test failure (`test_vmaf_cli_text_golden`,
  `test_vmaf_roi_usage`, `test_vmaf_vpl_usage`, `test_vmaf_per_shot`)
  instead of reaching
  users silently.
- The goldens are regeneratable receipts: after an intended text change,
  re-run the tool with the normalised invocation and refresh
  `core/tools/test/golden/`.
- Closes audit rows C15-01, C15-02, C15-03, C15-07, C15-09, C15-10,
  C15-11, C15-12 and C15-13 of issue #3214. Rows C15-04/05/06 (JSON Schema,
  XSD and CSV-column validation) stay open — they need new validation
  dependencies, which is a separate decision. Row C15-08 no longer applies:
  master's `test_vmaf_read_error_exit.sh` already greps the user-facing
  stderr strings (`problem while reading pictures`, `ended before`).
