# ADR-3561: Install CI ONNX Runtime from pinned NuGet packages

- **Status**: Accepted
- **Date**: 2026-11-18
- **Deciders**: VMAFx maintainers
- **Tags**: `ci`, `ai`, `supply-chain`

## Context

On 2026-11-18 the GitHub release source for the Linux ONNX Runtime
archives returned 404. The CPU and GPU coverage CI jobs need the C API,
shared libraries, and pkg-config metadata to exercise DNN builds.

## Decision

Use the `microsoft.ml.onnxruntime` (CPU) and
`microsoft.ml.onnxruntime.gpu.linux` (GPU; `microsoft.ml.onnxruntime.gpu`
is a meta package without libraries) NuGet packages, pinned per package by
source address pattern, header directory, library allow-list, and per
version size and SHA-256 in `scripts/ci/ort-nuget-pins.json`.

`scripts/ci/ort_nuget_install.py` selects `--package cpu|gpu`, checks the
package before extracting allow-listed files, and fails closed when the
source is gone or a pin does not match. With `--cache-dir`, a cached
package is reused only after passing the same size and SHA-256 check; a
mismatching cached file is deleted and fetched again once.

## Alternatives considered

| Option | Benefit | Reason not chosen |
| --- | --- | --- |
| Mirror | Decouples availability | Adds a service and a provenance chain |
| PyPI wheel | Available to Python jobs | Does not provide the same C SDK layout |
| Distribution package | Uses system tooling | Version and availability vary by runner |
| Wait | No source migration | Leaves affected CI jobs failing |

## Raising the version

Print the size and SHA-256 pin entry for a new version without writing
files or modifying `scripts/ci/ort-nuget-pins.json`:

```bash
python3 scripts/ci/ort_nuget_install.py --package cpu --print-pin 1.30.0
python3 scripts/ci/ort_nuget_install.py --package gpu --print-pin 1.30.0
```

Replace `1.30.0` with the version to measure.

## Consequences

- The installer includes both upstream licence notices under its prefix.
- `scripts/ci/check_ort_tarball_pins.py` verifies that every workflow step
  calling the helper names a pinned `ORT_VERSION` for its package.
- The development container uses the same installer; the guard checks tracked
  container files for old sources, direct NuGet downloads, and helper version
  pins, refusing addresses split across `RUN` or `ADD` continuations with
  comments and allowing container `ort_nuget_install.py` instructions only in
  the canonical `RUN` grammar (`RUN` without flags, exec form `RUN [`, or
  heredoc `<<`; continuation-joined body split on `&&` only; refusing `;`,
  `||`, `|`, `&`, `(`, `)`, `` ` ``, `$(`, `{` or `}` outside
  `${ORT_VERSION}`, `eval`, `sh -c`, `bash -c`, `env`, and quotes enclosing
  the helper token; each helper command matching `[ORT_VERSION=<literal>]`
  followed by `python3 [/build/vmaf/]scripts/ci/ort_nuget_install.py <flags>`
  with `/build/vmaf/` permitted for `dev/Containerfile:744`, unique helper
  argument-parser flags, and `--version` in `<literal>`, `$ORT_VERSION`,
  `${ORT_VERSION}`, `"${ORT_VERSION}"`, or `"$ORT_VERSION"`; and any
  non-helper `ORT_VERSION` segment matching `[export ]ORT_VERSION=<literal>`).
- Renovate tracks the `tracked.cpu` and `tracked.gpu` anchors in
  `scripts/ci/ort-nuget-pins.json` with the `nuget` datasource. A bump
  changes only the anchor; the guard refuses a tracked version without a
  size/SHA-256 entry, so the update path is to run `--print-pin`, add the
  entry, then raise the consumer `ORT_VERSION` sites.

## References

- req (paraphrased, 2026-11-18, runtime fetch source): the maintainer asked
  for one shared helper that fetches ONNX Runtime from the NuGet package with
  a pinned version and sha256, retries, and a named error when the source is
  gone, with no silent fallback.
- [Installer](../../scripts/ci/ort_nuget_install.py),
  [pins](../../scripts/ci/ort-nuget-pins.json), and
  [guard](../../scripts/ci/check_ort_tarball_pins.py).
