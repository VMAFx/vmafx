/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The frame planes the HIP twins of a context share (ADR-1408,
 * core/src/hip/shared_frame.c), without a device.
 *
 * shared_frame.c is compiled straight into this target against the stubs
 * below instead of the ROCm runtime and libvmaf: `hipMalloc` hands out heap
 * blocks, vmaf_hip_picture_upload() copies the host plane into them and
 * counts, and hipDeviceSynchronize() counts. That makes the contract of
 * shared_frame.h checkable on any host:
 *
 *   - a plane is uploaded once per frame, whoever asks and however often
 *     (T-HIP-TWIN-PRIVATE-PLANE-UPLOADS-2026-09-29), and the first upload of
 *     a frame takes along the planes of the frame before, so the host waits
 *     once (T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19);
 *   - the pictures are read before an acquire returns and never after
 *     vmaf_hip_shared_frame_end(), so a caller may refill them at once
 *     (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18);
 *   - a frame's planes stay untouched while a twin can still read them, and
 *     a twin that skipped a frame makes the next upload wait for the device.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "test.h"

#include "hip/common.h"
#include "hip/picture_hip.h"
#include "hip/shared_frame.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr`, and
 * this file mirrors the C spelling of the surface it stands in for.
 * ADR-1138. */

#define FRAME_W 12u
#define FRAME_H 6u
#define CHROMA_W 6u
#define CHROMA_H 3u
/* The picture rows are wider than the plane, like a real VmafPicture. */
#define LUMA_STRIDE 32
#define CHROMA_STRIDE 16

/* Two twins' private streams. The stubs never dereference a stream. */
#define STREAM_A ((uintptr_t)0x51u)
#define STREAM_B ((uintptr_t)0x77u)

/* ------------------------------------------------------------------ */
/* The runtime, as far as shared_frame.c uses it                        */
/* ------------------------------------------------------------------ */

int vmaf_hip_rc_to_errno(hipError_t rc)
{
    return (rc == hipSuccess) ? 0 : -EIO;
}

static unsigned g_dev_allocs;
static unsigned g_dev_frees;
static unsigned g_device_syncs;
static unsigned g_upload_calls;
static unsigned g_planes_uploaded;
static uintptr_t g_last_upload_stream;
/* When set, the next vmaf_hip_picture_upload() fails with this errno. */
static int g_upload_error;

hipError_t hipMalloc(void **ptr, size_t size)
{
    void *p = calloc(1u, (size != 0u) ? size : 1u);
    if (p == NULL)
        return hipErrorOutOfMemory;
    g_dev_allocs++;
    *ptr = p;
    return hipSuccess;
}

hipError_t hipFree(void *ptr)
{
    if (ptr != NULL)
        g_dev_frees++;
    free(ptr);
    return hipSuccess;
}

hipError_t hipDeviceSynchronize(void)
{
    g_device_syncs++;
    return hipSuccess;
}

/* The waiting upload of core/src/hip/picture_hip.c: the copy has read the
 * picture when this returns. */
int vmaf_hip_picture_upload(const VmafHipPlaneUpload *planes, unsigned n_planes, uintptr_t stream)
{
    g_upload_calls++;
    g_last_upload_stream = stream;
    if (g_upload_error != 0) {
        const int err = g_upload_error;
        g_upload_error = 0;
        return err;
    }
    for (unsigned i = 0u; i < n_planes; i++) {
        const VmafHipPlaneUpload *p = &planes[i];
        const uint8_t *src = p->pic->data[p->plane];
        uint8_t *dst = p->dst;
        for (size_t row = 0u; row < p->rows; row++) {
            (void)memcpy(dst + (row * p->dst_pitch), src + (row * (size_t)p->pic->stride[p->plane]),
                         p->row_bytes);
        }
        g_planes_uploaded++;
    }
    return 0;
}

