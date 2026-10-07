# Pictures: allocation, ownership and pools

Use this page to create `VmafPicture` frames, hand them to a context and
know who frees what. Declarations are in
[`picture.h`](../../core/include/libvmaf/picture.h) and
[`picture_v2.h`](../../core/include/libvmaf/picture_v2.h); the surrounding
call sequence is on the [lifecycle page](lifecycle.md).

## The rule in one paragraph

After `vmaf_read_pictures(ctx, ref, dist, i)` the context owns both pictures,
whatever the call returns. Do not call `vmaf_picture_unref()` on them
afterwards, not even after an error. The only exceptions are calls that had
nothing to take: a `NULL` context or one `NULL` picture, which return
`-EINVAL` and leave both pictures with the caller.

## `VmafPicture`

```c
typedef struct VmafPicture {
    enum VmafPixelFormat pix_fmt; /* YUV420P | YUV422P | YUV444P | YUV400P */
    unsigned bpc;                 /* 8, 10, 12 or 16 */
    unsigned w[3], h[3];          /* per-plane size in samples */
    ptrdiff_t stride[3];          /* per-plane row stride in bytes */
    void *data[3];                /* per-plane sample buffer */
    VmafRef *ref;                 /* internal refcount, do not touch */
    void *priv;                   /* internal, do not touch */
} VmafPicture;
```

| Format | Chroma planes | `w[1]` / `h[1]` |
| --- | --- | --- |
| `VMAF_PIX_FMT_YUV420P` | half width, half height (rounded up) | `ceil(w/2)` / `ceil(h/2)` |
| `VMAF_PIX_FMT_YUV422P` | half width | `ceil(w/2)` / `h` |
| `VMAF_PIX_FMT_YUV444P` | full size | `w` / `h` |
| `VMAF_PIX_FMT_YUV400P` | none (luma only) | `0` / `0` |
| `VMAF_PIX_FMT_UNKNOWN` | sentinel, rejected by the allocator | n/a |

Only planar layouts exist. Repack interleaved formats (NV12, YUYV) before
allocating.

## Allocate and release

```c
int vmaf_picture_alloc(VmafPicture *pic, enum VmafPixelFormat pix_fmt,
                       unsigned bpc, unsigned w, unsigned h);
int vmaf_picture_unref(VmafPicture *pic);
```

| | `vmaf_picture_alloc` | `vmaf_picture_unref` |
| --- | --- | --- |
| Does | Fills every field of `*pic` and allocates the planes. Refcount starts at 1. | Drops one reference. At zero the buffers are freed (or returned to the pool) and the descriptor is zeroed. |
| Inputs | `pic` non-NULL; `pix_fmt` not `UNKNOWN`; `w`, `h` greater than 0. | `pic` or `NULL` (no-op). |
| Errors | `-EINVAL` (NULL `pic`, unknown format, zero size), `-ENOMEM`. | negative errno on failure. |
| Ownership | Caller owns the picture until it is passed to `vmaf_read_pictures()`. | Pair one `unref` with each successful `alloc` that was not handed to a context. |
| Thread-safety | Not thread-safe; one context and its pictures per thread. | Same. |
| ABI | Upstream, stable. | Upstream, stable. |

Layout of the sample buffers:

- `bpc` 8: one byte per sample. `bpc` 10, 12 or 16: two bytes per sample,
  little-endian, value in the low bits.
- `stride[i]` is in bytes and is rounded up to a multiple of 64 samples
  (shifted left by one for `bpc` above 8). It can be larger than
  `w[i]` times the sample size, so always step rows by `stride[i]`.
- Each `data[i]` is allocated 64-byte aligned for the SIMD paths. Copy rows
  in with `memcpy` or a sample loop; do not cast to a wider type without
  re-checking alignment.
- The buffer is zero-filled at allocation, so padding bytes beyond `w[i]`
  read as zero. Write every row you intend to score; the zero fill is not a
  picture.

### Example: fill and submit a frame

```c
VmafPicture ref, dist;
int err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, 8, 576, 324);
if (err < 0) return err;
err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, 8, 576, 324);
if (err < 0) { vmaf_picture_unref(&ref); return err; }

/* ... fill ref.data[p] / dist.data[p] row by row using stride[p] ... */

err = vmaf_read_pictures(vmaf, &ref, &dist, 0);
if (err < 0)
    return err;   /* the context already released both pictures */
```

