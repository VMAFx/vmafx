# Public C API reference

libvmaf ships a stable C API under [`core/include/libvmaf/`](../../core/include/libvmaf/).
This page is the canonical reference for the *core* API (context / picture /
feature / model). GPU-backend entry points and the DNN session API each get
their own page:

- [core](index.md) — this page
- [gpu.md](gpu.md) — `libvmaf_cuda.h`, `libvmaf_sycl.h`,
  `libvmaf_hip.h`, `libvmaf_metal.h` (Vulkan removed per
  [ADR-0726](../adr/0726-drop-vulkan-backend.md))
- [dnn.md](dnn.md) — `libvmaf/dnn.h` (tiny-AI ONNX session)
- [mcp.md](mcp.md) — `libvmaf_mcp.h` (embedded MCP server)

## What each header exposes

| Header | Symbols | Purpose |
| --- | --- | --- |
| [`libvmaf.h`](../../core/include/libvmaf/libvmaf.h) | `VmafContext`, `VmafConfiguration`, lifecycle + scoring functions | Main entry point. Everything else is pulled in transitively. |
| [`picture.h`](../../core/include/libvmaf/picture.h) | `VmafPicture`, `VmafPixelFormat`, alloc / unref | Per-frame pixel container (YUV planes + metadata). |
| [`feature.h`](../../core/include/libvmaf/feature.h) | `VmafFeatureDictionary` | Key/value options passed to a feature extractor. |
| [`model.h`](../../core/include/libvmaf/model.h) | `VmafModel`, `VmafModelConfig`, `VmafModelCollection*` | Classic SVM model + bootstrap model collection. |
| [`dnn.h`](../../core/include/libvmaf/dnn.h) | `VmafDnnSession`, `VmafDnnConfig`, tiny-model attach | Tiny-AI (ONNX Runtime) surface. [Deep dive](dnn.md). |
| [`libvmaf_cuda.h`](../../core/include/libvmaf/libvmaf_cuda.h) | `VmafCudaState`, CUDA picture prealloc | CUDA backend. Only usable in a build with `-Denable_cuda=true`. [Deep dive](gpu.md#cuda). |
| [`libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h) | `VmafSyclState`, zero-copy frame buffers, dmabuf / VA / D3D11 import | SYCL backend. Only usable in a build with `-Denable_sycl=true`. [Deep dive](gpu.md#sycl). |
| ~~`libvmaf_vulkan.h`~~ | ~~`VmafVulkanState`, queue / device lifecycle, zero-copy `VkImage` import~~ | **Removed in [ADR-0726](../adr/0726-drop-vulkan-backend.md).** The header, source, and `enable_vulkan` Meson option no longer exist. Historical reference: [gpu.md#vulkan](gpu.md#vulkan). |
| [`libvmaf_hip.h`](../../core/include/libvmaf/libvmaf_hip.h) | `VmafHipState`, lifecycle, picture prealloc | AMD HIP/ROCm backend. Only usable in a build with `-Denable_hip=true`. [Deep dive](gpu.md#hip). |
| [`libvmaf_metal.h`](../../core/include/libvmaf/libvmaf_metal.h) | `VmafMetalState`, lifecycle, IOSurface import | Apple Metal backend. Runtime, IOSurface import, and the first eight feature kernels are usable in a build with `-Denable_metal=auto/enabled` on Apple Silicon; unsupported hosts return `-ENODEV`. [Deep dive](gpu.md#metal). |
| [`libvmaf_mcp.h`](../../core/include/libvmaf/libvmaf_mcp.h) | `VmafMcpServer`, `VmafMcpConfig`, transport start/stop | Embedded MCP server. Only usable in a build with `-Denable_mcp=true`. [Deep dive](mcp.md). |
| [`vmaf_assert.h`](../../core/include/libvmaf/vmaf_assert.h) | `VMAF_ASSERT*` macros | Internal assertion helpers. Not for public use — may disappear. |
| [`version.h`](../../core/include/libvmaf/libvmaf.h) (generated) | `VMAF_VERSION_MAJOR` etc. | Compile-time version constants. Run-time: `vmaf_version()`. |

All declarations are C (with `extern "C"` guards for C++ callers). The fork has
no C++ entry points in its public API.

## Compiling and linking

Install or build libvmaf, then include and link:

```c
#include <libvmaf/libvmaf.h>
#include <libvmaf/picture.h>
#include <libvmaf/model.h>
```

```text
cc app.c -o app $(pkg-config --cflags --libs libvmaf)
```

`pkg-config` is the canonical way to pick up the right include + link flags and
handles optional GPU backends automatically — when libvmaf was built with
`-Denable_cuda=true`, `pkg-config --libs` adds the CUDA link line; same for
SYCL.

## ABI stability

- **Stable** — the entire `libvmaf.h`, `picture.h`, `feature.h`, and `model.h`
  surface. These come from upstream Netflix/vmaf; the fork preserves them
  verbatim.
- **Stable, fork-added** — `dnn.h` public entry points (`vmaf_dnn_available`,
  `vmaf_use_tiny_model`, the session API). Structs may grow trailing fields
  across minor versions, callers should not over-read.
- **Experimental** — `libvmaf_sycl.h` zero-copy imports
  (`vmaf_sycl_import_va_surface`, `vmaf_sycl_import_d3d11_surface`, dmabuf
  entry points). Signatures may evolve as more backends are added.
- **Private** — `vmaf_assert.h` and anything prefixed `VMAF_ASSERT`. Do not
  depend on it.

Semantic versioning follows the independent VMAFx `vX.Y.Z` stream — see
[ADR-1127](../adr/1127-single-semver-release-stream.md). Every change to the
stable API that would break source or binary compatibility gets a major
version bump.

## Thread-safety

`VmafContext` itself is **not** re-entrant. A single context's scoring
lifecycle (init → feed pictures → score → close) must be driven from one
thread. Internally libvmaf parallelises feature extraction across
`VmafConfiguration.n_threads` workers — that threading is fully
self-contained.

You can run multiple `VmafContext` instances in parallel across threads with
no shared state beyond process-global constants.

Picture buffers (`VmafPicture.data[]`) are only safe to mutate or free after
`vmaf_picture_unref()` brings the refcount to zero. See
[Ownership and lifetime](#ownership-and-lifetime) below.

## Error semantics

Every non-void function returns `int` with these conventions:

- `0` — success.
- A negative number — error. The magnitude is a POSIX `errno` code (always
  positive in `errno.h`); negate to match:
  - `-EINVAL` — bad argument (null pointer, out-of-range enum, wrong shape).
  - `-EAGAIN` — feature still pending; retry after the producer side
    catches up. Returned by `vmaf_score_pooled` /
    `vmaf_score_pooled_model_collection` /
    `vmaf_score_at_index` when the requested frame range has been read
    via `vmaf_read_pictures` but the feature extractor has not yet
    completed. See [ADR-0154](../adr/0154-score-pooled-eagain-netflix-755.md). Not
    a fatal error — the typical fix is either flushing
    (`vmaf_read_pictures(NULL, NULL, 0)`) before scoring, or polling
    until success.
  - `-ENOMEM` — allocation failed.
  - `-ENOENT` — file not found (`vmaf_model_load_from_path` etc).
  - `-ENOSYS` — entry point compiled out (e.g. `vmaf_dnn_*` on a
    `-Denable_dnn=disabled` build).
  - `-EIO` — downstream library error (ONNX Runtime, libav, …).

`libvmaf` does not populate a thread-local last-error; the return code is the
sole error channel. A parallel diagnostic is written via the log callback
configured by `VmafConfiguration.log_level`.

The CLI collapses every negative return to process-exit code 1 and prints a
message — if you need fine-grained error discrimination, call the C API
directly.

## Lifecycle

```text
  ┌─────────────────┐
  │ vmaf_init()     │  → VmafContext*
  └────────┬────────┘
           │
  ┌────────▼─────────────────────┐
  │ vmaf_model_load[_from_path]  │  → VmafModel*
  │ vmaf_use_features_from_model │     (register feature extractors)
  │ vmaf_use_feature()           │     (optional extra features)
  └────────┬─────────────────────┘
           │
  ┌────────▼───────────────────────────┐
  │ loop:                              │
  │   vmaf_picture_alloc(ref)          │
  │   vmaf_picture_alloc(dist)         │
  │   fill planes                      │
  │   vmaf_read_pictures(ref, dist, i) │  (libvmaf takes ownership)
  │ vmaf_read_pictures(NULL, NULL, 0)  │  (flush)
  └────────┬───────────────────────────┘
           │
  ┌────────▼────────────────────────┐
  │ vmaf_score_pooled()             │  (or per-frame: vmaf_score_at_index)
  │ vmaf_feature_score_pooled()     │
  │ vmaf_write_output[_with_format] │
  └────────┬────────────────────────┘
           │
  ┌────────▼────────┐
  │ vmaf_model_destroy()           │
  │ vmaf_close()                   │
  └─────────────────┘
```

## Core configuration — `VmafConfiguration`

```c
typedef struct VmafConfiguration {
    enum VmafLogLevel log_level;   /* NONE | ERROR | WARNING | INFO | DEBUG */
    unsigned n_threads;             /* worker threads for feature extraction */
    unsigned n_subsample;           /* compute scores every Nth frame (1 = all) */
    uint64_t cpumask;               /* disable specific CPU ISAs (see below) */
    uint64_t gpumask;               /* disable BOTH CUDA and SYCL (any non-zero value) */
} VmafConfiguration;
```

`cpumask` bits (identical semantics to the `--cpumask` CLI flag):

| Bit | Disable |
| --- | --- |
| 1 | SSE2 / NEON |
| 2 | SSE3 / SSSE3 |
| 4 | SSE4.1 |
| 8 | AVX2 |
| 16 | AVX512 |
| 32 | AVX512ICL |

> **`gpumask` caveat.** Despite the `uint64_t` type and "bitmask" name,
> the field is treated as a boolean: any non-zero value disables *both*
> CUDA and SYCL in [`libvmaf.c:694-698`](../../core/src/libvmaf.c).
> There is no per-backend bit. Use `--no_cuda` / `--no_sycl` on the
> CLI for per-backend opt-out.
>
> **Even-`n_subsample` warning.** Setting `n_subsample` to an even value can
> produce inaccurate motion scores because the motion feature is frame-delta
> based. Prefer 1 (all frames) or an odd integer. See
> [upstream issue #1214](https://github.com/Netflix/vmaf/issues/1214).

## Core lifecycle API

| Function | Returns | Purpose |
| --- | --- | --- |
| `vmaf_init(VmafContext **out, VmafConfiguration cfg)` | 0 / -errno | Allocate a context. `*out` is owned by the caller; free with `vmaf_close()`. |
| `vmaf_version()` | `const char *` | Version string `vX.Y.Z + git sha`. Does not need `vmaf_init()`. |
| `vmaf_use_features_from_model(ctx, model)` | 0 / -errno | Register every feature a model needs. Deduplicates across multiple models. |
| `vmaf_use_features_from_model_collection(ctx, coll)` | 0 / -errno | Same, for a bootstrap model collection. |
| `vmaf_use_feature(ctx, "psnr", opts)` | 0 / -errno | Register an extra feature not required by any loaded model. Context takes ownership of `opts`; on success never free it yourself. |
| `vmaf_import_feature_score(ctx, name, value, index)` | 0 / -errno | Inject a pre-computed feature value (e.g. from a different pipeline). |
| `vmaf_read_pictures(ctx, ref, dist, index)` | 0 / -errno | Feed a frame pair. `ctx` takes ownership via `vmaf_picture_unref()`. `index` must be **strictly increasing** across successive calls — non-monotonic indices return `-EINVAL` (see [ADR-0152](../adr/0152-vmaf-read-pictures-monotonic-index.md)). Pass `NULL, NULL, 0` to flush after the last frame. |
| `vmaf_score_at_index(ctx, model, *score, index)` | 0 / -errno | Per-frame VMAF score. |
| `vmaf_score_at_index_model_collection(ctx, coll, *score, index)` | 0 / -errno | Per-frame bootstrap score (mean + stddev + 95% CI). |
| `vmaf_feature_score_at_index(ctx, name, *score, index)` | 0 / -errno | Per-frame feature score (e.g. `"psnr_y"`). |
| `vmaf_score_pooled(ctx, model, method, *score, lo, hi)` | 0 / -errno | Pooled VMAF over `[lo, hi]`. |
| `vmaf_score_pooled_model_collection(...)` | 0 / -errno | Pooled bootstrap. |
| `vmaf_feature_score_pooled(ctx, name, method, *score, lo, hi)` | 0 / -errno | Pooled feature score. |
| `vmaf_write_output(ctx, path, fmt)` | 0 / -errno | Write report with the default `%.6f` score format (Netflix-compatible per [ADR-0119](../adr/0119-cli-precision-default-revert.md)). |
| `vmaf_write_output_with_format(ctx, path, fmt, "%.17g")` | 0 / -errno | Write report with a caller-controlled printf format. Pass `NULL` for the `%.6f` default. Pass `"%.17g"` for IEEE-754 round-trip lossless. Format must take exactly one `double`. |
| `vmaf_preallocate_pictures(ctx, cfg)` | 0 / -errno | Allocate a reusable picture pool (CPU path; for GPU see [gpu.md](gpu.md)). |
| `vmaf_fetch_preallocated_picture(ctx, *pic)` | 0 / -errno | Pull a picture from the pool; return it via `vmaf_picture_unref()`. |
| `vmaf_close(ctx)` | 0 / -errno | Free the context. After this the pointer is invalid. |

### `VmafPoolingMethod`

`VmafPoolingMethod` specifies the temporal pooling strategy across frames in a sequence:

| `enum VmafPoolingMethod` value | String name | Description |
| --- | --- | --- |
| `VMAF_POOL_METHOD_MIN` | `"min"` | Minimum frame score across the window |
| `VMAF_POOL_METHOD_MAX` | `"max"` | Maximum frame score across the window |
| `VMAF_POOL_METHOD_MEAN` | `"mean"` | Arithmetic mean of frame scores |
| `VMAF_POOL_METHOD_HARMONIC_MEAN` | `"harmonic_mean"` | Harmonic mean of frame scores |
| `VMAF_POOL_METHOD_MEDIAN` | `"median"` | 50th percentile (median) frame score (ADR-1181) |
| `VMAF_POOL_METHOD_PERC5` | `"perc5"` | 5th percentile frame score (ADR-1181) |
| `VMAF_POOL_METHOD_PERC10` | `"perc10"` | 10th percentile frame score (ADR-1181) |
| `VMAF_POOL_METHOD_PERC20` | `"perc20"` | 20th percentile frame score (ADR-1181) |

Pooled output in XML / JSON reports computes all registered pooling methods in parallel.

## `VmafPicture`

```c
typedef struct VmafPicture {
    enum VmafPixelFormat pix_fmt;   /* YUV420P | YUV422P | YUV444P | YUV400P | UNKNOWN */
    unsigned bpc;                   /* 8, 10, 12, or 16 */
    unsigned  w[3], h[3];           /* per-plane dimensions */
    ptrdiff_t stride[3];            /* per-plane row stride in bytes */
    void     *data[3];              /* per-plane pixel buffer */
    VmafRef  *ref;                  /* INTERNAL — opaque refcount; do not access */
    void     *priv;                 /* INTERNAL — opaque private slot; do not access */
} VmafPicture;
```

Allocation:

```c
int vmaf_picture_alloc(VmafPicture *pic,
                       enum VmafPixelFormat pix_fmt,
                       unsigned bpc,
                       unsigned w, unsigned h);
int vmaf_picture_unref(VmafPicture *pic);
```

`vmaf_picture_alloc` sets `pix_fmt`, `bpc`, per-plane `w`/`h`/`stride`, and
allocates `data[0..N]` on the heap. `vmaf_picture_unref` decrements the
refcount and frees the buffer when it hits zero.

Bits-per-component & storage:

- `bpc == 8` — each sample is 1 byte.
- `bpc == 10`, `12`, `16` — each sample is 2 bytes (little-endian), with the
  valid bits in the low N and the high bits zero-padded.

### Ownership and lifetime

- After `vmaf_read_pictures(ctx, ref, dist, i)` returns 0, the context owns
  `ref` and `dist`. Do **not** call `vmaf_picture_unref()` on them — libvmaf
  will when the extractors are done.
- On error return from `vmaf_read_pictures`, ownership stays with the caller —
  you must unref.
- Stride may differ from `w * bytes_per_sample`. Always use `stride[i]` when
  writing pixel data; do not assume packing.
- `data[i]` alignment is implementation-defined (currently 64-byte aligned for
  SIMD). Copy in using `memcpy` or a pixel-at-a-time loop; do not pointer-cast
  to wider types without re-checking alignment.

## `VmafFeatureDictionary`

`VmafFeatureDictionary` is an opaque string→string map passed as
per-invocation options to a feature extractor.

```c
VmafFeatureDictionary *opts = NULL;

int err = vmaf_feature_dictionary_set(&opts, "enable_chroma", "true");
if (err < 0) { /* -errno */ }

err = vmaf_feature_dictionary_set(&opts, "enable_apsnr", "true");

err = vmaf_use_feature(ctx, "psnr", opts);
/* The call consumed `opts` unless it rejected an argument — see below. */
```

### Ownership: who frees the dictionary

Three calls accept a `VmafFeatureDictionary`: `vmaf_use_feature()`,
`vmaf_model_feature_overload()` and
`vmaf_model_collection_feature_overload()`. They share most of a rule, with
one deliberate difference.

> **All three:** a `-EINVAL` caused by a `NULL` argument takes nothing — the
> caller still owns the dictionary. On every other return, success or failure
> (including `-ENOMEM` from the internal merge/copy), the call has already
> released it and the caller **must not** free it.
>
> **`vmaf_use_feature()` only:** it also takes nothing when `feature_name`
> names no registered feature. It resolves the name against the global
> extractor registry and returns `-EINVAL` before touching the dictionary.

The difference matters, and getting it wrong is a double free. The two model
overloads match `feature_name` against the features of *one particular model*.
A name that matches nothing there is **not** an error — it is a successful
no-op that returns `0` — and the dictionary is consumed anyway. Only
`vmaf_use_feature()` can report an unknown name and hand the dictionary back.

In practice: free the dictionary yourself only when the call returned
`-EINVAL` **and** you either passed a `NULL` argument or called
`vmaf_use_feature()`. Otherwise never.

```c
/* Consumed — success. Freeing here would be a double free. */
if (vmaf_use_feature(ctx, "psnr", opts) == 0)
    opts = NULL;

/* Consumed — the merge ran out of memory. Still do not free. */

/* NOT consumed — vmaf_use_feature rejected the name before taking anything. */
if (vmaf_use_feature(ctx, "no_such_feature", opts2) == -EINVAL)
    vmaf_feature_dictionary_free(&opts2);

/* CONSUMED, and it returned 0. The model simply has no "psnr" feature to
 * overload, which is a no-op, not an error. Freeing opts3 here is a double
 * free — this is the case the old wording got wrong. */
if (vmaf_model_feature_overload(model, "psnr", opts3) == 0)
    opts3 = NULL;
```

Until ADR-1166 this contract was documented two different ways —
`<libvmaf/feature.h>` said the caller kept ownership on any failure,
`<libvmaf/model.h>` said ownership transferred unconditionally — so one of the
two readings was a latent double free
([Netflix/vmaf#1242](https://github.com/Netflix/vmaf/issues/1242)). All three
headers now state the rule above, and it matches what the implementation has
always done. The same report's `-ENOMEM` leak in
`vmaf_model_feature_overload()` and the swallowed copy error in
`vmaf_model_collection_feature_overload()` are fixed in the same change.

Each feature extractor publishes its own option keys — see
[../metrics/features.md](../metrics/features.md) for the full table of
recognised keys per feature.

## `VmafModel` and built-in versions

```c
typedef struct VmafModelConfig {
    const char *name;    /* display name in the report (e.g. "vmaf", "vmaf_neg") */
    uint64_t    flags;   /* OR of VmafModelFlags */
} VmafModelConfig;

enum VmafModelFlags {
    VMAF_MODEL_FLAGS_DEFAULT          = 0,
    VMAF_MODEL_FLAG_DISABLE_CLIP      = (1 << 0),  /* no [0,100] clamp */
    VMAF_MODEL_FLAG_ENABLE_TRANSFORM  = (1 << 1),
    VMAF_MODEL_FLAG_DISABLE_TRANSFORM = (1 << 2),
};

int vmaf_model_load(VmafModel **model, VmafModelConfig *cfg, const char *version);
int vmaf_model_load_from_path(VmafModel **model, VmafModelConfig *cfg, const char *path);
int vmaf_model_feature_overload(VmafModel *model, const char *feature_name,
                                VmafFeatureDictionary *opts_dict);
void vmaf_model_destroy(VmafModel *model);

/* Read feature names required by a loaded model. */
unsigned vmaf_model_feature_count(const VmafModel *model);
const char *vmaf_model_feature_name(const VmafModel *model, unsigned index);

/* Enumerate the built-in version strings compiled into this libvmaf. */
const void *vmaf_model_version_next(const void *prev, const char **version);

/* The version libvmaf scores with when no model is named. */
const char *vmaf_default_model_version(void);
```

Built-in version strings accepted by `vmaf_model_load`:

`vmaf_v0.6.1`, `vmaf_v0.6.1neg`, `vmaf_b_v0.6.3`, `vmaf_4k_v0.6.1`,
`vmaf_4k_v0.6.1neg`, plus `vmaf_float_*` equivalents (legacy float-precision
variants). See [../usage/cli.md#models](../usage/cli.md#models) for when to
pick which.

External JSON models loaded by `vmaf_model_load_from_path` allocate their
`feature_names`, `slopes`, `intercepts`, `feature_opts_dicts`, and
piecewise-linear score-transform `knots` arrays from the JSON payload. There
is no schema-level fixed feature or knot ceiling beyond available memory and
the unsigned parser counters; malformed array entries still fail closed with a
negative errno.

Discover the list programmatically rather than hard-coding it — the set
depends on the build's `VMAF_BUILT_IN_MODELS` and `VMAF_FLOAT_FEATURES`
flags:

```c
const void *handle = NULL;
const char *name   = NULL;
while ((handle = vmaf_model_version_next(handle, &name)) != NULL) {
    printf("built-in model: %s\n", name);
}
```

`vmaf_model_version_next` is an opaque-handle cursor: pass `NULL` on the
first call, pass the previous return on subsequent calls, stop when NULL is
returned. `*version` is left unmodified at end-of-iteration so the caller's
last value stays valid. Pass `version == NULL` if you only need the
iteration count. Returns NULL immediately when the library was built
without any built-in models. See
[ADR-0135](../adr/0135-port-netflix-1424-expose-builtin-model-versions.md)
for the contract's correctness-relevant details (NULL-on-first-call,
end-of-iteration semantics).

### Inspecting model features

Callers can query the features required by a loaded `VmafModel` without
touching opaque struct internals:

```c
const unsigned n = vmaf_model_feature_count(model);
for (unsigned i = 0; i < n; i++) {
    const char *feature_name = vmaf_model_feature_name(model, i);
    printf("feature %u: %s\n", i, feature_name);
}
```

`vmaf_model_feature_count` returns 0 if `model` is `NULL`. `vmaf_model_feature_name`
returns a pointer borrowed from the model (valid for the lifetime of `model`), or
`NULL` if `model` is `NULL` or `index >= n`.

### The default model

When a caller names no model, libvmaf scores with a single default. Read it
rather than assuming it:

```c
const char *dflt = vmaf_default_model_version();   /* "vmaf_v1.0.16_3d0h" */

VmafModel *model = NULL;
VmafModelConfig cfg = { .name = "vmaf" };
int err = vmaf_model_load(&model, &cfg, vmaf_default_model_version());
```

The returned string is owned by libvmaf. It is never `NULL`, must not be freed,
and stays valid for the life of the process. The call is thread-safe and does
no allocation.

**The default changed in 1.0.0.** The built-in default is
**`vmaf_v1.0.16_3d0h`** (ADR-1169). It was `vmaf_v0.6.1` in every earlier build
of this fork, and upstream Netflix still defaults to `vmaf_v0.6.1`.

The two models emit **different feature families**. `vmaf_v0.6.1` reports
`vif_scale0..3` and `motion2`; the v1.0.16 family does not emit those at all.
Code that reads individual feature keys out of the result — rather than just the
pooled `vmaf` score — will see a missing key, not a shifted number, if it assumed
the old family.

The NEG default stays on the v0.6.1 family (`vmaf_v0.6.1neg`): there is no
v1.0.16 NEG model, so appending `neg` to the default would name
`vmaf_v1.0.16_3d0hneg`, which does not exist and fails to load. The AOM CTC preset
also keeps `vmaf_v0.6.1` deliberately, because the CTC spec mandates that exact
model.

To keep the previous behaviour, name the model explicitly instead of relying on
the default:

```c
err = vmaf_model_load(&model, &cfg, "vmaf_v0.6.1");
```

C and C++ code compiled against these headers may use the
`VMAF_DEFAULT_MODEL_VERSION` macro instead, which expands to the same string at
compile time. Prefer the function from anything that is *not* compiled against
this header — language bindings especially — so the value comes from the
library actually loaded rather than from a constant copied into another source
tree and left to drift.

The legacy choice has a named constant of its own: `VMAF_NETFLIX_COMPAT_MODEL_VERSION`
(`"vmaf_v0.6.1"`, also in `libvmaf/model.h`) is the model the CLI selects under
`--netflix-compat` (see [the vmafx CLI page](../usage/vmafx-cli.md)). Use it when
code deliberately wants the Netflix-parity model rather than the fork default, so
the intent is visible and the string has one source.

This is the fork's single source of truth for the default: nothing else in the
tree hardcodes a fallback model name, and
`scripts/ci/check-default-model-single-source.sh` fails the build if anything
starts to. See [ADR-1168](../adr/1168-default-model-single-source.md), and
[docs/development/default-model.md](../development/default-model.md) for how to
change the default.

`vmaf_model_kind` — the fork added model-kind discrimination
(`VMAF_MODEL_KIND_SVM`, `VMAF_MODEL_KIND_DNN_FR`, `VMAF_MODEL_KIND_DNN_NR`,
`VMAF_MODEL_KIND_DNN_FILTER`), auto-detected from file extension + sidecar
JSON. See [ADR-0020](../adr/0020-tinyai-four-capabilities.md) and
[ADR-0022](../adr/0022-inference-runtime-onnx.md).
`VMAF_MODEL_KIND_DNN_FILTER` (added in
[ADR-0168](../adr/0168-tinyai-konvid-baselines.md)) is **registry-only** —
it identifies pre-/post-processing residual filters (e.g.
`learned_filter_v1.onnx` consumed by ffmpeg `vmaf_pre`) for the trust-root
sha256 audit, but is **never loaded by the libvmaf scoring path**;
`vmaf_score_at_index` / `vmaf_score_pooled` only operate on `DNN_FR` /
`DNN_NR` / `SVM` kinds.

### Model collections (bootstrap)

```c
int vmaf_model_collection_load(VmafModel **model,
                               VmafModelCollection **coll,
                               VmafModelConfig *cfg,
                               const char *version);
int vmaf_model_collection_load_from_path(VmafModel **model,
                                         VmafModelCollection **coll,
                                         VmafModelConfig *cfg,
                                         const char *path);
void vmaf_model_collection_destroy(VmafModelCollection *coll);
```

Returns scores as `VmafModelCollectionScore`:

```c
typedef struct {
    enum VmafModelCollectionScoreType type;  /* BOOTSTRAP for bootstrap models */
    struct {
        double bagging_score;   /* mean VMAF across bagged models */
        double stddev;          /* std-dev across bagged models */
        struct { struct { double lo, hi; } p95; } ci;  /* 95% confidence interval */
    } bootstrap;
} VmafModelCollectionScore;
```

See [../metrics/confidence-interval.md](../metrics/confidence-interval.md)
for what the 95% CI means operationally.

## End-to-end example

Score two 1080p raw YUV420P frames using the built-in `vmaf_v0.6.1` model and
add a PSNR sidecar. Prints the pooled mean to stdout with `%.17g` precision.

```c
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>

static int load_plane(FILE *fp, VmafPicture *pic, unsigned plane)
{
    const size_t row_sz = pic->w[plane] * ((pic->bpc > 8) ? 2U : 1U);
    uint8_t *dst = pic->data[plane];
    for (unsigned y = 0; y < pic->h[plane]; y++) {
        if (fread(dst, 1, row_sz, fp) != row_sz) return -EIO;
        dst += pic->stride[plane];
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s ref.yuv dist.yuv\n", argv[0]); return 2; }

    const unsigned W = 1920, H = 1080;

    VmafConfiguration cfg = { .log_level = VMAF_LOG_LEVEL_WARNING, .n_threads = 4 };
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err < 0) return 1;

    VmafModel *model = NULL;
    VmafModelConfig mcfg = { .name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT };
    err = vmaf_model_load(&model, &mcfg, "vmaf_v0.6.1");
    if (err < 0) goto done;

    err = vmaf_use_features_from_model(vmaf, model);
    if (err < 0) goto done;

    err = vmaf_use_feature(vmaf, "psnr", NULL);
    if (err < 0) goto done;

    FILE *fref  = fopen(argv[1], "rb");
    FILE *fdist = fopen(argv[2], "rb");
    if (!fref || !fdist) { err = -errno; goto done; }

    for (unsigned i = 0; ; i++) {
        VmafPicture ref = {0}, dist = {0};
        err = vmaf_picture_alloc(&ref,  VMAF_PIX_FMT_YUV420P, 8, W, H);
        if (err < 0) break;
        err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, 8, W, H);
        if (err < 0) { vmaf_picture_unref(&ref); break; }

        int eof = 0;
        for (unsigned p = 0; p < 3; p++) {
            if (load_plane(fref,  &ref,  p) < 0 ||
                load_plane(fdist, &dist, p) < 0) { eof = 1; break; }
        }
        if (eof) { vmaf_picture_unref(&ref); vmaf_picture_unref(&dist); break; }

        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err < 0) {
            /* ownership stays with caller on error */
            vmaf_picture_unref(&ref); vmaf_picture_unref(&dist);
            break;
        }
    }

    fclose(fref); fclose(fdist);

    /* flush */
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err < 0) goto done;

    double pooled = 0.0;
    err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &pooled, 0, UINT_MAX);
    if (err == 0) printf("VMAF (mean): %.17g\n", pooled);

    double psnr_pooled = 0.0;
    err = vmaf_feature_score_pooled(vmaf, "psnr_y", VMAF_POOL_METHOD_MEAN,
                                    &psnr_pooled, 0, UINT_MAX);
    if (err == 0) printf("PSNR-Y (mean): %.17g\n", psnr_pooled);