static void counters_reset(void)
{
    g_dev_allocs = 0u;
    g_dev_frees = 0u;
    g_device_syncs = 0u;
    g_upload_calls = 0u;
    g_planes_uploaded = 0u;
    g_last_upload_stream = 0u;
    g_upload_error = 0;
}

/* ------------------------------------------------------------------ */
/* Pictures                                                             */
/* ------------------------------------------------------------------ */

typedef struct TestPicture {
    VmafPicture pic;
    uint8_t luma[LUMA_STRIDE * FRAME_H];
    uint8_t chroma[2][CHROMA_STRIDE * CHROMA_H];
} TestPicture;

/* Every sample of plane `p` is `seed + p`, so a plane names its frame. */
static void picture_fill(TestPicture *t, unsigned seed)
{
    (void)memset(t->luma, (int)(seed & 0xFFu), sizeof(t->luma));
    (void)memset(t->chroma[0], (int)((seed + 1u) & 0xFFu), sizeof(t->chroma[0]));
    (void)memset(t->chroma[1], (int)((seed + 2u) & 0xFFu), sizeof(t->chroma[1]));
}

static void picture_init(TestPicture *t, unsigned seed)
{
    (void)memset(t, 0, sizeof(*t));
    t->pic.pix_fmt = VMAF_PIX_FMT_YUV420P;
    t->pic.bpc = 8u;
    t->pic.w[0] = FRAME_W;
    t->pic.h[0] = FRAME_H;
    t->pic.stride[0] = LUMA_STRIDE;
    t->pic.data[0] = t->luma;
    for (unsigned p = 1u; p < 3u; p++) {
        t->pic.w[p] = CHROMA_W;
        t->pic.h[p] = CHROMA_H;
        t->pic.stride[p] = CHROMA_STRIDE;
        t->pic.data[p] = t->chroma[p - 1u];
    }
    picture_fill(t, seed);
}

/* Whether the packed device plane `dev` holds `rows` x `row_bytes` samples
 * of `value`. */
static bool plane_holds(const void *dev, size_t row_bytes, size_t rows, unsigned value)
{
    const uint8_t *bytes = dev;
    for (size_t i = 0u; i < row_bytes * rows; i++) {
        if (bytes[i] != (uint8_t)(value & 0xFFu))
            return false;
    }
    return true;
}

static bool luma_holds(const void *dev, unsigned value)
{
    return plane_holds(dev, FRAME_W, FRAME_H, value);
}

/* The six planes psnr_hip and ciede_hip ask for. */
static void all_planes(VmafHipPlaneUpload *planes, const TestPicture *ref, const TestPicture *dis)
{
    const TestPicture *pics[2] = {ref, dis};
    for (unsigned i = 0u; i < 2u; i++) {
        for (unsigned p = 0u; p < 3u; p++) {
            planes[(i * 3u) + p] = (VmafHipPlaneUpload){
                .pic = &pics[i]->pic,
                .plane = p,
                .row_bytes = (p == 0u) ? FRAME_W : CHROMA_W,
                .rows = (p == 0u) ? FRAME_H : CHROMA_H,
            };
        }
    }
}

/* The reference luma through `src`; NULL when the acquire failed. */
static void *ref_luma(VmafHipPlaneSource *src, VmafHipSharedFrame *frame, TestPicture *ref,
                      uintptr_t stream)
{
    void *dev = NULL;
    if (vmaf_hip_plane_source_acquire_luma(src, frame, &ref->pic, NULL, stream, &dev, NULL) != 0)
        return NULL;
    return dev;
}

/* ------------------------------------------------------------------ */
/* Tests                                                                */
/* ------------------------------------------------------------------ */

/* One shared frame, a picture pair and three twins. */
typedef struct Fixture {
    VmafHipSharedFrame *frame;
    TestPicture ref;
    TestPicture dis;
    VmafHipPlaneSource twin[3];
} Fixture;