A complete program is on the [overview page](index.md#minimal-program).

## Sample range

Every sample of a picture of bit depth `bpc` must be at most 2^bpc - 1. The
default path does not check it;
`vmaf_set_sample_range_check_enabled()` turns on a check that refuses an
out-of-range picture with `-EINVAL`. See [Sample range](sample-range.md).

## Index rules

`index` must increase on every call to `vmaf_read_pictures()`. A repeated
or smaller index returns `-EINVAL`. Start at 0 and leave no gaps: after a
skipped index the motion extractors have no previous picture. See
[scoring before the flush and index gaps](lifecycle.md#scoring-before-the-flush-and-index-gaps).

## What the context keeps alive

- The reference picture of the frame before the current one stays alive
  until the next call.
- While a registered extractor reads frame `n-2`, the context also keeps
  the reference pictures of the two frames before the current one. That
  applies to `motion` and `motion_v2` with `motion_five_frame_window=true`,
  which the `vmaf_v1.0.16_hfr_*` models set
  ([ADR-1478](../adr/1478-motion-five-frame-window-port.md)).

If you allocate each picture with `vmaf_picture_alloc()` the only effect is
one more reference picture staying allocated while the window is on.

## Picture pools

A pool avoids per-frame allocation and is required for sensible
multi-threaded use.

```c
typedef struct VmafPictureConfiguration {
    struct { unsigned w, h, bpc; enum VmafPixelFormat pix_fmt; } pic_params;
    unsigned pic_cnt;   /* number of pictures in the pool; 0 disables */
} VmafPictureConfiguration;

int vmaf_preallocate_pictures(VmafContext *vmaf, VmafPictureConfiguration cfg);
int vmaf_fetch_preallocated_picture(VmafContext *vmaf, VmafPicture *pic);
```

| | `vmaf_preallocate_pictures` | `vmaf_fetch_preallocated_picture` |
| --- | --- | --- |
| Does | Allocates `pic_cnt` pictures of one geometry. | Takes one picture from the pool. |
| Ownership | The pool belongs to the context. | Return the picture with `vmaf_picture_unref()` or by passing it to `vmaf_read_pictures()`; it goes back into the pool. |
| Errors | `-EINVAL` (see sizing), `-ENOMEM`. | `-EINVAL` when no pool was preallocated; otherwise a negative errno. It blocks until a picture returns when every slot is in use. |
| Thread-safety | Not thread-safe. | Not thread-safe. |
| ABI | Upstream, stable. | Upstream, stable. |

Sizing the pool:

| Situation | Minimum `pic_cnt` |
| --- | --- |
| Serial run, no `n-2` extractor | 3 pictures serve it |
| `n_threads` workers | `2 * n_threads + 2` keeps every worker supplied |
| An extractor reads frame `n-2` (five-frame motion window) | 4: the two kept reference pictures plus the current pair |

A pool below 4 with a five-frame window is refused with `-EINVAL` by
whichever of `vmaf_preallocate_pictures()` and the registration
(`vmaf_use_feature()`, `vmaf_use_features_from_model()`) comes second. One
error line names `pic_cnt` and the minimum.

All pool slots share one geometry. A session that mixes resolutions needs a
fresh `vmaf_init()`. GPU picture pools are separate: see the
[GPU page](gpu.md).

## Picture v2 (`picture_v2.h`)

`VmafPicture2` adds an explicit backend discriminator and a non-owning
backend handle to the v1 layout. It exists for the dual-API window of
[ADR-0928](../adr/0928-vmaf-picture-v2-explicit-backend-state.md); v1
`VmafPicture` stays unchanged and is what `vmaf_read_pictures()` takes.
The migration plan is in
[the picture v2 migration guide](../architecture/vmaf-picture-v2-migration.md).

```c
typedef enum VmafBackendHandle {
    VMAF_BACKEND_HANDLE_NONE = 0,   /* CPU-resident */
    VMAF_BACKEND_HANDLE_CUDA = 1,   /* backend_handle is a CUstream */
    VMAF_BACKEND_HANDLE_SYCL = 2,   /* a VmafSyclState * cookie */
    VMAF_BACKEND_HANDLE_HIP = 3,    /* hipStream_t */
    VMAF_BACKEND_HANDLE_METAL = 4,  /* id<MTLCommandQueue>, bridged */
    VMAF_BACKEND_HANDLE_VULKAN = 5, /* reserved, Vulkan removed (ADR-0726) */
    VMAF_BACKEND_HANDLE__COUNT = 6  /* sentinel, not a valid handle */
} VmafBackendHandle;
```

`VmafPicture2` has the v1 fields (`pix_fmt`, `bpc`, `w`, `h`, `stride`,
`data`, `ref`, `priv`) followed by `backend`, `backend_handle` and
`_reserved[4]` (zero-initialised, reserved for additive growth).

| Function | Does | Errors |
| --- | --- | --- |
| `int vmaf_picture2_alloc(VmafPicture2 *pic, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w, unsigned h)` | Allocates a CPU-backed v2 picture with `backend = NONE`, `backend_handle = 0`. | `-EINVAL` (NULL, unknown format), `-ENOMEM` |
| `int vmaf_picture2_unref(VmafPicture2 *pic)` | Drops a reference and frees the planes at zero. Never touches `backend_handle`; the backend owns its stream or queue. `NULL` is a no-op. | negative errno |
| `int vmaf_picture_v1_to_v2(const VmafPicture *src, VmafPicture2 *dst)` | Promotes a v1 picture. Increments `src->ref`; the caller still owns `src` and must `vmaf_picture_unref()` it. | `-EINVAL` on NULL |
| `int vmaf_picture_v2_to_v1(const VmafPicture2 *src, VmafPicture *dst)` | Demotes a v2 picture, dropping `backend` and `backend_handle`. Increments `src->ref`; the caller still owns `src` and must `vmaf_picture2_unref()` it. | `-EINVAL` on NULL |
| `const char *vmaf_backend_handle_name(VmafBackendHandle backend)` | Static name such as `"cuda"`, `"sycl"`, `"none"`, or `"unknown"` for an out-of-range value. Never `NULL`; do not free. Safe from any thread. | none |

All v2 functions except `vmaf_backend_handle_name()` are not thread-safe
(one context and its pictures per thread). `data[]` is owned by the
producing backend's allocator and freed through `vmaf_picture2_unref()`;
`priv` belongs to the core and must never be freed by the caller.

!!! warning
    The SONAME bump that removes v1 is scheduled for a later major
    release. Until then do not pass a `VmafPicture2` where a `VmafPicture`
    is expected; convert with `vmaf_picture_v2_to_v1()` first.

```c
VmafPicture2 p2;
VmafPicture p1;
int err = vmaf_picture2_alloc(&p2, VMAF_PIX_FMT_YUV420P, 8, 576, 324);
if (err == 0) err = vmaf_picture_v2_to_v1(&p2, &p1);   /* p1 shares p2's planes */
/* ... fill, then vmaf_read_pictures(vmaf, &p1, ...) takes the p1 reference ... */
vmaf_picture2_unref(&p2);                               /* drop the caller's own reference */
```

## Converting pictures (`vmaf_picture_convert`)

`vmaf_picture_convert()` turns a picture into another pixel format, bit depth,
size and colour description through [zimg](https://github.com/sekrit-twc/zimg).
It is Netflix/vmaf `0497a0f29` with one difference, described below, and it is
opt-in at build time.

Build with zimg 2.7 or newer installed (found through `pkg-config`):

```bash
meson setup build core -Denable_zimg=true
```

Without `-Denable_zimg=true` (the default) the three functions below return
`-ENOTSUP` and log `libvmaf was built without zimg support`. A configure with
`-Denable_zimg=true` and no usable zimg stops at `meson setup`.

| Function | Does | Errors |
| --- | --- | --- |
| `int vmaf_picture_convert_context_init_with_color(VmafPictureConvertContext **ctx, const VmafPicture *src, const VmafColor *src_color, const VmafPictureConvertTarget *target)` | Builds a conversion for the format of `src` (pixel format, bit depth, size) with the colour `*src_color` to `*target`. `target->w` / `h` of 0 keep the source size. | `-EINVAL` (NULL, unset or unsupported colour value, target `bpc` outside 8 to 16, a graph zimg cannot build), `-ENOMEM`, `-ENOTSUP` |
| `int vmaf_picture_convert(VmafPictureConvertContext *ctx, VmafPicture *dst, const VmafPicture *src)` | Allocates `dst` with `vmaf_picture_alloc()` and converts into it. `src` must match the format the context was created with. Call it for any number of pictures. | `-EINVAL` (NULL, mismatched `src`, conversion failure; `dst` untouched), `-ENOMEM`, `-ENOTSUP` |
| `int vmaf_picture_convert_context_close(VmafPictureConvertContext *ctx)` | Frees the context. | `-EINVAL` (NULL), `-ENOTSUP` |

Every colour field must be set: range `LIMITED` or `FULL`; primaries `BT709`,
`BT2020` or `SMPTE432`; transfer `BT709` or `SMPTE2084`; matrix `BT709`,
`BT2020_NCL` or `ICTCP`. Pixel formats are the four planar ones of
`VmafPixelFormat`. `VMAF_RESAMPLE_DEFAULT` means bicubic.

```c
VmafColor sdr = { VMAF_COLOR_RANGE_LIMITED, VMAF_COLOR_PRIMARIES_BT709,
                  VMAF_COLOR_TRC_BT709, VMAF_COLOR_MATRIX_BT709 };
VmafPictureConvertTarget target = {
    .pix_fmt = VMAF_PIX_FMT_YUV444P, .bpc = 10, .color = sdr,
};
VmafPictureConvertContext *conv = NULL;
int err = vmaf_picture_convert_context_init_with_color(&conv, &src, &sdr, &target);
if (err == 0) err = vmaf_picture_convert(conv, &dst, &src);   /* dst is yours */
/* ... use dst, then vmaf_picture_unref(&dst) ... */
vmaf_picture_convert_context_close(conv);
```

### Difference from upstream

Upstream stores the colour description in a new `VmafPicture::color` member
and its `vmaf_picture_convert_context_init(ctx, src, target)` reads the source
colour from there. That inserts a member before `ref` and `priv` and breaks the
binary layout of `VmafPicture`, so the fork does not take it
([ADR-1822](../adr/1822-additive-picture-convert.md)): `VmafPicture` is
unchanged, the source colour is the `src_color` argument, and a converted
`dst` carries no colour (it is `target->color`, which you already hold). The
types, enumerators and the other two functions are upstream's. When upstream
releases the function the ADR describes the migration.

## Converting to a model's conversion target

A model can declare a [`conversion_target`](../models/v1.md#model-declared-conversion-target).
For such a model `vmaf_read_pictures()` converts both pictures to the target
before feature extraction (Netflix/vmaf `a6c0ba6d5`), through the conversion
above. The conversion runs on the host, before any upload to a GPU backend, so
CUDA, SYCL, HIP and Metal runs see the converted pictures; a device-side
conversion is not part of this.

Upstream reads each picture's source colour from `VmafPicture::color`; the
fork declares it once per input on the context instead:

```c
VmafColor pq = { VMAF_COLOR_RANGE_LIMITED, VMAF_COLOR_PRIMARIES_BT2020,
                 VMAF_COLOR_TRC_SMPTE2084, VMAF_COLOR_MATRIX_BT2020_NCL };
int err = vmaf_set_input_colorimetry(vmaf, &pq, &pq);   /* ref, dist; NULL = unspecified */
```

| Case | Result |
| --- | --- |
| model without `conversion_target` | pass-through; the colour is ignored |
| target and both inputs fully specified | each picture not already matching is converted; the others are passed on |
| target and an input unspecified or partly specified | `-EINVAL`, the log names the missing attributes |
| models of one run with different targets (or some with, some without) | `vmaf_use_features_from_model()` returns `-EINVAL` |
| target but no zimg in the build, a picture in device memory, or the SYCL zero-copy entry point `vmaf_read_pictures_sycl()` | `-ENOTSUP` |
| `vmaf_set_input_colorimetry()` after a picture was converted | `-EBUSY` |

`vmaf_set_input_colorimetry()` must be called before the first picture that is
converted, because the zimg context is built from it. As for any error of
`vmaf_read_pictures()`, the context releases the pictures it was given.

On the VMAFx API a frame carries its own colour in `VmafxFrameDesc.color`, and
`vmafx_context_set_default_color()`, which `vmaf_set_input_colorimetry()` calls,
gives the colour of the frames that carry none; see
[Frame colour](vmafx/index.md#frame-colour).
