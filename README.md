# VMAFx

[![Tests](https://github.com/VMAFx/vmafx/actions/workflows/tests-and-quality-gates.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/tests-and-quality-gates.yml)
[![Builds](https://github.com/VMAFx/vmafx/actions/workflows/libvmaf-build-matrix.yml/badge.svg)](https://github.com/VMAFx/vmafx/actions/workflows/libvmaf-build-matrix.yml)
[![Release](https://img.shields.io/github/v/release/VMAFx/vmafx?include_prereleases&sort=semver)](https://github.com/VMAFx/vmafx/releases)
[![License](https://img.shields.io/badge/License-BSD--2--Clause--Patent-blue.svg)](LICENSE)

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

The repository uses the [BSD-2-Clause-Patent license](LICENSE), preserving
upstream attribution and the license's patent grant.