static char *fixture_open(Fixture *fx, unsigned ref_seed, unsigned dis_seed)
{
    counters_reset();
    (void)memset(fx, 0, sizeof(*fx));
    picture_init(&fx->ref, ref_seed);
    picture_init(&fx->dis, dis_seed);
    mu_assert("create", vmaf_hip_shared_frame_create(&fx->frame) == 0 && fx->frame != NULL);
    return NULL;
}

static bool fixture_begin(Fixture *fx)
{
    return vmaf_hip_shared_frame_begin(fx->frame, &fx->ref.pic, &fx->dis.pic) == 0;
}

static char *fixture_close(Fixture *fx)
{
    for (unsigned i = 0u; i < 3u; i++)
        vmaf_hip_plane_source_close(&fx->twin[i]);
    vmaf_hip_shared_frame_destroy(&fx->frame);
    mu_assert("destroy clears the handle", fx->frame == NULL);
    mu_assert("every shared and private plane is freed", g_dev_frees == g_dev_allocs);
    return NULL;
}

static char *check_first_request_uploads(Fixture *fx, void **a_ref, void **a_dis)
{
    mu_assert("first luma acquire",
              vmaf_hip_plane_source_acquire_luma(&fx->twin[0], fx->frame, &fx->ref.pic,
                                                 &fx->dis.pic, STREAM_A, a_ref, a_dis) == 0);
    mu_assert("the first request uploads both luma planes in one call",
              g_upload_calls == 1u && g_planes_uploaded == 2u);
    mu_assert("the upload runs on the stream of the twin that asked first",
              g_last_upload_stream == STREAM_A);
    mu_assert("the device planes hold the pictures",
              luma_holds(*a_ref, 10u) && luma_holds(*a_dis, 20u));
    mu_assert("a second twin gets the same plane and uploads nothing",
              ref_luma(&fx->twin[1], fx->frame, &fx->ref, STREAM_B) == *a_ref &&
                  g_upload_calls == 1u);
    return NULL;
}

static char *check_chroma_joins(Fixture *fx, const void *a_ref, const void *a_dis)
{
    VmafHipPlaneUpload six[6];
    void *dev[6] = {NULL};
    all_planes(six, &fx->ref, &fx->dis);
    mu_assert("six-plane acquire",
              vmaf_hip_plane_source_acquire(&fx->twin[2], fx->frame, six, 6u, STREAM_B, dev) == 0);
    mu_assert("only the four chroma planes are still to upload, in one call",
              g_upload_calls == 2u && g_planes_uploaded == 6u);
    mu_assert("luma is the plane the first twin uploaded", dev[0] == a_ref && dev[3] == a_dis);
    mu_assert("chroma planes hold the pictures", plane_holds(dev[1], CHROMA_W, CHROMA_H, 11u) &&
                                                     plane_holds(dev[2], CHROMA_W, CHROMA_H, 12u) &&
                                                     plane_holds(dev[4], CHROMA_W, CHROMA_H, 21u) &&
                                                     plane_holds(dev[5], CHROMA_W, CHROMA_H, 22u));
    mu_assert("the count the device tests read",
              vmaf_hip_shared_frame_upload_count(fx->frame) == 6u);
    return NULL;
}

static char *test_plane_is_uploaded_once_per_frame(void)
{
    Fixture fx;
    void *a_ref = NULL;
    void *a_dis = NULL;
    mu_assert_msg(fixture_open(&fx, 10u, 20u));
    mu_assert("begin", fixture_begin(&fx));
    mu_assert_msg(check_first_request_uploads(&fx, &a_ref, &a_dis));
    mu_assert_msg(check_chroma_joins(&fx, a_ref, a_dis));
    vmaf_hip_shared_frame_end(fx.frame);
    mu_assert("no twin allocated a plane of its own", g_dev_allocs == 6u);
    return fixture_close(&fx);
}

/* A twin that asks after end() must not get the refilled picture passed off
 * as the announced frame, and must not upload into the shared planes: it
 * reads what the picture holds now, into planes of its own. */
