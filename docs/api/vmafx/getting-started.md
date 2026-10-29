# Getting started with the VMAFx C API

This page walks through one complete C program: it reads two raw YUV files,
hands each frame to the library as a host frame, scores the pair with the
default model, prints the pooled score and then prints the provenance record
that says which build, model and settings produced it. Every later guide
builds on these steps.

To score files without writing code, use the command line instead:
[Score your first pair](../../getting-started/first-score.md).

## What you need

- An installed VMAFx tree, or a build directory: the program links
  `libvmafx.so.1` and includes `<vmafx/vmafx.h>`. The
  [source-build guide](../../getting-started/index.md#build-from-source-any-platform)
  builds both.
- Two 8-bit YUV 4:2:0 files of the same size. The repository ships a pair:
  `testdata/ref_576x324_48f.yuv` and `testdata/dis_576x324_48f.yuv`
  (576x324, 48 frames).

## The program

<!-- compile -->
```c
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Read one 4:2:0 frame from `file` into a new host frame. */
static VmafxStatus read_frame(FILE *file, const VmafxFrameDesc *desc, VmafxFrame **out,
                              VmafxError **error)
{
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    VmafxStatus status = vmafx_frame_create_host(NULL, desc, out, error);
    if (status == VMAFX_OK)
        status = vmafx_frame_planes(*out, &planes, error);
    for (unsigned p = 0; status == VMAFX_OK && p < planes.n_planes; p++) {
        for (unsigned y = 0; y < planes.h[p]; y++) {
            unsigned char *row = (unsigned char *)planes.data[p] + (size_t)y * planes.stride[p];
            if (fread(row, 1, planes.w[p], file) != planes.w[p])
                status = VMAFX_E_IO;
        }
    }
    if (status != VMAFX_OK && *out != NULL) {
        vmafx_frame_unref(*out);
        *out = NULL;
    }
    return status;
}

static VmafxStatus score_files(VmafxContext *context, const VmafxModel *model, FILE *ref,
                               FILE *dist, const VmafxFrameDesc *desc, unsigned n_frames,
                               VmafxError **error)
{
    VmafxStatus status = VMAFX_OK;
    for (unsigned i = 0; status == VMAFX_OK && i < n_frames; i++) {
        VmafxFrame *r = NULL;
        VmafxFrame *d = NULL;
        status = read_frame(ref, desc, &r, error);
        if (status == VMAFX_OK)
            status = read_frame(dist, desc, &d, error);
        if (status == VMAFX_OK)
            status = vmafx_submit(context, r, d, i, error); /* consumes both frames */
        else if (r != NULL)
            vmafx_frame_unref(r);
    }
    if (status == VMAFX_OK)
        status = vmafx_flush(context, error);

    VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
    if (status == VMAFX_OK)
        status = vmafx_score_pooled(context, model, VMAFX_POOL_MEAN, 0, n_frames - 1, &pooled,
                                    error);
    if (status == VMAFX_OK)
        printf("%s (%s): %.6f\n", vmafx_model_name(model), vmafx_model_hash(model),
               pooled.value);
    return status;
}

static VmafxStatus print_provenance(VmafxContext *context, VmafxError **error)
{
    VmafxProvenance record = VMAFX_PROVENANCE_INIT;
    VmafxStatus status = vmafx_context_provenance(context, &record, error);
    if (status == VMAFX_OK)
        printf("vmafx %s (%s), simd %s, %" PRIu64 " frames\n%s\n", record.version,
               record.commit, record.simd, record.n_frames, record.digest);
    const char *json = NULL;
    if (status == VMAFX_OK)
        status = vmafx_context_provenance_json(context, VMAFX_PROVENANCE_JSON_CANONICAL, &json,
                                               error);
    if (status == VMAFX_OK)
        printf("%s\n", json);
    return status;
}

int main(int argc, char **argv)
{
    const char *ref_path = argc > 1 ? argv[1] : "testdata/ref_576x324_48f.yuv";
    const char *dist_path = argc > 2 ? argv[2] : "testdata/dis_576x324_48f.yuv";
    const unsigned w = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 576;
    const unsigned h = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 324;
    const unsigned n_frames = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : 48;
    if (w == 0 || h == 0 || n_frames == 0) {
        fprintf(stderr, "usage: %s REF DIST WIDTH HEIGHT FRAMES\n", argv[0]);
        return 2;
    }

    FILE *ref = fopen(ref_path, "rb");
    FILE *dist = fopen(dist_path, "rb");
    if (ref == NULL || dist == NULL) {
        perror("fopen");
        if (ref != NULL)
            (void)fclose(ref);
        if (dist != NULL)
            (void)fclose(dist);
        return 2;
    }

    VmafxError *error = NULL;
    VmafxContext *context = NULL;
    VmafxModel *model = NULL;
    VmafxFrameDesc desc = VMAFX_FRAME_DESC_INIT;
    desc.pix_fmt = VMAFX_PIXEL_FORMAT_YUV420P;
    desc.bpc = 8;
    desc.w = w;
    desc.h = h;

    VmafxStatus status = vmafx_context_create(NULL, &context, &error);
    if (status == VMAFX_OK)
        status = vmafx_model_load(NULL, vmafx_model_default_version(), &model, &error);
    if (status == VMAFX_OK)
        status = vmafx_context_use_model(context, model, &error); /* takes a reference */
    if (status == VMAFX_OK)
        status = vmafx_context_annotate(context, "input", ref_path, &error);
    if (status == VMAFX_OK)
        status = score_files(context, model, ref, dist, &desc, n_frames, &error);
    if (status == VMAFX_OK)
        status = print_provenance(context, &error);

    (void)fclose(ref);
    (void)fclose(dist);
    vmafx_model_unref(model);
    VmafxError *destroy_error = NULL;
    if (vmafx_context_destroy(context, &destroy_error) != VMAFX_OK)
        vmafx_error_free(destroy_error);
    return status == VMAFX_OK ? 0 : fail(status, error);
}
```

Build and run it from the repository root against an installed tree:

```bash
cc -o score score.c $(pkg-config --cflags --libs libvmafx)
./score
```

Against a build directory instead, point the compiler at the headers and the
library of that build (`build` here):

```bash
cc -o score score.c -I core/include -I build/include -L build/src -lvmafx
LD_LIBRARY_PATH=build/src ./score
```

On the repository's pair the first line is the score the command line prints
for the same files, followed by the record.

## Step by step

### Host frames

A frame is described by a `VmafxFrameDesc`: pixel format, bits per component
and size. `vmafx_frame_create_host(NULL, ...)` allocates a frame in host
memory that the library owns; `vmafx_frame_planes()` returns each plane's
pointer, visible width and height in bytes, and stride. The stride can be
larger than the width, so the program copies row by row. Frames that come
from a GPU decoder are imported instead of copied; see
[`vmafx/frame.h`](frame.md) and [`vmafx/device.h`](device.md).

### The model

`vmafx_model_load(NULL, vmafx_model_default_version(), ...)` loads the
default built-in model. `vmafx_context_use_model()` mounts it on the context
and takes its own reference, so the program can drop its reference at the
end with `vmafx_model_unref()`. A model loaded from a file uses
`vmafx_model_load_file()`; see [`vmafx/model.h`](model.md).

### Submit, flush, pool

`vmafx_submit()` takes one reference frame and one distorted frame with
their index, and consumes both: after the call the program no longer owns
them, whether it succeeded or not. `vmafx_flush()` waits until every
submitted frame is scored. `vmafx_score_pooled()` then pools the model's
per-frame scores over an inclusive frame range; here the arithmetic mean of
all frames. For scores that are ready while frames are still arriving, use
[window scores](windows.md).

### Provenance

`vmafx_context_annotate()` adds a key and value that only the caller knows,
here the input path, to the record. `vmafx_context_provenance()` fills a
`VmafxProvenance` with the library version and commit, the build flags and
floating-point policy, the backend, device and SIMD level, the frame
geometry and count, and two digests: `scores_digest` over the per-frame
scores and `digest` over the whole record. Two runs with the same digest
used the same build, model, settings and inputs and produced the same
scores. `vmafx_context_provenance_json()` returns the same record, with the
models, features and annotations, as canonical JSON (RFC 8785) for storage
next to the score. Field by field: [`vmafx/provenance.h`](provenance.md).

### Errors

Every call returns a `VmafxStatus`, and every call that can fail takes an
optional `VmafxError **`. The program passes one error object through all
calls, stops at the first failure and prints the status, the function, the
message and the subject. [`vmafx/error.h`](error.md) lists the accessors;
the status codes are in [`vmafx/types.h`](types.md#status-codes).

## Next steps

- [VMAFx API preview](index.md): the headers, the rules every call follows
  and the API's current state
- [Window scores](windows.md): pooled scores while a stream is still running
- [Generated reference](reference.md): every function, struct and enum
- [libvmaf migration table](compat.md): the VMAFx successor of every
  `libvmaf.h` function
