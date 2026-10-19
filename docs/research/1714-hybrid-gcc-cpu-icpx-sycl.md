<!-- markdownlint-disable MD013 MD041 MD060 -->
# Research-1714: GCC (C) + icpx (C++ / SYCL) container toolchain

Companion digest for [ADR-1714](../adr/1714-hybrid-gcc-cpu-icpx-sycl.md).

The measurements below up to "Reproducer" were taken on master `42fb501cc`,
before [ADR-1461](../adr/1461-strict-fp-every-translation-unit.md) and
[ADR-1495](../adr/1495-icx-system-libm.md) landed. Those two removed the
numerical part (libimf, icpx's fast model) from every icx build; see "After
the rebase onto `b01ffe42d`" for what was measured again.

## Setup

SYCL build image (icx / icpx 2026.1.0, GCC 15.2.0, lld) on a Xeon D-2143IT
with an Intel Arc A380 (`--device /dev/dri`). Reference: a plain GCC build
(`CC=gcc CXX=g++ -Db_lto=false`, SYCL off). Numerics: Netflix `src01` pair
(48 frames), `vmaf_v0.6.1` plus `cambi`, `speed_chroma`, `speed_temporal`,
`float_psnr`, `psnr`, `ciede`, `float_moment`, `float_ssim`, `ssim`,
`float_ms_ssim`, `float_vif`, `float_adm`, `float_motion` (46 per-frame
metrics), `--precision=max`, `--cpumask 48` (AVX2) and `63` (scalar). SYCL
builds use `-Dsycl_icpx_aot_targets=` (JIT) because the image has no ocloc;
`Containerfile.vmafx` installs ocloc and builds AOT. (`Containerfile.vmafx`:
[ADR-1715](../adr/1715-vmafx-sycl-ffmpeg-container.md).)

## icx build vs GCC (before)

Branch base (`CC=icx CXX=icpx`, x86 SIMD libraries strict-FP per ADR-1415):

| Metric | AVX2 vs GCC | scalar vs GCC |
|---|---|---|
| `psnr_cb` | 7.1e-15 (3 frames) | same |
| `speed_chroma_uv` | 9.5e-07 (1 frame) | same |
| `speed_chroma_v` | 1.2e-06 (1 frame) | same |
| `vmaf` (per frame) | 1.2e-12 (3 frames) | same |
| other 42 metrics | bit-exact | bit-exact |

Cause: the icx link uses Intel `libimf` for `pow` / `log10` / `cbrt`. On a
build linked with `-no-intel-lib=libimf` these go away; libsvm under icpx's
default `-fp-model=fast` adds ~5e-12 on every frame's VMAF score, removed by
`-fp-model=precise` on `svm.cpp`.

## Hybrid build (after)

`CC=gcc CXX=icpx CC_LD=lld CXX_LD=lld`, `-Db_lto=false`,
`-Dcpp_link_args=-no-intel-lib=libimf`, `test_link_kwargs`, libsvm precise:

| Check | Result |
|---|---|
| Build (all targets incl. tests) | 1962 / 1962 |
| `libvmaf.so` math library | `libm.so.6` only (no `libimf`, no `libsvml`) |
| CPU AVX2 vs GCC | 46 / 46 bit-exact |
| CPU scalar vs GCC | 46 / 46 bit-exact |
| `meson test --suite gpu` (Arc A380) | 63 OK, 0 fail, 1 skipped |
| `meson test --suite fast` | 286 OK, 1 fail: `test_meson_secret_env_sanitization`, caused by the new `run-all-tests.sh` calling `meson test` directly; fixed by routing it through `scripts/ci/run_meson_test.py` and listing it in the test's runner inventory (passes after the fix) |
| full `quality_runner_test.py` (values synced to Netflix upstream) | 61 passed, 1 skipped |

Without `test_link_kwargs` the hybrid build failed only in the C-only test
executables (`gcc: error: unrecognized command-line option '-fsycl'`); with
`-Db_lto=true` GCC LTO objects cannot go through the icpx/lld link.

## Timing

60 frames 1080p (checkerboard pair repeated 20x), `--cpumask 16`, one SYCL
warm-up run per binary, then 7 rounds with the two binaries interleaved
(order reversed every other round). icx build: CI-style `CC=icx CXX=icpx`
with the project default LTO; hybrid: as above (LTO off). Median (min)
seconds.

| Config | icx | hybrid | Delta (median) |
|---|---|---|---|
| CPU `vmaf_v0.6.1`, 1 thread | 8.425 (8.121) | 7.745 (7.485) | -8.1 % |
| CPU float `vmaf_v0.6.1`, 1 thread | 19.748 (19.345) | 17.491 (16.873) | -11.4 % |
| CPU `speed_chroma` + `speed_temporal`, 1 thread | 1.306 (1.264) | 1.199 (1.138) | -8.3 % |
| CPU `cambi`, 1 thread | 6.963 (6.881) | 6.117 (5.984) | -12.1 % |
| CPU `vmaf_v0.6.1`, 16 threads | 1.461 (1.404) | 1.421 (1.328) | -2.7 % |
| SYCL `vmaf_v0.6.1` (JIT) | 0.738 (0.725) | 0.744 (0.725) | +0.8 % (noise) |

## Not measured

CUDA and HIP host C code in `dev/Containerfile` is GCC-compiled after this
change; there is no NVIDIA or AMD GPU on the measurement host.

## Reproducer

```bash
CC=gcc CXX=icpx CC_LD=lld CXX_LD=lld meson setup build-hyb core \
  -Dbuildtype=release -Denable_sycl=true -Denable_cuda=false -Denable_dnn=disabled \
  -Db_lto=false
ninja -C build-hyb
python3 scripts/ci/run_meson_test.py -- -C build-hyb --suite fast
```

## After the rebase onto `b01ffe42d`

`vmafx:build-ocloc` image (icpx 2026.1.1, GCC 15.2.0, lld) on the Xeon
D-2143IT with the Arc A380. Three builds of the rebased branch: hybrid
(`CC=gcc CXX=icpx`, `-Db_lto=false`, SYCL AOT for `dg2-g11`), the GCC golden
profile (`scripts/ci/setup-golden-build.sh`) and an icx CPU-only build
(`CC=icx CXX=icpx`, default LTO).

| Check | Result |
|---|---|
| Build (all targets incl. tests) | 2171 / 2171 |
| Netflix golden gate, GCC golden build | 280 passed, 3 skipped |
| Netflix golden gate, hybrid build | 280 passed, 3 skipped |
| `meson test --suite fast` (hybrid) | 348 OK, 2 fail: `test_icx_system_libm`, `test_sycl_ordered_sum`; both fail the same way on master `b01ffe42d` built with `CC=icx CXX=icpx` in the same image (not caused by this branch) |
| `meson test --suite gpu` (hybrid, Arc A380) | 66 OK, 1 fail (`test_sycl_ordered_sum`, as above), 1 skipped |
| Master with `CC=gcc CXX=icpx`, no `test_link_kwargs` | test links fail: `gcc: error: unrecognized command-line option '-fsycl'` |
| CPU values vs the GCC build, `--precision max`, `--cpumask 0` and `48`: `src01` 48 frames, checkerboard 1 px 3 frames; default model plus 14 extractors | hybrid 2550 / 2550 identical; icx 2550 / 2550 identical |
| Time, `src01` 48 frames, model + `float_vif` + `float_adm` + `cambi` + `speed_chroma`, 1 thread, 5 interleaved runs, median (min) | hybrid 1.784 s (1.732), icx 1.846 s (1.804), GCC golden 1.864 s (1.747) |

The two failures: `test_icx_system_libm` runs `vmaf --version` under
`LD_BIND_NOW=1 LD_DEBUG=bindings`, and the oneAPI runtime's
`libur_loader.so.0` (loaded by `libsycl`) needs `libimf.so`; the loader
stops with "Relink libimf.so with libm.so.6 for IFUNC symbol cosf" and
SIGSEGV. Neither `libvmaf.so` nor `vmaf` needs `libimf`. `test_sycl_ordered_sum`'s
device walk returns a wrong sum on this A380 setup.
Master later made `test_icx_system_libm` trace the loader without running
the program (`T-ICX-LIBM-TEST-BIND-NOW-CRASH-2026-10-04`); the runtime crash
under `LD_BIND_NOW=1` is Intel's and stays open
(`T-SYCL-LD-BIND-NOW-LIBIMF-IFUNC-2026-10-03`). `test_sycl_ordered_sum` is fixed
on `fix/sycl-zerocopy-features`.

The numerical reasons of the first measurement are gone on this base; the
speed difference is within noise on this short run.

## Re-measurement on `b01ffe42d` at 1080p (2026-10-04)

The short run above (576x324, 48 frames) was too small to show the gain. A
1080p run settles it: checkerboard `..._0_0` vs `..._1_0` concatenated to 60
frames, `--backend cpu`, `--precision max --json`, 9 interleaved rounds with
the build order rotated, all builds `--buildtype=release -Db_lto=false`, gcc
15.2.0 and icx/icpx 2026.1.1 in `vmafx:build-ocloc`, Intel Xeon D-2143IT
(16 logical CPUs, `powersave` governor). Seconds, median (min):

| Configuration | icx | hybrid | GCC | hybrid vs icx | GCC vs icx | Spread icx / hyb / gcc |
| --- | --- | --- | --- | --- | --- | --- |
| 1 thread, `vmaf_v0.6.1` | 4.47 (4.41) | 4.25 (4.22) | 4.25 (4.20) | -4.9 % | -4.9 % | 2.2 / 1.8 / 3.1 % |
| 1 thread, `vmaf_float_v0.6.1` | 17.95 (17.83) | 16.00 (15.84) | 16.01 (15.92) | -10.9 % | -10.8 % | 1.2 / 1.9 / 1.4 % |
| 1 thread, `speed_chroma` + `speed_temporal` | 2.58 (2.54) | 2.55 (2.52) | 2.54 (2.52) | -1.1 % | -1.3 % | 3.1 / 1.9 / 1.9 % |
| 1 thread, `cambi` | 6.49 (6.46) | 6.07 (6.03) | 6.03 (6.01) | -6.5 % | -7.0 % | 0.9 / 1.8 / 1.2 % |
| 16 threads, `vmaf_v0.6.1` | 1.02 (0.99) | 0.99 (0.97) | 0.99 (0.96) | -2.6 % | -2.7 % | 5.1 / 5.3 / 6.0 % |
| SYCL `vmaf_v0.6.1`, Arc A380 | 0.74 (0.72) | 0.73 (0.72) | - | -0.6 % | - | 4.2 / 3.0 % |

All JSON outputs are identical across the three builds once the `fps` field
is removed (0 mismatches over 9 rounds and six configurations). The gain is
beyond the round-to-round spread for the three heavy single-threaded
configurations and within it for SpEED, 16 threads and SYCL. The hybrid
matches pure GCC everywhere, so the speed comes from GCC compiling the CPU C
code; the hybrid keeps it while still building the SYCL backend.
