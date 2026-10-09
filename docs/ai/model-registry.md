<!-- markdownlint-disable MD060 -->
# Tiny-model registry — schema and verification

The registry at [`model/tiny/registry.json`](../../model/tiny/registry.json) is
the trust root of libvmaf's tiny-AI surface. Every ONNX model shipped under
`model/tiny/` is indexed there with a SHA-256 pin, license metadata and a
Sigstore bundle path. T6-9 / [ADR-0211](../adr/0211-model-registry-sigstore.md)
formalised the schema and wired `--tiny-model-verify` to `cosign verify-blob`.

The registry holds 26 entries. Thirteen are CI smoke fixtures with
`smoke: true`, see [CI-only smoke fixtures](#ci-only-smoke-fixtures).

## Registry shape

```jsonc
{
  "$schema": "./registry.schema.json",
  "schema_version": 1,
  "models": [
    {
      "id": "learned_filter_v1",          // stable id; a label, not a --tiny-model value
      "kind": "filter",                    // fr | nr | filter
      "onnx": "learned_filter_v1.onnx",
      "opset": 17,
      "sha256": "412d537…e27",
      "quant_mode": "dynamic",             // fp32 | dynamic | static | qat
      "int8_sha256": "1cff6fe…2d3",
      "quant_accuracy_budget_plcc": 0.01,
      "license": "BSD-2-Clause-Patent",
      "license_url": "https://github.com/VMAFx/vmafx/blob/master/NOTICE",
      "sigstore_bundle": "learned_filter_v1.onnx.sigstore.json",
      "description": "Tiny residual filter for vmaf_pre — degraded → clean luma.",
      "notes": "Self-supervised on KoNViD-1k …"
    }
  ]
}
```

!!! note
    `--tiny-model` takes a file path. The registry `id` is a stable name used by
    the validator, docs and tooling. The CLI does not resolve it to a file.

The full JSON Schema is
[`model/tiny/registry.schema.json`](../../model/tiny/registry.schema.json). The
schema rejects unknown properties (`additionalProperties: false`).

### Fields

| Field | Required | Meaning |
| --- | --- | --- |
| `id` | yes | Stable identifier, `^[a-z0-9][a-z0-9_-]*$`, at most 64 characters. |
| `kind` | yes | `fr`, `nr` or `filter`. |
| `onnx` | yes | ONNX path relative to `model/tiny/`. |
| `sha256` | yes | Lowercase hex, 64 characters, of the exact ONNX bytes. A mismatch against the on-disk file is a hard error. |
| `opset` | non-smoke | ONNX opset of the default domain, 7 to 21, as the graph's `opset_import` declares it; the sidecar `opset` carries the same value. The validator checks both against the file and its int8 sibling. |
| `smoke` | no | `true` for CI load-path probes, which are not quality models. |
| `quant_mode` | no | `fp32` (default), `dynamic`, `static` or `qat`. Any other value than `fp32` makes the runtime load `<onnx>.int8.onnx`; see [quantization.md](quantization.md). |
| `int8_sha256` | iff `quant_mode != "fp32"` | SHA-256 of the `.int8.onnx` file. The validator checks it; the runtime redirect to the int8 file does not ([quantization.md](quantization.md)). |
| `quant_calibration_set` | `static` only | Calibration tensor blob, relative to the repo root. |
| `quant_accuracy_budget_plcc` | no | Maximum PLCC drop versus fp32; the CI `ai-quant-accuracy` job fails a quantised model beyond it. Default 0.01. |
| `license` | schema version 1 | SPDX identifier. Fork-trained models use `BSD-2-Clause-Patent`, matching libvmaf. Upstream-derived models keep the upstream license verbatim (LPIPS-Sq is `BSD-2-Clause`). |
| `license_url` | no | URL of the license text. |
| `sigstore_bundle` | no | Path relative to `model/tiny/`, ending in `.sigstore.json`. |
| `description` | no | One-line summary, distinct from `notes`. |
| `notes` | no | Free-text provenance and training recipe, at most 512 characters. |
| `release_url` | no | HTTPS URL of a release attachment holding the ONNX bytes, for a model not tracked in git; `scripts/ai/fetch-tiny-blobs.sh` downloads it and checks `sha256`. No shipped entry sets it, see [tiny-blob-storage.md](tiny-blob-storage.md). |

The Sigstore bundle file is generated at release time by
[`.github/workflows/supply-chain.yml`](../../.github/workflows/supply-chain.yml).
Before a release the path is declared but the file may be absent. The runtime
verifier (`--tiny-model-verify`) treats an absent bundle as fail-closed.

## Validate the registry

[`ai/scripts/validate_model_registry.py`](../../ai/scripts/validate_model_registry.py)
runs the JSON Schema check and the cross-file consistency check: every ONNX
exists, every `sha256` matches, every non-smoke entry has a sidecar. It also
reads each graph (and its int8 sibling) with
[`ai/src/aiutils/onnx_signature.py`](../../ai/src/aiutils/onnx_signature.py),
which needs no `onnx` package, and fails when the registry `opset` or a
sidecar's `opset`, `sha256`, `input_names` / `output_names`, `input_name` /
`output_name`, feature-list length, `encoder_vocab`, `codec_block_dim` or
`codec_block_layout` disagrees with the graph. It is a CI gate; run it before
pushing:

```bash
python3 ai/scripts/validate_model_registry.py \
    --out-json runs/tiny_model_registry_validation.json
# → OK: 26 registry entries valid against registry.schema.json
```

To validate an off-tree registry, pass it and its schema explicitly:

```bash
python3 ai/scripts/validate_model_registry.py /path/to/other-registry.json \
    --schema /path/to/registry.schema.json \
    --out-json runs/offtree_registry_validation.json
```

Without `jsonschema` installed the validator falls back to a structural check,
so distros without `python-jsonschema` still get the required-field invariants.
Install `jsonschema` for full Draft 2020-12 coverage.

The optional `--out-json` report records the verdict, model count, errors,
registry and schema paths, and the ADR-0661 `run_provenance` block. Use it for
release evidence and for debugging failures without CI log scrollback.

## Verify a model at runtime

`--tiny-model-verify` makes the loader check the model's Sigstore bundle before
it opens the file:

```bash
vmaf -r ref.y4m -d dis.y4m \
     --tiny-model model/tiny/learned_filter_v1.onnx \
     --tiny-model-verify
```

The loader runs these steps:

1. Look up the model's basename in `model/tiny/registry.json` (alongside the
   `.onnx` by default).
