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
- The development container uses the same installer. The guard checks old
  ONNX Runtime release sources, direct NuGet downloads, and helper version
  pins within the static scope below.

### Guard scope and limits

[`check_ort_tarball_pins.py`](../../scripts/ci/check_ort_tarball_pins.py)
implements bounded static checks for the repository's supported workflow and
container instruction forms. It is not a general shell or Dockerfile parser.
[`test_ort_tarball_single_source.py`](../../scripts/ci/tests/test_ort_tarball_single_source.py)
contains the guard's positive, negative, and boundary fixtures.

- Helper detection covers the entire logical container instruction, including
  a bare `ort_nuget_install.py` token and the module spelling
  `scripts.ci.ort_nuget_install`. A helper token in a later segment cannot
  hide an earlier unsupported command. Detection continues beyond the command
  parser's cap; a helper in that tail produces a limit finding at the
  instruction start rather than a successful scan.
- An instruction naming the helper is accepted only as a plain `RUN` (no
  flags, exec form, or heredoc). Only unquoted shell `&&` separates its simple
  commands; quoted `&&` remains part of an argument. Every segment must be an
  allowed helper call, `export ORT_VERSION=<literal>`,
  `set -e`/`set -eux`/`set -o pipefail`, or `ldconfig`. Other commands,
  including bare `ORT_VERSION=<literal>`, `.`/`source`, `--print-pin`, and
  inline overrides, produce a shape finding at the instruction start.
- Calls may use `python3 scripts/ci/ort_nuget_install.py`,
  `python3 /build/vmaf/scripts/ci/ort_nuget_install.py`, or
  `python3 -m scripts.ci.ort_nuget_install`. Each allowed helper flag may occur
  once; `--package` accepts `[A-Za-z0-9_-]+`, `--prefix` is required, and
  `--version` is a literal or an unquoted or double-quoted
  `$ORT_VERSION`/`${ORT_VERSION}`. Single-quoted variable text does not expand
  and is rejected.
- Each call uses its preceding supported `RUN` export, if present; otherwise
  it uses the effective `ORT_VERSION` before that instruction in the same
  build stage. The last assignment within `ARG` or `ENV` wins, and `ENV`
  takes precedence over `ARG`. Intervening instructions such as `WORKDIR`
  do not hide bindings. Only real `FROM` instructions establish stage
  boundaries: `FROM` text in a continuation or heredoc body does not.
  Unsupported dynamic values that affect version resolution fail closed.
- Continuation joining ignores blank and full-line comment lines consistently
  for helper detection, source checks, and stage bindings. Old release
  addresses split across `RUN` or `ADD` continuations remain forbidden.
  For every recognized NuGet URL, the guard checks the package segment. In
  container files, a referenced variable resolves to the stage value effective
  at that instruction; workflow references resolve against their owning step's
  bindings. A direct ONNX Runtime package URL or unresolved package variable
  is a finding; an unrelated variable containing an ONNX Runtime package name
  does not make a different package's URL a finding.
- ClangBuildAnalyzer retains its digest check. Checksum and extraction order
  is checked after the target download across logical `RUN` instructions in
  the same build stage, stopping before the next real `FROM`. Unrelated
  earlier checksum or `tar` commands do not satisfy or invalidate that
  download's order check. Comments and heredoc payloads do not provide
  executable checksum evidence. Workflow checks remain bounded by their step.
  Heredoc body newlines remain separate during source detection; only header
  continuations are joined.
- Repository discovery uses Git-tracked workflow files and container files
  named exactly `Dockerfile` or `Containerfile`, prefixed by `Dockerfile.` or
  `Containerfile.`, or suffixed by `.Dockerfile` or `.Containerfile` (including
  `docker/dev/*.Dockerfile`). Container names take precedence over `.yml`/
  `.yaml`: `Containerfile.yaml` uses the container checks. Untracked
  backups are outside repository discovery. Recognized Git discovery
  failures return exit 2; glob discovery is reserved for non-Git fixture
  directories. Explicit file arguments select the supplied files.
- The guard does not read scripts copied into an image and invoked with
  `RUN sh /install.sh`, or follow other cross-file shell calls. For example,
  `COPY install.sh /install.sh` followed by `RUN sh /install.sh` does not
  expose that script's contents to these checks. If that script calls the
  shared installer, the installer still enforces its own size and digest
  pins at execution time.

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
