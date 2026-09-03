/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/**
 *  VMAF Performance Benchmark & Validation Tool
 *
 *  Benchmarks feature extractors (CPU, CUDA, SYCL) using real video
 *  content derived from Big Buck Bunny at multiple resolutions.
 *
 *  Test data location: /tmp/vmaf_test/ (override with VMAF_TEST_DATA env
 *  or --data-dir flag).  Required files per resolution:
 *    ref_{W}x{H}.yuv  dis_{W}x{H}.yuv   (raw YUV420P 8-bit, 48 frames)
 *
 *  Generate with:
 *    ffmpeg -i bbb.mp4 -frames:v 48 -vf scale=W:H -pix_fmt yuv420p ref_WxH.yuv
 *    (dis = CRF 28 x264 encode of same, decoded back to raw)
 *
 *  Modes:
 *    vmaf_bench [--resolution WxH] [--frames N]         Performance benchmark
 *    vmaf_bench --validate [--resolution WxH]           GPU vs CPU correctness
 *    vmaf_bench --list-devices                           List GPU devices
 *    vmaf_bench --device N                               Select GPU device
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <math.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "libvmaf/picture.h"
#include "libvmaf/libvmaf.h"

#ifdef HAVE_CUDA
#include "libvmaf/libvmaf_cuda.h"
#endif

#ifdef HAVE_SYCL
#include "libvmaf/libvmaf_sycl.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is an
 * upstream-mirror file whose Netflix source spells the null pointer constant
 * `NULL` (every upstream sync would re-conflict against a keyword rewrite) and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* clock_gettime-based high-resolution timer */
