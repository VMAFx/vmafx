# Licensing

VMAFx is a fork of Netflix's libvmaf, and its files carry different licences.
Each file's `SPDX-License-Identifier` header decides which one applies to it
([ADR-1250](adr/1250-eupl-fork-relicense.md)):

- files the fork wrote are under the **EUPL-1.2**;
- files inherited, ported or translated from Netflix keep Netflix's
  **BSD-2-Clause-Patent** licence and copyright notice;
- code from other projects keeps its own terms, named in the file: IQA
  (BSD-3-Clause), Xiph and dav1d (BSD-2-Clause), x264's assembly macros (ISC),
  the CIEDE2000 maths (MIT), libsvm and libjxl (BSD-3-Clause).

Because `libvmaf` links fork code and Netflix code into one library, a copy of
`libvmaf` you distribute, modified or not, has to come with its source or a
pointer to it, and a modified one is distributed under the EUPL-1.2. The
[next section](#embedding-vmafx-in-another-product) goes through the cases.

The licence texts are in the repository
([ADR-1699](adr/1699-root-licence-files-eupl.md)):

| File | What it holds |
| --- | --- |
| [`LICENSE`](https://github.com/VMAFx/vmafx/blob/master/LICENSE) | the EUPL-1.2, the licence of the files the fork wrote (the same bytes as `LICENSES/EUPL-1.2.txt`) |
| [`NOTICE`](https://github.com/VMAFx/vmafx/blob/master/NOTICE) | Netflix's BSD-2-Clause-Patent text with its copyright notice, for the files inherited from Netflix |
| [`LICENSES/`](https://github.com/VMAFx/vmafx/tree/master/LICENSES) | every licence a file names |

No other file at the root is a licence file. `NOTICE` carries Netflix's text
under a name licence detectors do not read as a licence file, so `LICENSE` is
the only root licence file they find; each file's own header still decides.
Files without a header of their own, such as model data, are covered by
[`REUSE.toml`](https://github.com/VMAFx/vmafx/blob/master/REUSE.toml).

## Embedding VMAFx in another product

!!! note "Not legal advice"
    This section repeats what the two licence texts say and how the European
    Commission reads the EUPL. It is not legal advice. For a decision about
    your product, ask a lawyer.

The licences stay as they are; there is no dual or commercial licence
([ADR-1685](adr/1685-post-1-0-embedding-zero-copy-milestone.md)). Each public
header carries its own SPDX tag. `libvmaf.h`, `model.h`,
`picture.h`, `feature.h`, `macros.h` and `libvmaf_cuda.h` are Netflix's
(BSD-2-Clause-Patent). `libvmaf_sycl.h`, `libvmaf_hip.h`, `libvmaf_metal.h`,
`picture_v2.h`, `dnn.h`, `perceptual_weight.h`, `vmaf_assert.h` and
`libvmaf_mcp.h` are the fork's (EUPL-1.2). The library behind them contains
both, so an embedder meets both licences:

| What you do | Netflix files (BSD-2-Clause-Patent) | Fork files (EUPL-1.2) |
| --- | --- | --- |
| Distribute `libvmaf` or the `vmaf` binary, modified or not, inside your product | Reproduce the copyright notice, the conditions and the disclaimer in the documentation or other materials you ship ([`NOTICE`](https://github.com/VMAFx/vmafx/blob/master/NOTICE), condition 2) | Keep every notice and include a copy of the licence (Article 5, "Attribution right"). Provide the source, or name a repository where it is "easily and freely available", for as long as you distribute (Article 5, "Provision of Source Code") |
| Link your program against `libvmaf`, statically or dynamically | No condition on your program | The licence leaves what counts as a derivative work to the copyright law of the country in Article 15 (Article 1, "Derivative Works"). The Commission's reading: static and dynamic linking create no condition on the other program. There is no case law on it |
| Change `libvmaf` and distribute the result | As above | Distribute the changed library under the EUPL-1.2. If you combine it with a work under a licence in the EUPL's appendix (GPL, LGPL, MPL and others), you may use that licence instead. Mark the changed work as modified, with the date, and provide its source (Article 5) |
| Run it on a server and let others use it over a network | No condition | "Distribution or Communication" includes "providing access to its essential functionalities" (Article 1), so the source obligation applies to that service too |
| Patents | Each contributor grants a patent licence for its contributions (the licence's patent paragraph) | The licensor grants use of its patents "to the extent necessary to make use of the rights granted" (Article 2) |

Three more points:

- Using an unmodified library still obliges you to point to the source. The
  copyleft and modification duties begin only when you change the library.
- The project publishes `THIRD_PARTY_NOTICES.txt` and `licenses.tar.gz` next
  to the release files, and the release's source archives. Those are the
  notices, licence texts and source of that build, ready to pass on (see
  [What the published packages carry](#what-the-published-packages-carry)).
- A GPU build also needs the vendor's runtime (CUDA driver, Level Zero and the
  oneAPI SYCL runtime, ROCm), each under its vendor's own terms.

Sources (fetched 2026-10-05):

- EUPL-1.2, official English text:
  [interoperable-europe.ec.europa.eu, `EUPL-1.2 EN.txt`](https://interoperable-europe.ec.europa.eu/sites/default/files/custom-page/attachment/2020-03/EUPL-1.2%20EN.txt).
  The repository copy, [`LICENSES/EUPL-1.2.txt`](https://github.com/VMAFx/vmafx/blob/master/LICENSES/EUPL-1.2.txt),
  has the same words and differs only in dashes and spacing. Article 5,
  "Provision of Source Code": "When distributing or communicating copies of
  the Work, the Licensee will provide a machine-readable copy of the Source
  Code or indicate a repository where this Source will be easily and freely
  available for as long as the Licensee continues to distribute or communicate
  the Work."
- The Commission's EUPL FAQ,
  [interoperable-europe.ec.europa.eu/collection/eupl/faqs](https://interoperable-europe.ec.europa.eu/collection/eupl/faqs):
  "The EUPL is not viral: according to the provision of European Law
  (Directive EC 2009/24 recitals 10 & 15), static and dynamic linking can be
  implemented with other programs without barriers or conditions."
- The Commission's compatibility matrix,
  [interoperable-europe.ec.europa.eu/collection/eupl/matrix-eupl-compatible-open-source-licences](https://interoperable-europe.ec.europa.eu/collection/eupl/matrix-eupl-compatible-open-source-licences):
  "Since European case law is generally missing, the matrix suggests
  reasonable guidance without providing a guarantee that this suggestion will
  always be followed by a judge".
- BSD-2-Clause-Patent, SPDX text:
  [github.com/spdx/license-list-data](https://github.com/spdx/license-list-data/blob/main/text/BSD-2-Clause-Patent.txt),
  the same conditions and patent grant as the repository's
  [`NOTICE`](https://github.com/VMAFx/vmafx/blob/master/NOTICE).

## Models

The VMAF models (`model/*.json`, `*.pkl`) are Netflix's (BSD-2-Clause-Patent).
The BRISQUE model is the LIVE laboratory's release, used under its notice as
written ([ADR-1507](adr/1507-brisque-live-notice-terms.md)): a publication that
reports research using it acknowledges LIVE and CPS at UT Austin and cites the
two works the notice names. The tiny-AI models list their licence and upstream
in [`model/tiny/registry.json`](https://github.com/VMAFx/vmafx/blob/master/model/tiny/registry.json)
and in their model cards ([model registry](ai/model-registry.md)). Three of them
carry upstream weights and keep their authors' terms: FastDVDnet (MIT, Matias
Tassano), TransNet V2 (MIT, Tomáš Souček) and LPIPS-SqueezeNet (BSD-2-Clause,
Zhang et al., on torchvision's BSD-3-Clause SqueezeNet features). The models the
fork trained itself are BSD-2-Clause-Patent; their cards name the data they were
trained on and that data's terms.

## What the published packages carry

Every published container image and package carries the notices and licence
texts of everything in it, and publishes the source its copyleft parts require
next to it ([ADR-1513](adr/1513-production-artifact-licensing.md),
[ADR-1503](adr/1503-tester-artifact-licensing.md)):

| Package | Notices and licence texts | Corresponding source |
| --- | --- | --- |
| `ghcr.io/vmafx/vmafx:<tag>` (CPU) | `/usr/local/share/vmafx/licenses/` in the image | `ghcr.io/vmafx/vmafx:<tag>-source` |
| `ghcr.io/vmafx/vmafx:<tag>-server` | `/usr/local/share/vmafx/licenses/` | `ghcr.io/vmafx/vmafx:<tag>-server-source` |
| `ghcr.io/vmafx/vmafx:<tag>-cuda13`, `-rocm10`, `-oneapi2026` (and `-oneapi2025`) | `/usr/local/share/vmafx/licenses/`; the GPU vendors' texts under `nvidia/`, `rocm/` and `intel/` | `ghcr.io/vmafx/vmafx:<tag>-cuda13-source`, `-rocm10-source`, `-oneapi2026-source` |
| `ghcr.io/vmafx/vmafx-operator:<tag>`, `vmafx-server:<tag>` | `/usr/local/share/vmafx/licenses/` (Go modules under `go/`) | `<image>:<tag>-source` |
| `ghcr.io/vmafx/vmafx-node:<tag>` | `/usr/local/share/vmafx/licenses/`; FFmpeg's and SVT-AV1's files under `/usr/local/share/vmafx/ffmpeg/` and `svt-av1/`; the copied libraries' copyright files under `copied-packages/`, the FUSE tools' under `fuse-tools/` | `ghcr.io/vmafx/vmafx-node:<tag>-source` (FFmpeg as built with its configure line, Debian sources, Go module zips) |
| Tester images and bundles | `/opt/vmafx/licenses/`, `licenses/` in a bundle | `<image>-source` |
| `vmaf-mcp` on PyPI | the `licenses/` directory of the wheel's and the sdist's metadata (EUPL-1.2, BSD-2-Clause-Patent) | the sdist |
| GitHub release files (`libvmaf.so*`, `libvmafx.so*`, `vmaf`) | `THIRD_PARTY_NOTICES.txt` and `licenses.tar.gz` next to them on the release | the release's source archives |
| `models.tar.gz` (release) | `licenses/` inside the archive | the release's source archives |

`THIRD_PARTY_NOTICES.txt` in that directory names every component, its licence,
copyright lines and source; `texts/` holds the licence texts. The CPU image has
no shell, so copy the directory out to read it:

```bash
cid=$(docker create ghcr.io/vmafx/vmafx:<tag>)
docker cp "$cid:/usr/local/share/vmafx/licenses" ./vmafx-licenses
docker rm "$cid"
less ./vmafx-licenses/THIRD_PARTY_NOTICES.txt
```

The source image holds only files: pull it and copy `/sources` out the same way
(`SOURCES.txt` is its index). Each image also carries an attested SPDX SBOM;
[docker-production.md](development/docker-production.md#verifying-image-provenance)
shows how to verify it.

The FFmpeg in the node image is built with `--enable-gpl --enable-version3`
and is distributed under the GNU GPL version 3 or later; the Go programs list
every module they link with its licence in the `[go]` section of the notices
([ADR-1514](adr/1514-go-and-node-image-licensing.md)).

The GPU images carry only the vendor files `vmaf` loads
([ADR-1517](adr/1517-gpu-image-licensing.md)):

- the CUDA image holds no NVIDIA library. The CUDA kernels inside `libvmaf`
  contain NVIDIA code from the CUDA Toolkit (its headers and the `libdevice`
  maths library), distributed under the CUDA Toolkit EULA; the notices pass its
  terms on, and the GPU driver (`libcuda`) comes from your host;
- the ROCm image holds the HIP runtime libraries and the system libraries ROCm
  bundles for them (two of them LGPL, with their source in the `-rocm10-source`
  image);
- the oneAPI image holds the Intel SYCL runtime files the compiler's licence
  lists as redistributable, provided for use with the VMAFx programs in the
  image (the Intel EULA forbids reverse engineering them), and the Intel GPU
  compute runtime.

## Python packages

Each Python package declares, as its PEP 639 `License-Expression`, every licence
its files carry, and ships each text in `LICENSES/` (installed under the
package's `.dist-info/licenses/`)
([ADR-1560](adr/1560-python-package-licence-union.md)). The files counted are
those in the package's sdist and wheel; for the `vmaf` harness that includes
every repository file its compiled ADM extension is built from.

| Package | Directory | `License-Expression` |
| --- | --- | --- |
| `vmaf` (Python harness) | `python/` | `BSD-2-Clause-Patent AND BSD-2-Clause AND BSD-3-Clause-Clear AND EUPL-1.2` |
| `vmaf-mcp` | `mcp-server/vmaf-mcp/` | `EUPL-1.2 AND BSD-2-Clause-Patent` |
| `vmaf-tune` | `tools/vmaf-tune/` | `EUPL-1.2 AND BSD-2-Clause-Patent` |
| `vmaf-train` | `ai/` | `EUPL-1.2 AND BSD-2-Clause-Patent` |
| `vmaf-dev-llm` | `dev-llm/` | `EUPL-1.2 AND BSD-2-Clause-Patent` |
| `vmaf-roi-score` | `tools/vmaf-roi-score/` | `EUPL-1.2 AND BSD-2-Clause-Patent` |
| `vmaf-ensemble-training-kit` | `tools/ensemble-training-kit/` | `EUPL-1.2 AND BSD-2-Clause-Patent` |
| `vmafx-rc1-tester` (not published) | `tools/rc1-tester/` | `EUPL-1.2 AND MIT` (its sdist carries two MIT third-party notices) |

The expression describes the whole distribution, not each file: a module's own
header still says which licence applies to it. To read what an installed
package declares:

```bash
python3 -c "from importlib.metadata import metadata; print(metadata('vmaf-mcp')['License-Expression'])"
```

`python/test/setup_metadata_test.py` recomputes each union from the files and
fails when a package's metadata or texts disagree with them:

```bash
python3 -m pytest python/test/setup_metadata_test.py -k licence
```

## Rust crates and the Helm chart

The other package manifests follow the same rule: the licence field names the
licences of exactly the files the package ships
([ADR-1699](adr/1699-root-licence-files-eupl.md)).

| Package | Manifest | Field | Value |
| --- | --- | --- | --- |
| `vmafx-sys` crate | `bindings/rust/vmafx-sys/Cargo.toml` (inherits the workspace's) | `license` | `EUPL-1.2` |
| `vmafx` crate | `bindings/rust/vmafx/Cargo.toml` | `license` | `EUPL-1.2` |
| `vmafx-tad` crate (not published) | `core/src/feature/rust/tad/Cargo.toml` | `license` | `EUPL-1.2 AND BSD-2-Clause-Patent` (it ships the root `README.md`, BSD-2-Clause-Patent in `REUSE.toml`) |
| Helm chart `vmafx` | `deploy/helm/vmafx/Chart.yaml` | `artifacthub.io/license` | `EUPL-1.2 AND Apache-2.0` (its `values.schema.json` embeds Kubernetes type schemas, Apache-2.0; the chart ships `THIRD-PARTY-NOTICES.txt`, [ADR-2673](adr/2673-chart-licence-kubernetes-schemas.md)) |
| Its Prometheus Pushgateway subchart | `charts/prometheus-pushgateway-*.tgz` | its own `Chart.yaml` | `Apache-2.0` |

A crate's files are what `cargo package --list` puts into it; a chart's are its
own files, and each subchart in `charts/` is a chart of its own. No crate has
been published to crates.io and the chart is not published.

## Checking the licence metadata

`scripts/ci/check_licence_metadata.py` checks the root licence files and every
package manifest (`pyproject.toml`, `Cargo.toml`, `Chart.yaml`,
`package.json`) against the files, using only the Python standard library. The
required `Licence Provenance` check and a pre-commit hook run it:

```bash
python3 scripts/ci/check_licence_metadata.py
```

It prints one line per problem and exits 1, for example:

```text
LICENSE-MIT: a second root licence file (allowed: LICENSE; Netflix's text is NOTICE)
bindings/rust/vmafx/Cargo.toml: license = 'BSD-2-Clause-Patent', but its files carry EUPL-1.2
```

At the root it refuses every file name licensee scores as a licence file except
`LICENSE` (for example `LICENSE-MIT`, `COPYING`, `LICENSE-BSD-2-Clause-Patent`),
a `LICENSE` that is not the EUPL-1.2 text, a missing `NOTICE` and a `NOTICE`
without Netflix's copyright line.

Fix the manifest to the expression it names; when the files are what is wrong,
fix their headers or `REUSE.toml` instead. It exits 2 when a package selects its
files in a way it does not model (`include` or `exclude` in a crate,
`.helmignore`, a `files` list in `package.json`, a hatchling sdist selection):
model the selection in the script rather than guessing.

## Releases 1.0.0-rc.1 and 1.0.0-rc.2

The images and release files of the first two release candidates were
published before these rules
([Research-2140](research/2140-production-artifact-licence-audit.md)). The
project withdrew two kinds of image and completed the rest
([ADR-1578](adr/1578-published-rc-licence-companions.md)):

- **Withdrawn** on 2026-10-04, deleted from the registry together with the
  untagged images of the same runs:
  - `ghcr.io/vmafx/vmafx:v1.0.0-rc.1-rocm10` and `:v1.0.0-rc.2-rocm10`. They were
    the whole ROCm 10 development image, including `librocprof-trace-decoder.so`,
    a binary-only AMD library whose licence forbids distributing it, and ROCgdb
    (GPL-3.0) without its source.
  - The whole `ghcr.io/vmafx/vmafx-node` package, with its `v1.0.0-rc.1` and
    `v1.0.0-rc.2` images. Their FFmpeg was configured with `--enable-nonfree`,
    which makes the build not legally redistributable, and they shipped GPL and
    LGPL libraries without source. Every version of the package came from that
    recipe. The node image of 1.0.0-rc.3 is published into a new
    `vmafx-node` package, and a new package starts private until it is made
    public.

  Use 1.0.0-rc.3 or later for a ROCm or node image.
- **Yanked** on 2026-10-04: `vmaf-mcp` 1.0.0rc1 and 1.0.0rc2 on PyPI, all four
  files, with the reason "Wrong licence metadata: the files are EUPL-1.2, the
  metadata says BSD-2-Clause-Patent." A yanked release still installs when it
  is pinned exactly (`vmaf-mcp==1.0.0rc2`); the files' own SPDX headers are what
  applies to it.
- **Kept, unchanged**: the CPU, server, CUDA and oneAPI images, `vmafx-server`
  and `vmafx-operator` of both releases, and the release files (`libvmaf.so*`,
  `vmaf`, `models.tar.gz`). Each kept image now has:
  - its notices on the release page, as
    `THIRD_PARTY_NOTICES-<image>-<platform>.txt`, with its licence texts in
    `licenses-<image>.tar.gz`;
  - the corresponding source of its copyleft parts as the image `<tag>-source`
    in the same registry package;
  - an SPDX SBOM attested on its digest.

  The release files have `THIRD_PARTY_NOTICES-native.txt`,
  `THIRD_PARTY_NOTICES-models.txt` and `licenses-native.tar.gz` on the same page.
  The images and files themselves are not changed, and their digests stay valid.
  For example:

  ```bash
  docker pull ghcr.io/vmafx/vmafx:v1.0.0-rc.2-source
  gh release download v1.0.0-rc.2 --repo VMAFx/vmafx -p 'THIRD_PARTY_NOTICES-*'
  ```

The licence section of each release page lists the withdrawn images, the
yanked PyPI release, and every kept image with its digest and its source image.
The registry also lost 34 untagged images built from `master` after the release
candidates; no tag referred to them.

### The stale `LICENSE-MIT` in their source trees

The trees of `v1.0.0-rc.1` and `v1.0.0-rc.2` held a root file `LICENSE-MIT`
("MIT License", "Copyright (c) 2026 Lusoris"). It was a leftover of an earlier
plan to dual-license fork code under BSD-3-Clause-Plus-Patent or MIT
([ADR-0686](adr/0686-vmafx-rebrand-aggressive-modernization.md)), which
[ADR-1250](adr/1250-eupl-fork-relicense.md) replaced before the first release
candidate. ADR-1250 governs: fork-authored code is EUPL-1.2, Netflix's code is
BSD-2-Clause-Patent, and the SPDX header of each file is authoritative. The file
was removed on 2026-10-05 ([ADR-1699](adr/1699-root-licence-files-eupl.md)).

Copies of it remain where those trees were published
([Research-2143](research/2143-root-licence-files-and-package-manifests.md)):

- GitHub's source archives of `v1.0.0-rc.1`, `v1.0.0-rc.2` and the other tags
  made between 2026-05-28 and 2026-10-05. The tags stay.
- The Go module proxy's zips of `github.com/VMAFx/vmafx@v1.0.0-rc.1` and
  `@v1.0.0-rc.2`. The root `go.mod` retracts both versions (its `retract`
  block also withdraws `v1.0.0-rc.3`, see
  [ADR-3292](adr/3292-release-line-restart-0x.md)):

  ```text
  [v1.0.0-rc.1, v1.0.0-rc.2] // contained a stale root LICENSE-MIT; licensing is per-file SPDX (EUPL-1.2 fork code, BSD-2-Clause-Patent Netflix code), see docs/licensing.md
  ```

  The go command reads retractions from the `go.mod` of a module's latest
  version. For this module path that is `v3.0.0+incompatible`, a tag inherited
  from Netflix with no `go.mod`, so `go list -m -versions github.com/VMAFx/vmafx`
  keeps listing both candidates and shows no retraction (measured 2026-10-05).

No published container image, release file, tester bundle or Python package
contained the file.
