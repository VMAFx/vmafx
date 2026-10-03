<!-- markdownlint-disable MD013 MD024 MD046 -->
# Test VMAFx on your Mac without building anything

This page is for someone who has a machine the project does not own (an Apple
M-series Mac, an Ampere or Graviton box, an unusual x86 CPU) and wants to check the
fork on it. You build nothing, install no toolchain and need no repository checkout.
You run one prepared package, it prints one JSON report, and you can send that report
to the project and be credited for it.

There are two packages. On a Mac, run the **native bundle** first: it also exercises
the Metal backend, which no container can reach. The **container image** tests the
Linux arm64 code paths and works on any machine with Docker.

| | Native macOS bundle | Container image |
| :--- | :--- | :--- |
| Runs on | macOS on Apple silicon | any Docker host (Linux arm64 or amd64, Docker Desktop) |
| Exercises | NEON default dispatch against scalar, **every Metal twin against the CPU**, SIMD unit tests | NEON (or AVX2 / AVX-512) default dispatch against scalar and against baked references, SIMD unit tests, the Netflix golden gate |
| Does not exercise | SVE2 (Apple cores do not expose it), CUDA, SYCL, HIP, the Python golden gate | Metal, SVE2 on a core without it, GPU twins |
| You need | a terminal | Docker |

Both print what they did and did not exercise inside the report (`not_exercised`).

## A. Native macOS bundle (Apple silicon)

Five commands. Replace `<TESTER-TAG>` with the tag the maintainers give you
(it looks like `tester-20261003-1a2b3c4d`) and `<VERSION>` with the version in the
file name, the `git describe` of the tested commit (for example
`v1.0.0-rc.2-312-g1a2b3c4d`). The maintainer who sends you this page gives you both.

```sh
# 1. Download the archive and its checksum (curl sets no quarantine flag on the files).
curl -LO https://github.com/VMAFx/vmafx/releases/download/<TESTER-TAG>/vmafx-tester-macos-arm64-<VERSION>.tar.gz
curl -LO https://github.com/VMAFx/vmafx/releases/download/<TESTER-TAG>/vmafx-tester-macos-arm64-<VERSION>.tar.gz.sha256

# 2. Check the download against the checksum; it prints "OK" and nothing else.
shasum -a 256 -c vmafx-tester-macos-arm64-<VERSION>.tar.gz.sha256

# 3. Unpack into one new directory.
tar -xzf vmafx-tester-macos-arm64-<VERSION>.tar.gz

# 4. Run the report (a few minutes; more with a Metal device, which runs the Metal tests and
#    the parity gate); the JSON goes to report.json, a summary to the terminal.
cd vmafx-tester-macos-arm64-<VERSION> && ./run.sh > report.json

# 5. Look at the verdict before you send anything.
grep -E '"(verdict|cpu_model|hw_model)"' report.json
```

What the run does: it reads files in this directory, starts `build/tools/vmaf`, the
test executables in `tests/` and the parity gate in `tester/gate/` (with the bundled
interpreter), and writes the report. It writes nothing outside the
directory except your temporary directory. It needs no network: `run.sh` starts the
report under macOS's `sandbox-exec` with network access denied when your macOS offers
it, and says so on the terminal; the report program contains no network code in any
case. It never asks for `sudo`.

