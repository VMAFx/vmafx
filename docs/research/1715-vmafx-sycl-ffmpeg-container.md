<!-- markdownlint-disable MD013 MD041 MD060 -->
# Research-1715: porting `Containerfile.vmafx` onto master

Companion digest for [ADR-1715](../adr/1715-vmafx-sycl-ffmpeg-container.md).
Host: NAS with a Xeon D-2143IT and an Intel Arc A380, rootless podman.

## What had to change against the old branch's file

| Problem on master | Change |
|---|---|
| `ffmpeg-patches/series.txt` targets FFmpeg n9.0.2, the image cloned n8.1.1 | `FFMPEG_TAG=n9.0.2`, checked out with `scripts/ci/checkout-annotated-tag.sh` like `dev/Containerfile` |
| SYCL builds AOT by default and need ocloc (ADR-1360); the image pinned NEO 26.18 by hand | Intel GPU stack from `build-config.env` (NEO 26.35.39758.10, Level Zero 1.34.0) via `install-intel-ocloc.sh --components runtime` / `build` (ADR-1368) |
| `core/test/meson.build` `find_program()`s `scripts/ci/check-msvc-clz-shim.sh` at configure time | copy that one script before `meson setup` |
| `float_motion_sycl`, `float_vif_sycl` fail AOT for Xe2 (`lnl-m`, `bmg-g21`): "Kernel compiled with required subgroup size 8, which is unsupported on this platform" | AOT target list = default minus `lnl-m`, `bmg-g21`, `bmg-g31` at the time (`T-SYCL-XE2-SUBGROUP8-AOT-2026-10-02`). Master then fixed the kernels (#1842, ADR-1468: sub-group size 16), so after the rebase onto `b01ffe42d` the image uses the default list, Xe2 included |
| master's pytest config rejects `-p no:cacheprovider` (unknown `cache_dir` option is an error) | `-o cache_dir=/tmp/pytest-cache` |
| the golden gate failed 3-4 tests per run on resource-download timeouts from github.com/Netflix/vmaf_resource | `compat/python-vmaf/config.py` retries transient download errors (4 attempts, 2/4/8 s back-off; HTTP errors not retried), unit-tested in `python/test/config_download_retry_test.py` |
| `reference_report.py` relied on the CLI default model, which is not `vmaf_v0.6.1` on master | pins `--model version=vmaf_v0.6.1` |
| `run-all-tests.sh` ran `meson test` directly; `test_meson_secret_env_sanitization` forbids that (ADR-1333) | runs `scripts/ci/run_meson_test.py`, listed in the test's runner inventory |

| QSV in the image failed: `Error creating a MFX session: -9` for every `*_qsv` decoder / encoder (only the oneVPL dispatcher `libvpl2` was installed) | install the oneVPL GPU runtime `libmfx-gen1.2` in the runtime stage |

## Zero-copy check (QSV decode → `libvmaf_sycl`)

Test pairs encoded in the image with `hevc_qsv` (8-bit NV12 and 10-bit P010,
`global_quality 22`), then scored four ways on the same decoded frames: `vmaf`
CLI on CPU, `libvmaf` filter on CPU, `vmaf` CLI on SYCL, and `libvmaf_sycl`
with `-hwaccel qsv -hwaccel_output_format qsv` (VA surface → DMA-BUF → Level
Zero, no CPU readback), `vmaf_v0.6.1`, Arc A380:

| Clip | CPU VMAF | zero-copy vs CPU | NaN frames |
|---|---|---|---|
| src01 576x324, 8-bit | 75.953252 | bit-exact (all model features) | 0 |
| src01 576x324, 10-bit P010 | 76.058344 | bit-exact | 0 |
| checkerboard 1920x1080 x20, 8-bit | 39.844822 | bit-exact | 0 |
| checkerboard 1920x1080 x20, 10-bit P010 | 39.806130 | bit-exact | 0 |

Additional features requested with `feature=name=psnr|name=cambi` are
computed by the host-upload `libvmaf_sycl` path but silently missing from the
zero-copy path (only the model's SYCL extractors ran there). On the
`VMAFx/vmafx` master this branch was ported to, the zero-copy path refuses
them by name with `-ENOTSUP` instead
([ADR-1688](../adr/1688-sycl-zero-copy-luma-only-admission.md)).

## Verification

| Check | Result |
|---|---|
| Golden gate during `podman build` (before the retry change) | 267 passed / 4 failed, then 268 / 3 failed — every failure a download error (`Connection timed out`, `RemoteDisconnected`, `retrieval incomplete`) |
| Golden gate with retries | 271 passed, 12 skipped, 0 download errors |
| `check_aot_image` | 40 spir64_gen fat binaries; 10 IP versions for 16 targets; 0 partial |
| prod image size | 980 MB |
| `reference_report.py` in the image, Arc A380 | src01 76.667831 (ref 76.667830), checkerboard 1 px 35.068671 (ref 35.068667), 10 px 7.985899 (ref 7.985899): PASS on CPU and SYCL |
| SYCL vs CPU (src01) | `integer_adm2`, `integer_adm3`, `integer_vif_scale0`, `integer_motion2`, VMAF: delta 0.0 |
| FFmpeg filters | `libvmaf`, `libvmaf_sycl`, `libvmaf_tune`, `vmafmotion` |
| libvmaf math binding (`LD_DEBUG=bindings`) | `pow`, `powf`, `log10` → glibc `libm.so.6` |

## Reproducer

```bash
podman build -f Containerfile.vmafx -t vmafx-zerocopy-fix:latest .
podman run --rm --device /dev/dri --entrypoint bash vmafx-zerocopy-fix:latest \
  -c 'cd /opt/vmaf-selftest && python3 reference_report.py'
```