#ifdef _WIN32
#include <windows.h>
static double now_ms(void)
{
    /* Frequency is fixed at boot — query once and cache (static, zero-init). */
    static LARGE_INTEGER freq;
    LARGE_INTEGER cnt = {0};
    if (!freq.QuadPart)
        (void)QueryPerformanceFrequency(&freq);
    (void)QueryPerformanceCounter(&cnt);
    return freq.QuadPart ? (double)cnt.QuadPart / (double)freq.QuadPart * 1000.0 : 0.0;
}
#else
static double now_ms(void)
{
    struct timespec ts = {0, 0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

/* ==================== YUV file I/O ==================== */

#define MAX_TEST_FRAMES 48 /* frames available in test data */
#define DEFAULT_DATA_DIR "/tmp/vmaf_test"

static unsigned g_bpc = 8; /* configurable via --bpc */
static const char *g_datadir = NULL;

static const char *get_data_dir(void)
{
    if (!g_datadir) {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-1155: single-threaded benchmark initialization
        g_datadir = getenv("VMAF_TEST_DATA");
        if (!g_datadir || !g_datadir[0])
            g_datadir = DEFAULT_DATA_DIR;
    }
    return g_datadir;
}

typedef struct {
    FILE *ref_fp;
    FILE *dis_fp;
    unsigned width;
    unsigned height;
    size_t frame_bytes;
    uint8_t *ref_buf;
    uint8_t *dis_buf;
} YuvPair;

static int yuv_pair_open(YuvPair *yp, unsigned w, unsigned h)
{
    char ref_path[1280];
    char dis_path[1280];
    (void)snprintf(ref_path, sizeof(ref_path), "%s/ref_%ux%u.yuv", get_data_dir(), w, h);
    (void)snprintf(dis_path, sizeof(dis_path), "%s/dis_%ux%u.yuv", get_data_dir(), w, h);

    /* Canonicalize to a real, existing path before opening. realpath(3)
     * (POSIX) / _fullpath (Windows) collapses ".." and symlinks; if the
     * resolved path doesn't exist it returns NULL and we abort the open.
     * The data-dir input is trusted-by-design (this is a developer
     * benchmark binary, never invoked over a network surface — see file
     * header) but CodeQL's cpp/path-injection still flags getenv() flowing
     * to fopen(); canonicalisation eliminates the taint without
     * changing semantics for the legitimate use case. */
    char ref_resolved[PATH_MAX];
    char dis_resolved[PATH_MAX];
#ifdef _WIN32
    const char *ref_can = _fullpath(ref_resolved, ref_path, PATH_MAX);
    const char *dis_can = _fullpath(dis_resolved, dis_path, PATH_MAX);
#else
    const char *ref_can = realpath(ref_path, ref_resolved);
    const char *dis_can = realpath(dis_path, dis_resolved);
#endif
    yp->ref_fp = ref_can ? fopen(ref_can, "rb") : NULL;
    yp->dis_fp = dis_can ? fopen(dis_can, "rb") : NULL;
    if (!yp->ref_fp || !yp->dis_fp) {
        (void)fprintf(stderr,
                      "Cannot open test data for %ux%u\n"
                      "  ref: %s (%s)\n  dis: %s (%s)\n"
                      "Set VMAF_TEST_DATA or --data-dir to your data directory.\n",
                      w, h, ref_path, yp->ref_fp ? "ok" : "MISSING", dis_path,
                      yp->dis_fp ? "ok" : "MISSING");
        if (yp->ref_fp)
            (void)fclose(yp->ref_fp);
        if (yp->dis_fp)
            (void)fclose(yp->dis_fp);
        memset(yp, 0, sizeof(*yp));
        return -1;
    }

    yp->width = w;
    yp->height = h;
    yp->frame_bytes = (size_t)w * h * 3 / 2; /* YUV420P 8-bit */
    yp->ref_buf = malloc(yp->frame_bytes);
    yp->dis_buf = malloc(yp->frame_bytes);
    if (!yp->ref_buf || !yp->dis_buf) {
        free(yp->ref_buf);
        free(yp->dis_buf);
        (void)fclose(yp->ref_fp);
        (void)fclose(yp->dis_fp);
        memset(yp, 0, sizeof(*yp));
        return -1;
    }
    return 0;
}

static void copy_plane_y(const YuvPair *const yp, VmafPicture *const ref, VmafPicture *const dist,
                         const int hbd, const unsigned shift)
{
    const size_t w = yp->width;
    const size_t h = yp->height;
    for (size_t y = 0; y < h; y++) {
        if (hbd) {
            uint16_t *const rdst = (uint16_t *)((uint8_t *)ref->data[0] + y * ref->stride[0]);
            uint16_t *const ddst = (uint16_t *)((uint8_t *)dist->data[0] + y * dist->stride[0]);
            for (size_t x = 0; x < w; x++) {
                rdst[x] = (uint16_t)(yp->ref_buf[y * w + x]) << shift;
                ddst[x] = (uint16_t)(yp->dis_buf[y * w + x]) << shift;
            }
        } else {
            (void)memcpy((uint8_t *)ref->data[0] + y * ref->stride[0], yp->ref_buf + y * w, w);
            (void)memcpy((uint8_t *)dist->data[0] + y * dist->stride[0], yp->dis_buf + y * w, w);
        }
    }
}

static void copy_plane_u(const YuvPair *const yp, VmafPicture *const ref, VmafPicture *const dist,
                         const int hbd, const unsigned shift, const size_t y_bytes)
{
    const size_t uv_w = yp->width / 2;
    const size_t uv_h = yp->height / 2;
    for (size_t y = 0; y < uv_h; y++) {
        if (hbd) {
            uint16_t *const rdst = (uint16_t *)((uint8_t *)ref->data[1] + y * ref->stride[1]);
            uint16_t *const ddst = (uint16_t *)((uint8_t *)dist->data[1] + y * dist->stride[1]);
            for (size_t x = 0; x < uv_w; x++) {
                rdst[x] = (uint16_t)(yp->ref_buf[y_bytes + y * uv_w + x]) << shift;
                ddst[x] = (uint16_t)(yp->dis_buf[y_bytes + y * uv_w + x]) << shift;
            }
        } else {
            (void)memcpy((uint8_t *)ref->data[1] + y * ref->stride[1],
                         yp->ref_buf + y_bytes + y * uv_w, uv_w);
            (void)memcpy((uint8_t *)dist->data[1] + y * dist->stride[1],
                         yp->dis_buf + y_bytes + y * uv_w, uv_w);
        }
    }
}

static void copy_plane_v(const YuvPair *const yp, VmafPicture *const ref, VmafPicture *const dist,
                         const int hbd, const unsigned shift, const size_t y_bytes,
                         const size_t uv_bytes)
{
    const size_t uv_w = yp->width / 2;
    const size_t uv_h = yp->height / 2;
    for (size_t y = 0; y < uv_h; y++) {
        if (hbd) {
            uint16_t *const rdst = (uint16_t *)((uint8_t *)ref->data[2] + y * ref->stride[2]);
            uint16_t *const ddst = (uint16_t *)((uint8_t *)dist->data[2] + y * dist->stride[2]);
            for (size_t x = 0; x < uv_w; x++) {
                rdst[x] = (uint16_t)(yp->ref_buf[y_bytes + uv_bytes + y * uv_w + x]) << shift;
                ddst[x] = (uint16_t)(yp->dis_buf[y_bytes + uv_bytes + y * uv_w + x]) << shift;
            }
        } else {
            (void)memcpy((uint8_t *)ref->data[2] + y * ref->stride[2],
                         yp->ref_buf + y_bytes + uv_bytes + y * uv_w, uv_w);
            (void)memcpy((uint8_t *)dist->data[2] + y * dist->stride[2],
                         yp->dis_buf + y_bytes + uv_bytes + y * uv_w, uv_w);
        }
    }
}

static int yuv_pair_read_frame(YuvPair *const yp, const unsigned frame_idx, VmafPicture *const ref,
                               VmafPicture *const dist)
{
    const size_t offset = (size_t)frame_idx * yp->frame_bytes;
    if (fseek(yp->ref_fp, (long)offset, SEEK_SET) != 0) {
        perror("fseek(ref)");
        return -EIO;
    }
    if (fseek(yp->dis_fp, (long)offset, SEEK_SET) != 0) {
        perror("fseek(dis)");
        return -EIO;
    }

    if (fread(yp->ref_buf, 1, yp->frame_bytes, yp->ref_fp) != yp->frame_bytes ||
        fread(yp->dis_buf, 1, yp->frame_bytes, yp->dis_fp) != yp->frame_bytes) {
        (void)fprintf(stderr, "Short read at frame %u\n", frame_idx);
        return -1;
    }

    const size_t y_bytes = (size_t)yp->width * yp->height;
    const size_t uv_bytes = (size_t)(yp->width / 2) * (yp->height / 2);
    const int hbd = (g_bpc > 8);
    const unsigned shift = hbd ? (g_bpc - 8) : 0;

    copy_plane_y(yp, ref, dist, hbd, shift);
    copy_plane_u(yp, ref, dist, hbd, shift, y_bytes);
    copy_plane_v(yp, ref, dist, hbd, shift, y_bytes, uv_bytes);

    return 0;
}

static void yuv_pair_close(YuvPair *yp)
{
    if (yp->ref_fp)
        (void)fclose(yp->ref_fp);
    if (yp->dis_fp)
        (void)fclose(yp->dis_fp);
    free(yp->ref_buf);
    free(yp->dis_buf);
    memset(yp, 0, sizeof(*yp));
}

/* ==================== Backend enum ==================== */

enum Backend {
    BACKEND_CPU = 0,
    BACKEND_CUDA = 2,
    BACKEND_SYCL = 3,
};

typedef struct {
    const char *label;
    const char *feature;
    enum Backend backend;
} BenchTarget;

static const BenchTarget targets[] = {
    {"motion (CPU)", "motion", BACKEND_CPU},
    {"vif (CPU)", "vif", BACKEND_CPU},
    {"adm (CPU)", "adm", BACKEND_CPU},
    {"float_ssim (CPU)", "float_ssim", BACKEND_CPU},
    {"float_ms_ssim (CPU)", "float_ms_ssim", BACKEND_CPU},
    {"psnr (CPU)", "psnr", BACKEND_CPU},
#ifdef HAVE_CUDA
    {"motion (CUDA)", "motion_cuda", BACKEND_CUDA},
    {"vif (CUDA)", "vif_cuda", BACKEND_CUDA},
    {"adm (CUDA)", "adm_cuda", BACKEND_CUDA},
#endif
#ifdef HAVE_SYCL
    {"motion (SYCL)", "motion_sycl", BACKEND_SYCL},
    {"vif (SYCL)", "vif_sycl", BACKEND_SYCL},
    {"adm (SYCL)", "adm_sycl", BACKEND_SYCL},
#endif
};
static const int n_targets = sizeof(targets) / sizeof(targets[0]);

typedef struct {
    unsigned width;
    unsigned height;
} Resolution;

static const Resolution resolutions[] = {
    {576, 324}, {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160},
};
static const int n_resolutions = sizeof(resolutions) / sizeof(resolutions[0]);

#ifdef HAVE_SYCL
static int g_gpu_device_idx = -1; /* -1 = auto; applies to SYCL */

static int run_sycl_gpu_profile(unsigned w, unsigned h, unsigned n_frames)
{
    int err = 0;

    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 1,
        .n_subsample = 0,
        .cpumask = 0,
        .gpumask = 0,
    };

    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;

    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {
        .device_index = g_gpu_device_idx,
        .enable_profiling = 1,
    };
    err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err) {
        vmaf_close(vmaf);
        return err;
    }

    err = vmaf_sycl_profiling_enable(sycl_state);
    if (err) {
        fprintf(stderr, "Failed to enable SYCL profiling: %d\n", err);
        vmaf_close(vmaf);
        return err;
    }

    err = vmaf_sycl_import_state(vmaf, sycl_state);
    if (err) {
        vmaf_close(vmaf);
        return err;
    }

    /* Register all three SYCL features */
    err = vmaf_use_feature(vmaf, "motion_sycl", NULL);
    if (err) {
        vmaf_close(vmaf);
        return err;
    }
    err = vmaf_use_feature(vmaf, "vif_sycl", NULL);
    if (err) {
        vmaf_close(vmaf);
        return err;
    }
    err = vmaf_use_feature(vmaf, "adm_sycl", NULL);
    if (err) {
        vmaf_close(vmaf);
        return err;
    }

    /* Run frames */
    YuvPair yp = {.ref_fp = NULL,
                  .dis_fp = NULL,
                  .width = 0,
                  .height = 0,
                  .frame_bytes = 0,
                  .ref_buf = NULL,
                  .dis_buf = NULL};
    if (yuv_pair_open(&yp, w, h)) {
        vmaf_close(vmaf);
        return -1;
    }

    for (unsigned i = 0; i < n_frames; i++) {
        VmafPicture ref, dist;
        /* S9 fix (2026-05-30): vmaf_picture_alloc can fail (returns -ENOMEM
         * or -EINVAL). Previously the returns were discarded, and the
         * subsequent yuv_pair_read_frame -> ref->data[0] dereference would
         * crash on a sentinel-zero VmafPicture. Bail out with the libvmaf
         * error code instead. */
        err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, g_bpc, w, h);
        if (err) {
            (void)fprintf(stderr, "vmaf_picture_alloc(ref) failed at frame %u (err=%d)\n", i, err);
            break;
        }
        err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, g_bpc, w, h);
        if (err) {
            (void)fprintf(stderr, "vmaf_picture_alloc(dist) failed at frame %u (err=%d)\n", i, err);
            (void)vmaf_picture_unref(&ref);
            break;
        }
        if (yuv_pair_read_frame(&yp, i, &ref, &dist)) {
            (void)vmaf_picture_unref(&ref);
            (void)vmaf_picture_unref(&dist);
            break;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            break;
    }
    yuv_pair_close(&yp);

    /* Print per-kernel timing */
    (void)printf("SYCL Kernel Profile (%ux%u, %u-bit, %u frames)\n", w, h, g_bpc, n_frames);
    vmaf_sycl_profiling_print(sycl_state);

    /* S9 fix (2026-05-30): the final flush vmaf_read_pictures(..., NULL,
     * NULL, 0) signals end-of-stream to libvmaf; its return code surfaces
     * pooling / aggregation errors and previously was discarded. Capture
     * and propagate. */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err)
        (void)fprintf(stderr, "vmaf_read_pictures(flush) failed (err=%d)\n", err);
    (void)vmaf_close(vmaf);
    return err;
}
#endif /* HAVE_SYCL */