static char *check_acquire_after_end(Fixture *fx, const void *a_ref, const void *a_dis)
{
    void *b_ref = NULL;
    void *b_dis = NULL;
    mu_assert("acquire after end",
              vmaf_hip_plane_source_acquire_luma(&fx->twin[1], fx->frame, &fx->ref.pic,
                                                 &fx->dis.pic, STREAM_B, &b_ref, &b_dis) == 0);
    mu_assert("after end() a twin uploads into planes of its own",
              b_ref != a_ref && b_dis != a_dis && g_last_upload_stream == STREAM_B);
    mu_assert("the shared planes were not written after end()",
              luma_holds(a_ref, 10u) && luma_holds(a_dis, 20u) &&
                  vmaf_hip_shared_frame_upload_count(fx->frame) == 2u);
    mu_assert("the private planes hold the picture as it is now",
              luma_holds(b_ref, 30u) && luma_holds(b_dis, 40u));
    return NULL;
}

static char *test_picture_is_read_before_acquire_returns(void)
{
    Fixture fx;
    void *a_ref = NULL;
    void *a_dis = NULL;
    mu_assert_msg(fixture_open(&fx, 10u, 20u));
    mu_assert("begin", fixture_begin(&fx));
    mu_assert("acquire",
              vmaf_hip_plane_source_acquire_luma(&fx.twin[0], fx.frame, &fx.ref.pic, &fx.dis.pic,
                                                 STREAM_A, &a_ref, &a_dis) == 0);
    vmaf_hip_shared_frame_end(fx.frame);
    /* The caller has its pictures back and refills them with the next frame. */
    picture_fill(&fx.ref, 30u);
    picture_fill(&fx.dis, 40u);
    mu_assert("the device planes keep the frame they were uploaded for",
              luma_holds(a_ref, 10u) && luma_holds(a_dis, 20u));
    mu_assert_msg(check_acquire_after_end(&fx, a_ref, a_dis));
    return fixture_close(&fx);
}

/* Frame `n`, in which both twins read the reference luma; *dev is the plane
 * they read. */
static char *alternating_frame(Fixture *fx, unsigned n, void **dev)
{
    picture_fill(&fx->ref, 50u + n);
    mu_assert("begin", fixture_begin(fx));
    *dev = ref_luma(&fx->twin[0], fx->frame, &fx->ref, STREAM_A);
    mu_assert("both twins read one plane",
              *dev != NULL && ref_luma(&fx->twin[1], fx->frame, &fx->ref, STREAM_B) == *dev);
    vmaf_hip_shared_frame_end(fx->frame);
    mu_assert("the plane holds the frame", luma_holds(*dev, 50u + n));
    return NULL;
}

static char *test_frames_alternate_between_two_slots(void)
{
    Fixture fx;
    void *dev[3] = {NULL};
    mu_assert_msg(fixture_open(&fx, 0u, 0u));
    mu_assert_msg(alternating_frame(&fx, 0u, &dev[0]));
    mu_assert_msg(alternating_frame(&fx, 1u, &dev[1]));
    /* Twin 1's frame-0 kernels are collected only at its turn in frame 1,
     * after twin 0's upload of frame 1: that upload must not have touched
     * frame 0's plane. */
    mu_assert("the next frame goes to the other slot and leaves the previous plane untouched",
              dev[1] != dev[0] && luma_holds(dev[0], 50u));
    mu_assert_msg(alternating_frame(&fx, 2u, &dev[2]));
    mu_assert("the third frame reuses the first frame's plane", dev[2] == dev[0]);
    mu_assert("one upload per frame, no wait for the device, two planes and no reallocation",
              g_planes_uploaded == 3u && g_device_syncs == 0u && g_dev_allocs == 2u);
    return fixture_close(&fx);
}

/* One frame in which `first` acquires the reference luma, then `second`
 * (NULL: nobody else). */
