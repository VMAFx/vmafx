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
 *  VMAF VPL Zero-Copy Pipeline Demo
 *
 *  Decodes two video files with Intel VPL (hardware decode on Intel GPU)
 *  and computes VMAF scores using SYCL feature extractors.
 *
 *  Pipeline (zero-copy):
 *    VPL decode → VA surface → DMA-BUF → Level Zero → SYCL → VMAF
 *
 *  Usage:
 *    vmaf_vpl --ref ref.mp4 --dis dis.mp4 [--model vmaf_v0.6.1]
 *             [--frames N] [--device N] [--render-node /dev/dri/renderD128]
 *
 *  Requirements:
 *    - Intel GPU with hardware decode support
 *    - Intel VPL runtime (libmfx-gen / vpl-gpu-rt)
 *    - libva + libva-drm + Level Zero
 */

#include <assert.h>
#include <libvmaf/model.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat/path_utf8.h"
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include <vpl/mfx.h>

#include "libvmaf/picture.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "vmaf_close_retry.h"

/* SYCL surface import: DMA-BUF/VA-API on Linux */
#include "../src/sycl/dmabuf_import.h"

#define VPL_PIPELINE_CLEANUP_FAILED INT_MIN

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void print_usage(const char *argv0)
{
    (void)fprintf(stderr,
                  "Usage: %s --ref <file> --dis <file> [options]\n"
                  "\n"
                  "Options:\n"
                  "  --ref <file>        Reference video file\n"
                  "  --dis <file>        Distorted video file\n"
                  "  --model <name>      VMAF model name (default: vmaf_v0.6.1)\n"
                  "  --frames <N>        Max frames to process (0 = all)\n"
                  "  --device <N>        SYCL device index (default: 0)\n"
                  "  --render-node <path> VA-API render node (default: /dev/dri/renderD128)\n"
                  "  --fallback          Use host upload if zero-copy import fails\n"
                  "  --help              Show this help\n"
                  "\n"
                  "Pipeline: VPL decode → VA surface → DMA-BUF → Level Zero → SYCL → VMAF\n",
                  argv0);
}

/* ------------------------------------------------------------------ */
/* GPU acceleration + VPL setup                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int drm_fd;
    VADisplay va_display;
    mfxLoader loader;
    mfxSession session;
    mfxVideoParam decode_params;
    int width;
    int height;
    int bpc; /* 8 or 10 */
    int eof;
    /* Bitstream buffer */
    mfxBitstream bs;
    uint8_t *bs_buf;
    size_t bs_buf_size;
    FILE *fp;
} VplDecoder;

/* Wall-clock ceiling handed to MFXVideoCORE_SyncOperation, in milliseconds. */
#define VPL_SYNC_TIMEOUT_MS 60000

/* Back-off between DecodeFrameAsync retries when the device reports busy or
 * asks for another surface, in microseconds. */
#define VPL_DECODE_RETRY_US 1000

/* Retry ceiling for one vpl_decode_frame() call: VPL_SYNC_TIMEOUT_MS of
 * VPL_DECODE_RETRY_US back-offs. See vpl_decode_frame() for the argument. */
#define VPL_DECODE_MAX_ATTEMPTS ((VPL_SYNC_TIMEOUT_MS * 1000u) / VPL_DECODE_RETRY_US)

static void vpl_cleanup_gpu(VplDecoder *dec)
{
    if (dec->va_display)
        vaTerminate(dec->va_display);
    dec->va_display = nullptr;
    if (dec->drm_fd >= 0)
        (void)close(dec->drm_fd);
    dec->drm_fd = -1;
}

/* ---- VA-API GPU init ----
 *
 * Opens the DRM render node and brings up the VA display on it. Returns 0 on
 * success. On failure every handle this stage opened is already released, so
 * the caller returns straight to its own caller without further teardown. */
static int vpl_open_va_display(VplDecoder *dec, const char *render_node)
{
    dec->drm_fd = open(render_node, O_RDWR);
    if (dec->drm_fd < 0) {
        (void)fprintf(stderr, "Cannot open %s\n", render_node);
        return -1;
    }

    dec->va_display = vaGetDisplayDRM(dec->drm_fd);
    if (!dec->va_display) {
        (void)fprintf(stderr, "vaGetDisplayDRM failed\n");
        (void)close(dec->drm_fd);
        return -1;
    }

    int va_major;
    int va_minor;
    const VAStatus va_st = vaInitialize(dec->va_display, &va_major, &va_minor);
    if (va_st != VA_STATUS_SUCCESS) {
        (void)fprintf(stderr, "vaInitialize failed: %s\n", vaErrorStr(va_st));
        (void)close(dec->drm_fd);
        return -1;
    }

    printf("VA-API %d.%d on %s\n", va_major, va_minor, render_node);
    return 0;
}

/* ---- VPL loader + session ----
 *
 * Requires a hardware implementation reached through VA-API. Returns 0 on
 * success. On failure the loader and the VA display this decoder owns are
 * already torn down. */