/* ==================== Benchmark core ==================== */

static int bench_warmup(VmafContext *const vmaf, YuvPair *const yp, const unsigned w,
                        const unsigned h, double *const out_init_ms, const double t0)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, g_bpc, w, h);
    if (err) {
        (void)fprintf(stderr, "vmaf_picture_alloc(ref) failed (err=%d)\n", err);
        return err;
    }
    err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, g_bpc, w, h);
    if (err) {
        (void)fprintf(stderr, "vmaf_picture_alloc(dist) failed (err=%d)\n", err);
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    if (yuv_pair_read_frame(yp, 0, &ref, &dist) != 0) {
        (void)vmaf_picture_unref(&ref);
        (void)vmaf_picture_unref(&dist);
        return -1;
    }
    err = vmaf_read_pictures(vmaf, &ref, &dist, 0);
    if (err)
        return err;
    const double t1 = now_ms();
    *out_init_ms = t1 - t0;
    return 0;
}

static int bench_run_loop(VmafContext *const vmaf, YuvPair *const yp, const unsigned w,
                          const unsigned h, const unsigned n_frames, double *const out_total_ms,
                          double *const out_avg_ms)
{
    const double t0 = now_ms();
    int err = 0;
    for (unsigned i = 1; i < n_frames; i++) {
        VmafPicture r;
        VmafPicture d;
        err = vmaf_picture_alloc(&r, VMAF_PIX_FMT_YUV420P, g_bpc, w, h);
        if (err) {
            (void)fprintf(stderr, "vmaf_picture_alloc(r) failed at frame %u (err=%d)\n", i, err);
            break;
        }
        err = vmaf_picture_alloc(&d, VMAF_PIX_FMT_YUV420P, g_bpc, w, h);
        if (err) {
            (void)fprintf(stderr, "vmaf_picture_alloc(d) failed at frame %u (err=%d)\n", i, err);
            (void)vmaf_picture_unref(&r);
            break;
        }
        if (yuv_pair_read_frame(yp, i, &r, &d) != 0) {
            (void)vmaf_picture_unref(&r);
            (void)vmaf_picture_unref(&d);
            err = -1;
            break;
        }
        err = vmaf_read_pictures(vmaf, &r, &d, i);
        if (err)
            break;
    }
    const double t1 = now_ms();
    *out_total_ms = t1 - t0;
    *out_avg_ms = *out_total_ms / (n_frames - 1);
    return err;
}