static bool ref_frame(Fixture *fx, VmafHipPlaneSource *first, VmafHipPlaneSource *second)
{
    bool ok = fixture_begin(fx);
    ok = ok && ref_luma(first, fx->frame, &fx->ref, STREAM_A) != NULL;
    ok = ok && (second == NULL || ref_luma(second, fx->frame, &fx->ref, STREAM_B) != NULL);
    vmaf_hip_shared_frame_end(fx->frame);
    return ok;
}

static char *test_skipped_twin_makes_the_upload_wait(void)
{
    Fixture fx;
    mu_assert_msg(fixture_open(&fx, 1u, 2u));
    /* `every` runs each frame (a temporal twin under --subsample), `second`
     * every other frame. */
    VmafHipPlaneSource *every = &fx.twin[0];
    VmafHipPlaneSource *second = &fx.twin[1];

    mu_assert("frames 0 and 1 upload without a wait",
              ref_frame(&fx, every, second) && ref_frame(&fx, every, NULL) && g_device_syncs == 0u);
    /* Frame 2 writes the slot of frame 0, which `second` still holds: its
     * frame-0 kernels are not collected until its own turn. */
    mu_assert("frame 2", ref_frame(&fx, every, second));
    mu_assert("the upload waits for the device before it overwrites a held slot",
              g_device_syncs == 1u);

    /* In frame 4 the skipping twin asks first: it has collected its own
     * frame, and nobody else holds the slot. */
    mu_assert("frames 3 and 4", ref_frame(&fx, every, NULL) && ref_frame(&fx, second, every));
    mu_assert("a twin does not make the device wait for its own collected frame",
              g_device_syncs == 1u);

    vmaf_hip_plane_source_close(every);
    vmaf_hip_plane_source_close(second);
    mu_assert("a closed twin holds nothing",
              ref_frame(&fx, every, NULL) && ref_frame(&fx, every, NULL) && g_device_syncs == 1u);
    return fixture_close(&fx);
}

/* The extractor API used directly: no shared frame at all. */
static char *check_without_a_frame(Fixture *fx)
{
    const void *first_private = ref_luma(&fx->twin[0], NULL, &fx->ref, STREAM_A);
    mu_assert("without a shared frame the twin uploads into a plane of its own",
              first_private != NULL && luma_holds(first_private, 60u) && g_dev_allocs == 1u);
    picture_fill(&fx->ref, 61u);
    mu_assert("the private plane is reused and re-uploaded every frame",
              ref_luma(&fx->twin[0], NULL, &fx->ref, STREAM_A) == first_private &&
                  luma_holds(first_private, 61u) && g_dev_allocs == 1u);
    return NULL;
}

/* Requests the announced frame does not serve. */
static char *check_requests_not_served(Fixture *fx)
{
    TestPicture other;
    picture_init(&other, 80u);
    const void *strange = ref_luma(&fx->twin[1], fx->frame, &other, STREAM_B);
    mu_assert("a picture that was not announced is never served from the shared frame",
              strange != NULL && luma_holds(strange, 80u) &&
                  vmaf_hip_shared_frame_upload_count(fx->frame) == 0u);

    /* Half of the announced plane: not the layout the shared frame holds. */
    const VmafHipPlaneUpload half = {
        .pic = &fx->ref.pic, .plane = 0u, .row_bytes = FRAME_W, .rows = FRAME_H / 2u};
    void *half_dev = NULL;
    mu_assert("acquire of a part of a plane",
              vmaf_hip_plane_source_acquire(&fx->twin[2], fx->frame, &half, 1u, STREAM_B,
                                            &half_dev) == 0);
    mu_assert("a request for anything but the whole plane gets a private plane",
              half_dev != NULL && vmaf_hip_shared_frame_upload_count(fx->frame) == 0u);
    const void *shared = ref_luma(&fx->twin[0], fx->frame, &fx->ref, STREAM_A);
    mu_assert("the whole plane is still shared afterwards",
              shared != NULL && shared != half_dev &&
                  vmaf_hip_shared_frame_upload_count(fx->frame) == 1u);
    return NULL;
}