static int vpl_create_session(VplDecoder *dec)
{
    dec->loader = MFXLoad();
    if (!dec->loader) {
        (void)fprintf(stderr, "MFXLoad failed\n");
        vpl_cleanup_gpu(dec);
        return -1;
    }

    /* Require hardware implementation */
    mfxConfig cfg1 = MFXCreateConfig(dec->loader);
    mfxVariant val;
    val.Type = MFX_VARIANT_TYPE_U32;
    val.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
    MFXSetConfigFilterProperty(cfg1, (mfxU8 *)"mfxImplDescription.Impl", val);

    /* Set VA-API acceleration mode */
    mfxConfig cfg2 = MFXCreateConfig(dec->loader);
    val.Data.U32 = MFX_ACCEL_MODE_VIA_VAAPI;
    MFXSetConfigFilterProperty(cfg2, (mfxU8 *)"mfxImplDescription.AccelerationMode", val);

    const mfxStatus sts = MFXCreateSession(dec->loader, 0, &dec->session);
    if (sts != MFX_ERR_NONE) {
        (void)fprintf(stderr, "MFXCreateSession failed: %d\n", sts);
        MFXUnload(dec->loader);
        vpl_cleanup_gpu(dec);
        return -1;
    }

    return 0;
}

/* Tear down an already-created session, its loader and the VA display, in the
 * order the former `fail_session:` label used. */
static void vpl_close_session(VplDecoder *dec)
{
    MFXClose(dec->session);
    MFXUnload(dec->loader);
    vpl_cleanup_gpu(dec);
}

/* Bind the VA display to the session, open the elementary stream and allocate
 * the bitstream buffer. Returns 0 on success; on failure the caller runs
 * vpl_close_session(). */
static int vpl_attach_input(VplDecoder *dec, const char *filename)
{
    /* Pass VA display to VPL */
    const mfxStatus sts =
        MFXVideoCORE_SetHandle(dec->session, MFX_HANDLE_VA_DISPLAY, dec->va_display);
    if (sts != MFX_ERR_NONE) {
        (void)fprintf(stderr, "SetHandle(VA_DISPLAY) failed: %d\n", sts);
        return -1;
    }

    /* Open input file */
    dec->fp = vmaf_fopen_utf8(filename, "rb");
    if (!dec->fp) {
        (void)fprintf(stderr, "Cannot open %s\n", filename);
        return -1;
    }

    /* Allocate bitstream buffer (2 MB) */
    dec->bs_buf_size = (size_t)2U * 1024U * 1024U;
    dec->bs_buf = (uint8_t *)malloc(dec->bs_buf_size);
    if (!dec->bs_buf) {
        (void)fclose(dec->fp);
        dec->fp = nullptr;
        return -1;
    }

    dec->bs.Data = dec->bs_buf;
    dec->bs.MaxLength = (mfxU32)dec->bs_buf_size;

    return 0;
}

static int vpl_decoder_open(VplDecoder *dec, const char *filename, const char *render_node)
{
    memset(dec, 0, sizeof(*dec));
    dec->drm_fd = -1;

    if (vpl_open_va_display(dec, render_node) < 0) {
        return -1;
    }

    if (vpl_create_session(dec) < 0) {
        return -1;
    }

    if (vpl_attach_input(dec, filename) < 0) {
        vpl_close_session(dec);
        return -1;
    }

    return 0;
}

/* Read more data into the bitstream buffer */
static int vpl_read_bitstream(VplDecoder *dec)
{
    if (dec->eof)
        return 0;

    /* Move unprocessed data to the beginning of the buffer */
    if (dec->bs.DataOffset > 0) {
        memmove(dec->bs.Data, dec->bs.Data + dec->bs.DataOffset, dec->bs.DataLength);
        dec->bs.DataOffset = 0;
    }

    /* Fill the rest of the buffer */
    size_t space = dec->bs_buf_size - dec->bs.DataLength;
    if (space == 0)
        return 0;

    size_t nread = fread(dec->bs.Data + dec->bs.DataLength, 1, space, dec->fp);
    if (nread == 0 && feof(dec->fp)) {
        dec->eof = 1;
        return 0;
    }

    dec->bs.DataLength += (mfxU32)nread;
    return (int)nread;
}

/* Probe stream to determine codec and resolution */
static int vpl_probe_and_init(VplDecoder *dec, mfxU32 codec_id)
{
    /* Read initial data */
    vpl_read_bitstream(dec);

    /* Set up decode header query params */
    memset(&dec->decode_params, 0, sizeof(dec->decode_params));
    dec->decode_params.mfx.CodecId = codec_id;
    dec->decode_params.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    /* Decode header to get stream info */
    mfxStatus sts = MFXVideoDECODE_DecodeHeader(dec->session, &dec->bs, &dec->decode_params);
    if (sts != MFX_ERR_NONE) {
        (void)fprintf(stderr, "DecodeHeader failed: %d\n", sts);
        return -1;
    }

    dec->width = dec->decode_params.mfx.FrameInfo.CropW ? dec->decode_params.mfx.FrameInfo.CropW :
                                                          dec->decode_params.mfx.FrameInfo.Width;
    dec->height = dec->decode_params.mfx.FrameInfo.CropH ? dec->decode_params.mfx.FrameInfo.CropH :
                                                           dec->decode_params.mfx.FrameInfo.Height;

    /* Determine bit depth from fourcc */
    mfxU32 fourcc = dec->decode_params.mfx.FrameInfo.FourCC;
    if (fourcc == MFX_FOURCC_P010 || fourcc == MFX_FOURCC_Y210 || fourcc == MFX_FOURCC_Y410) {
        dec->bpc = 10;
    } else {
        dec->bpc = 8;
    }

    printf("Stream: %dx%d %d-bit (fourcc=0x%08x)\n", dec->width, dec->height, dec->bpc, fourcc);

    /* Initialize decoder */
    sts = MFXVideoDECODE_Init(dec->session, &dec->decode_params);
    if (sts != MFX_ERR_NONE && sts != MFX_WRN_PARTIAL_ACCELERATION) {
        (void)fprintf(stderr, "DECODE_Init failed: %d\n", sts);
        return -1;
    }

    if (sts == MFX_WRN_PARTIAL_ACCELERATION) {
        printf("Warning: partial HW acceleration\n");
    }

    return 0;
}

