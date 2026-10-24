# Testing SYCL zero-copy input on an Intel GPU

`scripts/test/sycl-dev-container.sh` builds libvmaf and FFmpeg from the current
worktree inside the SYCL toolchain image and runs meson tests or any command on
the Intel GPU (`/dev/dri`). Use it for the QSV zero-copy path of the
`libvmaf_sycl` filter, which needs the oneVPL GPU runtime and a QSV-enabled
FFmpeg build.

It runs the `vmaf-dev-mcp:local` image that `dev/docker-compose.yml` builds
([dev-MCP container](dev-mcp.md)), which carries icx/icpx, `ocloc`, the oneVPL
GPU runtime and the QSV build dependencies. Unlike `docker exec vmaf-dev-mcp`,
it builds the current worktree incrementally in a throwaway container and
publishes no artifacts.

## Modes

| Mode | What it does |
| ---- | ------------ |
| `libvmaf` | Configures (first run) and builds libvmaf with SYCL (SPIR-V JIT, no AOT targets), installs it into the cache prefix and copies the public headers FFmpeg's `check_pkg_config libvmaf` probes. |
| `ffmpeg` | Checks out `FFMPEG_TAG` from `FFMPEG_REMOTE` (`build-config.env`) into the cache once, resets it to `FFMPEG_COMMIT` on every run, applies the shared FFmpeg fix series with `scripts/ci/ffmpeg-shared-series.sh` ([ADR-3143](../adr/3143-ffmpeg-shared-fix-series.md)) and then `ffmpeg-patches/series.txt` with `git am --3way` (stops with `PATCH FAILED: <name>` on a conflict; the next run aborts the half-applied patch), configures with QSV, VAAPI and `--enable-libvmaf-sycl`, builds and installs. Run `libvmaf` first. |
| `test [args...]` | `meson test -C <cache>/build --print-errorlogs` with your arguments, for example `test --suite sycl`. |
| `exec <cmd...>` | Runs a command with the built `ffmpeg` and libvmaf on `PATH` / `LD_LIBRARY_PATH`. |

No mode, or an unknown one, prints usage and exits 2; a missing container
runtime exits 127.

## Environment