static VmafContext *bench_create_vmaf_context(const BenchTarget *const target)
{
    const int is_gpu = (target->backend != BACKEND_CPU);
    const VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 1,
        .n_subsample = 0,
        .cpumask = 0,
        .gpumask = is_gpu ? 0 : (unsigned)~0,
    };
    VmafContext *vmaf = NULL;
    const int err = vmaf_init(&vmaf, cfg);
    return (err == 0) ? vmaf : NULL;
}

typedef struct {
#ifdef HAVE_CUDA
    VmafCudaState *cu_state;
#endif
#ifdef HAVE_SYCL
    VmafSyclState *sycl_state;
#endif
    int dummy;
} BenchGpuState;

static int bench_init_gpu_state(const BenchTarget *const target, VmafContext *const vmaf,
                                BenchGpuState *const gpu)
{
#ifdef HAVE_CUDA
    if (target->backend == BACKEND_CUDA) {
        const VmafCudaConfiguration cu_cfg = {0};
        int err = vmaf_cuda_state_init(&gpu->cu_state, cu_cfg);
        if (err)
            return err;
        return vmaf_cuda_import_state(vmaf, gpu->cu_state);
    }
#endif
#ifdef HAVE_SYCL
    if (target->backend == BACKEND_SYCL) {
        const VmafSyclConfiguration sycl_cfg = {.device_index = g_gpu_device_idx};
        int err = vmaf_sycl_state_init(&gpu->sycl_state, sycl_cfg);
        if (err)
            return err;
        return vmaf_sycl_import_state(vmaf, gpu->sycl_state);
    }
#endif
    (void)target;
    (void)vmaf;
    (void)gpu;
    return 0;
}

static void bench_cleanup_resources(VmafContext *const vmaf, YuvPair *const yp, const bool yp_open,
                                    BenchGpuState *const gpu)
{
    if (yp_open)
        yuv_pair_close(yp);
    vmaf_close(vmaf);
#ifdef HAVE_CUDA
    if (gpu->cu_state)
        (void)vmaf_cuda_state_free(gpu->cu_state);
#endif
#ifdef HAVE_SYCL
    if (gpu->sycl_state)
        vmaf_sycl_state_free(&gpu->sycl_state);
#endif
    (void)gpu;
}

static int bench_feature(const BenchTarget *const target, const unsigned w, const unsigned h,
                         const unsigned n_frames, double *const out_init_ms,
                         double *const out_avg_ms, double *const out_total_ms)
{
    VmafContext *const vmaf = bench_create_vmaf_context(target);
    if (!vmaf)
        return -1;
    int err = 0;

    BenchGpuState gpu = {0};
    YuvPair yp = {.ref_fp = NULL,
                  .dis_fp = NULL,
                  .width = 0,
                  .height = 0,
                  .frame_bytes = 0,
                  .ref_buf = NULL,
                  .dis_buf = NULL};
    bool yp_open = false;

    err = bench_init_gpu_state(target, vmaf, &gpu);
    if (err)
        goto bench_cleanup;

    const double t0 = now_ms();
    err = vmaf_use_feature(vmaf, target->feature, NULL);
    if (err)
        goto bench_cleanup;

    if (yuv_pair_open(&yp, w, h) != 0) {
        err = -1;
        goto bench_cleanup;
    }
    yp_open = true;

    err = bench_warmup(vmaf, &yp, w, h, out_init_ms, t0);
    if (err)
        goto bench_cleanup;

    err = bench_run_loop(vmaf, &yp, w, h, n_frames, out_total_ms, out_avg_ms);

    const int flush_err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (flush_err)
        (void)fprintf(stderr, "vmaf_read_pictures(flush) failed (err=%d)\n", flush_err);

bench_cleanup:
    bench_cleanup_resources(vmaf, &yp, yp_open, &gpu);
    return err;
}