/* Sync a completed DecodeFrameAsync call and publish its VA surface.
 *
 * Returns 0 on success, with the surface reference handed to the caller
 * through @p out_held_surf. On error the reference is released here and -1 is
 * returned. Ordering matches the inline block this was lifted out of. */
static int vpl_publish_surface(VplDecoder *dec, mfxSyncPoint sync, mfxFrameSurface1 *out_surf,
                               VASurfaceID *out_surface, mfxFrameSurface1 **out_held_surf)
{
    const mfxStatus sts = MFXVideoCORE_SyncOperation(dec->session, sync, VPL_SYNC_TIMEOUT_MS);
    if (sts != MFX_ERR_NONE) {
        (void)fprintf(stderr, "SyncOperation failed: %d\n", sts);
        if (out_surf)
            out_surf->FrameInterface->Release(out_surf);
        return -1;
    }

    /* Extract VA surface handle */
    mfxHDL resource = nullptr;
    mfxResourceType res_type = MFX_RESOURCE_VA_SURFACE;
    mfxStatus gnh_sts = out_surf->FrameInterface->GetNativeHandle(out_surf, &resource, &res_type);

    if (gnh_sts == MFX_ERR_NONE && resource) {
        *out_surface = *(VASurfaceID *)resource;
    } else if (out_surf->Data.MemId) {
        *out_surface = *(VASurfaceID *)out_surf->Data.MemId;
    } else {
        (void)fprintf(stderr, "No VA surface in decoded frame\n");
        out_surf->FrameInterface->Release(out_surf);
        return -1;
    }

    /* Keep the surface reference — caller must release */
    *out_held_surf = out_surf;
    return 0;
}

/**
 * Decode one frame.
 *
 * Returns VASurfaceID via out_va_surface.
 * Caller must release via vpl_release_surface().
 * Returns 0 on success, 1 on EOF, negative on error.
 *
 * The attempt counter bounds the retry loop. Every attempt either returns a
 * frame, reports end of stream, refills the bitstream from the input file, or
 * sleeps VPL_DECODE_RETRY_US before asking the device again. The device-busy
 * retries are the case the bound exists for, so it is sized to the same
 * wall-clock ceiling the SyncOperation above is given: VPL_SYNC_TIMEOUT_MS
 * milliseconds at VPL_DECODE_RETRY_US per retry. The refill attempts do not
 * sleep and are charged against the same budget, so the guarantee is a bound
 * on attempts (Power of 10 rule 2), not exactly on wall clock; a stream would
 * have to need more than VPL_DECODE_MAX_ATTEMPTS refills for one frame to
 * notice the difference. A decoder that has produced neither a frame nor an
 * end-of-stream marker by then is wedged — previously the `for (;;)` spun on
 * it forever; now the tool reports it and gives up. Not yet exercised on real
 * Intel hardware: see ADR-1287 and docs/state.md.
 */
static int vpl_decode_frame(VplDecoder *dec, VASurfaceID *out_surface,
                            mfxFrameSurface1 **out_held_surf)
{
    mfxStatus sts;
    mfxSyncPoint sync = nullptr;
    mfxFrameSurface1 *out_surf = nullptr;

    *out_held_surf = nullptr;

    for (unsigned attempt = 0; attempt < VPL_DECODE_MAX_ATTEMPTS; attempt++) {
        /* Refill bitstream if needed */
        if (dec->bs.DataLength < dec->bs_buf_size / 2 && !dec->eof) {
            vpl_read_bitstream(dec);
        }

        int passing_null = (dec->bs.DataLength == 0 && dec->eof);
        sts = MFXVideoDECODE_DecodeFrameAsync(dec->session, passing_null ? nullptr : &dec->bs,
                                              nullptr, /* internal allocation */
                                              &out_surf, &sync);

        if (sts == MFX_ERR_NONE && sync) {
            /* Got a frame — sync and return */
            return vpl_publish_surface(dec, sync, out_surf, out_surface, out_held_surf);
        }

        if (sts == MFX_ERR_MORE_DATA) {
            if (passing_null)
                return 1; /* drain call returned no data → truly done */
            continue;     /* consumed input or need more — loop to drain */
        }

        if (sts == MFX_ERR_MORE_SURFACE || sts == MFX_WRN_DEVICE_BUSY) {
            /* Wait a bit and retry */
            usleep(VPL_DECODE_RETRY_US);
            continue;
        }

        if (sts < 0) {
            (void)fprintf(stderr, "DecodeFrameAsync failed: %d\n", sts);
            return -1;
        }

        /* Other warnings — retry */
        if (!sync)
            continue;
    }

    (void)fprintf(stderr, "DecodeFrameAsync yielded no frame after %u attempts\n",
                  (unsigned)VPL_DECODE_MAX_ATTEMPTS);
    return -1;
}

static void vpl_release_surface(mfxFrameSurface1 *surf)
{
    if (surf && surf->FrameInterface)
        surf->FrameInterface->Release(surf);
}

static void vpl_decoder_close(VplDecoder *dec)
{
    if (dec->session) {
        MFXVideoDECODE_Close(dec->session);
        MFXClose(dec->session);
    }
    if (dec->loader)
        MFXUnload(dec->loader);
    if (dec->fp)
        (void)fclose(dec->fp);
    free(dec->bs_buf);
    vpl_cleanup_gpu(dec);
    memset(dec, 0, sizeof(*dec));
    dec->drm_fd = -1;
}

