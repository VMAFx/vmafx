# VMAFx

[![HISS-16 Compliant](https://img.shields.io/badge/Standards-HISS--16%20Compliant-brightgreen)](AGENTS.md)
[![Tests](https://github.com/VMAFx/vmafx/actions/workflows/tests-and-quality-gates.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/tests-and-quality-gates.yml)
[![Builds](https://github.com/VMAFx/vmafx/actions/workflows/libvmaf-build-matrix.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/libvmaf-build-matrix.yml)
[![Release](https://img.shields.io/github/v/release/VMAFx/vmafx?include_prereleases&sort=semver)](https://github.com/VMAFx/vmafx/releases)
[![License](https://img.shields.io/badge/License-EUPL--1.2%20%C2%B7%20BSD--2--Clause--Patent-blue.svg)](docs/adr/1250-eupl-fork-relicense.md)

VMAFx is a fork of [Netflix/vmaf](https://github.com/Netflix/vmaf) for
perceptual video quality assessment. It extends libvmaf with GPU and SIMD
backends, additional metrics, and tools for scoring and model workflows.

**[Full documentation](https://vmafx.github.io/vmafx/)** ·
[Documentation source](docs/index.md)

## Get started

Follow the [installation and source-build guide](docs/getting-started/index.md)
for your platform. For a container workflow, see [Docker](docs/usage/docker.md).

After installation, compare a matching reference and distorted Y4M pair:

```sh
vmaf --reference reference.y4m --distorted distorted.y4m --json --output scores.json
```

See the [CLI reference](docs/usage/cli.md) for raw YUV geometry, model selection,
backend selection and output options. For compressed inputs such as MP4, use
[FFmpeg integration](docs/usage/ffmpeg.md).

## Guides and reference

| Task | Documentation |
| --- | --- |
| Score videos and choose output formats | [CLI reference](docs/usage/cli.md) |
| Use VMAFx in FFmpeg | [FFmpeg guide](docs/usage/ffmpeg.md) |
| Select hardware and check feature coverage | [GPU and SIMD backends](docs/backends/index.md) |
| Choose metrics and extractor options | [Feature reference](docs/metrics/features.md) |
| Choose a scoring model | [Models](docs/models/overview.md) |
| Embed libvmaf in an application | [C API](docs/api/index.md) |
| Train and run ONNX quality models | [Tiny AI](docs/ai/index.md) |
| Connect scoring tools through MCP | [MCP servers](docs/mcp/index.md) |

Build requirements, backend limitations and model defaults live in these
guides so they can be maintained alongside their implementations.

## Contribute

Start with [CONTRIBUTING.md](CONTRIBUTING.md) for setup, required checks and
pull-request expectations. The [engineering principles](docs/principles.md)
cover coding and numerical-correctness standards; the
[repository guide](docs/architecture/index.md) explains the source layout.

- [Report a bug or request a feature](https://github.com/VMAFx/vmafx/issues)
- [Security reporting policy](SECURITY.md)
- [Support development](https://ko-fi.com/lusoris)

## Project status

- [Roadmap and release goals](docs/roadmap.md)
- [Releases and downloads](https://github.com/VMAFx/vmafx/releases)
- [Changelog](CHANGELOG.md)
- [Release process](docs/development/release.md)

## Upstream and license

VMAFx builds on [Netflix/vmaf](https://github.com/Netflix/vmaf).
See [upstream releases](https://github.com/Netflix/vmaf/releases) for Netflix's
release history.

The repository carries two sets of terms, separated by provenance and recorded
per file as an `SPDX-License-Identifier`
([ADR-1250](docs/adr/1250-eupl-fork-relicense.md)):

- **Code inherited, ported or translated from Netflix/vmaf or another project**
  keeps the terms it already carries — [BSD-2-Clause-Patent](LICENSE) for
  Netflix's code, and its own licence for the libjxl, Xiph and IQA code the fork
  builds on. Those files carry the original copyright notice.
- **Fork-authored code** is licensed under [EUPL-1.2](LICENSES/EUPL-1.2.txt), a
  reciprocal licence.

**What that means in practice**: because the shipped `libvmaf` links both
together, redistributing a modified library obliges you to offer its source under
EUPL-1.2. If you need permissive terms, use
[Netflix/vmaf](https://github.com/Netflix/vmaf) upstream, which is unaffected.
The per-file tags are authoritative; this paragraph is a summary.

## Standards & Governance

This repository conforms to High-Integrity Systems Standards (HISS-16)
and modernized NASA JPL Power-of-10 rules.

| Gate | Command | Description |
| :--- | :--- | :--- |
| **Verification** | `make verify-all` | Runs full audit, test suite, and context integrity check |
| **HISS Audit** | `standardsctl audit` | Enforces zero technical debt regression against baseline |
| **Context Sync** | `standardsctl compile-context` | Transpiles canonical `AGENTS.md` to all AI targets |
