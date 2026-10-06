# Public C API reference

Start here to score a video pair from C: this page shows a complete program,
the header map and the rules every call shares. Per-function detail is on
the topic pages below. Declarations live in
[`core/include/libvmaf/`](../../core/include/libvmaf/).

This API is the compatibility library `libvmaf.so.3`, built on the
[VMAFx API](vmafx/index.md) of `libvmafx.so.1`; link it with
`pkg-config --libs libvmaf`, which adds `-lvmafx`. Its functions are
deprecated in favour of their VMAFx successors: the warnings are opt-in in
1.0 (`-DVMAF_ENABLE_DEPRECATION_WARNINGS`), and the
[migration table](vmafx/compat.md) names the successor of every function.

| Page | Covers |
| --- | --- |
| [Lifecycle](lifecycle.md) | `vmaf_init` to `vmaf_close`: configuration, registration, feeding, scoring, pooling, reports, backend introspection, device twins |
| [Pictures](pictures.md) | `VmafPicture`, allocation, ownership, pools, picture v2 |
| [Models and features](models-and-features.md) | Model loading, default model, collections, feature option dictionaries |
| [GPU backends](gpu.md) | `libvmaf_cuda.h`, `libvmaf_sycl.h`, `libvmaf_hip.h`, `libvmaf_metal.h` |
| [DNN sessions](dnn.md) | `libvmaf/dnn.h`, the tiny-AI ONNX session |
| [Embedded MCP server](mcp.md) | `libvmaf_mcp.h` |
| [Rust close and retry](rust-context-close.md) | Retry-safe ownership in the Rust wrappers |
| [Perceptual weighting](perceptual-weight.md) | `perceptual_weight.h` |

## Minimal program

This program scores two raw 8-bit 4:2:0 YUV files with the Netflix-compatible
`vmaf_v0.6.1` model and adds a PSNR feature. It prints the pooled means with
six decimals.

```c
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>

/* Fill one picture from a raw planar file. Returns 0, 1 at end of file, or -errno. */
static int read_frame(FILE *fp, VmafPicture *pic)
{
    const size_t sample = (pic->bpc > 8) ? 2U : 1U;
    for (unsigned p = 0; p < 3; p++) {
        const size_t row = pic->w[p] * sample;
        uint8_t *dst = pic->data[p];
        for (unsigned y = 0; y < pic->h[p]; y++) {
            if (fread(dst, 1, row, fp) != row)
                return feof(fp) ? 1 : -EIO;
            dst += pic->stride[p];
        }
    }
    return 0;
}

/* Feed every frame pair. Returns the frame count, or -errno. */
static int feed(VmafContext *vmaf, FILE *fref, FILE *fdist, unsigned w, unsigned h)
{
    unsigned n = 0;
    for (;;) {
        VmafPicture ref, dist;
        int err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, 8, w, h);
        if (err < 0)
            return err;
        err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, 8, w, h);
        if (err < 0) {
            vmaf_picture_unref(&ref);
            return err;
        }
        int rr = read_frame(fref, &ref);
        int rd = (rr == 0) ? read_frame(fdist, &dist) : rr;
        if (rr != 0 || rd != 0) {
            /* Nothing was handed to the context: release both pictures. */
            vmaf_picture_unref(&ref);
            vmaf_picture_unref(&dist);
            return (rr < 0) ? rr : (rd < 0) ? rd : (int)n;
        }
        /* The context owns both pictures from here on, whatever it returns. */
        err = vmaf_read_pictures(vmaf, &ref, &dist, n);
        if (err < 0)
            return err;
        n++;
    }
}

static int score(VmafContext *vmaf, VmafModel *model, unsigned n)
{
    int err = vmaf_read_pictures(vmaf, NULL, NULL, 0); /* flush */
    if (err < 0)
        return err;

    double vmaf_mean = 0.0, psnr_mean = 0.0;
    err = vmaf_score_pooled(vmaf, model, VMAF_POOL_METHOD_MEAN, &vmaf_mean, 0, n - 1);
    if (err < 0)
        return err;
    err = vmaf_feature_score_pooled(vmaf, "psnr_y", VMAF_POOL_METHOD_MEAN, &psnr_mean, 0, n - 1);
    if (err < 0)
        return err;
    printf("VMAF (mean):   %.6f\nPSNR-Y (mean): %.6f\n", vmaf_mean, psnr_mean);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s ref.yuv dist.yuv width height\n", argv[0]);
        return 2;
    }
    const unsigned w = (unsigned)atoi(argv[3]), h = (unsigned)atoi(argv[4]);
    FILE *fref = fopen(argv[1], "rb");
    FILE *fdist = fopen(argv[2], "rb");
    if (!fref || !fdist) {
        perror("fopen");
        return 1;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_WARNING, .n_threads = 4};
    VmafContext *vmaf = NULL;
    VmafModel *model = NULL;
    VmafModelConfig mcfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};

    int err = vmaf_init(&vmaf, cfg);
    if (err == 0)
        err = vmaf_model_load(&model, &mcfg, "vmaf_v0.6.1");
    if (err == 0)
        err = vmaf_use_features_from_model(vmaf, model);
    if (err == 0)
        err = vmaf_use_feature(vmaf, "psnr", NULL);
    if (err == 0)
        err = feed(vmaf, fref, fdist, w, h);
    if (err > 0)
        err = score(vmaf, model, (unsigned)err);
    else if (err == 0)
        err = -EINVAL; /* no frames */

    fclose(fref);
    fclose(fdist);

    int status = (err < 0) ? 1 : 0;
    if (vmaf) {
        int close_err = vmaf_close(vmaf);
        if (close_err != 0)
            close_err = vmaf_close(vmaf); /* one retry of the teardown-only context */
        if (close_err != 0)
            return 1; /* model must stay alive: close did not succeed */
    }
    vmaf_model_destroy(model); /* NULL is a no-op */
    return status;
}
```