/* ------------------------------------------------------------------ */
/* Guess codec from file extension                                     */
/* ------------------------------------------------------------------ */

static mfxU32 guess_codec(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext)
        return MFX_CODEC_HEVC;
    if (strcasecmp(ext, ".h264") == 0 || strcasecmp(ext, ".264") == 0 ||
        strcasecmp(ext, ".avc") == 0)
        return MFX_CODEC_AVC;
    if (strcasecmp(ext, ".h265") == 0 || strcasecmp(ext, ".265") == 0 ||
        strcasecmp(ext, ".hevc") == 0)
        return MFX_CODEC_HEVC;
    if (strcasecmp(ext, ".mp4") == 0 || strcasecmp(ext, ".mkv") == 0 ||
        strcasecmp(ext, ".webm") == 0) {
        /* Container → need a demuxer; we only handle elementary streams.
         * Suggest ffmpeg pre-extraction. */
        (void)fprintf(
            stderr,
            "Warning: %s appears to be a container file.\n"
            "  This tool only handles elementary streams (H.264/H.265).\n"
            "  Extract with: ffmpeg -i %s -c:v copy -bsf:v hevc_mp4toannexb output.h265\n"
            "  Or:           ffmpeg -i %s -c:v copy -bsf:v h264_mp4toannexb output.h264\n",
            filename, filename, filename);
        return MFX_CODEC_HEVC; /* try anyway */
    }
    if (strcasecmp(ext, ".av1") == 0 || strcasecmp(ext, ".ivf") == 0 ||
        strcasecmp(ext, ".obu") == 0)
        return MFX_CODEC_AV1;
    if (strcasecmp(ext, ".vp9") == 0)
        return MFX_CODEC_VP9;

    return MFX_CODEC_HEVC; /* default */
}

/* ------------------------------------------------------------------ */
/* Host upload fallback                                                */
/*                                                                     */
/* Used when DMA-BUF zero-copy import fails (e.g. older kernel without */
/* Level Zero VA import support, or DRM render node mismatch). Maps    */
/* the VA surface's Y plane to host memory via vaDeriveImage+          */
/* vaMapBuffer, copies it into a host-allocated VmafPicture, then runs */
/* the standard vmaf_read_pictures() path — which uploads the Y plane  */
/* into SYCL shared buffers internally and dispatches the extractor    */
/* loop. Slower than the zero-copy path (two memcpys: VA→host→SYCL),   */
/* but preserves numerical correctness.                                */
/* ------------------------------------------------------------------ */

/* Resources vpl_host_upload_fallback acquires, in acquisition order.
 *
 * The `have_*` flags drive vpl_fallback_release(), which releases exactly what
 * is still held, in the same order the former `cleanup:` label used. Keeping
 * the flags in one caller-owned struct is what lets the acquisition stages
 * below return early instead of jumping to a shared label. */
typedef struct {
    VADisplay va_display;
    VAImage ref_img;
    VAImage dis_img;
    void *ref_map;
    void *dis_map;
    VmafPicture ref_pic;
    VmafPicture dis_pic;
    int have_ref_img;
    int have_dis_img;
    int have_ref_map;
    int have_dis_map;
    int have_ref_pic;
    int have_dis_pic;
} VplFallbackState;

static void vpl_fallback_release(VplFallbackState *s)
{
    if (s->have_ref_pic)
        vmaf_picture_unref(&s->ref_pic);
    if (s->have_dis_pic)
        vmaf_picture_unref(&s->dis_pic);
    if (s->have_ref_map)
        vaUnmapBuffer(s->va_display, s->ref_img.buf);
    if (s->have_dis_map)
        vaUnmapBuffer(s->va_display, s->dis_img.buf);
    if (s->have_ref_img)
        vaDestroyImage(s->va_display, s->ref_img.image_id);
    if (s->have_dis_img)
        vaDestroyImage(s->va_display, s->dis_img.image_id);
}

/* Derive VAImages that alias the surfaces' backing storage and map them into
 * host memory. Returns 0 on success; on failure the caller's
 * vpl_fallback_release() undoes whatever was already acquired. */
static int vpl_fallback_map_surfaces(VplFallbackState *s, VASurfaceID ref_surf,
                                     VASurfaceID dis_surf)
{
    VAStatus st = vaDeriveImage(s->va_display, ref_surf, &s->ref_img);
    if (st != VA_STATUS_SUCCESS) {
        (void)fprintf(stderr, "vaDeriveImage(ref) failed: %d\n", st);
        return -1;
    }
    s->have_ref_img = 1;

    st = vaDeriveImage(s->va_display, dis_surf, &s->dis_img);
    if (st != VA_STATUS_SUCCESS) {
        (void)fprintf(stderr, "vaDeriveImage(dis) failed: %d\n", st);
        return -1;
    }
    s->have_dis_img = 1;

    st = vaMapBuffer(s->va_display, s->ref_img.buf, &s->ref_map);
    if (st != VA_STATUS_SUCCESS || !s->ref_map) {
        (void)fprintf(stderr, "vaMapBuffer(ref) failed: %d\n", st);
        return -1;
    }
    s->have_ref_map = 1;

    st = vaMapBuffer(s->va_display, s->dis_img.buf, &s->dis_map);
    if (st != VA_STATUS_SUCCESS || !s->dis_map) {
        (void)fprintf(stderr, "vaMapBuffer(dis) failed: %d\n", st);
        return -1;
    }
    s->have_dis_map = 1;

    return 0;
}