static char *test_unshared_requests_use_private_planes(void)
{
    Fixture fx;
    mu_assert_msg(fixture_open(&fx, 60u, 70u));
    mu_assert_msg(check_without_a_frame(&fx));
    mu_assert("begin", fixture_begin(&fx));
    mu_assert_msg(check_requests_not_served(&fx));
    vmaf_hip_shared_frame_end(fx.frame);
    return fixture_close(&fx);
}

static char *test_failed_upload_is_not_remembered(void)
{
    Fixture fx;
    void *dev = NULL;
    mu_assert_msg(fixture_open(&fx, 90u, 91u));
    mu_assert("begin", fixture_begin(&fx));
    g_upload_error = -EIO;
    mu_assert("a failed upload fails the acquire",
              vmaf_hip_plane_source_acquire_luma(&fx.twin[0], fx.frame, &fx.ref.pic, NULL, STREAM_A,
                                                 &dev, NULL) == -EIO);
    mu_assert("a failed upload is not counted", vmaf_hip_shared_frame_upload_count(fx.frame) == 0u);
    dev = ref_luma(&fx.twin[1], fx.frame, &fx.ref, STREAM_B);
    mu_assert("the next twin uploads the plane instead of reading an empty one",
              dev != NULL && luma_holds(dev, 90u) &&
                  vmaf_hip_shared_frame_upload_count(fx.frame) == 1u);
    vmaf_hip_shared_frame_end(fx.frame);
    return fixture_close(&fx);
}

/* Frames 2 and 3 of the test below: only the first twin still runs. */
static char *check_expectation_decays(Fixture *fx)
{
    mu_assert("frame 2", ref_frame(fx, &fx->twin[0], NULL));
    mu_assert("planes asked for in the frame before come along once more",
              g_upload_calls == 4u && g_planes_uploaded == 18u);
    mu_assert("frame 3", ref_frame(fx, &fx->twin[0], NULL));
    mu_assert("a plane nobody asked for in the frame before is not uploaded",
              g_upload_calls == 5u && g_planes_uploaded == 19u);
    return NULL;
}

static char *test_upload_takes_along_the_planes_of_the_frame_before(void)
{
    Fixture fx;
    VmafHipPlaneUpload six[6];
    void *dev[6] = {NULL};
    mu_assert_msg(fixture_open(&fx, 10u, 20u));
    all_planes(six, &fx.ref, &fx.dis);

    /* Frame 0: nothing is expected yet, so each twin that finds a plane
     * missing uploads it, and waits. */
    mu_assert("frame 0",
              fixture_begin(&fx) && ref_luma(&fx.twin[0], fx.frame, &fx.ref, STREAM_A) != NULL &&
                  vmaf_hip_plane_source_acquire(&fx.twin[1], fx.frame, six, 6u, STREAM_B, dev) ==
                      0);
    vmaf_hip_shared_frame_end(fx.frame);
    mu_assert("without a frame before, two twins make two upload calls",
              g_upload_calls == 2u && g_planes_uploaded == 6u);

    /* Frame 1: the twin that asks first uploads what both asked for. */
    mu_assert("frame 1",
              fixture_begin(&fx) && ref_luma(&fx.twin[0], fx.frame, &fx.ref, STREAM_A) != NULL);
    mu_assert("the first upload of a frame brings every plane of the frame before",
              g_upload_calls == 3u && g_planes_uploaded == 12u);
    mu_assert("the second twin finds its planes on the device and waits for nothing",
              vmaf_hip_plane_source_acquire(&fx.twin[1], fx.frame, six, 6u, STREAM_B, dev) == 0 &&
                  g_upload_calls == 3u && plane_holds(dev[5], CHROMA_W, CHROMA_H, 22u));
    vmaf_hip_shared_frame_end(fx.frame);

    mu_assert_msg(check_expectation_decays(&fx));
    return fixture_close(&fx);
}