static void print_separator(int cols)
{
    for (int i = 0; i < cols; i++)
        (void)fputc('-', stdout);
    (void)fputc('\n', stdout);
}

/* ==================== Validation mode ==================== */

#if defined(HAVE_CUDA) || defined(HAVE_SYCL)

typedef struct {
    const char *cpu_feature;
    const char *gpu_feature;
    const char *label;
    enum Backend backend;
    double tolerance;
    const char *score_names[16];
} ValidationPair;

static const ValidationPair validation_pairs[] = {
#ifdef HAVE_CUDA
    {"motion",
     "motion_cuda",
     "Motion/CU",
     BACKEND_CUDA,
     5e-6,
     {"VMAF_integer_feature_motion_score", "VMAF_integer_feature_motion2_score", NULL}},
    {"vif",
     "vif_cuda",
     "VIF/CU",
     BACKEND_CUDA,
     0.001,
     {"VMAF_integer_feature_vif_scale0_score", "VMAF_integer_feature_vif_scale1_score",
      "VMAF_integer_feature_vif_scale2_score", "VMAF_integer_feature_vif_scale3_score", NULL}},
    {"adm",
     "adm_cuda",
     "ADM/CU",
     BACKEND_CUDA,
     0.5,
     {"VMAF_integer_feature_adm2_score", "integer_adm_scale0", "integer_adm_scale1",
      "integer_adm_scale2", "integer_adm_scale3", NULL}},
#endif
#ifdef HAVE_SYCL
    {"motion",
     "motion_sycl",
     "Motion/SYCL",
     BACKEND_SYCL,
     5e-6,
     {"VMAF_integer_feature_motion2_score", NULL}},
    {"vif",
     "vif_sycl",
     "VIF/SYCL",
     BACKEND_SYCL,
     0.001,
     {"VMAF_integer_feature_vif_scale0_score", "VMAF_integer_feature_vif_scale1_score",
      "VMAF_integer_feature_vif_scale2_score", "VMAF_integer_feature_vif_scale3_score", NULL}},
    {"adm",
     "adm_sycl",
     "ADM/SYCL",
     BACKEND_SYCL,
     0.5,
     {"VMAF_integer_feature_adm2_score", "integer_adm_scale0", "integer_adm_scale1",
      "integer_adm_scale2", "integer_adm_scale3", NULL}},
#endif
};
static const int n_validation_pairs = sizeof(validation_pairs) / sizeof(validation_pairs[0]);

/* Run a feature extractor and collect per-frame scores */
static int run_feature_collect(const char *feature, enum Backend backend, unsigned w, unsigned h,
                               unsigned n_frames, const char *const *score_names,
                               double scores[][16])
{
    int err = 0;
    int is_gpu = (backend != BACKEND_CPU);

    VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = 1,
        .n_subsample = 0,
        .cpumask = 0,
        .gpumask = is_gpu ? 0 : (unsigned)~0,
    };

    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;

    /* T5 (state-leak audit 2026-05-30): hoist the GPU state pointers to
     * function scope so every early-return path (and the normal-exit
     * path) can release them. Previously these lived inside the
     * `#ifdef` branches and were leaked the instant `vmaf_use_feature`,
     * `yuv_pair_open`, or any per-frame call failed. */
#ifdef HAVE_CUDA
    VmafCudaState *cu_state = NULL;
#endif
#ifdef HAVE_SYCL
    VmafSyclState *sycl_state = NULL;
#endif

#ifdef HAVE_CUDA
    if (backend == BACKEND_CUDA) {
        VmafCudaConfiguration cu_cfg = {0};
        err = vmaf_cuda_state_init(&cu_state, cu_cfg);
        if (err) {
            vmaf_close(vmaf);
            return err;
        }
        err = vmaf_cuda_import_state(vmaf, cu_state);
        if (err) {
            vmaf_close(vmaf);
            (void)vmaf_cuda_state_free(cu_state);
            return err;
        }
    }
#endif
#ifdef HAVE_SYCL
    if (backend == BACKEND_SYCL) {
        VmafSyclConfiguration sycl_cfg = {.device_index = g_gpu_device_idx};
        err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
        if (err) {
            vmaf_close(vmaf);
            return err;
        }
        err = vmaf_sycl_import_state(vmaf, sycl_state);
        if (err) {
            vmaf_close(vmaf);
            vmaf_sycl_state_free(&sycl_state);
            return err;
        }
    }