| Variable | Default | Meaning |
| -------- | ------- | ------- |
| `SYCL_DEV_IMAGE` | `vmaf-dev-mcp:local` | Toolchain image. |
| `SYCL_DEV_JOBS` | `nproc` | Parallelism of `ninja` and `make` (1 to 999). On a shared host, set it lower. |
| `SYCL_DEV_CC` | `icx` | C compiler for the libvmaf build (master's SYCL test executables need a compiler that links `-fsycl`). |
| `SYCL_DEV_TIMEOUT` | `5400` | Wall-clock cap in seconds for one run (killed with SIGKILL). |
| `YUV_DIR` | `<main checkout>/python/test/resource/yuv` | Fixtures, mounted read-only at `/yuv`. |
| `HOST_REPO` | the main checkout | Host path of the main checkout. Inside a devcontainer the container runtime is the host's, so `-v` sources must be host paths; every mount under the main checkout is rewritten to the same suffix under `HOST_REPO`. |
| `CONTAINER_RT` | `podman`, then `docker` | Container CLI. |

Each run mounts the worktree at `/work`, passes `--device /dev/dri` and sets
`UR_L0_USE_IMMEDIATE_COMMANDLISTS=0`.

## Cache layout

Everything lives under `<worktree>/.cache/sycl-dev/` (git-ignored):
`build/` (meson), `prefix/` (libvmaf install), `ffmpeg-src/` and
`ffmpeg-prefix/`. Builds are incremental; delete the directory to start over.

## Reproducer: a feature that needs chroma fails loudly on zero-copy input

```bash
scripts/test/sycl-dev-container.sh libvmaf
scripts/test/sycl-dev-container.sh ffmpeg
scripts/test/sycl-dev-container.sh exec bash -c '
  cd /tmp
  for s in hrc00:ref hrc01:dis; do
    ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 \
      -i /yuv/src01_${s%%:*}_576x324.yuv -pix_fmt nv12 -c:v hevc_qsv \
      -profile:v main -global_quality 22 ${s##*:}.mp4
  done
  Q="-init_hw_device vaapi=va0:${RENDER_NODE:-/dev/dri/renderD128} -init_hw_device qsv=qr@va0 -init_hw_device qsv=qd@va0"
  ffmpeg -hide_banner $Q -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qd -i dis.mp4 \
    -hwaccel qsv -hwaccel_output_format qsv -hwaccel_device qr -i ref.mp4 \
    -lavfi "[0:v][1:v]libvmaf_sycl=feature=name=psnr" -f null -'
```

The filter routes `psnr` to its twin (`libvmaf_sycl: feature 'psnr' ->
psnr_sycl`, [ADR-3360](../adr/3360-sycl-filter-twin-routing.md)); `psnr_sycl`
needs chroma, so the first frame is refused
([ADR-1688](../adr/1688-sycl-zero-copy-luma-only-admission.md)) with
`vmaf_read_pictures_sycl: feature extractor "psnr_sycl" needs host pictures
...` and the filter's hint to use `hwdownload`, and `ffmpeg` exits non-zero.
The same command without `feature=name=psnr` prints a VMAF score, and
`feature=name=niqe` (no SYCL twin) fails at configuration with
`libvmaf_sycl: feature 'niqe' cannot run on zero-copy input: no SYCL twin`.
Each decoder input has its own QSV device (`qr`, `qd`), as
[the SYCL overview](../backends/sycl/overview.md) requires.

## End-to-end harness: `zerocopy-e2e.sh`

`scripts/test/zerocopy-e2e.sh` is the acceptance gate for the zero-copy fixes.
It QSV-encodes a ref/dis pair with `hevc_qsv -global_quality 22` (NV12 / Main
for 8 bit, P010 / Main10 for 10 bit) from the Netflix `src01` 576x324 pair and
the 1920x1080 checkerboard pair (repeated `CB_REPEAT`=20 times), then runs every
case through three legs:

| Leg | Pipeline | Role |
| --- | --- | --- |
| `cpu` | software decode, `libvmaf` | reference |
| `host` | software decode, `libvmaf_sycl` (host upload, SYCL twins) | must equal `cpu` exactly (`ciede` within its declared bound, below) |
| `zc` | QSV decode with one QSV device per input, `libvmaf_sycl` | must equal `host` once its stage is reached |

Every leg writes its scores with the filter option `score_fmt=%.17g` (patch
0016; override with `SCORE_FMT`), so the comparator sees full doubles. At the
default `%.6f` a difference below 5e-7 is invisible and "exact" would only mean
"equal to six decimals".

One case has no `cpu` leg: `motion_uv`, integer `motion` with
`motion_add_uv=true`. The CPU integer `motion` extractor has no such option (only
`float_motion` does, covered by `float_motion_uv`), so the comparator declares it
`reference=host`: the harness runs the `host` and `zc` legs only, and `zc` (every
`--repeat` run included) must equal host upload of `motion_sycl` exactly. That is
a weaker statement than "equals the CPU" and the verdict list says so: no
`host-vs-cpu` line exists for it. `--list` prints the reference leg as its last
column.

Feature cases switch the default model off (`model=`) so only the named feature
is measured; the two model cases use `model=version=...`.

Every leg runs both inputs to their end. The harness never cuts a leg with
`-frames:v` or `-t`: FFmpeg enforces those after the filtergraph, so a
`libvmaf*` filter can score one more pair than `ffmpeg` outputs (see
[Score exactly N frames](../usage/ffmpeg.md#score-exactly-n-frames)). To score a
prefix, use `trim=end_frame=N` on both inputs.

```bash
scripts/test/sycl-dev-container.sh libvmaf
scripts/test/sycl-dev-container.sh ffmpeg
scripts/test/sycl-dev-container.sh exec bash scripts/test/zerocopy-e2e.sh --stage 1 --out /work/.cache/sycl-dev/e2e-s1 --depths 8 --bench
```

Run 8-bit and 10-bit as separate invocations with separate `--out` directories
(`--depths 8`, `--depths 10`); set `SYCL_DEV_TIMEOUT` above the launcher's
default if a run needs more than 90 minutes. Flags: `--stage {1,2,3}`
(required), `--out DIR` (required), `--clips src01,checkerboard`, `--depths
8,10`, `--cases id,id` (ids from `python3 scripts/test/zerocopy_e2e_compare.py
--list`), `--yuv DIR` (default `/yuv`), `--bench`, `--repeat N`. `--bench`
adds `-benchmark` to the zero-copy leg of `model-vmaf_v0.6.1` on the
checkerboard and prints `ZC-E2E BENCH <clip> <depth> frames=<n> rtime=<s>
fps=<f>`, the throughput baseline later stages compare against. An encode
failure prints `ENCODE FAILED` and exits 1. The case files stay in `--out` as
`<clip>_<depth>bit__<case>.<leg>.{json,rc,err}`.

`--repeat N` (default 1) runs the zero-copy leg N times per case; the extra
runs are kept as the legs `zc-r2` to `zc-rN`, and a case at its stage fails as
`zc-nondeterministic` when any of the N runs differs from host upload. Use it
for anything timing dependent: a zero-copy fault that shows up in only some
runs passes a single run most of the time.

```bash
scripts/test/sycl-dev-container.sh exec bash scripts/test/zerocopy-e2e.sh --stage 1 \
  --out /work/.cache/sycl-dev/e2e-rep8 --depths 8 --cases cambi,vif,model-vmaf_v0.6.1 --repeat 10
```

### Stages

`--stage N` states how far the zero-copy work has come; a case is held to
numeric parity once `N` reaches its stage and must fail loudly before that.
On this tree the zero-copy import carries luma only
([ADR-1688](../adr/1688-sycl-zero-copy-luma-only-admission.md)), so run
`--stage 1`: the stage-2 and stage-3 cases need chroma or the host-staging twins
on the shared planes, the post-1.0 zero-copy import of
[ADR-1685](../adr/1685-post-1-0-embedding-zero-copy-milestone.md), and must be
refused with an error that names the extractor.

| Stage | Cases that must match numerically |
| --- | --- |
| 1 | `vif`, `adm`, `motion`, `motion_v2`, `cambi`, `float_moment`, `psnr_luma` and `psnr_hvs_luma` (`enable_chroma=false`), `model-vmaf_v0.6.1` |
| 2 | adds `psnr`, `psnr_hvs` (chroma), `motion_uv` (host-upload reference) |
| 3 | adds `float_ssim`, `float_ms_ssim`, `float_psnr`, `float_adm`, `float_vif`, `float_motion`, `float_motion_uv`, `ssim`, `ciede`, `ssimulacra2`, `speed_chroma`, `speed_temporal`, `model-vmaf_float_v0.6.1` |

`zerocopy_e2e_compare.py` (unit-tested by `test_zerocopy_e2e_compare.py`) turns
the files into verdicts, one `ZC-E2E <clip> <depth> <case> <verdict>` line per
case and a last line `ZC-E2E SUMMARY stage=<n> pass=<p> fail=<f> nonexact=<x>`.
The exit status is 0 only for `fail=0 nonexact=0` and at least one pass.

| Verdict | Meaning |
| --- | --- |
| `PASS` | zero-copy output has every CPU metric and equals host upload exactly; for `ciede` the line also gives the measured host-vs-CPU difference and the declared bound |
| `PASS loud-fail` | a later-stage case exited non-zero and its stderr names the CPU feature or its SYCL twin |
| `FAIL silent-drop` | a successful zero-copy run lacks a metric the CPU run has |
| `FAIL zc-vs-host` | zero-copy differs from host upload by any amount |
| `FAIL zc-nondeterministic` | with `--repeat N`, at least one of the N zero-copy runs differs from host upload or failed; the detail names the runs (`zc-r<k>`) |
| `FAIL zc-failed` | a case at its stage failed on zero-copy |
| `FAIL unexpected-success` | a later-stage case succeeded on zero-copy |
| `FAIL unnamed-failure` | a later-stage case failed without naming the feature |
| `FAIL missing-leg`, `cpu-failed`, `host-failed` | a leg's files are absent or a reference leg failed |
| `NONEXACT host-vs-cpu` | the SYCL twin on host upload differs from the CPU extractor (at full precision); the tolerance the parity gate would allow is printed for information only and never applied |

The only declared bound is `ciede`: `ciede_sycl` runs the CPU's arithmetic on
fp32 pairs and differs from the CPU extractor where glibc's `powf` and fp64
functions round differently ([ADR-1436](../adr/1436-sycl-ciede-cpu-arithmetic.md),
`T-CUDA-CIEDE-LIBM-RESIDUAL-2026-10-01`). The comparator takes the bound from
`LIBM_TWINS["ciede"]` in `scripts/ci/cross_backend_calibration.py` (`1e-9`)
instead of repeating it. Measured at full precision: 1.112e-11 on one src01
8-bit frame, 0 on every other frame and clip. Zero-copy against host upload
stays exact for `ciede` too.

`VMAF_integer_feature_motion_sad_score` is the one CPU output exempt from the
metric set: the SYCL `motion` twin does not write it on any input path, so it
cannot be a zero-copy regression (`SYCL_TWIN_OMITTED` in the comparator).

## What runs where

The zero-copy end-to-end runs are local / container only: the self-hosted
Arc A380 CI runner has no FFmpeg or oneVPL. CI carries the device unit tests
instead (`--suite sycl`, for example `test_sycl_zero_copy_admission` and
`test_sycl_zero_copy_model_gate`), which emulate the VA import by writing the
shared upload slots directly.

Two contracts need no device and run on every pull request and in pre-commit:

```bash
make sycl-zerocopy-contract
```

It runs `ffmpeg-patches/test/check-sycl-feature-routing.sh` (the text of patch
0005: twin routing and NV12 / P010 only) and
`scripts/test/test_zerocopy_e2e_compare.py` (every verdict of the comparator,
including `reference=host` cases and the declared `ciede` bound; needs `pytest`,
see `requirements/locks/pytest-timeout.txt`). CI step: `FFmpeg Patch Stack`,
`.github/workflows/ffmpeg-patch-stack.yml`; hook: `sycl-zerocopy-contract` in
`.pre-commit-config.yaml`.

On a host with more than one GPU, `/dev/dri/renderD128` may belong to another
vendor: run `exec env RENDER_NODE=/dev/dri/renderD<n> bash -c ...` with the
Intel node (its `/sys/class/drm/renderD<n>/device/vendor` reads `0x8086`).
`scripts/test/zerocopy-e2e.sh` finds that node itself and takes
`--render-node PATH` to override it.