done:
    if (model) vmaf_model_destroy(model);
    if (vmaf)  vmaf_close(vmaf);
    return err < 0 ? 1 : 0;
}
```

Build:

```shell
cc app.c -o app $(pkg-config --cflags --libs libvmaf)
```

Run against the Netflix golden pair:

```shell
./app src01_hrc00_576x324.yuv src01_hrc01_576x324.yuv
# VMAF (mean): 76.668905019705577
# PSNR-Y (mean): 30.755064343...
```

Note: this example reads `1920x1080` — change `W`, `H` when running against
the 576×324 fixture.

## Backend introspection — `vmaf_context_get_backend()`

```c
#include "libvmaf/libvmaf.h"

enum VmafBackend backend;
int err = vmaf_context_get_backend(vmaf, &backend);
```

Returns the compute backend that was imported into `vmaf` via a
`vmaf_<backend>_import_state()` call. For CPU-only contexts (no GPU state
imported) the value is `VMAF_BACKEND_UNKNOWN` (0).

| `enum VmafBackend` value | Integer | Meaning |
| --- | --- | --- |
| `VMAF_BACKEND_UNKNOWN` | 0 | CPU-only — no GPU backend imported |
| `VMAF_BACKEND_CUDA` | 1 | CUDA backend (`vmaf_cuda_import_state`) |
| `VMAF_BACKEND_SYCL` | 2 | SYCL backend (`vmaf_sycl_import_state`) |
| `VMAF_BACKEND_METAL` | 3 | Metal backend (`vmaf_metal_import_state`) |
| `VMAF_BACKEND_HIP` | 4 | HIP/ROCm backend (`vmaf_hip_import_state`) |
| `VMAF_BACKEND_VULKAN` | 5 | Reserved — Vulkan removed in ADR-0726 |

Returns `0` on success; `-EINVAL` if `vmaf` or `out` is `NULL`.

New enum values may be appended in future releases. Callers should treat
unknown values as `VMAF_BACKEND_UNKNOWN` (i.e. use a `default:` branch in
any `switch`). See [ADR-0804](../adr/0804-vmaf-context-get-backend.md).

## UTF-8 path contract

All path arguments accepted across libvmaf public C API functions, CLI
tools, and model loaders are UTF-8 on every platform:

- `vmaf_write_output(ctx, path, fmt)` / `vmaf_write_output_with_format(...)`
- `vmaf_model_load_from_path(*model, *cfg, path)`
- `vmaf_model_collection_load_from_path(*coll, *cfg, path)`
- `vmaf_use_tiny_model(ctx, path, ...)`
- CLI options (`--reference`, `--distorted`, `--output`, `--model`, etc.)
  across `vmaf`, `vmaf_per_shot`, `vmaf_roi`, and `vmaf_bench`

On POSIX platforms (Linux, macOS), filesystem paths are byte sequences and
standard system calls handle UTF-8 transparently.

On Windows (`_WIN32`), standard C runtime functions (`fopen`, `_open`)
interpret narrow `char *` strings using the legacy system ANSI code page,
failing on non-ASCII characters. libvmaf establishes a universal UTF-8 contract
via internal wide-character conversion shims
([ADR-1182](../adr/1182-windows-utf8-path-contract.md)):

```c
#include "compat/path_utf8.h"