What the run does not do: it does not install anything, change settings, contact a
server, or read your files outside this directory. The report contains no host name,
user name, serial number or UUID (see [What the report contains](#what-the-report-contains)).

### What is in the bundle

Sizes are approximate; the exact file list with sizes is `bundle-files.txt` in the
workflow run that built it.

| Path | What | Size |
| :--- | :--- | ---: |
| `run.sh` | the one command (about 20 lines of `sh`) | 1 KB |
| `runtime/` | a Python 3.13 interpreter ([python-build-standalone](https://github.com/astral-sh/python-build-standalone), pinned by SHA-256, standard library only) | about 45 MB |
| `tester/` | the report program, plain Python (`tools/rc1-tester/` in the repository) | 0.2 MB |
| `tester/gate/` | the parity gate the report runs on Metal, plain Python (`scripts/ci/cross_backend_parity_gate.py` with its list of exact twins and the decision records that list cites) | 0.5 MB |
| `build/tools/vmaf` | the VMAFx command line tool; libvmaf is linked in, Metal is on, no ONNX Runtime | about 10 MB |
| `tests/` | unit test executables, including the Metal parity tests | about 40 MB |
| `python/test/resource/` | Netflix test videos, each checked against a pinned SHA-256 | about 57 MB |
| `reference/`, `image/` | scores recorded by the build, manifests | under 1 MB |

### Remove it afterwards

```sh
cd .. && rm -rf vmafx-tester-macos-arm64-<VERSION> vmafx-tester-macos-arm64-<VERSION>.tar.gz*
```

### Check the download more closely (optional)

The checksum in step 2 comes from the same place as the archive, so it detects a broken
download, not a forged one. Two independent checks tie the archive to the repository's
hosted build. Either needs a tool you may not have; neither is needed to run the bundle.

```sh
# GitHub build provenance (needs the GitHub CLI, `gh`):
gh attestation verify vmafx-tester-macos-arm64-<VERSION>.tar.gz -R VMAFx/vmafx

# Sigstore keyless signature (needs `cosign`; the .bundle file is next to the archive):
cosign verify-blob --bundle vmafx-tester-macos-arm64-<VERSION>.tar.gz.bundle \
  --certificate-identity-regexp '^https://github.com/VMAFx/vmafx/\.github/workflows/macos-tester-bundle\.yml@refs/heads/master$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  vmafx-tester-macos-arm64-<VERSION>.tar.gz
```

To read what you are about to run: `run.sh` is in the bundle,
the report program is [`tools/rc1-tester/src/vmaf_rc1_tester/hw_report.py`](https://github.com/VMAFx/vmafx/blob/master/tools/rc1-tester/src/vmaf_rc1_tester/hw_report.py)
and its neighbours `hw_*.py`, and the build is
[`scripts/ci/build-macos-tester-bundle.sh`](https://github.com/VMAFx/vmafx/blob/master/scripts/ci/build-macos-tester-bundle.sh)
run by [`macos-tester-bundle.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/macos-tester-bundle.yml).

### Gatekeeper

The binaries carry an ad-hoc code signature (macOS on Apple silicon refuses unsigned
code) and no Apple notarization, because the project has no Apple Developer ID. A file
downloaded with `curl` has no `com.apple.quarantine` attribute, so Gatekeeper does not
check it and the bundle runs. If you downloaded the archive with a browser instead,
macOS marked it as quarantined and will refuse the first run; clear the mark on the
unpacked directory (it only removes that attribute, nothing else) and run again:

```sh
xattr -dr com.apple.quarantine vmafx-tester-macos-arm64-<VERSION>
```

## B. Container image

You need Docker and nothing else. `<VERSION>` is the same string as above. On an Apple silicon Mac Docker Desktop runs a Linux
arm64 virtual machine, so the container tests the fork's aarch64 code on your real CPU.
It cannot reach Metal and SVE2 is not exposed by Apple cores; the report says so.

```sh
# 1. Check the signature of the image (needs `cosign`; skip if you do not have it).
cosign verify \
  --certificate-identity-regexp '^https://github.com/VMAFx/vmafx/\.github/workflows/docker-publish-tester\.yml@refs/(heads/master|tags/v.*)$' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  ghcr.io/vmafx/vmafx:<VERSION>-tester

# 2. Pull it and note its digest.
docker pull ghcr.io/vmafx/vmafx:<VERSION>-tester
docker image inspect --format '{{index .RepoDigests 0}}' ghcr.io/vmafx/vmafx:<VERSION>-tester

# 3. Run it. This exact line makes the container unable to reach the network, write
#    its own filesystem, gain privileges or keep any Linux capability.
docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --tmpfs /tmp \
  ghcr.io/vmafx/vmafx:<VERSION>-tester > report.json
```

No volume mount is needed; the report goes to your standard output (`report.json`) and a
short summary to the terminal. The run takes about eight minutes on a fast desktop CPU (most of it the golden gate); allow longer on a laptop. Exit status
0 means everything the run exercised agrees; the report is printed either way.
To record the image digest in the report add `--image-digest sha256:...` after the image
name.

To read what runs: the Containerfile is
[`docker/Dockerfile.tester`](https://github.com/VMAFx/vmafx/blob/master/docker/Dockerfile.tester)
and the report program is `tools/rc1-tester/src/vmaf_rc1_tester/hw_report.py` and its
`hw_*.py` neighbours. The image is built only by
[`docker-publish-tester.yml`](https://github.com/VMAFx/vmafx/blob/master/.github/workflows/docker-publish-tester.yml)
from a tagged commit, signed keyless with cosign and attested like the other VMAFx
images. It is about 1.05 GB on disk and about 270 MB to download; it holds a CPU-only build,
no compiler, and runs as a numeric non-root user.

## What the report contains

One JSON document (schema: [`docs/hardware-reports/report.schema.json`](../hardware-reports/report.schema.json)):

- **Host facts**: CPU model string, the allow-listed `cpuinfo` / `sysctl` keys, CPU
  feature flags, `AT_HWCAP` / `AT_HWCAP2` on Linux, the dispatch flags the fork selects
  (NEON always on arm64, SVE2 only when the kernel reports it, as `core/src/arm/cpu.c`
  does), kernel release, macOS version and hardware model identifier (for example
  `Mac16,1`) and the Metal device name and family on a Mac.
- **Dispatch equivalence**: every CPU feature extractor on four fixtures at
  `--precision max` (`%.17g`), default dispatch against scalar C: identical and
  differing value counts per fixture and, for a difference, metric, first frame and
  both values.
- **Reference equivalence** (container): the same scores against reference scores the
  image build recorded with its own binary. A difference here is the interesting
  finding.
- **Metal equivalence** (macOS bundle): every CPU extractor with `--backend metal`
  against the CPU, with the extractors that really ran on Metal, counts, first
  differing frame, both values and the largest absolute difference.
- **Metal parity gate** (macOS bundle, `metal_gate`): the project's parity gate,
  `scripts/ci/cross_backend_parity_gate.py`, run on every fixture with
  `--backends cpu metal --hold-exact metal`: every Metal twin against its CPU
  extractor, compared exactly at full precision (ciede at its `1e-9` math-library
  bound), with the option sets the gate has cells for (`enable_lcs`, `debug`, the
  five-frame motion window). A feature a Metal twin cannot run on a fixture is
  listed under `left_out` with the reason.
- **Unit tests** and, in the container, the **Netflix golden gate**: passed, failed,
  skipped, names of failures. For the Metal parity tests the report also keeps the
  verdict of every test case (`unit_tests.cases`) and the message of a failing one.
- **Metal state rows** (macOS bundle, `metal_rows`): for every open Metal row of the
  project's bug ledger, the cases, fixture scores and gate cells of this run that
  close it, and a verdict: `pass` (every one of them passed on this Mac), `fail`, or
  `not_measured` (for example no Metal device). See
  [Metal state rows](#metal-state-rows).
- **What was not exercised and why**, the source commit, tool versions and a schema
  version.

It does not contain: a host name, user name, home directory, serial number, UUID, MAC
address, IP address or any network identifier. The CPU model and hardware model
identifier are the only things that identify your machine's kind. Read `report.json`
before you send it; it is plain text.

Exit status 0 means every check that ran agreed. A Metal run on a Mac with no usable
Metal device is reported as `no_device` and is not a failure. A report with verdict
`fail` is as welcome as a passing one: it is the finding.

### Metal state rows

The macOS report says, row by row, which open Metal defects of
[`docs/state.md`](../state.md) this run measured. The map from a row to its
measurements is `image/metal-rows.json` in the bundle (in the repository
`tools/rc1-tester/image/metal-rows.json`): test cases of the Metal parity tests,
which compare a Metal twin with its CPU extractor with `==` on synthetic fixtures
(16-bit full-range noise, frames below 17 pixels, option sets, identical pairs),
Metal scores on the four fixtures, and cells of the parity gate. A row whose
verdict is `pass` has every measurement it names passing on this Mac; the
maintainers close it from that evidence. The summary on the terminal prints the
counts (`metal state rows: N measured passing, ...`).

On a Mac without a usable Metal device every Metal case is skipped, the gate is
`no_device` and every row is `not_measured`; that is not a failure.

## Send the report and get credit

### As a pull request (credited as the commit author)

Your commit carries your own git identity, so GitHub lists you as a contributor. The
run prints the file name to use (`file name for a pull request: ...`). With the GitHub
CLI (`gh`) and `git` set up with your name and e-mail address:

```sh
gh repo fork VMAFx/vmafx --clone && cd vmafx
git switch -c hardware-report && cp ../report.json <file name printed by the run>
git add docs/hardware-reports && git commit -m "docs(hardware-reports): add a report from <your CPU>"
git push -u origin hardware-report && gh pr create --repo VMAFx/vmafx --fill
```

Edit the `"note"` field of the JSON first if you want to add a line (for example the Mac
model and your Docker version); nothing else may be edited, because CI checks the
integrity hash the tool wrote and refuses a hand-edited file. A maintainer regenerates
the index page when the pull request is merged.

### Without opening a pull request

Open an issue with the
[hardware report form](https://github.com/VMAFx/vmafx/issues/new?template=hardware_report.yml)
and attach `report.json`. A maintainer commits it for you; give a name and an e-mail
address in the form if you want a `Co-authored-by:` line with your credit.

Reports from outside the project's own hosts are listed on the
[hardware reports page](../hardware-reports/index.md).

## If something goes wrong

- `run.sh: this bundle is for macOS on Apple silicon` — the bundle is arm64-only; use
  the container on other machines.
- macOS says the developer cannot be verified — see [Gatekeeper](#gatekeeper).
- The report ends `verdict: fail` — that is a result, send it.
- Anything that stops before a report is printed — send the terminal output in an issue.
