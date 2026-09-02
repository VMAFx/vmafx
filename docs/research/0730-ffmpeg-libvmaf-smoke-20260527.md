<!-- markdownlint-disable MD013 MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->

# Research digest 0730: ffmpeg libvmaf filter end-to-end smoke test (2026-05-27)

## Summary

Full end-to-end integration test of the fork's `ffmpeg-patches/` stack applied
against FFmpeg `n8.1`. All 15 patches applied cleanly. The `libvmaf` filter
produced a VMAF score of **76.6678** on the Netflix golden pair — matching the
golden assertion in `vmafexec_test.py` to four decimal places. Three bugs were
surfaced; one is an input-ordering trap documented below.

---

## Build environment

| Component | Value |
|-----------|-------|
| FFmpeg ref | `n8.1` (tag `n8.1`, commit `9047fa1`) |
| Patches applied | 15 of 15 (`series.txt`) |
| libvmaf version | 3.0.0 (CPU-only build at `libvmaf/build-cpu/`) |
| Host OS | CachyOS (Arch Linux, kernel 7.0.10) |
| gcc | 16.1.1 |
| Build flags | `--enable-libvmaf --enable-gpl --enable-version3 --disable-doc` |

### Pkg-config workaround required

The system-installed `libvmaf.so` (at `/usr/local/lib/`) was built with the
SYCL backend and requires `libsycl.so.8` from oneAPI 2025.0, but the host
carries 2026.0 (`libsycl.so.8` ABI name unchanged but SOVERSION differed).
FFmpeg configure failed with unresolved SYCL symbols.

**Workaround:** A custom `libvmaf.pc` pointing at the local CPU-only build
(`libvmaf/build-cpu/`) was placed under `/tmp/vmaf-pc/` and used via
`PKG_CONFIG_PATH=/tmp/vmaf-pc`. This is the canonical approach from
`SKILL.md` (step 5). The recommended fix is described in Bug 1 below.

---

## Patch application result

All 15 patches from `ffmpeg-patches/series.txt` applied cleanly via `git am
--3way` on a fresh `n8.1` clone. No conflicts.

Filters registered by the patched build:

| Filter | Notes |
|--------|-------|
| `libvmaf` | Upstream filter with fork extensions (tiny_model, backend selectors, cpumask/gpumask) |
| `libvmaf_cuda` | CUDA zero-copy path (requires `ffnvcodec`) |
| `libvmaf_sycl` | SYCL zero-copy path (VAAPI/QSV) |
| `libvmaf_vulkan` | Vulkan zero-copy path (VkImage import) |
| `libvmaf_metal` | Metal zero-copy path (IOSurface, macOS only) |
| `libvmaf_tune` | VMAF-driven CRF recommender helper |
| `vmaf_pre` | Learned pre-processing filter (tiny-AI, requires DNN-enabled libvmaf + ONNX model) |
| `vmafmotion` | Motion-score only filter (upstream) |

---

## libvmaf filter test

### Golden pair: src01 576x324

Command:

```bash
ffmpeg \
  -f rawvideo -s 576x324 -r 25 -pix_fmt yuv420p \
  -i python/test/resource/yuv/src01_hrc01_576x324.yuv \  # main (distorted)
  -f rawvideo -s 576x324 -r 25 -pix_fmt yuv420p \
  -i python/test/resource/yuv/src01_hrc00_576x324.yuv \  # reference
  -lavfi '[0:v][1:v]libvmaf=log_path=/tmp/libvmaf.json:log_fmt=json' \
  -f null -
```

Result: **VMAF score = 76.667830**
Golden assertion: `76.66783025` (places=4)
Delta: `< 0.0001` — **PASS**.

Note: `[0:v]` is the **main (distorted)** input and `[1:v]` is the **reference**
in the `libvmaf` filter. This is the opposite of the Python runner's argument
order (`ref, dis`). See Bug 2 for the input-ordering trap.

### Edge cases