/* Allocate host VmafPictures (YUV420P). VMAF reads only the Y plane; U/V are
 * left as zeros by vmaf_picture_alloc. Returns 0 on success. */
static int vpl_fallback_alloc_pictures(VplFallbackState *s, int w, int h, int bpc)
{
    if (vmaf_picture_alloc(&s->ref_pic, VMAF_PIX_FMT_YUV420P, (unsigned)bpc, (unsigned)w,
                           (unsigned)h) != 0) {
        (void)fprintf(stderr, "vmaf_picture_alloc(ref) failed\n");
        return -1;
    }
    s->have_ref_pic = 1;

    if (vmaf_picture_alloc(&s->dis_pic, VMAF_PIX_FMT_YUV420P, (unsigned)bpc, (unsigned)w,
                           (unsigned)h) != 0) {
        (void)fprintf(stderr, "vmaf_picture_alloc(dis) failed\n");
        return -1;
    }
    /* Read back by vpl_fallback_release() if a later stage fails before
     * vmaf_read_pictures() takes ownership and clears the flag. */
    s->have_dis_pic = 1;

    return 0;
}

/* Copy both Y planes into the host pictures, drop the VA mappings, and hand
 * the pictures to VMAF. Returns 0 on success, or the vmaf_read_pictures()
 * error code. */
static int vpl_fallback_dispatch(VplFallbackState *s, int w, int h, int bpc, VmafContext *vmaf,
                                 unsigned frame_idx)
{
    /* Copy Y plane row-by-row to account for VA pitch ≠ width. NV12/P010
     * store Y at plane 0 (offsets[0], pitches[0]). */
    const size_t bytes_per_pixel = (size_t)((bpc + 7) / 8);
    const size_t row_bytes = (size_t)w * bytes_per_pixel;
    const uint8_t *ref_y = (const uint8_t *)s->ref_map + s->ref_img.offsets[0];
    const uint8_t *dis_y = (const uint8_t *)s->dis_map + s->dis_img.offsets[0];
    uint8_t *ref_dst = (uint8_t *)s->ref_pic.data[0];
    uint8_t *dis_dst = (uint8_t *)s->dis_pic.data[0];

    for (int y = 0; y < h; y++) {
        memcpy(ref_dst + (size_t)y * (size_t)s->ref_pic.stride[0],
               ref_y + (size_t)y * (size_t)s->ref_img.pitches[0], row_bytes);
        memcpy(dis_dst + (size_t)y * (size_t)s->dis_pic.stride[0],
               dis_y + (size_t)y * (size_t)s->dis_img.pitches[0], row_bytes);
    }

    /* Unmap + destroy VA images now that the Y plane is copied. No need
     * to hold them across the (potentially slow) VMAF dispatch. */
    vaUnmapBuffer(s->va_display, s->dis_img.buf);
    s->have_dis_map = 0;
    vaUnmapBuffer(s->va_display, s->ref_img.buf);
    s->have_ref_map = 0;
    vaDestroyImage(s->va_display, s->dis_img.image_id);
    s->have_dis_img = 0;
    vaDestroyImage(s->va_display, s->ref_img.image_id);
    s->have_ref_img = 0;

    /* vmaf_read_pictures uploads to SYCL shared buffers internally and
     * dispatches the extractor loop. It also unrefs ref_pic/dis_pic. */
    const int err = vmaf_read_pictures(vmaf, &s->ref_pic, &s->dis_pic, frame_idx);
    s->have_ref_pic = 0;
    s->have_dis_pic = 0;
    if (err) {
        (void)fprintf(stderr, "vmaf_read_pictures failed at frame %u: %d\n", frame_idx, err);
        return err;
    }

    return 0;
}

static int vpl_host_upload_fallback(VADisplay va_display, VASurfaceID ref_surf,
                                    VASurfaceID dis_surf, int w, int h, int bpc, VmafContext *vmaf,
                                    unsigned frame_idx)
{
    assert(va_display != nullptr);
    assert(vmaf != nullptr);
    assert(w > 0 && h > 0);
    assert(bpc == 8 || bpc == 10);

    VplFallbackState state = {.va_display = va_display};
    int ret = -1;

    if (vpl_fallback_map_surfaces(&state, ref_surf, dis_surf) == 0 &&
        vpl_fallback_alloc_pictures(&state, w, h, bpc) == 0) {
        ret = vpl_fallback_dispatch(&state, w, h, bpc, vmaf, frame_idx);
    }

    vpl_fallback_release(&state);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

/* Command-line options, defaulted by main() before parsing. */
typedef struct {
    const char *ref_file;
    const char *dis_file;
    const char *model_name;
    const char *render_node;
    int max_frames;
    int device_idx;
    int use_fallback;
} VplToolOptions;

/* Parse a non-negative int option value. Returns 0 on success, -1 when the
 * text is malformed, negative or above INT_MAX. */
static int vpl_parse_int_option(const char *text, int *out)
{
    char *end = nullptr;
    const long v = strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < 0 || v > INT_MAX) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

/* Fill @p opt from argv.
 *
 * Returns 0 when parsing succeeded and the tool should run. Returns 1 when the
 * caller must exit immediately; *exit_code then carries the process status
 * (0 for --help, 1 for a usage error). */
static int vpl_parse_options(int argc, char *argv[], VplToolOptions *opt, int *exit_code)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ref") && i + 1 < argc) {
            opt->ref_file = argv[++i];
        } else if (!strcmp(argv[i], "--dis") && i + 1 < argc) {
            opt->dis_file = argv[++i];
        } else if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            opt->model_name = argv[++i];
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            if (vpl_parse_int_option(argv[++i], &opt->max_frames) != 0) {
                (void)fprintf(stderr, "Invalid --frames value: %s\n", argv[i]);
                *exit_code = 1;
                return 1;
            }
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            if (vpl_parse_int_option(argv[++i], &opt->device_idx) != 0) {
                (void)fprintf(stderr, "Invalid --device value: %s\n", argv[i]);
                *exit_code = 1;
                return 1;
            }
        } else if (!strcmp(argv[i], "--render-node") && i + 1 < argc) {
            opt->render_node = argv[++i];
        } else if (!strcmp(argv[i], "--fallback")) {
            opt->use_fallback = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            print_usage(argv[0]);
            *exit_code = 0;
            return 1;
        } else {
            (void)fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            *exit_code = 1;
            return 1;
        }
    }

    if (!opt->ref_file || !opt->dis_file) {
        (void)fprintf(stderr, "Error: --ref and --dis are required\n");
        print_usage(argv[0]);
        *exit_code = 1;
        return 1;
    }

    return 0;
}

