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
  pins. A container instruction that names the helper is accepted only as a
  plain `RUN` (no flags, exec form, or heredoc) whose continuation-joined body
  splits on `&&` into allow-listed simple commands: a helper call, an
  `export ORT_VERSION=<literal>` assignment, `set -e`/`set -eux`/`set -o
  pipefail`, or `ldconfig`. Every other command, including bare
  `ORT_VERSION=<literal>`, `.`/`source`, `--print-pin`, and inline overrides,
  is a shape finding at the instruction start.
- Helper calls may use `python3 <path>/scripts/ci/ort_nuget_install.py` or
  `python3 -m scripts.ci.ort_nuget_install`. Each helper flag may appear once;
  `--package` accepts `[A-Za-z0-9_-]+`, `--prefix` is required, and
  `--version` is a literal or `$ORT_VERSION`/`${ORT_VERSION}`. An
  `ORT_VERSION` exported in the same `RUN` wins; otherwise the guard reads
  `ARG`/`ENV` literals across the whole build stage up to the previous `FROM`,
  so a `WORKDIR` between the binding and the call no longer hides it.
- The guard reads tracked files only, so an untracked `Containerfile.bak` is
  outside the check. It does not follow a script that the image copies and
  runs (`COPY install.sh /install.sh`, `RUN sh /install.sh`); the installer
  still enforces its own pin and digest checks in that path.
- Follow-up: the `renovate.json` custom manager (`renovate.json:603-617`,
  `matchStrings` at `renovate.json:610-614` matching `ORT_SHA256`) still
  references the old GitHub release tarball source.

## References

- req (paraphrased, 2026-11-18, runtime fetch source): the maintainer asked
  for one shared helper that fetches ONNX Runtime from the NuGet package with
  a pinned version and sha256, retries, and a named error when the source is
  gone, with no silent fallback.
- [Installer](../../scripts/ci/ort_nuget_install.py),
  [pins](../../scripts/ci/ort-nuget-pins.json), and
  [guard](../../scripts/ci/check_ort_tarball_pins.py).
