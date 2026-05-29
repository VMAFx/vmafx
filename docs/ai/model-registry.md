<!-- markdownlint-disable MD060 -->
# Tiny-model registry — schema and verification

The registry at [`model/tiny/registry.json`](../../model/tiny/registry.json)
is the **trust root** for libvmaf's tiny-AI surface. Every ONNX model
shipped under `model/tiny/` is indexed here with a SHA-256 pin, license
metadata, and a Sigstore bundle path. T6-9 / [ADR-0211](../adr/0211-model-registry-sigstore.md)
formalised the schema and wired `--tiny-model-verify` to `cosign verify-blob`.

## Registry shape

```jsonc
{
  "$schema": "./registry.schema.json",
  "schema_version": 1,
  "models": [
    {
      "id": "learned_filter_v1",          // kebab-case; --tiny-model=<id>
      "kind": "filter",                    // fr | nr | filter
      "onnx": "learned_filter_v1.onnx",
      "opset": 17,
      "sha256": "412d537…e27",
      "quant_mode": "dynamic",             // fp32 | dynamic | static | qat
      "int8_sha256": "1cff6fe…2d3",
      "quant_accuracy_budget_plcc": 0.01,
      "license": "BSD-3-Clause-Plus-Patent",
      "license_url": "https://github.com/VMAFx/vmafx/blob/master/LICENSE",
      "sigstore_bundle": "learned_filter_v1.onnx.sigstore.json",
      "description": "Tiny residual filter for vmaf_pre — degraded → clean luma.",
      "notes": "Self-supervised on KoNViD-1k …"
    }
  ]
}
```

The full JSON Schema is at
[`model/tiny/registry.schema.json`](../../model/tiny/registry.schema.json).
Fields documented inline; key invariants:

- `sha256` is **lowercase hex, 64 chars**. Mismatch against the on-disk
  ONNX bytes is a hard error.
- `int8_sha256` is required iff `quant_mode != "fp32"`.
- `sigstore_bundle` is a path relative to `model/tiny/` and must end in
  `.sigstore.json`. The bundle file itself is generated at release time
  by [`.github/workflows/supply-chain.yml`](../../.github/workflows/supply-chain.yml);
  pre-release the path is declared but the file may be absent. The
  runtime verifier (`--tiny-model-verify`) treats absence as a fail-closed
  signal.
- `license` is an SPDX identifier when possible; for fork-trained models
  this is `BSD-3-Clause-Plus-Patent` (matches libvmaf). Upstream-derived
  models carry the upstream license verbatim (e.g. LPIPS-Sq is `BSD-2-Clause`).

## Validating the registry

The Python validator at
[`ai/scripts/validate_model_registry.py`](../../ai/scripts/validate_model_registry.py)
runs both the JSON Schema check and the cross-file consistency check
(every ONNX exists, every sha256 matches, every non-smoke entry has a
sidecar). It is a CI gate; run it locally before pushing:

```bash
python3 ai/scripts/validate_model_registry.py \
    --out-json runs/tiny_model_registry_validation.json
# → OK: 5 registry entries valid against registry.schema.json
```

Pass a different registry / schema explicitly when working off-tree:

```bash
python3 ai/scripts/validate_model_registry.py /path/to/other-registry.json \
    --schema /path/to/registry.schema.json \
    --out-json runs/offtree_registry_validation.json
```

The validator falls back to a structural check when `jsonschema` is not
installed, so distros without `python-jsonschema` still get the
required-field invariants. Install `jsonschema` for full Draft 2020-12
coverage.

The optional `--out-json` report records the pass/fail verdict, model count,
errors, registry/schema paths, and ADR-0661 `run_provenance`. Use it for
release evidence and for debugging registry failures without relying on CI log
scrollback.

## Runtime verification — `--tiny-model-verify`

```bash
vmaf -r ref.y4m -d dis.y4m \
     --tiny-model model/tiny/learned_filter_v1.onnx \
     --tiny-model-verify
```

When the flag is set the loader:

1. Looks up the model's basename in `model/tiny/registry.json`
   (alongside the `.onnx`, by default).
2. Reads the entry's `sigstore_bundle` path.
3. Spawns
   `cosign verify-blob --bundle=<path> --certificate-identity-regexp …
   --certificate-oidc-issuer https://token.actions.githubusercontent.com
   <onnx>` via `posix_spawnp(3p)` (no shell — explicit argv array).
4. Refuses to load on any non-zero exit, missing `cosign`, missing
   bundle, or missing registry entry.

