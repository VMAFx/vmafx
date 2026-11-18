# ADR-3559: Install CI ONNX Runtime from pinned NuGet packages

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
python3 scripts/ci/ort_nuget_install.py --package cpu --print-pin <version>
python3 scripts/ci/ort_nuget_install.py --package gpu --print-pin <version>
```

## Consequences

- The installer includes both upstream licence notices under its prefix.
- `scripts/ci/check_ort_tarball_pins.py` verifies that every workflow step
  calling the helper names a pinned `ORT_VERSION` for its package.
- Follow-up: `dev/Containerfile:749` (built by
  `.github/workflows/dev-container-build.yml` on pull requests that touch
  container inputs or run the full tier),
  `dev/AGENTS.d/runtime-dependencies.md:29`, and `renovate.json:603` still
  reference the old GitHub release tarball source.
