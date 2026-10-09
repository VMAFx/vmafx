# VMAFx C API (preview)

The VMAFx API is the successor of the `libvmaf.h` API: headers under
`vmafx/`, functions named `vmafx_*`, and a model built around contexts,
devices, refcounted frames, models, window scores and a provenance record
behind every score ([ADR-1852](../../adr/1852-vmafx-api-redesign.md)). It is
an RC4 preview: its ABI (version 0.1) may still change until `v1.0.0` freezes
it ([ADR-1897](../../adr/1897-vmafx-abi-0x-numbering.md)). The
existing [`libvmaf.h` API](../index.md) keeps working on the same engine.

This build carries the core of the API: contexts with their own log
callback, options, models and model sets, devices, host frames, imported
frames with fences and frame pools, submission, synchronous scores and
[asynchronous window scores](windows.md). Imports run on the CPU device in
every build and on CUDA and HIP devices in builds with those backends; the
SYCL and Metal imports, the full provenance record and reports follow in
later RC4 work.

Every declaration, the Python binding and the
[reference pages](reference.md) are generated from one definition,
`core/api/vmafx.toml`; the [API generation guide](../../development/api-generation.md)
explains how to change it.

## Headers

`#include <vmafx/vmafx.h>` brings in every core header. Each header can also be
included on its own and includes what its declarations need:

| Header | Declares |
| --- | --- |
| `vmafx/version.h` | ABI version macros, `vmafx_version_string`, `vmafx_abi_version` |
| `vmafx/types.h` | `VmafxStatus` and the status codes, `VmafxBackend`, `VmafxPixelFormat`, `VmafxPool`, `VmafxLogLevel`, `VmafxOptions`, the export macros |
| `vmafx/error.h` | `VmafxError`, its accessors, `VmafxSubjectKind`, `vmafx_status_name` |
| `vmafx/context.h` | Contexts, the log callback, options, registration, submission, extractor introspection, feature resolution |
| `vmafx/device.h` | `VmafxDevice`, `VmafxDeviceInfo`, enumeration and profiling |
| `vmafx/frame.h` | Host frames, imported frames (`VmafxFrameImport`), fences (`VmafxFence`), frame pools |
| `vmafx/model.h` | Models and model sets |
| `vmafx/score.h` | Per-frame and pooled scores, window scores and the window clock |
| `vmafx/provenance.h` | `VmafxProvenance`, `vmafx_context_provenance` |
| `vmafx/report.h`, `dnn.h`, `mcp.h` | Nothing yet; later RC4 work fills them |
| `vmafx/libvmaf_bridge.h` | Optional (not included by `vmafx.h`): the bridge to a `libvmaf.h` handle |

The [reference index](reference.md) links one generated page per header with
every function, struct field and constant.

## Score two videos

```c
#include <stdio.h>
#include <string.h>

#include <vmafx/vmafx.h>

static int fail(VmafxStatus status, VmafxError *error)
{
    fprintf(stderr, "%s in %s: %s [%s]\n", vmafx_status_name(status),
            vmafx_error_function(error), vmafx_error_message(error),
            vmafx_error_subject(error));
    vmafx_error_free(error);
    return 1;
}

/* Copy one tightly packed 8-bit 4:2:0 picture into a new host frame. */
static VmafxStatus host_frame(const VmafxFrameDesc *desc, const unsigned char *src,
                              VmafxFrame **out, VmafxError **error)
{
    VmafxStatus status = vmafx_frame_create_host(NULL, desc, out, error);
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    if (status == VMAFX_OK)
        status = vmafx_frame_planes(*out, &planes, error);
    for (unsigned p = 0; status == VMAFX_OK && p < planes.n_planes; p++) {
        for (unsigned y = 0; y < planes.h[p]; y++, src += planes.w[p])
            memcpy((unsigned char *)planes.data[p] + y * planes.stride[p], src, planes.w[p]);
    }
    return status;
}

int score(const unsigned char *const *ref, const unsigned char *const *dist, unsigned n)
{
    VmafxContext *context = NULL;
    VmafxModel *model = NULL;
    VmafxError *error = NULL;
    VmafxStatus status = vmafx_context_create(NULL, &context, &error);
    if (status == VMAFX_OK)
        status = vmafx_model_load(NULL, vmafx_model_default_version(), &model, &error);
    if (status == VMAFX_OK)
        status = vmafx_context_use_model(context, model, &error); /* takes a reference */
    const VmafxFrameDesc desc = {.struct_size = sizeof(desc),
                                 .pix_fmt = VMAFX_PIXEL_FORMAT_YUV420P,
                                 .bpc = 8, .w = 1920, .h = 1080};
    for (unsigned i = 0; status == VMAFX_OK && i < n; i++) {
        VmafxFrame *r = NULL;
        VmafxFrame *d = NULL;
        status = host_frame(&desc, ref[i], &r, &error);
        if (status == VMAFX_OK)
            status = host_frame(&desc, dist[i], &d, &error);
        if (status == VMAFX_OK)
            status = vmafx_submit(context, r, d, i, &error); /* consumes both */
        else
            vmafx_frame_unref(r);
    }
    if (status == VMAFX_OK)
        status = vmafx_flush(context, &error);
    VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
    if (status == VMAFX_OK)
        status = vmafx_score_pooled(context, model, VMAFX_POOL_MEAN, 0, n - 1, &pooled, &error);
    if (status == VMAFX_OK)
        printf("%s %s: %.6f\n", vmafx_model_name(model), vmafx_model_hash(model), pooled.value);
    vmafx_model_unref(model);
    if (status != VMAFX_OK)
        return fail(status, error);
    status = vmafx_context_destroy(context, &error);
    return status == VMAFX_OK ? 0 : fail(status, error);
}
```

