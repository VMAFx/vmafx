<!-- markdownlint-disable MD013 MD024 MD036 MD060 -->
# `vmaf-tune` — quality-aware encode automation harness

`vmaf-tune` finds the encoder settings that reach a target VMAF score.
It drives FFmpeg over a grid of encoder parameters, scores each encode
with [`vmaf`](cli.md), and either writes a JSONL corpus of
`(source, encoder, params, bitrate, vmaf)` rows or answers a question
directly: which CRF hits VMAF 92, which codec is cheapest at that
target, what a per-title bitrate ladder looks like.

The tool is a fork-added Python package
([ADR-0237](../adr/0237-quality-aware-encode-automation.md),
[Research-0044](../research/0044-quality-aware-encode-automation.md)).
A Go port of the same subcommands is described in
[vmafx-tune-go.md](vmafx-tune-go.md).

## Install

1. Install the package from the repository root. The core install has no
   runtime dependencies beyond the Python standard library:

    ```shell
    pip install -e tools/vmaf-tune
    ```

    This installs two equivalent console scripts, `vmaf-tune` and
    `vmafx-tune`. Without installing, run the checkout shim instead:

    ```shell
    python tools/vmaf-tune/vmaf-tune --help
    ```

2. Put the external binaries on `PATH` (or pass `--ffmpeg-bin` /
   `--vmaf-bin`):

    - `ffmpeg` built with the encoders you want to tune
      (`--enable-libx264`, plus `--enable-libsvtav1` for `libsvtav1`,
      and so on). `ffprobe` is needed for container sources and HDR
      detection.
    - `vmaf`, this fork's CLI, built with meson (see
      [getting started](../getting-started/index.md)).