static char *check_acquire_arguments(Fixture *fx)
{
    VmafHipPlaneUpload seven[VMAF_HIP_SOURCE_MAX_PLANES + 1u];
    void *dev[VMAF_HIP_SOURCE_MAX_PLANES + 1u] = {NULL};
    for (unsigned i = 0u; i <= VMAF_HIP_SOURCE_MAX_PLANES; i++) {
        seven[i] = (VmafHipPlaneUpload){
            .pic = &fx->ref.pic, .plane = 0u, .row_bytes = FRAME_W, .rows = FRAME_H};
    }
    VmafHipPlaneSource *twin = &fx->twin[0];
    mu_assert("no plane is not a request",
              vmaf_hip_plane_source_acquire(twin, fx->frame, seven, 0u, STREAM_A, dev) == -EINVAL);
    mu_assert("more planes than a frame has are refused",
              vmaf_hip_plane_source_acquire(twin, fx->frame, seven, VMAF_HIP_SOURCE_MAX_PLANES + 1u,
                                            STREAM_A, dev) == -EINVAL);
    mu_assert("acquire needs a handle",
              vmaf_hip_plane_source_acquire(NULL, fx->frame, seven, 1u, STREAM_A, dev) == -EINVAL);
    mu_assert("acquire needs a request and a result",
              vmaf_hip_plane_source_acquire(twin, fx->frame, NULL, 1u, STREAM_A, dev) == -EINVAL &&
                  vmaf_hip_plane_source_acquire(twin, fx->frame, seven, 1u, STREAM_A, NULL) ==
                      -EINVAL);
    const VmafHipPlaneUpload empty = {
        .pic = &fx->ref.pic, .plane = 0u, .row_bytes = 0u, .rows = 1u};
    mu_assert("an empty plane is refused",
              vmaf_hip_plane_source_acquire(twin, NULL, &empty, 1u, STREAM_A, dev) == -EINVAL);
    return NULL;
}

static char *test_arguments_are_checked(void)
{
    Fixture fx;
    mu_assert("create needs somewhere to put the frame",
              vmaf_hip_shared_frame_create(NULL) == -EINVAL);
    mu_assert_msg(fixture_open(&fx, 5u, 6u));
    mu_assert("no shared frame is nothing to share, not an error",
              vmaf_hip_shared_frame_begin(NULL, &fx.ref.pic, &fx.dis.pic) == 0);
    mu_assert("begin needs both pictures",
              vmaf_hip_shared_frame_begin(fx.frame, NULL, &fx.dis.pic) == -EINVAL &&
                  vmaf_hip_shared_frame_begin(fx.frame, &fx.ref.pic, NULL) == -EINVAL);
    mu_assert_msg(check_acquire_arguments(&fx));
    mu_assert("nothing was uploaded or allocated", g_upload_calls == 0u && g_dev_allocs == 0u);

    /* Teardown entry points take what they can be handed. */
    vmaf_hip_shared_frame_end(NULL);
    vmaf_hip_plane_source_close(NULL);
    vmaf_hip_shared_frame_destroy(NULL);
    mu_assert("no shared frame counts no upload", vmaf_hip_shared_frame_upload_count(NULL) == 0u);
    return fixture_close(&fx);
}

static char *run_sharing_tests(void)
{
    mu_run_test(test_plane_is_uploaded_once_per_frame);
    mu_run_test(test_picture_is_read_before_acquire_returns);
    mu_run_test(test_frames_alternate_between_two_slots);
    mu_run_test(test_upload_takes_along_the_planes_of_the_frame_before);
    return NULL;
}

static char *run_edge_tests(void)
{
    mu_run_test(test_skipped_twin_makes_the_upload_wait);
    mu_run_test(test_unshared_requests_use_private_planes);
    mu_run_test(test_failed_upload_is_not_remembered);
    mu_run_test(test_arguments_are_checked);
    return NULL;
}

char *run_tests(void)
{
    char *msg = run_sharing_tests();
    return (msg != NULL) ? msg : run_edge_tests();
}

/* NOLINTEND(modernize-use-nullptr) */
