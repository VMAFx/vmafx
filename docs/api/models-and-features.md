# Models and feature options

!!! warning "Deprecated: `libvmaf.h` compatibility API"
    This page documents the compatibility library `libvmaf.so.3`. New code
    uses the [VMAFx API](vmafx/index.md); the [migration table](vmafx/compat.md)
    names the successor of every function.

Use this page to load a VMAF model, register feature extractors, pass
extractor options and read the default model. Declarations are in
[`model.h`](../../core/include/libvmaf/model.h) and
[`feature.h`](../../core/include/libvmaf/feature.h). The call sequence that
uses them is on the [lifecycle page](lifecycle.md).

## Load a model

```c
VmafModel *model = NULL;
VmafModelConfig cfg = { .name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT };
int err = vmaf_model_load(&model, &cfg, vmaf_default_model_version());
if (err < 0) { /* -EINVAL unknown version, -ENOMEM */ }
/* ... use it ... then, after vmaf_close(ctx) returned 0: */
vmaf_model_destroy(model);
```

| Function | Does | Errors |
| --- | --- | --- |
| `int vmaf_model_load(VmafModel **model, VmafModelConfig *cfg, const char *version)` | Loads a built-in model by version string. No filesystem access. | `-EINVAL` (unknown version, logged), `-ENOMEM` |
| `int vmaf_model_load_from_path(VmafModel **model, VmafModelConfig *cfg, const char *path)` | Loads a `.json` model file (UTF-8 path, see [path encoding](index.md#path-encoding)). SVM models only; `.pkl` is rejected. ONNX tiny-AI models use `vmaf_use_tiny_model()` ([DNN page](dnn.md)). | `-ENOENT` (no such file), `-EINVAL` (malformed JSON, `.pkl`), `-ENOMEM` |
| `void vmaf_model_destroy(VmafModel *model)` | Frees the model. `NULL` is a no-op. | none |
| `unsigned vmaf_model_feature_count(const VmafModel *model)` | Number of features the model needs; 0 for `NULL`. | none |
| `const char *vmaf_model_feature_name(const VmafModel *model, unsigned index)` | Name of feature `index`, borrowed from the model; `NULL` if `model` is `NULL` or `index` is out of range. | none |
| `const void *vmaf_model_version_next(const void *prev, const char **version)` | Cursor over built-in version strings. | none |
| `const char *vmaf_default_model_version(void)` | Version used when no model is named. | none |

Ownership, thread-safety and ABI for all of them:

- The caller owns the `VmafModel` and releases it with
  `vmaf_model_destroy()`. A model registered with a context
  (`vmaf_use_features_from_model()`) is reference-counted: the context takes
  its own reference, so you may destroy your model right after the call
  succeeds (before or after `vmaf_close()`, across retried closes too) and the
  model is freed when the last reference goes. A failed registration takes no
  reference. Before this change the context only borrowed the model and it had
  to outlive `vmaf_close()` (ADR-1755); code written for that contract keeps
  working.
- A model handed to a `VmafModelCollection` belongs to the collection; do
  not destroy it separately.
- `VmafModelConfig.name` is copied; the caller may free its string after the
  call. `NULL` means `"vmaf"` for built-in models and the file's
  `model_name` for path-loaded ones.
- Nothing here is thread-safe except `vmaf_default_model_version()`: use one
  context and its models per thread.
- `vmaf_model_load*()` and `vmaf_model_destroy()` are upstream, stable.
  `vmaf_default_model_version()` and the feature-name accessors are
  fork-added, additive.

### Model flags

```c
typedef struct VmafModelConfig {
    const char *name;    /* display name in the report, or NULL */
    uint64_t    flags;   /* OR of VmafModelFlags */
} VmafModelConfig;

enum VmafModelFlags {
    VMAF_MODEL_FLAGS_DEFAULT          = 0,
    VMAF_MODEL_FLAG_DISABLE_CLIP      = (1 << 0),  /* no [0,100] clamp */
    VMAF_MODEL_FLAG_ENABLE_TRANSFORM  = (1 << 1),
    VMAF_MODEL_FLAG_DISABLE_TRANSFORM = (1 << 2),
};
```

`VMAF_MODEL_FLAGS_DEFAULT` honours the model file's own settings.

### Built-in versions

`vmaf_model_load()` accepts the versions compiled into the library. The set
depends on the build's `VMAF_BUILT_IN_MODELS` and `VMAF_FLOAT_FEATURES`
flags, so discover it instead of hard-coding it:

```c
const void *handle = NULL;
const char *name   = NULL;
while ((handle = vmaf_model_version_next(handle, &name)) != NULL)
    printf("built-in model: %s\n", name);
```

The cursor rules:

1. Pass `NULL` as `prev` on the first call, then the previous return value.
2. Stop when it returns `NULL`. `*version` is left unmodified at the end,
   so the caller's last value stays valid.
3. Pass `version == NULL` if you only need the count.
4. It returns `NULL` at once when the library has no built-in models
   ([ADR-0135](../adr/0135-port-netflix-1424-expose-builtin-model-versions.md)).

Typical built-in names:

| Family | Versions |
| --- | --- |
| Default (fork) | `vmaf_v1.0.16_3d0h`, `vmaf_v1.0.16_3d0h_2160`, `vmaf_v1.0.16_5d0h`, `vmaf_v1.0.16_1d5h_2160`, and the `vmaf_v1.0.16_hfr_*` variants |
| Netflix-compatible | `vmaf_v0.6.1`, `vmaf_v0.6.1neg`, `vmaf_v0.6.1still`, `vmaf_b_v0.6.3`, `vmaf_4k_v0.6.1`, `vmaf_4k_v0.6.1neg` |
| Float-precision variants | `vmaf_float_*` equivalents |

See [the CLI models section](../usage/cli.md#models) for when to pick which.
`vmaf_v0.6.1still` is `vmaf_v0.6.1` with motion forced to 0
(`motion_force_zero`), for still images and single-frame scoring; it
comes from upstream Netflix/vmaf PR 1391.

External JSON models loaded by `vmaf_model_load_from_path()` allocate
`feature_names`, `slopes`, `intercepts`, `feature_opts_dicts` and the
piecewise-linear score-transform `knots` from the JSON payload. There is no
fixed feature or knot ceiling beyond memory and the unsigned parser
counters. A malformed array entry fails closed with a negative errno.

### Inspect a model's features

```c
const unsigned n = vmaf_model_feature_count(model);
for (unsigned i = 0; i < n; i++)
    printf("feature %u: %s\n", i, vmaf_model_feature_name(model, i));
```

The returned name pointer is valid for the lifetime of `model`.

## The default model

When a caller names no model, libvmaf scores with
**`vmaf_v1.0.16_3d0h`** ([ADR-1169](../adr/1169-default-model-v1-0-16.md)).
Read it instead of assuming it:

```c
const char *dflt = vmaf_default_model_version();   /* "vmaf_v1.0.16_3d0h" */
```

The returned string is owned by libvmaf. It is never `NULL`, must not be
freed and stays valid for the life of the process. The call is thread-safe
and allocates nothing.

The default changed in 1.0.0. It was `vmaf_v0.6.1` in every earlier build
of this fork, and upstream Netflix still defaults to `vmaf_v0.6.1`. To keep
the old behaviour name the model explicitly:

```c
err = vmaf_model_load(&model, &cfg, "vmaf_v0.6.1");
```

!!! warning
    The two families emit different feature keys. `vmaf_v0.6.1` reports
    `vif_scale0..3` and `motion2`; the v1.0.16 family emits neither. Code
    that reads individual feature keys, rather than only the pooled `vmaf`
    score, sees a missing key, not a shifted number.

Related constants and rules:

| Name | Value and use |
| --- | --- |
| `VMAF_DEFAULT_MODEL_VERSION` | Macro with the same string, fixed at compile time. Prefer the function in language bindings so the value comes from the loaded library. |
| `VMAF_NETFLIX_COMPAT_MODEL_VERSION` | `"vmaf_v0.6.1"`, the model the CLI selects under `--netflix-compat` ([CLI page](../usage/vmafx-cli.md)). Use it where you want Netflix parity. |
| NEG default | Stays on `vmaf_v0.6.1neg`. There is no v1.0.16 NEG model; `vmaf_v1.0.16_3d0hneg` does not exist and fails to load. |
| AOM CTC preset | Keeps `vmaf_v0.6.1`, because the CTC spec mandates it. |

The default has one source of truth: nothing else in the tree hard-codes a
fallback model name, and `scripts/ci/check-default-model-single-source.sh`
fails the build if something starts to. See
[ADR-1168](../adr/1168-default-model-single-source.md) and
[changing the default](../development/default-model.md).

## Model kinds

The fork adds model-kind discrimination, `enum VmafModelKind` in `model.h`,
auto-detected from the file extension and a sidecar JSON
([ADR-0020](../adr/0020-tinyai-four-capabilities.md),
[ADR-0022](../adr/0022-inference-runtime-onnx.md)):

| Kind | Meaning |
| --- | --- |
| `VMAF_MODEL_KIND_SVM` | Classic SVM model. |
| `VMAF_MODEL_KIND_DNN_FR` | Full-reference tiny-AI model. |
| `VMAF_MODEL_KIND_DNN_NR` | No-reference tiny-AI model. |
| `VMAF_MODEL_KIND_DNN_FILTER` | Pre- and post-processing residual filter ([ADR-0168](../adr/0168-tinyai-konvid-baselines.md)). |

`VMAF_MODEL_KIND_DNN_FILTER` is registry-only. It identifies filters such as
`learned_filter_v1.onnx` (consumed by ffmpeg `vmafx_pre`) for the trust-root
sha256 audit and is never loaded by the scoring path.
`vmaf_score_at_index()` and `vmaf_score_pooled()` operate on `SVM`,
`DNN_FR` and `DNN_NR` only.

## Model collections (bootstrap)

A collection bundles bagged sub-models and returns a mean, a standard
deviation and a 95% confidence interval.

```c
int vmaf_model_collection_load(VmafModel **model, VmafModelCollection **coll,
                               VmafModelConfig *cfg, const char *version);
int vmaf_model_collection_load_from_path(VmafModel **model,
                                         VmafModelCollection **coll,
                                         VmafModelConfig *cfg,
                                         const char *path);
int vmaf_model_collection_feature_overload(VmafModel *model,
                                           VmafModelCollection **coll,
                                           const char *feature_name,
                                           VmafFeatureDictionary *opts_dict);
void vmaf_model_collection_destroy(VmafModelCollection *coll);
```

- `model` receives the lead sub-model, owned by the collection: do not
  pass it to `vmaf_model_destroy()`.
- Destroy the collection with `vmaf_model_collection_destroy()`, after
  `vmaf_close()` returned 0.
- Errors: `-EINVAL` (unknown version), `-ENOENT` (path), `-ENOMEM`.
- All four are upstream, stable.

Scores come back as `VmafModelCollectionScore`:

```c
typedef struct VmafModelCollectionScore {
    enum VmafModelCollectionScoreType type;  /* BOOTSTRAP for bootstrap models */
    struct {
        double bagging_score;   /* mean across sub-models */
        double stddev;          /* standard deviation across sub-models */
        struct { struct { double lo, hi; } p95; } ci;  /* 95% CI */
    } bootstrap;
} VmafModelCollectionScore;
```

See [the confidence-interval page](../metrics/confidence-interval.md) for
what the interval means.

## Feature options: `VmafFeatureDictionary`

`VmafFeatureDictionary` is an opaque string-to-string map that carries
per-extractor options. Each extractor publishes its own keys; the full table
is on [the features page](../metrics/features.md).

```c
VmafFeatureDictionary *opts = NULL;
int err = vmaf_feature_dictionary_set(&opts, "enable_chroma", "true");
if (err == 0)
    err = vmaf_feature_dictionary_set(&opts, "enable_apsnr", "true");
if (err == 0)
    err = vmaf_use_feature(ctx, "psnr", opts);   /* consumes opts, see below */
```

| Function | Does |
| --- | --- |
| `int vmaf_feature_dictionary_set(VmafFeatureDictionary **dict, const char *key, const char *val)` | Adds or replaces a key; allocates the dictionary when `*dict` is `NULL`. Copies `key` and `val`. Numeric values are normalised, so `"1"` and `" 1.0 "` compare equal. Returns 0, `-EINVAL` (NULL or empty argument) or `-ENOMEM`; on failure the caller still owns `*dict`. |
| `int vmaf_feature_dictionary_free(VmafFeatureDictionary **dict)` | Frees a dictionary you still own and sets `*dict` to `NULL`; a `NULL` handle is a no-op. |

Both are upstream, stable, and not thread-safe.

### Who frees the dictionary

Three calls accept a dictionary: `vmaf_use_feature()`,
`vmaf_model_feature_overload()` and
`vmaf_model_collection_feature_overload()`. They share one rule.

!!! warning
    A `-EINVAL` caused by a `NULL` argument takes nothing: the caller still
    owns the dictionary. On every other return, success or failure
    (including `-ENOMEM` from the internal merge or copy), the call has
    already released it and the caller must not free it.

`vmaf_use_feature()` adds one case. It also takes nothing when
`feature_name` names no registered feature: it resolves the name against the
global extractor registry and returns `-EINVAL` before touching the
dictionary.

The two model overloads differ. They match `feature_name` against the
features of one particular model. A name that matches nothing there is a
successful no-op that returns `0`, and the dictionary is consumed anyway.
Only `vmaf_use_feature()` can report an unknown name and hand the dictionary
back.

An invalid option value for a known extractor also returns `-EINVAL` but
consumes the dictionary, so the error number alone does not tell you which
rule applied. Free the dictionary yourself only after an early argument
rejection or after `vmaf_use_feature()` rejects an unknown name.

```c
/* Consumed on success: freeing here would be a double free. */
if (vmaf_use_feature(ctx, "psnr", opts) == 0)
    opts = NULL;

/* NOT consumed: the name was rejected before anything was taken. */
if (vmaf_use_feature(ctx, "no_such_feature", opts2) == -EINVAL)
    vmaf_feature_dictionary_free(&opts2);

/* CONSUMED although it returned 0: the model has no "psnr" feature, which
 * is a no-op, not an error. Freeing opts3 here is a double free. */
if (vmaf_model_feature_overload(model, "psnr", opts3) == 0)
    opts3 = NULL;
```

`vmaf_use_features_from_model()` borrows the model's option dictionaries
and registers private copies. A rejected option or a failed copy releases
the failed private copy and leaves the model available for correction or
retry. Registration is not transactional: features registered before a
later error stay registered. Worker-context creation follows the same
private-copy cleanup. Valid feature scores do not change. See
[the ownership regression](../research/2048-model-registration-ownership-2026-09-08.md)
for the fault controls and their platform limits.

## Feature registration identity

`vmaf_use_feature()` and `vmaf_use_features_from_model()` deduplicate by the
emitted feature key, including parsed feature parameters:

- A default motion extractor and one with `motion_force_zero=true` both stay
  registered. Their scores use separate keys, such as
  `VMAF_integer_feature_motion2_score` and `integer_motion2_force_0`.
- Equivalent defaults, canonical option names and aliases share one
  registration.
- Equivalent CPU and GPU twins keep the first registered context.

Check every registration return value. If allocating comparison keys or
growing registration storage fails, the call returns `-ENOMEM` and keeps the
contexts registered earlier.

## History

- Before [ADR-1166](../adr/1166-upstream-issue-harvest.md) the
  ownership contract was documented two ways: `<libvmaf/feature.h>` said the
  caller kept ownership on any failure, `<libvmaf/model.h>` said ownership
  transferred unconditionally, so one reading was a latent double free
  ([Netflix/vmaf#1242](https://github.com/Netflix/vmaf/issues/1242)). All
  three headers now state the rule above, which matches what the
  implementation always did. The same report's `-ENOMEM` leak in
  `vmaf_model_feature_overload()` and the swallowed copy error in
  `vmaf_model_collection_feature_overload()` were fixed in that change.