/* Open and probe both decoders and check that the streams agree on geometry.
 *
 * Returns 0 on success, -1 on failure — in which case every decoder this
 * function had already opened is closed again, in the order the inline body
 * closed them. */
static int vpl_open_pair(VplDecoder *ref_dec, VplDecoder *dis_dec, const VplToolOptions *opt)
{
    const mfxU32 ref_codec = guess_codec(opt->ref_file);
    const mfxU32 dis_codec = guess_codec(opt->dis_file);

    printf("Opening reference: %s (codec=0x%08x)\n", opt->ref_file, ref_codec);
    if (vpl_decoder_open(ref_dec, opt->ref_file, opt->render_node) < 0)
        return -1;
    if (vpl_probe_and_init(ref_dec, ref_codec) < 0) {
        vpl_decoder_close(ref_dec);
        return -1;
    }

    printf("Opening distorted: %s (codec=0x%08x)\n", opt->dis_file, dis_codec);
    if (vpl_decoder_open(dis_dec, opt->dis_file, opt->render_node) < 0) {
        vpl_decoder_close(ref_dec);
        return -1;
    }
    if (vpl_probe_and_init(dis_dec, dis_codec) < 0) {
        vpl_decoder_close(dis_dec);
        vpl_decoder_close(ref_dec);
        return -1;
    }

    /* Check dimensions match */
    if (ref_dec->width != dis_dec->width || ref_dec->height != dis_dec->height) {
        (void)fprintf(stderr, "Error: resolution mismatch: ref=%dx%d dis=%dx%d\n", ref_dec->width,
                      ref_dec->height, dis_dec->width, dis_dec->height);
        vpl_decoder_close(dis_dec);
        vpl_decoder_close(ref_dec);
        return -1;
    }

    return 0;
}

/* SYCL state, VMAF context and model, released together by
 * vpl_pipeline_close(). The model and imported SYCL state must outlive every
 * retryable vmaf_close() attempt. */
typedef struct {
    VmafSyclState *sycl_state;
    VmafContext *vmaf;
    VmafModel *model;
} VplPipeline;

static int vpl_pipeline_close(VplPipeline *pipe)
{
    const int close_err = vmaf_tool_close_context(&pipe->vmaf);
    if (close_err) {
        (void)fprintf(stderr,
                      "vmaf_vpl: context cleanup failed after %u attempts (err=%d); "
                      "retaining model and SYCL state\n",
                      VMAF_TOOL_CLOSE_MAX_ATTEMPTS, close_err);
        return close_err;
    }
    if (pipe->model) {
        vmaf_model_destroy(pipe->model);
        pipe->model = nullptr;
    }
    if (pipe->sycl_state)
        vmaf_sycl_state_free(&pipe->sycl_state);
    return 0;
}

/* Register SYCL feature extractors.
 * These provide the same feature scores as CPU extractors
 * (e.g. VMAF_integer_feature_vif_scale0_score), so the VMAF model
 * can consume them directly without registering CPU extractors.
 * A registration failure is reported and the run continues. */
static void vpl_register_features(VmafContext *vmaf)
{
    const char *features[] = {"vif_sycl", "adm_sycl", "motion_sycl"};
    for (int i = 0; i < 3; i++) {
        const int err = vmaf_use_feature(vmaf, features[i], nullptr);
        if (err) {
            (void)fprintf(stderr, "vmaf_use_feature(%s) failed: %d\n", features[i], err);
        }
    }
}

/* Load VMAF model (for final score computation).
 * We do NOT call vmaf_use_features_from_model() because the SYCL
 * extractors already provide the required features. A load failure leaves
 * pipe->model NULL and is not fatal — features still compute. */
static void vpl_load_model(VplPipeline *pipe, const char *model_name)
{
    VmafModelConfig model_cfg = {
        .name = model_name,
        .flags = VMAF_MODEL_FLAGS_DEFAULT,
    };
    int err = vmaf_model_load(&pipe->model, &model_cfg, model_name);
    if (err) {
        /* Try as file path if built-in name lookup fails */
        err = vmaf_model_load_from_path(&pipe->model, &model_cfg, model_name);
    }
    if (err) {
        (void)fprintf(stderr, "vmaf_model_load(%s) failed: %d\n", model_name, err);
        /* Continue without model — features still compute */
    }
}

