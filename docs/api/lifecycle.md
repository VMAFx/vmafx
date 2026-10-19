# Context lifecycle: init, score, close

Use this page for the order of calls that scores a video pair with libvmaf
and for the contract of each call: ownership, errors, thread-safety and ABI
status. A complete program is on the [overview page](index.md#minimal-program).
All declarations are in
[`libvmaf.h`](../../core/include/libvmaf/libvmaf.h).

Every function below shares these rules unless its row says otherwise:

- **Thread-safety:** not thread-safe. Drive one `VmafContext` from one
  thread. `vmaf_version()` is the exception and is safe from any thread.
- **Errors:** return `0` on success and a negative `errno` on failure; see
  [error semantics](index.md#error-semantics).
- **ABI:** stable. Entry points marked "fork-added" are additive over
  upstream Netflix/vmaf and keep their signatures across minor releases.

## Call order

```text
  vmaf_init()                         -> VmafContext*
      |
  vmaf_model_load[_from_path]()       -> VmafModel*
  vmaf_use_features_from_model()      register the extractors a model needs
  vmaf_use_feature()                  optional extra extractors
      |
  loop per frame pair:
    vmaf_picture_alloc() x2           (or fetch from a pool)
    fill the planes
    vmaf_read_pictures(ref, dist, i)  the context takes both pictures
  vmaf_read_pictures(NULL, NULL, 0)   flush
      |
  vmaf_score_pooled() / vmaf_score_at_index()
  vmaf_feature_score_pooled()
  vmaf_write_output[_with_format]()
      |
  vmaf_close()   retry on nonzero; on exact 0, vmaf_model_destroy()
```

The context owns a reference to every model registered with
`vmaf_use_features_from_model()` (or a model collection): destroy your own
model, or the collection, whenever you like after the call succeeds. This holds
across nonzero `vmaf_close()` results too. Imported backend states are still
borrowed: keep them alive through every nonzero `vmaf_close()` result and free
them only after it returns exactly 0.

## Configuration: `VmafConfiguration`

```c
typedef struct VmafConfiguration {
    enum VmafLogLevel log_level;  /* NONE | ERROR | WARNING | INFO | DEBUG */
    unsigned n_threads;           /* worker threads; 0 = library default */
    unsigned n_subsample;         /* score every Nth frame; 0 or 1 = all */
    uint64_t cpumask;             /* disable CPU instruction sets */
    uint64_t gpumask;             /* any non-zero value disables CUDA and SYCL */
} VmafConfiguration;
```

Diagnostics go through the logger at `log_level`; they are not returned.

`cpumask` has the same meaning as the `--cpumask` CLI flag. Only the bits of
the host architecture take effect; bits for another architecture are
ignored.

| Bit | x86 / x86-64: disables | arm64: disables |
| --- | --- | --- |
| 1 | SSE2 | NEON (forces scalar) |
| 2 | SSE3 / SSSE3 | SVE2 |
| 4 | SSE4.1 | |
| 8 | AVX2 | |
| 16 | AVX512 | |
| 32 | AVX512ICL | |

!!! warning "`gpumask` is a boolean"
    Despite the `uint64_t` type, any non-zero value disables the GPU
    feature-extractor selection for both CUDA and SYCL, and libvmaf falls
    back to the CPU. There is no per-backend bit; use `--no_cuda` or
    `--no_sycl` on the CLI. HIP and Metal are not gated by `gpumask`: they
    are active only when a state was imported with
    `vmaf_hip_import_state()` or `vmaf_metal_import_state()`.
    The check is in `core/src/libvmaf.c` (`gpumask`, ADR-0530).

!!! warning "Even `n_subsample`"
    An even `n_subsample` can give inaccurate motion scores because the
    motion feature is frame-delta based. Prefer 1 or an odd integer. See
    [upstream issue #1214](https://github.com/Netflix/vmaf/issues/1214).

## Open and close

| Function | Does | Ownership and errors |
| --- | --- | --- |
| `int vmaf_init(VmafContext **out, VmafConfiguration cfg)` | Allocates a context. | `*out` is output-only: its incoming value is never read, so an uninitialised pointer is fine, and it is `NULL` after any failure. A handle that still holds an open context is overwritten, not closed: close it first ([ADR-1396](../adr/1396-vmaf-init-output-only-handle.md)). `-EINVAL` if `out` is `NULL`, otherwise the failing set-up step's errno. |
| `int vmaf_close(VmafContext *ctx)` | Closes registered, pooled and worker-private extractor contexts, then frees the context. | See [close and retry](#close-and-retry). `-EINVAL` for `NULL`. |
| `const char *vmaf_version(void)` | Version string, `vX.Y.Z` plus git sha. Needs no context. | Owned by the library, valid for the process lifetime, never freed. Safe from any thread. |

### Close and retry

`vmaf_close()` uses a prepare/commit teardown. It first closes the
registered, pooled and worker-private extractor contexts without freeing
their owners. If any close callback fails it returns a negative errno and
retains the context, which is then teardown-only.

1. Check the result. Only an exact `0` means the context is freed.
2. On any nonzero result do not call scoring functions and do not release
   imported GPU states or models.
3. Call `vmaf_close()` again with the same pointer.
4. Set the pointer to `NULL` and release the dependencies only after it
   returns `0`.

```c
int err = vmaf_close(ctx);
if (err != 0)
    err = vmaf_close(ctx);   /* retry the retained teardown-only context */
if (err == 0)
    ctx = NULL;              /* now release models and backend states */
```

Passing the pointer again after a successful close is undefined behaviour
and is not detected. The safe Rust wrappers enforce this rule in types: see
[Rust context close and retry](rust-context-close.md).

## Register features

| Function | Does | Ownership and errors |
| --- | --- | --- |
| `int vmaf_use_features_from_model(ctx, model)` | Registers every extractor the model needs and picks the imported backend's twin of each where it exists. Deduplicates across models. | The model is borrowed until `vmaf_close()` returns 0. `-EINVAL` when an extractor reads frame `n-2` and a pool below 4 pictures was preallocated. |
| `int vmaf_use_features_from_model_collection(ctx, coll)` | Same for a bootstrap collection. | The collection is borrowed until close succeeds. |
| `int vmaf_use_feature(ctx, "psnr", opts)` | Registers one extractor by its exact name, with optional options. | The context takes `opts` on every path except an argument rejection or an unknown name; see [who frees the dictionary](models-and-features.md#who-frees-the-dictionary). `-EINVAL` for an unknown name or the five-frame pool rule. |
| `int vmaf_import_feature_score(ctx, name, value, index)` | Injects a precomputed feature value for picture `index`, for example from another pipeline or a feature without an extractor. | Value copied. |

Registration is deduplicated by emitted key; see
[feature registration identity](models-and-features.md#feature-registration-identity).
Model and option handling is on [the models page](models-and-features.md).

## Feed pictures

```c
int vmaf_read_pictures(VmafContext *ctx, VmafPicture *ref,
                       VmafPicture *dist, unsigned index);
```

`vmaf_read_pictures()` queues one frame pair. Register extractors first.

- **Ownership:** the context owns `ref` and `dist` whatever it returns. Do
  not unref them, even after an error. Only a `NULL` context or one `NULL`
  picture takes nothing (`-EINVAL`). Details:
  [pictures](pictures.md#the-rule-in-one-paragraph)
  ([ADR-1431](../adr/1431-read-pictures-owns-pictures-on-every-return.md)).
- **Index:** strictly increasing, starting at 0, no gaps. A repeated or
  smaller index returns `-EINVAL`
  ([ADR-0152](../adr/0152-vmaf-read-pictures-monotonic-index.md)).
- **Bit depth:** the float extractors `float_ssim`, `float_ms_ssim`,
  `float_adm`, `float_vif` and `float_motion` (and their GPU twins) take 8, 10,
  12 or 16-bit pictures. At any other depth the call returns `-EINVAL` and logs
  `<extractor>: picture bit depth N is not supported`; before, they read 9, 11,
  13, 14 and 15-bit samples as 8-bit bytes and returned a wrong score without
  an error. Extractors that score odd depths, such as `psnr_hvs` at 9 and 11
  bits, are unaffected.
- **Flush:** call `vmaf_read_pictures(ctx, NULL, NULL, 0)` after the last
  frame so every extractor completes.

Pools for the pictures: `vmaf_preallocate_pictures()` and
`vmaf_fetch_preallocated_picture()` are documented on the
[pictures page](pictures.md#picture-pools).

## Read scores

| Function | Returns |
| --- | --- |
| `vmaf_score_at_index(ctx, model, &score, index)` | Per-frame VMAF score. |
| `vmaf_score_at_index_model_collection(ctx, coll, &score, index)` | Per-frame bootstrap score: mean, standard deviation, 95% interval. |
| `vmaf_feature_score_at_index(ctx, "psnr_y", &score, index)` | Per-frame feature score. |
| `vmaf_score_pooled(ctx, model, method, &score, lo, hi)` | Pooled VMAF over `[lo, hi]`. |
| `vmaf_score_pooled_model_collection(ctx, coll, method, &score, lo, hi)` | Pooled bootstrap score. |
| `vmaf_feature_score_pooled(ctx, "psnr_y", method, &score, lo, hi)` | Pooled feature score. |

Outputs are written to `*score` and nothing is allocated. A call returns
the final value or an error, never a partial value. Before the flush it can
return `-EAGAIN`; see
[scoring before the flush](#scoring-before-the-flush-and-index-gaps).
`-EINVAL` covers invalid arguments and a feature name no registered
extractor writes.

```c
double mean = 0.0;
int err = vmaf_score_pooled(ctx, model, VMAF_POOL_METHOD_MEAN, &mean, 0, n - 1);
```

### `VmafPoolingMethod`

| Enumerator | Value | Pooled result | Memory |
| --- | --- | --- | --- |
| `VMAF_POOL_METHOD_UNKNOWN` | 0 | sentinel, rejected with `-EINVAL` | |
| `VMAF_POOL_METHOD_MIN` | 1 | minimum per-frame score | O(1) |
| `VMAF_POOL_METHOD_MAX` | 2 | maximum per-frame score | O(1) |
| `VMAF_POOL_METHOD_MEAN` | 3 | arithmetic mean | O(1) |
| `VMAF_POOL_METHOD_HARMONIC_MEAN` | 4 | harmonic mean of `score + 1`, minus 1 | O(1) |
| `VMAF_POOL_METHOD_MEDIAN` | 5 | 50th percentile | O(n), sorts |
| `VMAF_POOL_METHOD_PERC5` | 6 | 5th percentile ("worst 5%") | O(n), sorts |
| `VMAF_POOL_METHOD_PERC10` | 7 | 10th percentile | O(n), sorts |
| `VMAF_POOL_METHOD_PERC20` | 8 | 20th percentile | O(n), sorts |

The four order-statistic methods
([ADR-1188](../adr/1188-percentile-pooling-methods.md))
sort the pooled per-frame scores and interpolate linearly between
neighbouring ranks. This equals
`numpy.percentile(scores, q, method="linear")`, the rule the Python harness
applies through `ListStats.perc10` and friends, so both surfaces report the
same number for the same frames.

```c
double worst10 = 0.0;
int err = vmaf_score_pooled(ctx, model, VMAF_POOL_METHOD_PERC10, &worst10, 0, n - 1);
```

Notes and limits:

- **Weighting:** percentiles are pure order statistics, so ADR-1118
  perceptual spatial weighting does not change them, as for `MIN` and `MAX`.
  Only `MEAN` and `HARMONIC_MEAN` have weighted forms.
- **Subsampling:** `n_subsample` skips the same frames for every method.
- **Memory:** a percentile pool keeps `8 * n_frames` bytes and sorts them.
  Prefer a bounded `index_high` over `UINT_MAX` on long sequences.
- **Reports:** pooled output in XML and JSON still contains exactly `min`,
  `max`, `mean`, `harmonic_mean`; percentiles are available through the API
  calls only.
- **Append-only:** enumerator values are append-only. `VMAF_POOL_METHOD_NB`
  is a deprecated count sentinel, not a stable value; do not switch on it
  or persist it. `VMAF_HAVE_PERCENTILE_POOLING` is defined when the
  percentile enumerators exist, for code that builds against older headers.

### Scoring before the flush and index gaps

A query made while pictures are still being read returns the final value or
an error:

- `-EAGAIN`: a feature the score needs is not written yet. `motion2` and
  `motion3` of picture *i* need picture *i + 1*. GPU extractors finish a
  frame after the submitting call has returned (CUDA `motion` in batches of
  eight), and worker threads (`n_threads > 0`) finish pictures out of step
  with the caller.
- `-EINVAL`: invalid arguments, or a feature name no registered extractor
  writes.

Before returning either code for a picture already read, libvmaf waits for
the worker threads and, on CUDA, collects every frame the device has already
finished, then reads once more. A score a worker thread is still computing is
therefore returned, not refused, even for the newest picture: with worker
threads the collector may have no slot for that picture yet, and before
2026-10-06 `vmaf_feature_score_at_index()` and `vmaf_feature_score_pooled()`
answered `-EINVAL` at once in that case
(`core/test/test_feature_score_fed_frame.c`). What still needs a later
picture stays `-EAGAIN`: `motion2` and `motion3` of the newest picture, and a
CUDA batch the device has not finished. Treat `-EAGAIN` as "not yet": flush
and ask again, or ask again after more pictures. `vmaf_score_pooled()`
returns it when any picture of the interval is missing a score
([ADR-0154](../adr/0154-score-pooled-eagain-netflix-755.md)).

An index that skips values is accepted, because single-picture features
(`psnr`, `vif`, `adm`) do not care. The motion extractors compare each
picture with the one before it, so after a gap:

- `motion2` and `motion3` are never written for the pictures that follow;
- the picture before the gap gets the `motion2` the last picture of a
  stream gets;
- queries keep returning `-EAGAIN` after the flush.

Feed every index from 0 without gaps whenever the model uses `motion2`
(`vmaf_v0.6.1` and the models derived from it do). Measured on the Netflix
576x324 pair with indices 0, 1, 2, 4, 5: `vmaf` at indices 0 to 2 is
returned (index 2 with the last-picture `motion2`), and indices 3 to 5
return `-EAGAIN` after the flush.

## Write a report

```c
int vmaf_write_output(VmafContext *ctx, const char *path, enum VmafOutputFormat fmt);
int vmaf_write_output_with_format(VmafContext *ctx, const char *path,
                                  enum VmafOutputFormat fmt,
                                  const char *score_format);
```

| Argument | Meaning |
| --- | --- |
| `path` | Output file, UTF-8 (see [path encoding](index.md#path-encoding)). |
| `fmt` | `VMAF_OUTPUT_FORMAT_XML`, `_JSON`, `_CSV` or `_SUB` (SubRip cues, one per frame). `_NONE` is a sentinel and is rejected. |
| `score_format` | One printf format taking exactly one `double`. `NULL` selects `"%.6f"`, the Netflix-compatible default ([ADR-0119](../adr/0119-cli-precision-default-revert.md)). `"%.17g"` gives IEEE-754 round-trip output, which is what `--precision=max` does. The string is not validated and must stay valid during the call. |

`vmaf_write_output()` is upstream and always uses `"%.6f"`;
`vmaf_write_output_with_format()` is fork-added. Call both after the flush.

## Backend introspection

```c
enum VmafBackend backend;
int err = vmaf_context_get_backend(ctx, &backend);
```

Returns the backend imported into the context through a
`vmaf_<backend>_import_state()` call, or `VMAF_BACKEND_UNKNOWN` for a
CPU-only context. Fork-added
([ADR-0804](../adr/0804-vmaf-context-get-backend.md)).

| `enum VmafBackend` | Value | Meaning |
| --- | --- | --- |
| `VMAF_BACKEND_UNKNOWN` | 0 | CPU only, no GPU state imported |
| `VMAF_BACKEND_CUDA` | 1 | `vmaf_cuda_import_state()` |
| `VMAF_BACKEND_SYCL` | 2 | `vmaf_sycl_import_state()` |
| `VMAF_BACKEND_METAL` | 3 | `vmaf_metal_import_state()` |
| `VMAF_BACKEND_HIP` | 4 | `vmaf_hip_import_state()` |
| `VMAF_BACKEND_VULKAN` | 5 | reserved, Vulkan removed (ADR-0726) |

Errors: `-EINVAL` if `ctx` or `out` is `NULL`. New values may be appended;
treat unknown values as `VMAF_BACKEND_UNKNOWN` (use a `default:` branch).

## Device twins and the extractors that ran

`vmaf_use_feature()` registers exactly the extractor you name. A model's
features are different: `vmaf_use_features_from_model()` picks the imported
backend's twin of each one, keeps the CPU extractor when the twin cannot
honour the model's options, and swaps in the CPU extractor at the first
picture when the twin cannot run that geometry. Two fork-added functions
expose that choice to callers that register features by name, such as the
`vmaf` CLI ([ADR-1359](../adr/1359-cli-feature-backend-twin.md)).

### `vmaf_feature_backend_twin()`

```c
int vmaf_feature_backend_twin(VmafContext *vmaf, const char *feature_name,
                              const VmafFeatureDictionary *opts_dict,
                              const VmafPictureConfiguration *pic_cfg,
                              const char **twin_name,
                              const char **unsupported_option);
```

It asks which extractor of the imported backend computes what the CPU
extractor `feature_name` computes, and whether that twin can run with
`opts_dict` on pictures of `pic_cfg`'s geometry. Call it after the
backend's `vmaf_<backend>_import_state()`.

- Pass `NULL` for `pic_cfg` to skip the geometry check; `pic_cnt` is
  ignored.
- Nothing is registered and `opts_dict` is not consumed.
- `*twin_name` is static. `*unsupported_option` points into `opts_dict`, so
  read it before freeing the dictionary or passing it to
  `vmaf_use_feature()`.

| Return | Meaning | `*twin_name` | `*unsupported_option` |
| --- | --- | --- | --- |
| `0` | Use the twin: register `*twin_name` with `vmaf_use_feature()` | twin | `NULL` |
| `-ENOENT` | The backend has no twin of this extractor | `NULL` | `NULL` |
| `-ENOTSUP` | The twin cannot honour an option | twin | the option key |
| `-ENOTSUP` | The twin cannot run this geometry with these options | twin | `NULL` |
| `-ENODEV` | No backend imported, or a non-zero `gpumask` disables its extractors | `NULL` | `NULL` |
| `-EINVAL` | `vmaf`, `feature_name` or `twin_name` is `NULL`, or `feature_name` is not a registered CPU extractor (unknown name or a twin name) | `NULL` | `NULL` |

Another negative errno means an option value could not be parsed.

```c
const char *twin = NULL, *option = NULL;
const char *name = "ciede";
int err = vmaf_feature_backend_twin(vmaf, name, opts, &pic_cfg, &twin, &option);
if (err == 0)
    name = twin;                                  /* e.g. "ciede_sycl" */
else if (err == -ENOTSUP && option)
    fprintf(stderr, "%s cannot honour %s; using the CPU\n", twin, option);
err = vmaf_use_feature(vmaf, name, opts);         /* consumes opts */
```

### `vmaf_registered_feature_extractor()`

```c
int vmaf_registered_feature_extractor(VmafContext *vmaf, unsigned index,
                                      const char **name,
                                      enum VmafBackend *backend);
```

Reports the extractor registered at `index`, after duplicate registrations
were merged, and the backend it runs on. `VMAF_BACKEND_UNKNOWN` means the
CPU.

1. Call it after the final flush, so a model feature whose twin was replaced
   by the CPU extractor at the first picture shows as the CPU.
2. Walk `index` from `0` until it returns `-ENOENT`.
3. `name` is static storage; do not free it.

Returns `-EINVAL` when `vmaf`, `name` or `backend` is `NULL`. The CLI builds
its `backend_used` and `feature_backends` JSON keys from it
([backend receipt](../usage/cli.md#backend-receipt-in-json-output)).