2. Read the entry's `sigstore_bundle` path.
3. Read the bundle and check its one signing certificate: the identity must be
   the supply-chain workflow on a release tag (or `master`) and the owner ID
   VMAFx's `288567244` ([security.md](security.md#layer-4-signature-verification-opt-in)).
4. Spawn `cosign verify-blob` with `--bundle=<private copy>`,
   `--certificate-identity-regexp …`,
   `--certificate-oidc-issuer https://token.actions.githubusercontent.com` and
   the `.onnx` path, through `posix_spawnp(3p)` with an explicit argv array, no
   shell. The expression is anchored at both ends.
5. Refuse to load on a non-zero exit, a missing `cosign`, a missing bundle, a
   missing registry entry, or a certificate of another identity or owner.

The flag is off by default for dev-friendliness; production deployments should
set it. `cosign` must be on `$PATH`; install a prebuilt binary from the
[Sigstore release page](https://github.com/sigstore/cosign/releases). The C entry
point is `vmaf_dnn_verify_signature(onnx_path, registry_path)` in
[`core/include/libvmaf/dnn.h`](../../core/include/libvmaf/dnn.h); both arguments
are NULL-tolerant in the documented way.

### Directory jail

The registry pins model identity. The optional `VMAF_TINY_MODEL_DIR`
environment variable constrains model location. Set it on production hosts:

```bash
export VMAF_TINY_MODEL_DIR=/opt/vmaf-models
vmaf --tiny-model /opt/vmaf-models/vmaf_tiny_v2.onnx --tiny-model-verify ...
```

At load time libvmaf canonicalises the jail and the requested ONNX path. The
model must resolve below the jail. Sibling-prefix escapes, symlink escapes,
missing jail directories, and jail values that point at files fail closed with
`-EACCES`. The jail does not replace registry verification or the operator
allowlist: run all three layers together in production
(see [security.md](security.md)).

## What the registry is not

- Not the inference contract. Per-model input and output names, normalisation
  and expected ranges live in the sidecar JSON next to the ONNX
  (`<basename>.json`). The registry stays small and easy to audit. Attached
  multi-output models may use sidecar `output_names[]` for stable scalar-score
  suffixes in report keys.
- Not the operator-allowlist source of truth. That is
  `core/src/dnn/op_allowlist.c`. The registry pins identity; the allowlist
  constrains content.
- Not a path allowlist. Use `VMAF_TINY_MODEL_DIR` to reject model paths outside
  a trusted directory.

## Add a model

1. Drop the `.onnx` and the matching `<basename>.json` sidecar under
   `model/tiny/`.
2. Compute the digest: `sha256sum model/tiny/<name>.onnx`.
3. Add an entry to `registry.json`. Use existing entries as templates; the
   schema enforces required fields.
4. Leave bundle generation to the release. Before a release the
   `sigstore_bundle` path may point at a file that does not exist yet.
5. Run `python3 ai/scripts/validate_model_registry.py` and fix what it reports.

The `/add-model <path>` skill scaffolds steps 1 to 4.

## CI-only smoke fixtures

Thirteen registry entries (`smoke: true`) exist to exercise the loader and
validator in CI. They are not user-facing surfaces, are exempt from the
ADR-0042 five-point model-card requirement, and are excluded from doc-coverage
checks. Do not set them to `smoke: false` without a model card and the full
ADR-0042 bar.

| Registry id | Notes |
|---|---|
| `smoke_v0` | Minimal ONNX graph used by the `test_model_loader.c` smoke test. |
| `smoke_fp16_v0` | Same graph with fp16 weights; exercises the fp16 loader path. |
| `smoke_multi_output_v0` | Multi-output fixture (`mean_score` and `peak_score`) used by `test_vmaf_use_tiny_model.c`; exercises the attached multi-output path. |
| `smoke_v0_symbolic_batch` | Symbolic-batch fixture (dynamic first dim) used by `test_vmaf_use_tiny_model.c`; exercises the batch-agnostic load path. |
| `dists_sq_placeholder_v0` | Superseded placeholder; replaced by `dists_sq` (real weights). Card: [dists_sq](models/dists_sq.md). |
| `mobilesal_placeholder_v0` | Placeholder until the full U-2-Net weights clear compliance (ADR-0257). Card: [mobilesal](models/mobilesal.md). |
| `vmaf_tiny_v1` | Superseded by `vmaf_tiny_v2`; kept for loader back-compat tests. |
| `vmaf_tiny_v1_medium` | Superseded by `vmaf_tiny_v2`; kept for loader back-compat tests. |
| `fr_regressor_v2_ensemble_v1_seed0` to `_seed4` (five entries) | Smoke placeholders for the ensemble members; the production weights are LOSO-validated and described in each seed sidecar. See [fr_regressor_v2 probabilistic](models/fr_regressor_v2_probabilistic.md). |

## `lpips_sq_v1` and its cards

The registry entry `lpips_sq_v1` (`smoke: false`) points to `lpips_sq.onnx`.
Two model-card pages describe the same model:
[lpips_sq](models/lpips_sq.md) and [lpips_sq_v1](models/lpips_sq_v1.md). The
registry id and the file name differ by the `_v1` suffix; this is a tracked
cosmetic gap (scaffold-audit ADR-0621 P3-5).