/* Bring up SYCL, the VMAF context, the SYCL extractors, the model and the
 * zero-copy frame buffers. Returns 0 on success; on failure everything already
 * built is released, in the order the inline cleanup used, and the caller
 * returns without touching *pipe again. */
static int vpl_pipeline_open(VplPipeline *pipe, const VplToolOptions *opt, int w, int h, int bpc)
{
    /* ---- Set up SYCL state ---- */
    VmafSyclState *sycl_state = nullptr;
    VmafSyclConfiguration sycl_cfg = {.device_index = opt->device_idx, .enable_profiling = 0};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err) {
        (void)fprintf(stderr, "vmaf_sycl_state_init failed: %d\n", err);
        return -1;
    }
    pipe->sycl_state = sycl_state;

    /* ---- Set up VMAF context ---- */
    VmafContext *vmaf = nullptr;
    VmafConfiguration vmaf_cfg = {
        .log_level = VMAF_LOG_LEVEL_INFO,
        .n_threads = 1,
        .n_subsample = 0,
        .cpumask = 0,
    };
    err = vmaf_init(&vmaf, vmaf_cfg);
    if (err) {
        (void)fprintf(stderr, "vmaf_init failed: %d\n", err);
        const int cleanup_err = vpl_pipeline_close(pipe);
        return cleanup_err ? VPL_PIPELINE_CLEANUP_FAILED : -1;
    }
    pipe->vmaf = vmaf;

    err = vmaf_sycl_import_state(pipe->vmaf, pipe->sycl_state);
    if (err) {
        (void)fprintf(stderr, "vmaf_sycl_import_state failed: %d\n", err);
        const int cleanup_err = vpl_pipeline_close(pipe);
        return cleanup_err ? VPL_PIPELINE_CLEANUP_FAILED : -1;
    }

    vpl_register_features(pipe->vmaf);
    vpl_load_model(pipe, opt->model_name);

    /* ---- Init frame buffers for zero-copy ---- */
    err = vmaf_sycl_init_frame_buffers(pipe->vmaf, w, h, bpc);
    if (err) {
        (void)fprintf(stderr, "vmaf_sycl_init_frame_buffers failed: %d\n", err);
        const int cleanup_err = vpl_pipeline_close(pipe);
        return cleanup_err ? VPL_PIPELINE_CLEANUP_FAILED : -1;
    }

    return 0;
}

/* One decoded frame pair plus the VPL surface references that back it. */
typedef struct {
    VASurfaceID ref_surf;
    VASurfaceID dis_surf;
    mfxFrameSurface1 *ref_held;
    mfxFrameSurface1 *dis_held;
} VplFramePair;

static void vpl_release_pair(VplFramePair *pair)
{
    vpl_release_surface(pair->ref_held);
    vpl_release_surface(pair->dis_held);
}

/* Decode the next frame of both streams.
 *
 * Returns 0 when both frames are in hand (the caller owns the references),
 * 1 at end of stream and -1 on a decode error; on the latter two the
 * references are already released. */
static int vpl_decode_pair(VplDecoder *ref_dec, VplDecoder *dis_dec, VplFramePair *pair,
                           int frame_idx)
{
    const int r1 = vpl_decode_frame(ref_dec, &pair->ref_surf, &pair->ref_held);
    const int r2 = vpl_decode_frame(dis_dec, &pair->dis_surf, &pair->dis_held);

    if (r1 == 1 || r2 == 1) {
        printf("End of stream at frame %d\n", frame_idx);
        vpl_release_pair(pair);
        return 1;
    }
    if (r1 < 0 || r2 < 0) {
        (void)fprintf(stderr, "Decode error at frame %d\n", frame_idx);
        vpl_release_pair(pair);
        return -1;
    }

    return 0;
}

/* Import decoded surfaces into SYCL shared buffers:
 * VA-API surface → DMA-BUF → Level Zero → SYCL (zero-copy).
 *
 * Returns 0 when the caller should keep processing this frame — either the
 * import succeeded, or it failed and --fallback demoted *dmabuf_ok so the
 * host-upload path takes over from here on. Returns -1 when the run must stop
 * because the import failed with no fallback allowed. */
static int vpl_import_pair(VmafSyclState *sycl_state, const VplDecoder *ref_dec,
                           const VplDecoder *dis_dec, const VplFramePair *pair, int w, int h,
                           int bpc, int frame_idx, int use_fallback, int *dmabuf_ok)
{
    int err =
        vmaf_sycl_import_va_surface(sycl_state, ref_dec->va_display, pair->ref_surf, 1, w, h, bpc);
    if (!err) {
        err = vmaf_sycl_import_va_surface(sycl_state, dis_dec->va_display, pair->dis_surf, 0, w, h,
                                          bpc);
    }
    if (!err) {
        return 0;
    }

    (void)fprintf(stderr, "DMA-BUF import failed at frame %d: %d\n", frame_idx, err);
    if (!use_fallback) {
        return -1;
    }
    (void)fprintf(stderr, "Falling back to host upload path\n");
    *dmabuf_ok = 0;
    return 0;
}

/* ---- Decode + compute loop ----
 *
 * Runs until end of stream, the --frames ceiling, or the first unrecoverable
 * error. Returns the number of frames submitted to VMAF. */