#endif

    err = vmaf_use_feature(vmaf, feature, NULL);
    if (err) {
        vmaf_close(vmaf);
#ifdef HAVE_CUDA
        if (cu_state)
            (void)vmaf_cuda_state_free(cu_state);
#endif
#ifdef HAVE_SYCL
        if (sycl_state)
            vmaf_sycl_state_free(&sycl_state);
#endif
        return err;
    }

    YuvPair yp = {.ref_fp = NULL,
                  .dis_fp = NULL,
                  .width = 0,
                  .height = 0,
                  .frame_bytes = 0,
                  .ref_buf = NULL,
                  .dis_buf = NULL};
    if (yuv_pair_open(&yp, w, h)) {
        vmaf_close(vmaf);
#ifdef HAVE_CUDA
        if (cu_state)
            (void)vmaf_cuda_state_free(cu_state);
#endif
#ifdef HAVE_SYCL
        if (sycl_state)
            vmaf_sycl_state_free(&sycl_state);
#endif
        return -1;
    }

    for (unsigned i = 0; i < n_frames; i++) {
        VmafPicture r, d;
        /* ADR-1081: check alloc returns; a zeroed VmafPicture on failure
         * causes a null-deref in yuv_pair_read_frame. */
        err = vmaf_picture_alloc(&r, VMAF_PIX_FMT_YUV420P, g_bpc, w, h);
        if (err) {
            (void)fprintf(stderr, "vmaf_picture_alloc(r) failed at frame %u (err=%d)\n", i, err);
            yuv_pair_close(&yp);
            vmaf_close(vmaf);
#ifdef HAVE_CUDA
            if (cu_state)
                (void)vmaf_cuda_state_free(cu_state);
#endif
#ifdef HAVE_SYCL
            if (sycl_state)
                vmaf_sycl_state_free(&sycl_state);
#endif
            return err;
        }
        err = vmaf_picture_alloc(&d, VMAF_PIX_FMT_YUV420P, g_bpc, w, h);
        if (err) {
            (void)fprintf(stderr, "vmaf_picture_alloc(d) failed at frame %u (err=%d)\n", i, err);
            (void)vmaf_picture_unref(&r);
            yuv_pair_close(&yp);
            vmaf_close(vmaf);
#ifdef HAVE_CUDA
            if (cu_state)
                (void)vmaf_cuda_state_free(cu_state);
#endif
#ifdef HAVE_SYCL
            if (sycl_state)
                vmaf_sycl_state_free(&sycl_state);
#endif
            return err;
        }
        if (yuv_pair_read_frame(&yp, i, &r, &d)) {
            (void)vmaf_picture_unref(&r);
            (void)vmaf_picture_unref(&d);
            yuv_pair_close(&yp);
            vmaf_close(vmaf);
#ifdef HAVE_CUDA
            if (cu_state)
                (void)vmaf_cuda_state_free(cu_state);
#endif
#ifdef HAVE_SYCL
            if (sycl_state)
                vmaf_sycl_state_free(&sycl_state);
#endif
            return -1;
        }
        err = vmaf_read_pictures(vmaf, &r, &d, i);
        if (err) {
            yuv_pair_close(&yp);
            vmaf_close(vmaf);
#ifdef HAVE_CUDA
            if (cu_state)
                (void)vmaf_cuda_state_free(cu_state);
#endif
#ifdef HAVE_SYCL
            if (sycl_state)
                vmaf_sycl_state_free(&sycl_state);
#endif
            return err;
        }
    }
    yuv_pair_close(&yp);
    /* ADR-1081: capture flush return; a silent discard here masks
     * aggregation/pooling errors in validation mode. */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err)
        (void)fprintf(stderr, "vmaf_read_pictures(flush) failed (err=%d)\n", err);

    /* Collect scores */
    for (unsigned i = 0; i < n_frames; i++) {
        for (int s = 0; score_names[s]; s++) {
            double val = 0.0;
            err = vmaf_feature_score_at_index(vmaf, score_names[s], &val, i);
            scores[i][s] = err ? NAN : val;
        }
    }

    vmaf_close(vmaf);
#ifdef HAVE_CUDA
    if (cu_state)
        (void)vmaf_cuda_state_free(cu_state);
#endif
#ifdef HAVE_SYCL
    if (sycl_state)
        vmaf_sycl_state_free(&sycl_state);
#endif
    return 0;
}

static int run_validation(unsigned w, unsigned h, unsigned n_frames)
{
    int total_fail = 0;
    char res_str[16];
    snprintf(res_str, sizeof(res_str), "%ux%u", w, h);

    for (int p = 0; p < n_validation_pairs; p++) {
        const ValidationPair *vp = &validation_pairs[p];

        /* Count score names */
        int n_scores = 0;
        while (vp->score_names[n_scores])
            n_scores++;

        double (*cpu_scores)[16] = calloc(n_frames, sizeof(*cpu_scores));
        double (*gpu_scores)[16] = calloc(n_frames, sizeof(*gpu_scores));
        if (!cpu_scores || !gpu_scores) {
            fprintf(stderr, "allocation failed\n");
            free(cpu_scores);
            free(gpu_scores);
            return -1;
        }

        int err_cpu = run_feature_collect(vp->cpu_feature, BACKEND_CPU, w, h, n_frames,
                                          vp->score_names, cpu_scores);
        int err_gpu = run_feature_collect(vp->gpu_feature, vp->backend, w, h, n_frames,
                                          vp->score_names, gpu_scores);

        if (err_cpu || err_gpu) {
            printf("  %-10s @ %s: SKIP (cpu_err=%d gpu_err=%d)\n", vp->label, res_str, err_cpu,
                   err_gpu);
            free(cpu_scores);
            free(gpu_scores);
            continue;
        }

        /* Compare scores per frame per metric */
        for (int s = 0; s < n_scores; s++) {
            double max_diff = 0.0;
            int any_nan = 0;
            for (unsigned f = 0; f < n_frames; f++) {
                double c = cpu_scores[f][s];
                double v = gpu_scores[f][s];
                // Both NaN is acceptable (e.g. motion2 at index 1)
                if (isnan(c) && isnan(v))
                    continue;
                // One NaN but not the other is a real mismatch
                if (isnan(c) || isnan(v)) {
                    any_nan = 1;
                    continue;
                }
                double diff = fabs(c - v);
                if (diff > max_diff)
                    max_diff = diff;
            }

            const double tol = vp->tolerance;
            int pass = !any_nan && max_diff <= tol;
            const char *status = pass ? "PASS" : (any_nan ? "NaN!" : "FAIL");

            if (!pass)
                total_fail++;

            printf("  %-10s @ %s  %-45s  max_diff=%.2e  [%s]\n", vp->label, res_str,
                   vp->score_names[s], max_diff, status);
        }

        free(cpu_scores);
        free(gpu_scores);
    }

    return total_fail;
}