| Test | Input | Result | Notes |
|------|-------|--------|-------|
| Invalid model name | `model=version=vmaf_NONEXISTENT` | `Error: Invalid argument` | Correct — filter init fails gracefully |
| Mismatched resolution | 576x324 main vs 160x90 reference | `input width must match` / `input height must match` | Proper error messages; exit code 1 |
| Single-frame (identity) | `src01_hrc00_576x324_1frames.yuv` vs itself | Score 97.428480, 1 frame | **PASS** |
| 10-bit 4:2:0 input | `src01_hrc01_576x324.yuv420p10le.yuv` vs reference | Score 82.564223, 3 frames | **PASS** |

---

## vmaf_pre filter test

The `vmaf_pre` filter registered and responded to `-h filter=vmaf_pre` correctly.
A runtime test with an ONNX model was **blocked** — see Bug 3.

---

## Bugs surfaced

### Bug 1 — System SYCL version lock-out prevents default libvmaf detection

**Severity:** High (blocks vanilla `./configure --enable-libvmaf` from detecting the installed library)

**Root cause:** The system-installed `libvmaf.so` at `/usr/local/lib/` was built against
oneAPI 2025.0's `libsycl.so`. The host now has oneAPI 2026.0; the linker fails with
unresolved `sycl::_V1::queue` symbols during the configure link test.

**Impact:** Any user running `./configure --enable-libvmaf` without a custom
`PKG_CONFIG_PATH` pointing at a CPU-only or current-SYCL build will get
`libvmaf >= 2.0.0 not found` even though 3.0.0 is installed.

**Workaround:** Use `PKG_CONFIG_PATH` pointing at the CPU-only build's
`meson-uninstalled/libvmaf-uninstalled.pc` (or a hand-crafted `.pc` pointing at
the local build dir). The skill (`build-ffmpeg-with-vmaf/SKILL.md`) documents this.

**Suggested fix:** Rebuild the system libvmaf install against the current oneAPI 2026.0,
or maintain a CPU-only install path alongside the GPU build.

---

### Bug 2 — Input ordering trap: libvmaf filter reverses ref/dis vs Python runner

**Severity:** Medium (user-facing correctness issue — silent wrong scores)

**Description:** FFmpeg's `libvmaf` filter uses `[0:v]` = **main (distorted)**
and `[1:v]` = **reference**. The Netflix Python runner and the VMAF CLI use the
conventional `ref, dis` order. When a user feeds the pair in `ref, dis` order
(matching every other VMAF interface), the filter silently computes reference
quality against itself as distorted, yielding an inflated score:

```text
# WRONG: ref=hrc00 as main, dis=hrc01 as reference
-i src01_hrc00_576x324.yuv  -i src01_hrc01_576x324.yuv
-lavfi '[0:v][1:v]libvmaf'
=> VMAF score: 83.782079   (WRONG — artificially high)

# CORRECT: dis=hrc01 as main, ref=hrc00 as reference
-i src01_hrc01_576x324.yuv  -i src01_hrc00_576x324.yuv
-lavfi '[0:v][1:v]libvmaf'
=> VMAF score: 76.667830   (matches golden)
```

This is an upstream FFmpeg API convention that the fork inherits. It is documented in
`ffmpeg-patches/README.md` but not surfaced in any filter-level warning.

**Suggested improvement:** Add a note to the filter's `AVOption` description or emit a
`av_log(ctx, AV_LOG_WARNING, ...)` if the two inputs appear identical (possible swapped
ref/dis heuristic). Alternatively, document clearly at `docs/usage/ffmpeg-filter.md`.

---

### Bug 3 — vmaf_pre runtime failure when libvmaf is CPU-only (no DNN), no ORT 1.22 on host

**Severity:** Medium (blocks vmaf_pre end-to-end testing)

**Sub-issue A — CPU build lacks DNN:** `libvmaf/build-cpu/` was compiled without
`-Denable_dnn=true`; `vmaf_dnn_available()` returns 0. The filter correctly prints
`vmaf_pre: libvmaf was built without --enable_dnn` and returns `-ENOSYS`.
This is expected behavior, not a bug per se, but it blocks smoke-testing `vmaf_pre`
with the default CPU build.

**Sub-issue B — ORT version mismatch:** `libvmaf/build-dnn/` was linked against
ORT 1.22.0, which is not present on the host. Available ORT versions are 1.20.1 and 1.26.0.
The `VERS_1.22.0` versioned symbol check fails at `dlopen` time:

```text
libonnxruntime.so.1: version `VERS_1.22.0' not found
```

**Sub-issue C — No C3/vmaf_pre ONNX model shipped:** No pre-processing/learned-filter
ONNX model exists in the `model/` tree. `vmaf_pre` is scaffolded and working at the
filter-plumbing level, but unusable until a model artifact is added.

**Status:** `vmaf_pre` is **untestable end-to-end** until either (a) the host gains
ORT 1.22.0 or (b) build-dnn is rebuilt against ORT 1.20.1/1.26.0 and a pre-processing
model is added to `model/`.

---

## Filter help output (libvmaf)

Fork-added options confirmed present in `ffmpeg -h filter=libvmaf`:

```text
tiny_model        Path to tiny ONNX model (libvmaf --enable_dnn).
tiny_device       tiny-model device: auto|cpu|cuda|openvino|openvino-npu|...
tiny_threads      tiny-model CPU EP intra-op threads.
tiny_fp16         Request fp16 I/O when the device supports it.
sycl_device       SYCL device index (-1 = disabled).
sycl_profile      Enable SYCL queue profiling.
vulkan_device     Vulkan device index (-1 = disabled, 0+ = device index).
cuda              Enable the CUDA backend on the primary device.
hip_device        HIP device index (-1 = disabled, 0+ = device index).
metal_device      Metal device index (-2 = disabled, ...).
cpumask           Bitmask to disable SIMD ISAs.
gpumask           Bitmask to disable GPU dispatch.
```

---

## Reproducer

```bash
# Clone and apply patches
git clone --depth=100 --branch n8.1 https://github.com/FFmpeg/FFmpeg.git /tmp/ffmpeg-n8.1
cd <vmaf-repo>
git -C /tmp/ffmpeg-n8.1 config user.email "you@example.com"
git -C /tmp/ffmpeg-n8.1 config user.name "You"
for p in $(grep -v '^#' ffmpeg-patches/series.txt); do
  git -C /tmp/ffmpeg-n8.1 am --3way "ffmpeg-patches/$p" || break
done

# Create local libvmaf.pc for CPU-only build
mkdir -p /tmp/vmaf-pc
cat > /tmp/vmaf-pc/libvmaf.pc <<'EOF'
prefix=<absolute-path-to>/libvmaf/build-cpu
includedir=<absolute-path-to>/libvmaf/include
libdir=${prefix}/src
Name: libvmaf
Version: 3.0.0
Libs: -L${libdir} -lvmaf
Libs.private: -pthread -lm
Cflags: -I${includedir} -I${includedir}/libvmaf
EOF

# Configure and build
cd /tmp/ffmpeg-n8.1
PKG_CONFIG_PATH=/tmp/vmaf-pc \
  ./configure --prefix=/tmp/ffmpeg-vmafx-install \
    --enable-libvmaf --enable-gpl --enable-version3 --disable-doc
make -j$(nproc)

# Smoke test (correct input order: distorted first, reference second)
LD_LIBRARY_PATH=<path-to>/libvmaf/build-cpu/src \
  ./ffmpeg \
    -f rawvideo -s 576x324 -r 25 -pix_fmt yuv420p \
    -i <vmaf-repo>/python/test/resource/yuv/src01_hrc01_576x324.yuv \
    -f rawvideo -s 576x324 -r 25 -pix_fmt yuv420p \
    -i <vmaf-repo>/python/test/resource/yuv/src01_hrc00_576x324.yuv \
    -lavfi '[0:v][1:v]libvmaf=log_path=/tmp/libvmaf.json:log_fmt=json' \
    -f null -
# Expected: VMAF score: 76.667830
```

---

## References

- `ffmpeg-patches/series.txt` — patch series definition
- `ffmpeg-patches/README.md` — patch descriptions
- `python/test/vmafexec_test.py` — golden assertion (`76.66783025`)
- `libvmaf/build-cpu/`, `libvmaf/build-dnn/` — local libvmaf builds used
- `.claude/skills/build-ffmpeg-with-vmaf/SKILL.md` — skill definition