Build against an installed tree with `pkg-config --cflags --libs libvmaf`;
in this preview the API lives in the same library as `libvmaf.h`. RC4 splits
it as ADR-1852 decides: `libvmafx.so.1` (pkg-config `libvmafx`) with
`libvmaf.so.3` as a thin compatibility library on top.

## Contexts and logging

`vmafx_context_create()` takes a `VmafxContextConfig` (or `NULL` for the
defaults): log level, worker threads, subsampling, CPU and GPU masks, a log
callback with its user pointer, and the longest host wait of the import rule
(`import_retry_wait_ns`, see [the import rule](#admission-and-the-import-rule)).

- **With a log callback** the context receives, as one line each, every
  message the library raises for it at or below its `log_level`: on the
  calling thread during its calls, on its worker threads (`n_threads` > 0)
  while they work for it, and every failure reported without an error
  out-parameter. Nothing of the context reaches the process log, and the
  context does not change the process log level, so contexts in one process
  can log at different levels without seeing each other's lines.
- The callback runs on the thread that raised the message, a library worker
  thread included, and may run on several threads at once: make it
  thread-safe, and do not call back into the context from it.
- **Without one** messages go to the process log (stderr), and the context
  sets the process level, as `vmaf_init()` always did.
- A model belongs to no context: the messages of a model load go to the
  `log_callback` of its `VmafxModelConfig` (with its own `log_level`), or to
  the process log without one.
- `libvmaf.h` calls, also on a handle bridged from a VMAFx context, keep the
  process log.

`vmafx_context_set_option()` sets the context options `perceptual_weight`
(`0` / `1`), `perceptual_weight_strength` (a finite number >= 0) and
`check_sample_range` (`0` / `1`, off by default). With `check_sample_range`
on, `vmafx_submit()` refuses a pair in which a sample of a 9- to 15-bit frame
is above 2^bpc - 1 with `VMAFX_E_INVALID` and logs its plane, row, column and
value; a frame in device memory cannot be scanned and is `VMAFX_E_NOTSUP`
([Sample range](../sample-range.md),
[ADR-1918](../../adr/1918-sample-range-contract-opt-in-check.md)).
`vmafx_context_destroy()` keeps the retry contract of ADR-1336: on a nonzero
status the context and everything it holds stay valid and the destroy can be
called again.

## Registering what to score

| Call | Registers |
| --- | --- |
| `vmafx_context_use_feature(context, "psnr", options, error)` | One extractor; `options` (`VmafxOptions`, may be `NULL`) is copied |
| `vmafx_context_use_model(context, model, error)` | The extractors of every feature the model reads; the context holds a reference to the model until it is destroyed ([ADR-1755](../../adr/1755-collector-owns-mounted-model.md)) |
| `vmafx_context_use_model_set(context, set, error)` | The same for every member of a bootstrap model set |
| `vmafx_context_import_score(context, feature, index, value, error)` | A score computed elsewhere |

`vmafx_options_set(&options, key, value, error)` builds an option set (the
first call creates it); free it with `vmafx_options_free()`. A feature name in
`vmafx_feature_resolve()` answers which extractor would compute it on this
context: the CPU extractor without a device backend, its device twin with one,
or `VMAFX_E_NOTSUP` naming the option the twin cannot honour
([ADR-1359](../../adr/1359-cli-feature-backend-twin.md)).
`vmafx_context_frame_retention()` says how many earlier reference frames the
context keeps after a submit (1, or 2 when an extractor reads frame n-2,
[ADR-1478](../../adr/1478-motion-five-frame-window-port.md)), and
`vmafx_context_max_in_flight()` how many frames of each input it holds at most
when a submit returns, worker threads included, so a producer can size its
frame pool ([frames in flight](windows.md#frames-in-flight-and-backpressure)).

## Models

`vmafx_model_load(config, version, &model, error)` loads a built-in model
(`vmafx_model_builtin_next()` lists them, `vmafx_model_default_version()` is
`vmaf_v1.0.16_3d0h`); `vmafx_model_load_file()` loads a JSON file.
`vmafx_model_set_load()` and `vmafx_model_set_load_file()` load a bootstrap
model set (`vmaf_b_v0.6.3`). Models and sets are refcounted
(`vmafx_model_ref` / `vmafx_model_unref`, `vmafx_model_set_ref` /
`vmafx_model_set_unref`).

`vmafx_model_hash()` and `vmafx_model_set_hash()` return the SHA-256 of the
bytes as loaded, as 64 hex digits: the same value `sha256sum` prints for the
model file, also for a built-in model (it is that file, embedded).
The repository checks every model JSON out with LF line endings on every
platform, so a model file and a build's built-in models hash the same on
Windows, Linux and macOS. A copy converted to CRLF is different bytes and
hashes differently.

`vmafx_model_override_feature(model, extractor, options, error)` changes the
options a model passes to one extractor, for example the normalised viewing
distance of ADM (`adm_norm_view_dist`). It is refused with `VMAFX_E_BUSY`
once the model is shared (another reference exists): a model a context may
use is immutable.

## Frames

Frames are refcounted. Host frames live on the CPU device:

- `vmafx_frame_create_host(device, desc, &frame, error)` allocates planes;
  `vmafx_frame_planes()` tells where to write the samples (`device` may be
  `NULL` for the CPU).
- `vmafx_frame_wrap_host(device, desc, planes, &frame, error)` borrows the
  caller's planes without a copy. `planes.release(planes.user)` is called once,
  on whatever thread drops the last reference; until then the planes must stay
  valid and unchanged.

`vmafx_submit(context, reference, distorted, index, error)` consumes one
reference of each frame on every path, failures included. Indices increase
strictly and every frame keeps the first frame's geometry; a mismatch is
refused naming `reference` or `distorted`. One frame can be scored by several
contexts with different options without a second decode or a copy: take one
more reference per context with `vmafx_frame_ref()` and submit it to each.
The same frame as both inputs of one submit needs two references.
`vmafx_flush()` finishes the stream; the scores that read a later frame
(`motion2` / `motion3` of the last frame) become final only then. Every other
frame's are final once the frame after it is scored
([ADR-2090](../../adr/2090-motion-window-incremental.md)).

### Frame colour

A model can declare a
[`conversion_target`](../../models/v1.md#model-declared-conversion-target): the
colour space its features are defined in. For such a model `vmafx_submit()`
converts both frames of a pair to that target before anything reads them, and
needs to know their colour; a model without one ignores frame colour.

- `VmafxFrameDesc.color` (a `VmafxColor`: range, primaries, transfer
  characteristic, matrix) is the colour of a frame. Frames made by
  `vmafx_frame_create_host()`, `vmafx_frame_wrap_host()`,
  `vmafx_frame_pool_create()` and `vmafx_context_preallocate()` carry the
  colour of their `desc`. Imported and converted frames carry none, and so
  does a frame `vmafx_frame_from_picture()` makes from a libvmaf picture that
  is not a view of a frame.
- A frame carries none when every member of its colour is `UNKNOWN` (the
  `VMAFX_FRAME_DESC_INIT` value). `vmafx_context_set_default_color(context,
  reference, distorted, error)` gives the colour of such frames, per input;
  `NULL` leaves that input's default unset.
- A pair whose colour (its own, else the default) is not fully specified is
  refused with `VMAFX_E_INVALID`, and the log names the missing members: a
  guessed colour would give a plausible but wrong score. A conversion needs a
  build with zimg (`-Denable_zimg=true`); without it `VMAFX_E_NOTSUP`.
- The conversion is built from the first pair it converts. From then on a pair
  of another colour is refused with `VMAFX_E_BUSY` and not counted, and so is
  `vmafx_context_set_default_color()`.

```c
const VmafxColor pq = {VMAFX_COLOR_RANGE_LIMITED, VMAFX_COLOR_PRIMARIES_BT2020,
                       VMAFX_COLOR_TRC_SMPTE2084, VMAFX_COLOR_MATRIX_BT2020_NCL};
VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
desc.pix_fmt = VMAFX_PIXEL_FORMAT_YUV420P;
desc.bpc = 10;
desc.w = 3840;
desc.h = 2160;
desc.color = pq; /* or: vmafx_context_set_default_color(context, &pq, &pq, &error) */
```

A caller compiled against a header without `VmafxFrameDesc.color` passes a
shorter `struct_size`; its frames carry no colour.

## Device frames and fences

A producer that already holds a frame in memory a device can read, a decoder
for example, hands it over with `vmafx_frame_import()` instead of copying it
into a host frame ([ADR-1929](../../adr/1929-vmafx-device-frames-fences.md)).
Every build imports host memory on the CPU device; a build with the CUDA
backend imports CUDA device memory, CUDA arrays and OpenGL textures on CUDA
devices ([CUDA devices](#cuda-devices) below), and a build with the HIP
backend imports HIP device memory, dma-bufs, HIP arrays and OpenGL textures on
HIP devices ([HIP devices](#hip-devices)). The SYCL and Metal imports arrive
behind the same calls and types.

### Devices

| Call | Does |
| --- | --- |
| `vmafx_device_count(backend, &count, error)` | Devices of a backend; a backend this build has none for is `VMAFX_E_NOTSUP`, never a silent 0 |
| `vmafx_device_info(backend, index, &info, error)` | What device `index` is and imports, without creating it |
| `vmafx_device_create(desc, &device, error)` | A device from a backend and an index, or from your runtime's objects (`desc.external`) |
| `vmafx_device_describe(device, &info, error)` | The same information for a created device |
| `vmafx_context_use_device(context, device, error)` | Score a context on the device; call it before registering features or models |

`VmafxDeviceInfo.memory_kinds` and `fence_kinds` have bit `1 << k` set for
every `VmafxMemoryKind` and `VmafxFenceKind` `k` the device imports (the CPU:
`VMAFX_MEMORY_HOST`; `VMAFX_FENCE_NONE` and `VMAFX_FENCE_HOST`). The struct
grows at the end in later releases (the largest frame, bit depths and chroma
layouts each device scores), so pass `VMAFX_DEVICE_INFO_INIT`.

### Importing a frame

A `VmafxFrameImport` names the memory kind, the pixel layout and, per plane,
the handle (for host memory its address), offset, pitch, format modifier and
optionally the size of the memory object. Planar layouts are used where they
are; NV12, P010 and P016 become planar frames by a de-interleave and, for
P010, a shift down by 6, so an imported frame scores bit for bit as the same
frame created on the host. Nothing is copied through the host: a layout the
device cannot read (a tiled modifier on host memory, a memory kind the
device does not bind) is refused with `VMAFX_E_NOTSUP` naming the field.
Setting `VMAFX_IMPORT_ALLOW_COPY` in `flags` allows a copy on the device for
such a layout; a zeroed `flags` means zero copy only.

The producer's memory must stay valid and unchanged until the frame's
release fence is signalled:

```c
/* An NV12 frame the producer wrote on another thread; it signals `written`
 * when the planes are complete. */
VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
imp.memory = VMAFX_MEMORY_HOST;
imp.pix_fmt = VMAFX_PIXEL_FORMAT_NV12;
imp.bpc = 8;
imp.w = 1920;
imp.h = 1080;
imp.n_planes = 2;
imp.plane[0].handle = (uintptr_t)luma;
imp.plane[0].pitch = luma_pitch;
imp.plane[1].handle = (uintptr_t)chroma;      /* Cb, Cr, Cb, Cr, ... */
imp.plane[1].pitch = chroma_pitch;
imp.acquire = written;                        /* a VMAFX_FENCE_HOST fence */

VmafxFrame *frame = NULL;
VmafxFence reusable = VMAFX_FENCE_INIT;
status = vmafx_context_import_frame(context, NULL, &imp, "main", &frame, &error);
if (status == VMAFX_OK)
    status = vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &reusable, &error);
if (status == VMAFX_OK)
    status = vmafx_submit(context, reference, frame, index, &error);
/* ... later, before the producer writes into luma / chroma again: */
vmafx_fence_wait(&reusable, UINT64_MAX, NULL);
vmafx_fence_destroy(&reusable, NULL);
```

### Fences

A `VmafxFence` is passed by pointer; `kind` says what it holds. The library
borrows the acquire fence of an import for the call (it takes a reference,
duplicates a descriptor or queues a device-side wait), so you may destroy
yours right after. Fences the library returns are yours to release once with
`vmafx_fence_destroy()`.

| Call | Does |
| --- | --- |
| `vmafx_fence_create(device, VMAFX_FENCE_HOST, &fence, error)` | An unsignalled host fence, for a producer to signal |
| `vmafx_fence_signal(&fence, error)` | Signal it from the host |
| `vmafx_fence_wait(&fence, timeout_ns, error)` | `VMAFX_OK` once signalled; `VMAFX_E_TIMEOUT` after `timeout_ns`; a timeout of 0 polls and answers `VMAFX_PENDING`, without an error; `UINT64_MAX` waits without a limit |
| `vmafx_frame_release_fence(frame, kind, &fence, error)` | A fence signalled when the last reference of the frame is gone, in every context it was submitted to |

A build with the CUDA backend implements `VMAFX_FENCE_CUDA_EVENT` and
`VMAFX_FENCE_GL_SYNC` for CUDA devices ([CUDA devices](#cuda-devices)); a
build with the HIP backend implements `VMAFX_FENCE_HIP_EVENT`,
`VMAFX_FENCE_GL_SYNC` and, as an acquire fence, `VMAFX_FENCE_SYNC_FILE` for
HIP devices ([HIP devices](#hip-devices)). In every build
`vmafx_fence_wait()` waits on a `VMAFX_FENCE_GL_SYNC` (`glClientWaitSync()`,
which needs a GL context of the sync's share group current on the calling
thread) and, on Linux, on a `VMAFX_FENCE_SYNC_FILE` descriptor (`poll()`);
both stay the producer's, so `vmafx_fence_destroy()` refuses them with
`VMAFX_E_NOTSUP`. The SYCL events, Metal shared events and
Windows shared fences are declared kinds; until their backends land they are
answered with `VMAFX_E_NOTSUP` naming the kind.

### Admission and the import rule

Before a frame is counted, every extractor registered on the context must be
able to read it where it lives without a host copy; `vmafx_context_admit()`
asks the same question ahead of time. A frame in device memory is refused by
a CPU extractor and by an extractor of another backend, and the error names
each refusing extractor and why.

`vmafx_context_import_frame()` is the import rule the FFmpeg filters follow
(decision D8 of [ADR-1852](../../adr/1852-vmafx-api-redesign.md)): it imports
and admits the frame; a transient failure (`VMAFX_E_BUSY`, for example a CPU
import whose acquire fence is not signalled yet, or `VMAFX_E_TIMEOUT`) is
retried once after a host wait on the acquire fence; a second failure, or
any other, fails with one message that names the input
(`main`, `reference`), backend, device, memory kind, pixel format, depth,
size, every plane's modifier, the cause and the attempts, for example
`main: backend cpu device 0, memory HOST, nv12 8-bit 176x144, modifiers 0x0
0x100000000000002: plane 1: modifier ... (1 attempt; no host copy was made)`.

The host wait lasts at most the context's `VmafxContextConfig.import_retry_wait_ns`:
0 (what `VMAFX_CONTEXT_CONFIG_INIT` sets, and what an older caller's shorter
struct gets) means 10 seconds; 1 ns to 600000000000 ns (10 minutes) is used
as given; a larger value makes `vmafx_context_create()` fail with
`VMAFX_E_RANGE` naming `config.import_retry_wait_ns`, rather than being
clamped. Set it lower when a stalled producer should fail the pipeline
sooner, higher when the producer's frames take longer than that to finish.

### Frame pools

`vmafx_frame_pool_create(device, desc, count, &pool, error)` allocates
`count` frames once (the successor of `vmaf_preallocate_pictures()`; size it
with `vmafx_context_frame_retention()` plus the frames you hold).
`vmafx_frame_pool_acquire()` hands out a free frame or answers
`VMAFX_E_BUSY` when every frame is in use; a frame returns to its pool when
its last reference is dropped, and its release fence is signalled then.
`vmafx_frame_pool_destroy()` drops your reference: frames still in use stay
valid until they return.

### CUDA devices

In a build with the CUDA backend
([ADR-2023](../../adr/2023-vmafx-cuda-device-frames.md)):

| Descriptor | Device |
| --- | --- |
| `desc.backend = VMAFX_BACKEND_CUDA`, `desc.index = n` (or -1 for the first) | CUDA device `n`; the library retains its primary context and creates the stream it reads frames on |
| `desc.external[0] = (uintptr_t)cu_context`, `desc.external[1] = (uintptr_t)cu_stream` | Your context, and your stream as the library's stream (0: the library creates one in your context); both stay yours and must outlive the device |

`vmafx_context_use_device(context, device, error)` makes the context score on
the device: each feature registered afterwards runs on its CUDA twin. A
feature without a twin, or whose twin cannot honour an option you set, runs on
the CPU (a warning names it); host frames still score, frames in CUDA memory
are then refused by admission naming that extractor.

What a CUDA device imports:

| `memory` | Planes | Bound or converted |
| --- | --- | --- |
| `VMAFX_MEMORY_DEVICE_POINTER` | `handle` + `offset`: a device address in the device's context; `pitch` in bytes | Planar planes are read where they are, no copy, when each row starts 8-byte aligned and `pitch` is a multiple of 8 and at least the row rounded up to 8 bytes (the CUDA twins load rows 4 or 8 bytes at a time); NV12 / P010 / P016 are planarised on the device |
| `VMAFX_MEMORY_DEVICE_ARRAY` | `handle`: a `CUarray` of the plane's size (1 channel; the NV12 chroma array 2 channels), 8- or 16-bit | NV12 / P010 / P016 are planarised on the device; a planar frame needs `VMAFX_IMPORT_ALLOW_COPY` (one device copy per plane) |
| `VMAFX_MEMORY_GL_TEXTURE` | `handle`: a `GL_TEXTURE_2D` name of the GL context current on the calling thread (NV12: an `GL_R8` luma and a `GL_RG8` chroma texture) | As for arrays; the textures are registered and mapped for the import and unmapped when the frame is released |

A bound plane that does not meet the alignment is refused with
`VMAFX_E_NOTSUP` naming its `offset` or `pitch`; with
`VMAFX_IMPORT_ALLOW_COPY` it is copied on the device instead (logged once).
No path copies a frame through the host.

| Acquire fence | The CUDA device |
| --- | --- |
| `VMAFX_FENCE_CUDA_EVENT` | Makes its stream wait on your event: record it on your stream after the work that writes the planes; nothing waits on the host |
| `VMAFX_FENCE_HOST` | Takes a signalled fence; an unsignalled one is `VMAFX_E_BUSY` (`vmafx_context_import_frame()` waits and retries once) |
| `VMAFX_FENCE_GL_SYNC` | GL texture imports: a signalled `GLsync` is taken; an unsignalled one is `VMAFX_E_BUSY` and the import rule waits on it with `glClientWaitSync()` (the GL context current on the thread) |

Release fences of a CUDA frame:

- `VMAFX_FENCE_HOST`: signalled when the device has run the frame's last
  reader, in every context the frame was submitted to.
- `VMAFX_FENCE_CUDA_EVENT`: an event the library records on its stream behind
  the frame's last reader, at the moment the last reference is dropped. A
  stream wait on a CUDA event that was not recorded yet does not wait, so make
  your stream wait on it from the frame's release callback
  (`VmafxFrameImport.release`, called after the recording) or after
  `vmafx_fence_wait()` returned `VMAFX_OK`; `vmafx_fence_wait()` itself answers
  `VMAFX_PENDING` / waits until the event is recorded and complete.

A decoder that reuses its surfaces without a host wait:

```c
typedef struct Surface {
    CUstream stream;   /* the decoder's stream */
    VmafxFence free;   /* the CUDA_EVENT release fence */
    int in_use;
} Surface;

static void surface_released(void *user)  /* runs after the event is recorded */
{
    Surface *s = user;
    cuStreamWaitEvent(s->stream, (CUevent)s->free.handle, 0);
    vmafx_fence_destroy(&s->free, NULL);
    s->in_use = 0;                         /* the decoder may write into it again */
}

VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
imp.memory = VMAFX_MEMORY_DEVICE_POINTER;
imp.pix_fmt = VMAFX_PIXEL_FORMAT_NV12;
imp.bpc = 8;
imp.w = 1920;
imp.h = 1080;
imp.n_planes = 2;
imp.plane[0].handle = (uintptr_t)luma_devptr;
imp.plane[0].pitch = pitch;
imp.plane[1].handle = (uintptr_t)chroma_devptr;
imp.plane[1].pitch = pitch;
imp.acquire.kind = VMAFX_FENCE_CUDA_EVENT;
imp.acquire.handle = (uintptr_t)decoded;  /* recorded on the decoder's stream */
imp.release = surface_released;
imp.user = surface;

status = vmafx_context_import_frame(context, cuda_device, &imp, "main", &frame, &error);
if (status == VMAFX_OK)
    status = vmafx_frame_release_fence(frame, VMAFX_FENCE_CUDA_EVENT, &surface->free, &error);
```

Ordering. Every frame of a CUDA device is read on the device's one stream, so
one import scored by several contexts on the device needs nothing more, and
its release fences are signalled after the last reader of any of them. Frames
whose acquire fence the device waited on are not synchronised again; frames
without one (pool frames, host frames uploaded by the engine, `libvmaf.h`
callers) keep the engine's once-per-frame synchronisation of the context
([ADR-1199](../../adr/1199-cuda-picture-handover-barrier.md)). A CUDA frame
pool (`vmafx_frame_pool_create()` with a CUDA device) hands out frames whose
planes are device memory (`vmafx_frame_planes()` gives the addresses); write
them on any stream of the device's context and submit, without a fence (the
engine's synchronisation orders your write). `vmafx_frame_pool_acquire()`
hands a frame out only once the device has run the readers of its previous
use, waiting for them on the host when they have not.

`VMAFX_DEVICE_PROFILING` is refused on a CUDA device; profile with the
vendor's profiler (Nsight Systems: `nsys profile --trace=cuda` shows that an
import makes no host-to-device or device-to-host copy of the frame; only the
features' few-byte results come back).

### HIP devices

In a build with the HIP backend
([ADR-2092](../../adr/2092-vmafx-hip-device-frames.md)):

| Descriptor | Device |
| --- | --- |
| `desc.backend = VMAFX_BACKEND_HIP`, `desc.index = n` (or -1 for the first) | HIP device `n`; the library creates the stream it reads frames on |
| `desc.external[0] = (uintptr_t)hip_stream`, `desc.external[1] = 0` | Your stream as the library's stream, on the stream's device (HIP has no context object); it stays yours and must outlive the device |

`vmafx_context_use_device()` makes the context score on the device, as on
CUDA: each feature registered afterwards runs on its HIP twin, and a feature
without one runs on the CPU (a warning names it) and is refused for device
frames by admission. Every HIP twin reads HIP device frames. A HIP device has
no frame pools: `vmafx_frame_pool_create()` refuses it with `VMAFX_E_NOTSUP`
naming `device`.

What a HIP device imports:

| `memory` | Planes | Bound or converted |
| --- | --- | --- |
| `VMAFX_MEMORY_DEVICE_POINTER` | `handle` + `offset`: a device address on the device; `pitch` in bytes | Planar planes are read where they are, at any offset and pitch; NV12 / P010 / P016 are planarised on the device |
| `VMAFX_MEMORY_DMABUF` (Linux) | `fd`: a dma-buf descriptor (the library duplicates it; yours stays open and yours); `offset`, `pitch`; `modifier` 0 (linear), `plane_index` 0; `size` 0 or at most the dma-buf's size | The dma-buf is imported as external memory and mapped whole, once per descriptor of the frame; then as device pointers |
| `VMAFX_MEMORY_DEVICE_ARRAY` | `handle`: a `hipArray_t` of the plane's size (1 channel; the NV12 chroma array 2 channels), 8- or 16-bit | NV12 / P010 / P016 are planarised on the device; a planar frame needs `VMAFX_IMPORT_ALLOW_COPY` (one device copy per plane) |
| `VMAFX_MEMORY_GL_TEXTURE` (Linux) | `handle`: a `GL_TEXTURE_2D` name of the GLX context current on the calling thread, which must render on the device's GPU (NV12: a `GL_R8` luma and a `GL_RG8` chroma texture) | As for arrays; registered read-only and mapped for the import, unmapped when the frame is released. Not on ROCm 10.1, whose runtime cannot read a mapped GL texture: refused, see below |

A tiled dma-buf (a modifier other than linear) is refused with
`VMAFX_E_NOTSUP` naming the plane's `modifier`, and a `size` larger than the
dma-buf with `VMAFX_E_RANGE` naming the plane's `size`: the runtime would map
memory that is not the buffer's. A GL import without a GLX context of the
device's GPU (an EGL context, another GPU's renderer, no context) is refused
with `VMAFX_E_NOTSUP` naming `desc.memory` before the runtime's GL interop is
called; on a host with two GPUs, make the GL context on the device's GPU
(`DRI_PRIME`). A HIP runtime that maps a texture but refuses to read it (ROCm
10.1, the version the project's builds use, refuses every read; ROCm 7.2
reads them) makes the import `VMAFX_E_NOTSUP` naming `desc.memory`, the
texture's extent and the runtime's version. No path copies a frame through
the host.

| Acquire fence | The HIP device |
| --- | --- |
| `VMAFX_FENCE_HIP_EVENT` | Makes its stream wait on your event: record it on your stream after the work that writes the planes; nothing waits on the host |
| `VMAFX_FENCE_SYNC_FILE` | A signalled descriptor is taken; an unsignalled one is `VMAFX_E_BUSY`, and `vmafx_context_import_frame()` waits on it with `poll()` and retries once. For a dma-buf, pass the descriptor `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` returns for reading. It stays yours |
| `VMAFX_FENCE_HOST` | Takes a signalled fence; an unsignalled one is `VMAFX_E_BUSY` (the import rule waits and retries once) |
| `VMAFX_FENCE_GL_SYNC` | GL texture imports: as on CUDA |

Release fences of a HIP frame:

- `VMAFX_FENCE_HOST`: signalled when the device has run the frame's last
  reader, in every context the frame was submitted to.
- `VMAFX_FENCE_HIP_EVENT`: an event the library records on its stream behind
  the frame's last reader when the last reference is dropped. Before that,
  `hipEventQuery()` on it reports it complete (the runtime does so for an
  event never recorded), so make your stream wait on it from the release
  callback (`VmafxFrameImport.release`, called after the recording) or after
  `vmafx_fence_wait()` returned `VMAFX_OK`; `vmafx_fence_wait()` answers
  `VMAFX_PENDING` until the event is recorded and complete.
- `VMAFX_FENCE_SYNC_FILE` is refused with `VMAFX_E_NOTSUP`: no HIP operation
  signals a kernel fence (ROCm 10.1 imports no external semaphore that could
  carry one). Use `HIP_EVENT` or the release callback.

```c
/* An NV12 frame a decoder exported linear, as one dma-buf with both planes
 * (`prime`, a VADRMPRIMESurfaceDescriptor of two layers). */
VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
imp.memory = VMAFX_MEMORY_DMABUF;
imp.pix_fmt = VMAFX_PIXEL_FORMAT_NV12;
imp.bpc = 8;
imp.w = 1920;
imp.h = 1080;
imp.n_planes = 2;
for (int i = 0; i < 2; i++) {
    imp.plane[i].fd = prime.objects[0].fd;
    imp.plane[i].offset = prime.layers[i].offset[0];
    imp.plane[i].pitch = prime.layers[i].pitch[0];
    imp.plane[i].modifier = prime.objects[0].drm_format_modifier;  /* 0: linear */
}

struct dma_buf_export_sync_file sync = {.flags = DMA_BUF_SYNC_READ, .fd = -1};
ioctl(prime.objects[0].fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &sync);
imp.acquire.kind = VMAFX_FENCE_SYNC_FILE;
imp.acquire.fd = sync.fd;

status = vmafx_context_import_frame(context, hip_device, &imp, "main", &frame, &error);
close(sync.fd);  /* the library took what it needs */
```

Ordering. Every read of a HIP frame's memory is a copy or a conversion on the
device's one stream, which the twins' own streams wait for, so one import
scored by several contexts on the device needs nothing more, and its release
fences are signalled after the last reader of any of them.

`VMAFX_DEVICE_PROFILING` is refused on a HIP device; profile with the
vendor's tools. The runtime's API log (`AMD_LOG_LEVEL=3`) lists every copy
with its direction: an import makes device-to-device copies on the library
stream and no host-to-device or device-to-host copy of the frame.

## Scores

| Call | Returns |
| --- | --- |
| `vmafx_feature_score` | One feature at one frame, with the extractor that produced it |
| `vmafx_score_frame` | A model's score at one frame (`VmafxScore`) |
| `vmafx_score_frame_model_set` | A set's bootstrap score at one frame (`VmafxModelSetScore`) |
| `vmafx_score_pooled`, `vmafx_feature_score_pooled` | A model or a feature pooled over `[first, last]` with a `VmafxPool` method (`VmafxPooledScore`) |
| `vmafx_score_pooled_model_set` | Each of a set's four values pooled |

Every value comes from the same engine code as the `libvmaf.h` call, so the
two are equal bit for bit (`core/test/test_vmafx_bitexact.c` checks every
feature and model score of the Netflix golden pair, both checkerboards and a
10-bit pair). A score that is not final yet returns `VMAFX_PENDING`, where the
`libvmaf.h` call returns `-EAGAIN`: an answer, not a failure, so no error is
created and the output struct is not written.

A model set's frame scores are predicted once and then read back, so its
per-frame and pooled calls can be mixed in one session and repeated (PR #2206).

[Window scores](windows.md) are the asynchronous form: ask for a pooled score
over a range of frames before they are final, and poll, wait for or receive
it when they are, with the same values bit for bit.

## Rules every call follows

- **Status codes** are stable on every platform: `VMAFX_OK` (0), `VMAFX_PENDING`
  (1, the score is not final yet, or a polled fence is not signalled) and
  negative errors (`VMAFX_E_INVALID`, `VMAFX_E_NOTFOUND`, `VMAFX_E_BUSY`,
  `VMAFX_E_RANGE`, `VMAFX_E_TIMEOUT`, `VMAFX_E_ABI`, ...; see
  [`vmafx/types.h`](types.md)).
- **Errors name what failed.** Pass a `VmafxError **` as the last argument to
  receive the status, a message, the subject (the parameter, option, feature,
  extractor, model, frame, plane or path), what kind of subject it is
  (`vmafx_error_subject_kind()`), the function that failed and the engine's
  errno; free it with `vmafx_error_free()`. Pass `NULL` to get the status
  only: the message then goes to the context's log callback at `ERROR`, or to
  stderr, whatever the log level, so no failure is silent.
- **Structs carry their size.** Initialise every struct with its `*_INIT`
  macro (it sets `struct_size`). A newer library reads only the fields an
  older caller compiled in and uses the documented defaults for the rest; an
  input struct smaller than the struct was when it was introduced is
  `VMAFX_E_ABI`. An output struct receives what its `struct_size` holds, and
  `struct_size` is set to the bytes written.
- **Strings** the library returns live as long as the object they came from;
  `vmafx_version_string()` and extractor names for the whole process.

On Linux every `vmafx_*` function carries the symbol version of the ABI minor
that introduced it (`nm -D libvmaf.so` shows `vmafx_context_create@@VMAFX_0.1`),
so a program built against a newer minor fails to load against an older
library instead of failing at its first call. The `vmaf_*` functions keep their
unversioned symbols.

## Python

The generated binding uses only the standard library (`ctypes`) and loads the
library you name; there is no search fallback:

```python
from vmafx import Library, Pool

lib = Library("/usr/local/lib/libvmaf.so.3")   # or set VMAFX_LIBRARY
with lib.context() as context:
    context.import_score("external", 0, 1.5)
    print(lib.version_string(), context.feature_score_pooled("external", Pool.MEAN, 0, 0))
```

A failed call raises `vmafx.VmafxError` with `status`, `message`, `subject`
and `errno`; `VMAFX_PENDING` raises its subclass `VmafxPending`. The module
checks its struct layouts against the definition when it is imported. The
binding wraps the calls with plain arguments; the rest are reachable through
`Library.raw` until the generated language bindings land. The package sits
in `bindings/python/vmafx/` in the source tree.

## Migrating from libvmaf.h

The engine and the VMAFx API ship as `libvmafx.so.1` (pkg-config `libvmafx`,
headers `vmafx/*.h`). The `libvmaf.h` API is a separate, thin library,
`libvmaf.so.3` (pkg-config `libvmaf`), written on the exported `vmafx_*`
functions only ([ADR-2094](../../adr/2094-libvmaf-compat-library-split.md)).
Programs written for libvmaf keep building and running unchanged:
`pkg-config --libs libvmaf` gives `-lvmaf -lvmafx`, and a binary linked
against an earlier `libvmaf.so.3` runs against this one. The
[migration table](compat.md) lists every libvmaf function with the VMAFx
calls it is built on.

To migrate one call at a time, keep the libvmaf handles and convert them with
`vmafx/libvmaf_bridge.h`: `vmafx_context_from_libvmaf()` /
`vmafx_context_libvmaf_handle()` for contexts, `vmafx_model_from_libvmaf()`
and `vmafx_model_set_from_libvmaf()` for models and collections, and
`vmafx_frame_from_picture()` / `vmafx_frame_to_picture()` for pictures (each
takes a new reference, so the caller keeps its own).

To find the calls to replace, compile with the deprecation warnings on. They
are opt-in in this release, on by default in 1.1, and the functions go in 2.0
([ADR-1852](../../adr/1852-vmafx-api-redesign.md) decision D7):

```bash
cc -DVMAF_ENABLE_DEPRECATION_WARNINGS -c my_program.c $(pkg-config --cflags libvmaf)
# my_program.c:12: warning: 'vmaf_init' is deprecated: use vmafx_context_create
```

Two libvmaf functions newer than the first VMAFx release map onto it as
follows ([migration table](compat.md)):

- `vmaf_set_sample_range_check_enabled(vmaf, enabled)` is the context option
  `check_sample_range`: `vmafx_context_set_option(context,
  "check_sample_range", "1", &error)`.
- `vmaf_set_input_colorimetry(vmaf, ref, dist)` declares the colour once per
  input, because a libvmaf picture has no colour member; it is
  `vmafx_context_set_default_color()`. A VMAFx frame can carry its own colour
  in `VmafxFrameDesc.color` instead ([Frame colour](#frame-colour)). Both
  answer `-EBUSY` / `VMAFX_E_BUSY` once a picture has been converted.

### What the compat library does differently

Return values, outputs and scores are libvmaf's: `test_compat_conformance`
runs every libvmaf function through libvmaf's own code and through the compat
library and requires the same results, every score bit for bit. A few side
effects differ on purpose:

| Call | libvmaf | Compat library |
| --- | --- | --- |
| `vmaf_write_output()` with an unknown format | `-EINVAL`, after truncating the file | `-EINVAL`; the file is not touched |
| JSON / XML reports | No provenance record | The provenance record of the context, and the backend receipt (`backend_used`, `feature_backends`) ([ADR-2073](../../adr/2073-vmafx-provenance-record.md)) |
| `vmaf_model_feature_overload()` naming an extractor the model reads no feature of | 0; listed in the model's overrides | 0; changes nothing and is not listed in the provenance record |
| Changing a model that a context already uses, or a collection's lead model directly | Allowed | `-EBUSY`: a model a context may use is immutable |

### Backends that still use the engine's functions

The CUDA and SYCL functions (`libvmaf_cuda.h`, `libvmaf_sycl.h`), and the HIP
and Metal ones in builds with those backends, are still the engine's own
definitions, exported from `libvmafx.so.1` under a `VMAF_LEGACY_<BACKEND>`
symbol version, until the matching RC4 device-frame work lands (the
[migration table](compat.md) names it per function). Programs link them
through `pkg-config --libs libvmaf` as before. In a build without HIP or
Metal, their functions are compat functions that report the backend as
absent, as libvmaf's stubs did.