The flag is **off by default** for dev-friendliness. Production
deployments should set it on. `cosign` must be on `$PATH`; install
prebuilt binaries from the [Sigstore release page](https://github.com/sigstore/cosign/releases).

The C entry point is `vmaf_dnn_verify_signature(onnx_path, registry_path)`
in [`core/include/libvmaf/dnn.h`](../../core/include/libvmaf/dnn.h);
both arguments are NULL-tolerant in the documented way.

## Directory jail — `VMAF_TINY_MODEL_DIR`

The registry pins model identity. The optional `VMAF_TINY_MODEL_DIR`
environment variable constrains model location. Set it to the trusted
model directory on production hosts:

```bash
export VMAF_TINY_MODEL_DIR=/opt/vmaf-models
vmaf --tiny-model /opt/vmaf-models/vmaf_tiny_v2.onnx --tiny-model-verify ...
```

At load time libvmaf canonicalises both the jail and the requested ONNX
path. The model must resolve below the jail directory; sibling-prefix
escapes, symlink escapes, missing jail directories, and jail values that
point at files fail closed with `-EACCES`. The jail does not replace
registry verification or the operator allowlist: run all three layers
together for production deployments.

## What the registry is *not*

- **Not** the inference contract — per-model input/output names,
  normalisation, and expected ranges live in the sidecar JSON next to
  the ONNX (`<basename>.json`). The registry stays small and easy to
  audit; the sidecar carries the runtime knobs. Attached multi-output
  models may use sidecar `output_names[]` to provide stable scalar-score
  suffixes for report keys.
- **Not** the operator-allowlist source of truth — that's
  `core/src/dnn/op_allowlist.c`. The registry pins identity; the
  allowlist constrains content.
- **Not** a path allowlist — use `VMAF_TINY_MODEL_DIR` when deployments
  need to reject model paths outside a caller-trusted directory.

## Adding a new model

1. Drop the `.onnx` and matching `<basename>.json` sidecar under `model/tiny/`.
2. Compute the sha256: `sha256sum model/tiny/<name>.onnx`.
3. Add an entry to `registry.json` — see existing entries as templates;
   the schema enforces required fields.
4. Bundle generation happens at release time; pre-release the
   `sigstore_bundle` path may point at a not-yet-existing file.
5. Run `python3 ai/scripts/validate_model_registry.py` and fix any reported issues.

The `/add-model <path>` skill scaffolds steps 1–4 for you.

## CI-only smoke fixtures (`smoke: true`)

Several registry entries exist solely to exercise the loader / validator
pipeline in CI and are **not user-facing surfaces**. They are exempt from
the ADR-0042 five-point model-card requirement; no `docs/ai/models/` card
is expected or needed for them.

| Registry id | Notes |
|---|---|
| `smoke_v0` | Minimal ONNX graph used by `test_model_loader.c` smoke test. |
| `smoke_fp16_v0` | Same graph, fp16 weights — exercises the fp16 loader path. |
| `smoke_multi_output_v0` | Multi-output fixture (mean_score + peak_score) used by `test_vmaf_use_tiny_model.c` — exercises the attached multi-output DNN path. |
| `smoke_v0_symbolic_batch` | Symbolic-batch fixture (dynamic first dim) used by `test_vmaf_use_tiny_model.c` — exercises the batch-agnostic load path. |
| `dists_sq_placeholder_v0` | Superseded placeholder; replaced by `dists_sq` (real weights). The non-placeholder card lives at `docs/ai/models/dists_sq.md`. |
| `mobilesal_placeholder_v0` | Placeholder until the full U-2-Net weights clear compliance (ADR-0257). Non-placeholder card: `docs/ai/models/mobilesal.md`. |
| `vmaf_tiny_v1` | Superseded by `vmaf_tiny_v2`; kept for loader back-compat tests. |
| `vmaf_tiny_v1_medium` | Superseded by `vmaf_tiny_v2`; kept for loader back-compat tests. |

These ids are excluded from doc-coverage checks. Do not promote them to
`smoke: false` without first shipping a model card and the full ADR-0042 bar.

## `lpips_sq_v1` — name vs card mismatch

The registry entry `lpips_sq_v1` (`smoke: false`) points to `lpips_sq.onnx`.
Its model card is at `docs/ai/models/lpips_sq.md` (without the `_v1` suffix).
The card covers the same model; the naming divergence is a tracked cosmetic
gap (scaffold-audit ADR-0621 P3-5). Until the card is renamed, treat
`lpips_sq.md` as the authoritative card for `lpips_sq_v1`.
