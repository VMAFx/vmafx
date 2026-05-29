# `model/other_models/` — model card index (legacy / experimental)

This directory contains older and experimental Netflix VMAF models. These are
**not the production defaults** — they are retained for backward-compatibility,
research, and regression testing. The canonical production models are in
`model/` (root-level JSON) and `model/tiny/`.

## Contents and status

| File | Architecture | Status | Notes |
| --- | --- | --- | --- |
| `nflxall_vmafv1.pkl` | Nu-SVR (libsvm) | Legacy | VMAF v1 trained on nflxall corpus |
| `nflxall_vmafv2.pkl` | Nu-SVR (libsvm) | Legacy | VMAF v2 trained on nflxall corpus |
| `nflxall_vmafv3.pkl` | Nu-SVR (libsvm) | Legacy | VMAF v3 trained on nflxall corpus |
| `nflxall_vmafv3a.pkl` | Nu-SVR (libsvm) | Legacy | VMAF v3a variant |
| `nflxall_vmafv4.pkl` | Nu-SVR (libsvm) | Legacy | VMAF v4 trained on nflxall corpus |
| `nflxtrain_vmafv1.pkl` | Nu-SVR (libsvm) | Legacy | VMAF v1 trained on nflxtrain corpus |
| `nflxtrain_vmafv2.pkl` | Nu-SVR (libsvm) | Legacy | VMAF v2 trained on nflxtrain corpus |
| `nflxtrain_vmafv3.pkl` | Nu-SVR (libsvm) | Legacy | VMAF v3 trained on nflxtrain corpus |
| `nflxtrain_vmafv3a.pkl` | Nu-SVR (libsvm) | Legacy | VMAF v3a variant |
| `nflxtrain_norm_type_none.pkl` | Nu-SVR | Legacy research | No normalisation (unnormalised features) |
| `nflxall_libsvmnusvr_currentbest.pkl` | Nu-SVR | Legacy | nflxall "current best" snapshot |
| `nflxtrain_libsvmnusvr_currentbest.pkl` | Nu-SVR | Legacy | nflxtrain "current best" snapshot |
| `nflx_v1.pkl` | Nu-SVR | Legacy | Early nflx-corpus model |
| `nflx_vmaff_rf_v1.pkl` | Random Forest | Legacy research | RF feature fusion baseline |
| `nflx_vmaff_rf_v2.pkl` | Random Forest | Legacy research | RF feature fusion v2 |
| `vmaf_v0.6.0.pkl` | Nu-SVR | Legacy | v0.6.0 predecessor to `vmaf_v0.6.1` |
| `vmaf_4k_v0.6.1rc.pkl` | Nu-SVR | Legacy | 4K release-candidate pkl |
| `niqe_v0.1.pkl` | NIQE statistical | Legacy | No-reference NIQE quality model |
| `model_V8a.model` | libsvm model | Legacy | Named pre-versioning model |

## Training data + provenance

All models in this directory are trained on Netflix proprietary DMOS corpora
(`nflxtrain` = Netflix training set; `nflxall` = full Netflix corpus).
Neither corpus is publicly distributed. Upstream lineage: Netflix/vmaf.

## Operating point

All models use the 6 canonical libvmaf features (`vif_scale0..3`, `adm2`,
`motion`). CPU inference via libsvm (pkl) or libsvm model format. Output is
VMAF score `[0, 100]`.

## Known limits

- These models are superseded by `vmaf_v0.6.1.json` (single-model) or the
  bootstrap ensembles (`vmaf_rb_v0.6.3/`) for production use.
- pkl format requires the Python VMAF harness; the JSON format models
  (`vmaf_v0.6.1.json` etc.) are used by the C libvmaf binary.
- NIQE (`niqe_v0.1.pkl`) is a no-reference model with different output
  semantics — lower is better, range is not `[0, 100]`.

## License + lineage

BSD-3-Clause-Plus-Patent (upstream Netflix). All model files in this
directory are verbatim from Netflix/vmaf upstream.