Build and run it against the Netflix golden pair (576x324, 48 frames):

```shell
cc app.c -o app $(pkg-config --cflags --libs libvmaf)
./app src01_hrc00_576x324.yuv src01_hrc01_576x324.yuv 576 324
```

Expected output (checked against a CPU build):

```text
VMAF (mean):   76.667831
PSNR-Y (mean): 30.755064
```

With the default model `vmaf_v1.0.16_3d0h` the same pair pools to
82.816060; see [the default model](models-and-features.md#the-default-model).

Points to notice in the program:

- `vmaf_read_pictures()` owns both pictures from the call onwards, so
  `feed()` never unrefs them after it, not even on error.
- Pictures that were never submitted (end of file, read error) are unref'd
  by the caller.
- `vmaf_close()` is retried once, and the model is destroyed only after it
  returned 0.

## Core lifecycle API

Find a symbol: this table maps each core function and type to the page that
documents it.

| Symbol | Page |
| --- | --- |
| `vmaf_init`, `vmaf_close`, `vmaf_version` | [Open and close](lifecycle.md#open-and-close), [close and retry](lifecycle.md#close-and-retry) |
| `VmafConfiguration`, `cpumask`, `gpumask` | [Configuration](lifecycle.md#configuration-vmafconfiguration) |
| `vmaf_use_features_from_model[_collection]`, `vmaf_use_feature`, `vmaf_import_feature_score` | [Register features](lifecycle.md#register-features) |
| `vmaf_read_pictures`, `vmaf_preallocate_pictures`, `vmaf_fetch_preallocated_picture` | [Feed pictures](lifecycle.md#feed-pictures), [pools](pictures.md#picture-pools) |
| `vmaf_score_*`, `vmaf_feature_score_*` | [Read scores](lifecycle.md#read-scores) |
| `VmafPoolingMethod` | [Pooling methods](lifecycle.md#vmafpoolingmethod) |
| `vmaf_write_output[_with_format]` | [Write a report](lifecycle.md#write-a-report) |
| `vmaf_context_get_backend`, `vmaf_feature_backend_twin`, `vmaf_registered_feature_extractor` | [Backend introspection](lifecycle.md#backend-introspection), [device twins](lifecycle.md#device-twins-and-the-extractors-that-ran) |
| `VmafPicture`, `vmaf_picture_alloc`, `vmaf_picture_unref`, `VmafPicture2` | [Pictures](pictures.md) |
| `VmafModel`, `vmaf_model_load*`, `vmaf_default_model_version`, `VmafModelKind` | [Models](models-and-features.md) |
| `VmafFeatureDictionary` | [Feature options](models-and-features.md#feature-options-vmaffeaturedictionary) |

### VmafPoolingMethod

The pooling methods and which of them the reports carry are described under
[pooling methods](lifecycle.md#vmafpoolingmethod).

### VmafFeatureDictionary

Feature options and their ownership rules are described under
[feature options](models-and-features.md#feature-options-vmaffeaturedictionary).

### Scoring before the flush and index gaps

`-EAGAIN` before the flush, and what an index that jumps ahead does, are
described under
[scoring before the flush and index gaps](lifecycle.md#scoring-before-the-flush-and-index-gaps).

## What each header exposes

| Header | Symbols | Purpose |
| --- | --- | --- |
| [`libvmaf.h`](../../core/include/libvmaf/libvmaf.h) | `VmafContext`, `VmafConfiguration`, lifecycle and scoring functions | Main entry point; includes `feature.h`, `model.h`, `picture.h`. |
| [`picture.h`](../../core/include/libvmaf/picture.h) | `VmafPicture`, `VmafPixelFormat`, alloc and unref | Per-frame pixel container. [Pictures](pictures.md). |
| [`picture_v2.h`](../../core/include/libvmaf/picture_v2.h) | `VmafPicture2`, `VmafBackendHandle`, converters | Picture with explicit backend state. [Pictures](pictures.md#picture-v2-picture_v2h). |
| [`feature.h`](../../core/include/libvmaf/feature.h) | `VmafFeatureDictionary` | Options for a feature extractor. [Models and features](models-and-features.md). |
| [`model.h`](../../core/include/libvmaf/model.h) | `VmafModel`, `VmafModelConfig`, `VmafModelCollection*`, `VmafModelKind` | SVM model, bootstrap collection, default version. |
| [`perceptual_weight.h`](../../core/include/libvmaf/perceptual_weight.h) | perceptual weight reader | Pelorus-driven pooling weights. [Perceptual weighting](perceptual-weight.md). |
| [`dnn.h`](../../core/include/libvmaf/dnn.h) | `VmafDnnSession`, `VmafDnnConfig`, tiny-model attach | Tiny-AI (ONNX Runtime). [DNN](dnn.md). Installed when `enable_dnn` is `enabled` or `auto`. |
| [`libvmaf_cuda.h`](../../core/include/libvmaf/libvmaf_cuda.h) | `VmafCudaState`, CUDA picture preallocation | CUDA backend; needs `-Denable_cuda=true`. [GPU](gpu.md#cuda). |
| [`libvmaf_sycl.h`](../../core/include/libvmaf/libvmaf_sycl.h) | `VmafSyclState`, frame buffers, dmabuf, VA and D3D11 import | SYCL backend; needs `-Denable_sycl=true`. [GPU](gpu.md#sycl). |
| [`libvmaf_hip.h`](../../core/include/libvmaf/libvmaf_hip.h) | `VmafHipState`, lifecycle, device listing | AMD HIP/ROCm backend; needs `-Denable_hip=true`. [GPU](gpu.md#hip). |
| [`libvmaf_metal.h`](../../core/include/libvmaf/libvmaf_metal.h) | `VmafMetalState`, lifecycle, IOSurface import | Apple Metal backend: runtime, IOSurface import and 17 registered feature extractors on Apple Silicon with `-Denable_metal=auto` or `enabled`; unsupported hosts return `-ENODEV`. [GPU](gpu.md#metal). |
| [`libvmaf_mcp.h`](../../core/include/libvmaf/libvmaf_mcp.h) | `VmafMcpServer`, `VmafMcpConfig`, transport start and stop | Embedded MCP server; needs `-Denable_mcp=true`. [MCP](mcp.md). |
| [`macros.h`](../../core/include/libvmaf/macros.h) | `VMAF_EXPORT` | Symbol visibility; always installed, included by every header. |
| `version.h` (generated) | `VMAF_API_VERSION_*` | Compile-time version constants. At run time use `vmaf_version()`. |
| [`vmaf_assert.h`](../../core/include/libvmaf/vmaf_assert.h) | `VMAF_ASSERT*` | Internal assertion helpers. Not installed; not public. |

All declarations are C with `extern "C"` guards for C++ callers. The public
API has no C++ entry points. The Vulkan backend and its header were removed
([ADR-0726](../adr/0726-drop-vulkan-backend.md)).

## Compile and link

```c
#include <libvmaf/libvmaf.h>
#include <libvmaf/picture.h>
#include <libvmaf/model.h>
```

```shell
cc app.c -o app $(pkg-config --cflags --libs libvmaf)
```

`pkg-config` is the supported way to pick up the include path and `-lvmaf`.
Add `--static` to also list the private link libraries of the enabled
backends and of ONNX Runtime. Backend headers are installed only for the
backends the build enabled (see [GPU
backends](gpu.md#when-these-headers-apply)).

## ABI stability

| Surface | Status |
| --- | --- |
| `libvmaf.h`, `picture.h`, `feature.h`, `model.h` | Stable. Upstream-origin functions keep their upstream signatures; fork-added functions (marked "fork-added" in the topic pages) are additive. |
| `dnn.h` (`vmaf_dnn_available`, `vmaf_use_tiny_model`, the session API) | Stable, fork-added. Structs may grow trailing fields across minor versions; do not over-read them. |
| `picture_v2.h` | Fork-added, additive; v1 `VmafPicture` stays supported during the dual-API window ([ADR-0928](../adr/0928-vmaf-picture-v2-explicit-backend-state.md)). |
| `libvmaf_sycl.h` zero-copy imports (`vmaf_sycl_import_va_surface`, `vmaf_sycl_import_d3d11_surface`, dmabuf entry points) | Experimental. Signatures may change as backends are added. |
| `vmaf_assert.h`, `VMAF_ASSERT*` | Private; not installed. |

Versioning follows the VMAFx `vX.Y.Z` stream
([ADR-1127](../adr/1127-single-semver-release-stream.md)). Any change that
breaks source or binary compatibility of the stable API bumps the major
version.

## Thread-safety

- A `VmafContext` is not re-entrant. Drive one context's lifecycle (init,
  feed, score, close) from one thread.
- Internally libvmaf runs feature extraction on `VmafConfiguration.n_threads`
  workers; that threading is self-contained.
- Several contexts may run in parallel on different threads. They share no
  state beyond process-global constants.
- Picture buffers (`VmafPicture.data[]`) are safe to mutate or free only
  after `vmaf_picture_unref()` brings the refcount to zero. See
  [pictures](pictures.md).

## Error semantics

Every non-void function returns `int`: `0` on success, a negative number
on error. The magnitude is a POSIX `errno` value.

| Code | Meaning |
| --- | --- |
| `-EINVAL` | Bad argument: NULL pointer, out-of-range enum, wrong shape, a non-increasing picture index, an unknown name. |
| `-EAGAIN` | A requested score is not written yet. Returned by the score functions before the flush. Not fatal: flush, or retry. See [scoring before the flush](lifecycle.md#scoring-before-the-flush-and-index-gaps). |
| `-ENOMEM` | Allocation failed. |
| `-ENOENT` | File not found (`vmaf_model_load_from_path` and similar), or the end of an enumeration. |
| `-ENOSYS` | Entry point compiled out, for example `vmaf_dnn_*` in a `-Denable_dnn=disabled` build. |
| `-ENODEV` | No device or backend available (twin lookup, Metal on an unsupported host). |
| `-ENOTSUP` | The device twin cannot honour an option or geometry. |
| `-EIO` | Downstream library error (ONNX Runtime, libav, and so on). |

libvmaf keeps no thread-local last-error: the return code is the only error
channel. A parallel diagnostic goes to the logger at
`VmafConfiguration.log_level`.

### CLI exit codes

The `vmaf` CLI does not map errno values to exit codes. It returns:

| Exit code | Meaning |
| --- | --- |
| `0` | Success |
| `100` | An explicitly requested backend failed to initialise |
| `101` | No frames were decoded |
| `102` | An input stream failed to read |
| `255` | Any other error (the CLI returns -1) |

Call the C API directly when you need fine-grained error discrimination.

## Path encoding

All filesystem paths accepted by VMAFx-owned entry points
(`vmaf_write_output`, `vmaf_write_output_with_format`,
`vmaf_model_load_from_path`, `vmaf_model_collection_load_from_path` and the
model reader helpers) are UTF-8 strings on every platform.

| Platform | Behaviour |
| --- | --- |
| POSIX (Linux, macOS, BSD) | Passed unchanged to `open` and `fopen`, which treat paths as raw bytes. |
| Windows | Decoded with `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)` and passed to `_wopen` and `_wfopen`. Non-ASCII paths resolve regardless of the active code page (`GetACP()`). Invalid UTF-8 fails with `errno = EILSEQ` (or `-EINVAL`). |

The vendored Pelorus entry point `pel_x265_csv_parse()` is a temporary
exception: its pinned upstream source still uses the Windows narrow CRT. It
is tracked in `docs/state.md` and must be fixed in `VMAFx/pelorus` before
re-vendoring under the ADR-1113 mirror invariant. Background:
[ADR-1182](../adr/1182-windows-utf8-path-contract.md).

## Doxygen reference

For browsable per-symbol HTML, run the standalone Doxygen build for the
public headers. It is separate from the full-tree generator so the warning
bar stays tight on the installable headers.

```bash
sudo apt-get install -y --no-install-recommends doxygen   # one-off
mkdir -p build/doxygen-public-api
doxygen core/doc/Doxyfile.public-api
open build/doxygen-public-api/html/index.html
```

The `doxygen-public-api` workflow runs the same command on every PR that
touches `core/include/libvmaf/` or the Doxyfile, gates the merge through
`required-aggregator.yml` with `DOXYGEN_WARNING_CEILING: "0"`, and publishes
the HTML and the warning log as artifacts. The build is warning-clean and
fails closed (`WARN_AS_ERROR = YES`); see
[ADR-0953](../adr/0953-doxygen-public-api-clean.md) and
[ADR-1315](../adr/1315-doxygen-public-api-fail-closed.md).

## Related

- [CLI walkthrough](../usage/cli.md): mirrors this API one to one.
- [Feature names and options](../metrics/features.md).
- [ADR-0119](../adr/0119-cli-precision-default-revert.md): the `%.6f`
  default (supersedes [ADR-0006](../adr/0006-cli-precision-17g-default.md)).
- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md): the
  documentation rule these pages satisfy.
- [ADR-1182](../adr/1182-windows-utf8-path-contract.md): Windows UTF-8 path
  contract.
