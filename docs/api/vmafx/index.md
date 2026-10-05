# VMAFx C API (preview)

The VMAFx API is the successor of the `libvmaf.h` API: headers under
`vmafx/`, functions named `vmafx_*`, and a model built around contexts,
devices, refcounted frames, models, window scores and a provenance record
behind every score ([ADR-1852](../../adr/1852-vmafx-api-redesign.md)). It is
an RC4 preview: its ABI (version 0.1) may still change until `v1.0.0` freezes
it ([ADR-1897](../../adr/1897-vmafx-abi-0x-numbering.md)). The
existing [`libvmaf.h` API](../index.md) keeps working on the same engine.

This build carries the core of the API: contexts with their own log
callback, options, models and model sets, the CPU device, host frames,
submission and synchronous scores. Device frames and fences, asynchronous
window scores, the full provenance record and reports follow in later RC4
work.

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
| `vmafx/device.h` | `VmafxDevice` (the CPU in this build) |
| `vmafx/frame.h` | Host frames: allocated or borrowed planes |
| `vmafx/model.h` | Models and model sets |
| `vmafx/score.h` | Per-frame and pooled scores |
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
defaults): log level, worker threads, subsampling, CPU and GPU masks, and a
log callback with its user pointer.

- **With a log callback** the context receives, as one line each, the
  messages at or below its `log_level` that the engine raises on the calling
  thread during this context's calls, and every failure reported without an
  error out-parameter. It does not change the process log level, so contexts
  in one process can log at different levels.
- **Without one** messages go to the process log (stderr), and the context
  sets the process level, as `vmaf_init()` always did.
- Messages the engine raises on its worker threads (`n_threads` > 0) go to
  the process log in both cases.

`vmafx_context_set_option()` sets the context options `perceptual_weight`
(`0` / `1`) and `perceptual_weight_strength` (a finite number >= 0).
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
[ADR-1478](../../adr/1478-motion-five-frame-window-port.md)), so a producer
can size its frame pool.

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

`vmafx_model_override_feature(model, extractor, options, error)` changes the
options a model passes to one extractor, for example the normalised viewing
distance of ADM (`adm_norm_view_dist`). It is refused with `VMAFX_E_BUSY`
once the model is shared (another reference exists): a model a context may
use is immutable.

## Frames

Frames are refcounted host frames on the CPU device in this build:

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
`vmafx_flush()` finishes the stream; the scores of the last frames (motion)
become final only then.

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

A model set's per-frame call and its pooled call each predict the set's
members and write their scores once per frame, so call one of them per frame
range in a session; calling the pooled score after the per-frame score of a
frame in it fails in `libvmaf.h` as well
(`docs/state.md`, T-MODEL-SET-SCORE-NOT-IDEMPOTENT-2026-10-05).

## Rules every call follows

- **Status codes** are stable on every platform: `VMAFX_OK` (0), `VMAFX_PENDING`
  (1, the score is not final yet) and negative errors (`VMAFX_E_INVALID`,
  `VMAFX_E_NOTFOUND`, `VMAFX_E_BUSY`, `VMAFX_E_RANGE`, `VMAFX_E_ABI`, ...; see
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

## libvmaf functions on this API

`vmaf_init`, `vmaf_close`, `vmaf_version` and `vmaf_feature_score_at_index`
are generated shims on the VMAFx calls. Their behaviour is unchanged: the
same `NULL` checks, `*vmaf` cleared on failure, the engine's own negative
errno on failure, and a failed `vmaf_close()` still leaves the context valid
for a retry. Every other `libvmaf.h` function is unchanged; a `libvmaf.h`
handle and a `VmafxContext` convert into each other through
`vmafx/libvmaf_bridge.h` for callers that migrate one call at a time.