3. Optional extras, installed as `pip install -e "tools/vmaf-tune[NAME]"`:

    | Extra | Adds | For |
    |---|---|---|
    | `fast` | Optuna | the TPE search of [`fast`](vmaf-tune-fast-path.md) |
    | `report` | matplotlib | the charts of [`report`](vmaf-tune-report.md); without it the report renders its tables and a placeholder per chart |
    | `onnx` | ONNX Runtime | ONNX inference: the `fast` proxy, the per-shot predictor and the saliency models |
    | `dev` | pytest, ruff, Optuna, matplotlib, ONNX Runtime | running the test suite (see [Tests](#tests)) |

`vmaf-tune --version` prints the package version.

## Quick start

Sweep two presets and three CRF values over one 1080p clip. The grid is
the Cartesian product of `--preset` and `--crf`, so this writes six
rows:

```shell
vmaf-tune corpus \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --preset medium --preset slow \
    --crf 22 --crf 28 --crf 34 \
    --output corpus.jsonl
```

Then ask the corpus which CRF reaches a target:

```shell
vmaf-tune recommend --from-corpus corpus.jsonl --target-vmaf 92
```

The [corpus page](vmaf-tune-corpus.md) lists every `corpus` flag and the
row schema. [Recommend](vmaf-tune-recommend.md) covers the target-VMAF
and target-bitrate queries.

## Subcommands

`vmaf-tune --help` lists 14 subcommands. Each one has a page with its
flag table and examples.

| Subcommand | Purpose | Page |
|------------|---------|------|
| `corpus` | Encoder grid sweep plus scoring; writes the JSONL corpus | [corpus](vmaf-tune-corpus.md) |
| `recommend` | Lowest-bitrate encode that meets a target VMAF, or the row closest to a target bitrate | [recommend](vmaf-tune-recommend.md) |
| `predict` | Predict per-shot VMAF with an ONNX predictor and validate it | [predict](vmaf-tune-predict.md) |
| `fast` | Proxy model plus Bayesian search, verified once by a real encode | [fast path](vmaf-tune-fast-path.md) |
| `tune-per-shot` | Per-shot CRF zones from shot detection | [per-shot](vmaf-tune-per-shot.md) |
| `recommend-saliency` | Saliency-aware ROI encode | [saliency-aware](vmaf-tune-saliency-aware.md) |
| `ladder` | Per-title bitrate ladder (Pareto ABR) | [ladder](vmaf-tune-ladder.md) |
| `compare` | Codec comparison at matched VMAF | [compare](vmaf-tune-compare.md) |
| `benchmark` | Offline cross-codec ranking from an existing JSONL | [benchmark](vmaf-tune-benchmark.md) |
| `auto` | Pick the cheapest strategy and run it | [auto](vmaf-tune-auto.md) |
| `prefilter` | Pelorus deband strength plus CRF joint autotune | [prefilter](vmaf-tune-prefilter.md) |
| `report` | Render a profile card from compare, ladder and per-shot JSON | [report](vmaf-tune-report.md) |
| `encode-profile` | Run one recommendation from a report profile | [report](vmaf-tune-report.md) |
| `sidecar` | Train and inspect the local predictor bias correction | [predict](vmaf-tune-predict.md) |

There is no `hdr` subcommand. HDR handling is automatic on `corpus` and
is controlled with `--auto-hdr`, `--force-sdr`, `--force-hdr-pq` and
`--force-hdr-hlg`; see [HDR and sampling](vmaf-tune-hdr-and-sampling.md).

```text
ref.yuv ─► corpus ─► corpus.jsonl ─► recommend ──► CRF for a target
                          │        └► benchmark ─► encoder ranking
                          └─────────► predict / sidecar (predictor training)

ref.yuv ─► compare | ladder | tune-per-shot | fast | auto ─► JSON ─► report
                                                                       └► encode-profile
```

## Topic pages

These pages cover behaviour shared across subcommands.

| Topic | Page |
|-------|------|
| Codec registry, preset names, quality ranges | [codec adapters](vmaf-tune-codec-adapters.md) |
| AV1 encoders (libaom, SVT-AV1, SVT-AV1-HDR knobs) | [AV1 codecs](vmaf-tune-codec-av1.md) |
| NVENC, AMF, QSV and Apple VideoToolbox encoders | [hardware codecs](vmaf-tune-codec-hardware.md) |
| x265, VP9 and VVenC encoders | [software codecs](vmaf-tune-codec-software.md) |
| Which libvmaf backend scores the encodes | [score backend](vmaf-tune-score-backend.md) |
| Automatic VMAF model choice per resolution | [resolution-aware](vmaf-tune-resolution-aware.md) |
| HDR detection and sample-clip mode | [HDR and sampling](vmaf-tune-hdr-and-sampling.md) |
| Two-pass encoding (`--two-pass`) | [multi-pass](vmaf-tune-multipass.md) |
| Coarse-to-fine CRF search | [coarse-to-fine](vmaf-tune-coarse-to-fine.md) |
| Target-VMAF bisect contract | [bisect](vmaf-tune-bisect.md) |
| No-reference early elimination (`--fast-nr`) | [fast NR](vmaf-tune-fast-nr.md) |
| Encode cache (Python API) | [cache](vmaf-tune-cache.md) |
| Scratch directory, disk space, environment variables | [workdir](vmaf-tune-workdir.md) |
| Ladder variants | [bitrate grid](vmaf-tune-bitrate-ladder.md), [default sampler](vmaf-tune-ladder-default-sampler.md) |
| FFmpeg filter wiring and patches | [FFmpeg wiring](vmaf-tune-ffmpeg.md) |

## Design records

| Subject | Record |
|---------|--------|
| Tool and corpus | [ADR-0237](../adr/0237-quality-aware-encode-automation.md), [Research-0061](../research/0061-vmaf-tune-capability-audit.md) |
| `tune-per-shot` | [ADR-0392](../adr/0392-vmaf-tune-phase-d-per-shot.md) |
| `recommend-saliency` | [ADR-0293](../adr/0293-vmaf-tune-saliency-aware.md) (consumes `vmaf-roi` sidecars) |
| `ladder` | [ADR-0295](../adr/0295-vmaf-tune-phase-e-bitrate-ladder.md) |
| `fast` | [ADR-0276](../adr/0276-vmaf-tune-fast-path.md), [ADR-0291](../adr/0291-fr-regressor-v2-prod-ship.md) |
| `prefilter` | [ADR-1116](../adr/1116-autotune-prefilter-control-plane.md) |
| `benchmark` | [ADR-0424](../adr/0424-vmaf-tune-corpus-benchmark.md) |
| `sidecar` | [ADR-0394](../adr/0394-local-sidecar-training.md) |
| `encode-profile` | [ADR-0643](../adr/0643-vmaf-tune-encoder-profile-contract.md) |

## Tests

```shell
python3 -m venv .venv-tune
.venv-tune/bin/pip install -e "tools/vmaf-tune[dev]"
.venv-tune/bin/python -m pytest tools/vmaf-tune/tests/
```

The `dev` extra holds every package the suite imports, so a run from it
has no failure and no skip for a missing package. Each test runs in its
own temporary working directory (`tests/conftest.py`). The suite mocks
`subprocess.run` almost everywhere, so it needs neither `ffmpeg` nor a
built `vmaf`; the tests that need more skip and name what is missing:

| Skip reason | Precondition |
|---|---|
| no vmaf binary reachable | this fork's `vmaf` CLI: `VMAF_BIN_FOR_TESTS=/path/to/vmaf`, `build/tools/vmaf`, or a `vmaf` on `PATH` that has `--backend` |
| `set VMAF_TUNE_INTEGRATION=1` | opt-in runs against the real `ffmpeg` / libx265 |
| the `train` extra | PyTorch for the predictor-training test |
| h264_qsv not compiled in or VA-API driver too old | an Intel GPU whose QSV encoder works |
| BBB corpus missing | the BBB clip under `/workspace/.corpus/bbb_e2e/` (dev container) |

## Former section names

ADRs and the changelog archive link to sections of the page this overview
replaced; each heading points to the page that now holds the content.

### `fast` subcommand — proxy + Bayesian + GPU-verify (Phase A.5)

Now on [`vmaf-tune fast`](vmaf-tune-fast-path.md).

### Codec adapter contract

Now under [adapter contract](vmaf-tune-codec-adapters.md#adapter-contract).

### Per-content-type recipes (F.4)

Now under [per-content-type
recipes](vmaf-tune-auto.md#per-content-type-recipes).

### Sample-clip mode

Now under [clip sampling](vmaf-tune-hdr-and-sampling.md#clip-sampling).