/* Opens a file using a UTF-8 path on Windows (_wfopen) or fopen on POSIX */
FILE *vmaf_fopen_utf8(const char *path, const char *mode);

/* Opens a file descriptor using a UTF-8 path on Windows (_wopen) or open */
int vmaf_open_utf8(const char *path, int flags, int mode);
```

On Windows, invalid UTF-8 byte sequences set `errno = EILSEQ` and return
`NULL` (or `-1`). Paths longer than 4096 bytes return `NULL` / `-1` with
`errno = ENAMETOOLONG` per NASA/JPL Power of 10.

## Doxygen reference (auto-generated)

For browsable per-symbol HTML, run the standalone doxygen build the fork
ships for the public-API surface — separate from the meson-driven
full-tree generator so the warning bar stays tight on the installable
headers:

```bash
sudo apt-get install -y --no-install-recommends doxygen   # one-off
mkdir -p build/doxygen-public-api
doxygen core/doc/Doxyfile.public-api
open build/doxygen-public-api/html/index.html             # browse
```

The `doxygen-public-api` GitHub Actions workflow runs the same command
on every PR that touches `core/include/libvmaf/` or the Doxyfile and
publishes the rendered HTML + the warning log as build artifacts.
The build is warning-clean — see
[ADR-0953](../adr/0953-doxygen-public-api-clean.md).

## Related

- [gpu.md](gpu.md) — CUDA / SYCL additions to the lifecycle
- [dnn.md](dnn.md) — tiny-AI session API
- [../usage/cli.md](../usage/cli.md) — the `vmaf` CLI walkthrough mirrors this
  API 1:1
- [../metrics/features.md](../metrics/features.md) — feature names + options
- [ADR-0119](../adr/0119-cli-precision-default-revert.md) — current `%.6f`
  default for `vmaf_write_output_with_format(..., NULL)` (supersedes
  [ADR-0006](../adr/0006-cli-precision-17g-default.md))
- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) — the doc-substance
  rule this page satisfies
