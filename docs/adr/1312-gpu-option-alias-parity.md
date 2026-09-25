<!-- markdownlint-disable MD013 MD060 -->
# ADR-1312: GPU option aliases match CPU collector keys

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `gpu`, `feature-options`, `compatibility`, `testing`

## Context

ADR-1183 made a non-default feature option's alias part of the published
collector key. A GPU twin can therefore compute the right number but publish
it under a key that neither the CPU extractor nor a model requests when its
option alias differs from the CPU authority.

The state ledger recorded eighteen such divergences across the CUDA, SYCL,
and HIP twins. Eight had already been repaired on the current collector: six
`float_adm` CSF aliases, SYCL `float_motion.motion_force_zero`, and SYCL
`integer_vif.vif_skip_scale0`. A source-contract red cap over all eighteen
sites found the remaining ten:

- five motion twins lacked `motion_force_zero` alias `force_0`;
- three floating-point VIF twins lacked `vif_kernelscale` alias `ks`; and
- two integer VIF twins lacked `vif_skip_scale0` alias `ssclz`.

A repository-wide consumer scan found only the CPU-authoritative spellings in
live models, tests, snapshots, and user documentation. The divergent or
missing spellings had no compatibility consumer to preserve.

## Decision

1. The CPU extractor's option alias is authoritative for an equivalent GPU
   twin option.
2. Add the ten missing aliases without changing option names, defaults,
   ranges, kernel arguments, or numerical behavior.
3. Register `core/test/test_gpu_option_alias_contract.py` in the fast suite.
   It parses the balanced option initializer at each of the eighteen known
   sites and requires the CPU-authoritative alias, without needing a GPU.
4. Keep option capability separate from alias identity. An alias match does
   not claim that every non-default value runs on device; capability and CPU
   fallback gaps remain separately tracked.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep backend-specific aliases | Avoids source edits | Publishes incompatible feature keys for equivalent configurations | Rejected: collector-key compatibility is a correctness contract |
| Cover only the ten currently failing sites | Smallest test | Allows one of the eight earlier repairs to regress silently | Rejected: the complete known inventory is cheap to enforce |
| Require device parity tests only | Exercises runtime behavior | Hardware-specific lanes cannot guard every source table on every PR | Rejected: retain device parity and add a device-free structural contract |
| Generate all GPU option tables from CPU definitions | Eliminates manual drift | Couples extractors whose supported options and state layouts intentionally differ | Deferred: disproportionate redesign for a bounded bug fix |

## Consequences

- **Positive:** equivalent CPU and GPU feature parameters now derive the same
  collector-key suffix at all eighteen audited sites.
- **Positive:** ordinary CPU-only CI detects alias drift before a hardware
  parity lane can publish or consume the wrong key.
- **Negative:** an intentional future alias divergence must update the
  compatibility decision and contract rather than changing one table alone.
- **Neutral:** Netflix golden assertions, score snapshots, models, and GPU
  kernels are unchanged.

## References

- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) — option-aware GPU dispatch and collector-key behavior.
- [ADR-1214](1214-float-adm-csf-scale-watson-mode-and-aliases.md) — existing twin alias and semantics invariant.
- [Research-2104](../research/2104-gpu-option-alias-parity-2026-09-25.md) — inventory, red cap, and consumer audit.
- Source: `req` — “we fix everything until we cant find anything anymore for now”.