#endif /* HAVE_CUDA || HAVE_SYCL */

/* ==================== Main ==================== */

typedef struct {
    unsigned n_frames;
    int res_idx;
    int validate_mode;
    int list_devices;
    int gpu_profile_mode;
    int gpu_only;
} BenchOptions;

static void print_bench_help(void)
{
    (void)printf("Usage: vmaf_bench [OPTIONS]\n\n"
                 "Performance benchmark mode (default):\n"
                 "  --frames N        Number of frames per benchmark (default: 10, max: 48)\n"
                 "  --resolution WxH  Single resolution to test (default: all)\n"
                 "  --bpc N           Bits per component (8, 10, 12, 16; default: 8)\n"
                 "  --data-dir PATH   Path to test data directory (default: %s)\n"
                 "                    Override with VMAF_TEST_DATA env var\n\n"
                 "GPU device selection:\n"
                 "  --list-devices    List available GPU devices\n"
                 "  --device N        Select GPU device by index (default: auto)\n\n"
                 "Validation mode (GPU vs CPU correctness):\n"
                 "  --validate        Compare GPU vs CPU output scores\n"
                 "  --frames N        Number of frames to compare (default: 10)\n"
                 "  --resolution WxH  Single resolution to test (default: all)\n\n"
                 "GPU profiling mode (per-shader timing):\n"
                 "  --gpu-profile     Print per-shader GPU timing breakdown\n"
                 "  --gpu-only        Skip CPU features in benchmark mode\n",
                 DEFAULT_DATA_DIR);
}

static int parse_resolution_arg(const char *const arg, int *const res_idx)
{
    char *end = NULL;
    const long rw_l = strtol(arg, &end, 10);
    if (end == arg || *end != 'x' || rw_l <= 0 || rw_l > INT_MAX) {
        (void)fprintf(stderr, "Invalid --resolution: %s\n", arg);
        return -1;
    }
    const char *p = end + 1;
    char *end2 = NULL;
    const long rh_l = strtol(p, &end2, 10);
    if (end2 == p || *end2 != '\0' || rh_l <= 0 || rh_l > INT_MAX) {
        (void)fprintf(stderr, "Invalid --resolution: %s\n", arg);
        return -1;
    }
    const unsigned rw = (unsigned)rw_l;
    const unsigned rh = (unsigned)rh_l;
    for (int j = 0; j < n_resolutions; j++) {
        if (resolutions[j].width == rw && resolutions[j].height == rh) {
            *res_idx = j;
            return 0;
        }
    }
    (void)fprintf(stderr,
                  "Unknown resolution %ux%u. "
                  "Supported: 576x324, 640x480, 1280x720, "
                  "1920x1080, 3840x2160\n",
                  rw, rh);
    return -1;
}

// NOLINTNEXTLINE(readability-non-const-parameter) — ADR-1155: modified when HAVE_SYCL is enabled
static int parse_bench_gpu_opt(const char *const arg, int *const i, const int argc,
                               char *const argv[], BenchOptions *const opts)
{
#if !defined(HAVE_SYCL)
    (void)i;
    (void)argc;
    (void)argv;
#endif
    if (strcmp(arg, "--gpu-only") == 0) {
        opts->gpu_only = 1;
        return 1;
    }
    if (strcmp(arg, "--list-devices") == 0) {
        opts->list_devices = 1;
        return 1;
    }
#if defined(HAVE_SYCL)
    if (strcmp(arg, "--gpu-profile") == 0) {
        opts->gpu_profile_mode = 1;
        return 1;
    }
    if (strcmp(arg, "--device") == 0 && *i + 1 < argc) {
        char *end = NULL;
        const long v = strtol(argv[++(*i)], &end, 10);
        if (end == argv[*i] || *end != '\0' || v < 0 || v > INT_MAX) {
            (void)fprintf(stderr, "Invalid --device value: %s\n", argv[*i]);
            return -1;
        }
        g_gpu_device_idx = (int)v;
        return 1;
    }
#else
    if (strcmp(arg, "--gpu-profile") == 0) {
        (void)fprintf(stderr, "--gpu-profile requires SYCL support\n");
        return -1;
    }
    if (strcmp(arg, "--device") == 0 && *i + 1 < argc) {
        (void)fprintf(stderr, "--device requires SYCL support\n");
        return -1;
    }
#endif
    return 0;
}

