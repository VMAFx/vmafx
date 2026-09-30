<!-- markdownlint-disable MD013 MD060 -->
# RC3 home GPU retest kit

`scripts/dev/rc3-home-gpu-retest.sh` runs, in one go, the verify-and-time
commands that the open RC3 rows of [`docs/state.md`](../state.md) carry for
the home GPU box `ryzen-4090-arc`: an RTX 4090 (CUDA), an Arc A380 (SYCL over
Level Zero) and the Zen 5 integrated GPU, gfx1036 (HIP). Each row states what
its twin must match and how to time it; the kit runs exactly those commands,
compares the outputs, and writes one summary line per row.

Use it to record what `master` does on these devices, and then to check a pull
request against that record. The design decision is
[ADR-1386](../adr/1386-rc3-home-gpu-retest-kit.md); the first `master` baseline
and what it found are in
[Research-1386](../research/1386-rc3-home-gpu-retest-master-baseline.md).

## Quick start

From the repository root, with the fixtures in place (see
[Fixtures](#fixtures)) and a build that has the backend you want to test:

```bash
# CUDA and HIP in one gcc build (the SYCL row builds its own image, below)
meson setup build core -Denable_cuda=true -Denable_hip=true -Denable_hipcc=true \
    -Dhip_gfx_targets=gfx1036
nice -n 10 ninja -C build -j4

scripts/dev/rc3-home-gpu-retest.sh --list          # what it will run
scripts/dev/rc3-home-gpu-retest.sh --backend cuda  # the CUDA rows only
scripts/dev/rc3-home-gpu-retest.sh                 # every row
```

A full run takes about an hour on an otherwise idle box. Most of that is the
4K CPU references (ssimulacra2 and SpEED) and the oneAPI image build.

## Comparing a pull request with master

Several rows ask for a "before" and an "after" run: the ported twin must give
exactly the numbers the old one gave. The kit keeps every JSON it writes, so
a run on `master` serves as the "before" for later runs:

```bash
git switch --detach origin/master && ninja -C build
scripts/dev/rc3-home-gpu-retest.sh --out ~/rc3/master

git switch --detach <pr-branch> && ninja -C build
scripts/dev/rc3-home-gpu-retest.sh --baseline ~/rc3/master --out ~/rc3/pr-1637
```

With `--baseline`, every entry that produces GPU JSON compares it frame by frame
with the file of the same name in the baseline. For the psnr_hvs, motion_v2
and HIP shared-upload rows a difference fails the entry, because those rows
require identical output. For the other rows it is reported as information,
since those ports are meant to change the numbers.

## Options

| Option | Default | Meaning |
|---|---|---|
| `--backend cuda\|hip\|sycl` | all three | Run only this backend's entries. Repeatable. |
| `--only ROW-ID` | every row | Run only this `docs/state.md` row. Repeatable. |
| `--build-dir DIR` | `build` | The build whose `tools/vmaf` the entries run, for the CPU reference and the twin alike. |
| `--build-dir BACKEND=DIR` | — | A build for one backend, for example `hip=build-hip`. [ADR-1185](../adr/1185-backend-perf-baseline-methodology.md) prefers one build per backend for timing. |
| `--baseline DIR` | none | The `--out` directory of an earlier run, compared as described above. |
| `--out DIR` | `<build>/rc3-retest/<UTC time>-<commit>` | Where the logs, JSON and summary go. |
| `--list` | — | Print the entries and exit. |
| `--dry-run` | — | Print every command and run none. No device, build or fixture is needed. |
| `--no-timing` | timing on | Skip the ms/frame measurements. |
| `--reps N` | 3 | Timing repetitions (the rows ask for 3). |
| `--threads N` | 16 | CPU extractor threads (the rows ask for 16). |
| `--netflix-dir DIR` | `python/test/resource/yuv` | Where the Netflix 576x324 pair lives. |
| `--bbb-dir DIR` | `testdata/bbb` | Where the 3840x2160 BBB pair lives. |
| `--cuda-device N` | the RTX 4090 | CUDA device index in PCI order. |
| `--hip-device N` | the gfx1036 agent | HIP device index. |
| `--sycl-selector SEL` | the A380, `level_zero:N` | `ONEAPI_DEVICE_SELECTOR` for the SYCL row. |
| `--image REF` | built from the checkout | An existing oneAPI image to test instead of building one. |
| `--lock-dir DIR` | `~/.cache/vmafx-locks` | Where the per-device lock files live. |

The exit status is 0 when every selected entry passed or was skipped, 1 when
an entry missed its row's expectation, and 2 when an entry could not run
(missing build or fixture, a crash, a timeout) or the arguments were wrong.

## What each entry runs

Every entry holds its row's own commands. A CPU reference always comes from
the same `vmaf` binary as the twin.

| Row | Backend | Runs | Passes when |
|---|---|---|---|
| `T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29`, `T-HIP-PSNR-HVS-HOST-CONVERT-2026-09-29` | cuda, hip | `--feature psnr_hvs --precision max` on the twin and the 16-thread CPU; 576x324, and 22 frames of 4K; timing | within [ADR-1361](../adr/1361-psnr-hvs-area-scaled-parity-tolerance.md) (5e-4 at 576x324, 3.34e-3 at 4K) and `feature_backends` names `psnr_hvs_<backend>` |
| `T-GPU-MOTION-V2-INT64-VERTICAL-2026-09-29` | cuda, hip | the same with `--feature motion_v2` | identical to the CPU |
| `T-HIP-TWIN-PRIVATE-PLANE-UPLOADS-2026-09-29` | hip | `--feature psnr --feature psnr_hvs --feature motion_v2` in one run; timing | the three twins ran; with `--baseline`, identical to it |
| `T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29`, `T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29` | cuda, hip | `--feature motion --precision=max` on the twin and the serial CPU, Netflix pair | `integer_motion2` identical |
| `T-CUDA-FLOAT-SSIM-SCALE-GT1-2026-09-29`, `T-HIP-FLOAT-SSIM-SCALE-GT1-2026-09-29` | cuda, hip | 2 frames of 4K `--feature float_ssim`; then `speed_gpu_parity.py --feature float_ssim --max-abs-diff 5e-5` | no fallback warning, the twin in `feature_backends`, and the parity script exits 0 |
| `T-CUDA-SSIMULACRA2-HOST-COMBINE-2026-09-29`, `T-HIP-SSIMULACRA2-HOST-COMBINE-2026-09-29` | cuda, hip | `speed_gpu_parity.py --feature ssimulacra2 --max-abs-diff 1e-9` | the script exits 0 |
| `T-CUDA-CAMBI-HOST-RESIDUAL-2026-09-29`, `T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29` | cuda, hip | `--feature cambi_<backend>` against the CPU `cambi`, 576x324 and 50 frames of 4K; timing | 576x324 bit-exact, 4K within 2.2e-15 |
| `T-CUDA-SPEED-HOST-RESIDUAL-2026-09-29`, `T-HIP-SPEED-HOST-RESIDUAL-2026-09-29` | cuda, hip | `speed_gpu_parity.py` (speed_chroma and speed_temporal) | bit-identical (exit 0) |
| `T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05` | hip | `adm_hip` against the CPU `adm`, plain and with the default model's options, 576x324 and 50 frames of 4K; `test_hip_adm_parity`, `test_hip_adm_small_border`, `test_hip_adm_wide_rounding`; `python/test/gpu_default_model_test.py`; default-model timing | `integer_aim` and `integer_adm3` identical, `integer_adm2` within 5e-5 (places=4), the tests pass |
| `T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29` | sycl | `docker build ... --target final-oneapi2026` from the checkout, then the default model on the tracked 576x324 pair with `--backend cpu` and `--backend sycl` in the image | the SYCL run reports `sycl` and a pooled VMAF within 5e-5 of the CPU run |

"Timing" means the rows' method: runs of 2 and N frames (N = 22 at 4K; 48 at
576x324 for psnr_hvs, motion_v2 and the upload row; 22 for cambi and the
default model), three repetitions, reported as the median of
`(t(N) - t(2)) / (N - 2)` in ms per frame, for the twin and for the 16-thread
CPU. `speed_gpu_parity.py` times its own runs the same way with N = 22.

Two rows have steps the kit does not run, because they change source files:
the ADM row's "flip the `hip` entry of `BACKENDS` to `True`" in
`gpu_default_model_test.py` (the kit runs the test as committed), and the
test-extension and port steps of the float_ssim rows. Those belong to the pull
request that closes the row.

## Fixtures

- **Netflix pair:** `python/test/resource/yuv/src01_hrc0{0,1}_576x324.yuv`,
  the same files the golden tests use.
- **BBB 4K:** `testdata/bbb/ref_3840x2160_200f.yuv` and
  `dis_3840x2160_200f.yuv` (gitignored, about 2.5 GB each). The recipe is in
  [Per-backend performance baselines](backend-perf-baselines.md#how-to-reproduce);
  the source is Blender's
  `https://download.blender.org/demo/movies/BBB/bbb_sunflower_2160p_30fps_normal.mp4.zip`
  (632 MB).
- **SYCL image row:** the tracked `testdata/ref_576x324_48f.yuv` and
  `dis_576x324_48f.yuv`.

In a git worktree, symlink the two gitignored directories from the main
checkout instead of copying them.

## Devices, locks and load

The kit finds each device and pins every run to it:

| Backend | Found with | Pinned with |
|---|---|---|
| CUDA | `nvidia-smi` (the RTX 4090) | `CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=N` |
| HIP | `rocm_agent_enumerator` (the gfx1036 agent) | `HIP_VISIBLE_DEVICES=N` |
| SYCL | `sycl-ls` (the A380 under Level Zero; sources `/opt/intel/oneapi/setvars.sh` if needed) | `ONEAPI_DEVICE_SELECTOR=level_zero:N` |

A backend whose device is missing is skipped with a note; pass the matching
`--*-device` option to name one.

Several jobs share this box, so every device run holds that device's lock:
`cuda-4090.lock`, `hip-gfx1036.lock` or `sycl-a380.lock` under `--lock-dir`.
A timing block takes the lock before its clock starts and holds it through all
repetitions, so waiting for another job never counts as run time. A whole
`speed_gpu_parity.py` run holds the lock too, because the script times the
twin itself. The oneAPI image build holds `sycl-build.lock` and builds with
`VMAF_BUILD_JOBS=4`. CPU reference runs take no lock.

Timings from a shared host are not benchmarks. Each timing note records the
1-minute load average and, for CUDA, what the 4090 was already using, so two
runs can be compared honestly. For publishable numbers use
[the baseline harness](backend-perf-baselines.md) on an idle box.

## Output

`--out` holds:

- `host.txt`: time, commit, load, the build and device of each backend.
- `summary.md` and `summary.tsv`: one line per entry with its result
  (`PASS`, `FAIL`, `ERROR`, `SKIP` or `DRY-RUN`) and its key numbers.
- `<ROW>/<backend>/log.txt`: every command with its output, and the JSON of
  every run (`nf-<backend>.json`, `bbb-cpu.json`, ...). These files are what
  `--baseline` compares.

## Keeping the kit in step with docs/state.md

The kit does not read `docs/state.md` at run time: each entry spells out its
row's commands. So a pull request that adds a row with a verify command for
this box, or changes such a command, changes the entry in
`scripts/dev/rc3-home-gpu-retest.sh` too, and one that closes a row may drop
its entry. `scripts/dev/tests/test_rc3_home_gpu_retest.py` checks that every
entry names a row that exists in `docs/state.md`; the
`rc3-home-gpu-retest-contract` pre-commit hook runs it whenever the kit or
`docs/state.md` changes.

## Related

- [Cross-backend GPU-parity gate](cross-backend-gate.md): the per-feature
  tolerances the rows refer to.
- [`scripts/dev/speed_gpu_parity.py`](../../scripts/dev/speed_gpu_parity.py):
  the parity-and-timing script several rows call.
- [Per-backend performance baselines](backend-perf-baselines.md): publishable
  throughput numbers.