static int vpl_run_loop(VplPipeline *pipe, VplDecoder *ref_dec, VplDecoder *dis_dec,
                        const VplToolOptions *opt, int w, int h, int bpc)
{
    int frame_idx = 0;
    int dmabuf_ok = 1; /* initially assume DMA-BUF import works */

    while (opt->max_frames == 0 || frame_idx < opt->max_frames) {
        VplFramePair pair;
        if (vpl_decode_pair(ref_dec, dis_dec, &pair, frame_idx) != 0) {
            break;
        }

        if (dmabuf_ok && vpl_import_pair(pipe->sycl_state, ref_dec, dis_dec, &pair, w, h, bpc,
                                         frame_idx, opt->use_fallback, &dmabuf_ok) < 0) {
            vpl_release_pair(&pair);
            break;
        }

        if (!dmabuf_ok) {
            /* Host upload fallback: VA→host→SYCL via vmaf_read_pictures.
             * Slower than zero-copy but numerically equivalent. */
            const int fb_err =
                vpl_host_upload_fallback(ref_dec->va_display, pair.ref_surf, pair.dis_surf, w, h,
                                         bpc, pipe->vmaf, (unsigned)frame_idx);
            vpl_release_pair(&pair);
            if (fb_err) {
                (void)fprintf(stderr, "Host upload fallback failed at frame %d: %d\n", frame_idx,
                              fb_err);
                break;
            }
            frame_idx++;
            if (frame_idx % 10 == 0)
                printf("  Processed %d frames (host fallback)...\n", frame_idx);
            continue;
        }

        /* Release VPL surface references now that import is done */
        vpl_release_pair(&pair);

        /* Submit frame to VMAF (zero-copy path) */
        const int err = vmaf_read_pictures_sycl(pipe->vmaf, frame_idx);
        if (err) {
            (void)fprintf(stderr, "vmaf_read_pictures_sycl failed at frame %d: %d\n", frame_idx,
                          err);
            break;
        }

        frame_idx++;
        if (frame_idx % 10 == 0) {
            printf("  Processed %d frames...\n", frame_idx);
        }
    }

    return frame_idx;
}

/* Print the pooled VMAF score and the first few per-frame feature scores. */
static void vpl_print_results(const VplPipeline *pipe, int frame_idx, double elapsed_s)
{
    printf("\n--- Results ---\n");
    printf("Frames: %d\n", frame_idx);
    printf("Time:   %.3f s (%.1f FPS)\n", elapsed_s, elapsed_s > 0 ? frame_idx / elapsed_s : 0);

    /* Print VMAF scores */
    if (pipe->model && frame_idx > 0) {
        double vmaf_score;
        const int err = vmaf_score_pooled(pipe->vmaf, pipe->model, VMAF_POOL_METHOD_MEAN,
                                          &vmaf_score, 0, frame_idx - 1);
        if (!err) {
            printf("VMAF:   %.6f (mean)\n", vmaf_score);
        } else {
            (void)fprintf(stderr, "vmaf_score_pooled failed: %d\n", err);
        }
    }

    /* Print per-frame feature scores */
    for (int f = 0; f < frame_idx && f < 5; f++) {
        double vif0 = 0;
        double adm2 = 0;
        double motion = 0;
        vmaf_feature_score_at_index(pipe->vmaf, "VMAF_integer_feature_vif_scale0_score", &vif0, f);
        vmaf_feature_score_at_index(pipe->vmaf, "VMAF_integer_feature_adm2_score", &adm2, f);
        vmaf_feature_score_at_index(pipe->vmaf, "VMAF_integer_feature_motion2_score", &motion, f);
        printf("  Frame %d: vif0=%.6f adm2=%.6f motion2=%.6f\n", f, vif0, adm2, motion);
    }
    if (frame_idx > 5)
        printf("  ... (%d more frames)\n", frame_idx - 5);
}

int main(int argc, char *argv[])
{
    VplToolOptions opt = {
        .model_name = VMAF_DEFAULT_MODEL_VERSION,
        .render_node = "/dev/dri/renderD128",
    };

    int exit_code = 0;
    if (vpl_parse_options(argc, argv, &opt, &exit_code) != 0) {
        return exit_code;
    }

    /* ---- Open VPL decoders ---- */
    VplDecoder ref_dec;
    VplDecoder dis_dec;
    if (vpl_open_pair(&ref_dec, &dis_dec, &opt) < 0) {
        return 1;
    }

    const int w = ref_dec.width;
    const int h = ref_dec.height;
    const int bpc = ref_dec.bpc > dis_dec.bpc ? ref_dec.bpc : dis_dec.bpc;

    printf("Resolution: %dx%d @ %d-bit\n", w, h, bpc);

    VplPipeline pipe = {.sycl_state = nullptr};
    const int open_err = vpl_pipeline_open(&pipe, &opt, w, h, bpc);
    if (open_err < 0) {
        if (open_err != VPL_PIPELINE_CLEANUP_FAILED) {
            vpl_decoder_close(&dis_dec);
            vpl_decoder_close(&ref_dec);
        }
        return 1;
    }

    printf("\nStarting VPL decode → SYCL VMAF pipeline...\n\n");
    const double t_start = now_ms();
    const int frame_idx = vpl_run_loop(&pipe, &ref_dec, &dis_dec, &opt, w, h, bpc);

    /* Flush SYCL pipeline */
    const int err = vmaf_flush_sycl(pipe.vmaf);
    if (err)
        (void)fprintf(stderr, "vmaf_flush_sycl failed: %d\n", err);

    const double t_end = now_ms();
    vpl_print_results(&pipe, frame_idx, (t_end - t_start) / 1000.0);

    /* Cleanup */
    const int cleanup_err = vpl_pipeline_close(&pipe);
    if (cleanup_err)
        return EXIT_FAILURE;
    vpl_decoder_close(&dis_dec);
    vpl_decoder_close(&ref_dec);

    return EXIT_SUCCESS;
}