static int parse_bench_opt(int *const i, const int argc, char *const argv[],
                           BenchOptions *const opts)
{
    const char *const arg = argv[*i];
    const int gpu_rc = parse_bench_gpu_opt(arg, i, argc, argv, opts);
    if (gpu_rc != 0)
        return (gpu_rc < 0) ? -1 : 0;

    if (strcmp(arg, "--frames") == 0 && *i + 1 < argc) {
        char *end = NULL;
        const long v = strtol(argv[++(*i)], &end, 10);
        if (end == argv[*i] || *end != '\0' || v < 0 || v > INT_MAX) {
            (void)fprintf(stderr, "Invalid --frames value: %s\n", argv[*i]);
            return -1;
        }
        opts->n_frames = (v < 2) ? 2u : (unsigned)v;
        return 0;
    }
    if (strcmp(arg, "--resolution") == 0 && *i + 1 < argc)
        return parse_resolution_arg(argv[++(*i)], &opts->res_idx);

    if (strcmp(arg, "--bpc") == 0 && *i + 1 < argc) {
        char *end = NULL;
        const long v = strtol(argv[++(*i)], &end, 10);
        if (end == argv[*i] || *end != '\0' || v < 0 || v > 16 ||
            (v != 8 && v != 10 && v != 12 && v != 16)) {
            (void)fprintf(stderr, "Unsupported bpc: %ld (use 8, 10, 12, or 16)\n", v);
            return -1;
        }
        g_bpc = (unsigned)v;
        return 0;
    }
    if (strcmp(arg, "--validate") == 0) {
        opts->validate_mode = 1;
        return 0;
    }
    if (strcmp(arg, "--data-dir") == 0 && *i + 1 < argc) {
        g_datadir = argv[++(*i)];
        return 0;
    }
    if (strcmp(arg, "--help") == 0) {
        print_bench_help();
        return 1;
    }
    return 0;
}

static int run_bench_loop(const int r_start, const int r_end, const unsigned n_frames,
                          const int gpu_only)
{
    (void)printf("VMAF Performance Benchmark (%s)\nData: %s\nFrames per test: %u\n\n",
                 vmaf_version(), get_data_dir(), n_frames);
    const int col_w = 88;
    (void)printf("%-28s  %8s  %8s  %8s  %8s  %8s\n", "Feature", "Res", "Init ms", "Avg ms",
                 "Total ms", "FPS");
    print_separator(col_w);

    for (int t = 0; t < n_targets; t++) {
        if (gpu_only && targets[t].backend == BACKEND_CPU)
            continue;
        for (int r = r_start; r < r_end; r++) {
            const unsigned w = resolutions[r].width;
            const unsigned h = resolutions[r].height;
            double init_ms = 0;
            double avg_ms = 0;
            double total_ms = 0;
            char res_str[16];
            (void)snprintf(res_str, sizeof(res_str), "%ux%u", w, h);

            const int err =
                bench_feature(&targets[t], w, h, n_frames, &init_ms, &avg_ms, &total_ms);
            if (err) {
                (void)printf("%-28s  %8s  %8s  %8s  %8s  %8s\n", targets[t].label, res_str, "FAIL",
                             "-", "-", "-");
                continue;
            }
            const double fps = (n_frames - 1) / (total_ms / 1000.0);
            (void)printf("%-28s  %8s  %8.1f  %8.2f  %8.1f  %8.1f\n", targets[t].label, res_str,
                         init_ms, avg_ms, total_ms, fps);
            (void)fflush(stdout);
        }
        if (r_end - r_start > 1)
            print_separator(col_w);
    }
    return 0;
}

static int run_bench_validation_mode(const int r_start, const int r_end, const unsigned n_frames)
{
#if defined(HAVE_CUDA) || defined(HAVE_SYCL)
    (void)printf("VMAF GPU Correctness Validation (%s)\nData: %s\nFrames per test: %u, bpc: %u\n\n",
                 vmaf_version(), get_data_dir(), n_frames, g_bpc);
    int total_fail = 0;
    for (int r = r_start; r < r_end; r++) {
        total_fail += run_validation(resolutions[r].width, resolutions[r].height, n_frames);
    }
    (void)printf("\n%s\n", (total_fail == 0) ? "ALL PASSED" : "FAILURES");
    return total_fail > 0 ? 1 : 0;
#else
    (void)fprintf(stderr, "No GPU backend enabled, cannot validate\n");
    (void)r_start;
    (void)r_end;
    (void)n_frames;
    return 1;
#endif
}

int main(int argc, char *argv[])
{
    BenchOptions opts = {.n_frames = 10,
                         .res_idx = -1,
                         .validate_mode = 0,
                         .list_devices = 0,
                         .gpu_profile_mode = 0,
                         .gpu_only = 0};

    for (int i = 1; i < argc; i++) {
        const int rc = parse_bench_opt(&i, argc, argv, &opts);
        if (rc < 0)
            return 1;
        if (rc > 0)
            return 0;
    }

    if (opts.list_devices) {
#ifdef HAVE_SYCL
        return (vmaf_sycl_list_devices() < 0) ? 1 : 0;
#else
        (void)fprintf(stderr, "No GPU backend enabled\n");
        return 0;
#endif
    }

    if (opts.n_frames > MAX_TEST_FRAMES) {
        (void)fprintf(stderr, "Warning: capping --frames %u to %d (available in test data)\n",
                      opts.n_frames, MAX_TEST_FRAMES);
        opts.n_frames = MAX_TEST_FRAMES;
    }

    const int r_start = opts.res_idx >= 0 ? opts.res_idx : 0;
    const int r_end = opts.res_idx >= 0 ? opts.res_idx + 1 : n_resolutions;

    if (opts.gpu_profile_mode) {
#ifdef HAVE_SYCL
        const unsigned pw = (opts.res_idx >= 0) ? resolutions[opts.res_idx].width : 3840u;
        const unsigned ph = (opts.res_idx >= 0) ? resolutions[opts.res_idx].height : 2160u;
        return run_sycl_gpu_profile(pw, ph, opts.n_frames);
#else
        (void)fprintf(stderr, "No GPU backend enabled\n");
        return 1;
#endif
    }

    if (opts.validate_mode)
        return run_bench_validation_mode(r_start, r_end, opts.n_frames);

    return run_bench_loop(r_start, r_end, opts.n_frames, opts.gpu_only);
}

/* NOLINTEND(modernize-use-nullptr) */
