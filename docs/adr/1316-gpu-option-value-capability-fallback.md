<!-- markdownlint-disable MD013 MD060 -->
# ADR-1316: Mark extractor options whose implementation is default-only

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Lusoris
- **Tags**: `gpu`, `feature-options`, `compatibility`, `testing`

## Context

[ADR-1183](1183-model-options-gate-gpu-twin-selection.md) makes model-driven
GPU dispatch fall back to the CPU when a twin does not declare one of the
model's option keys. That name-only check misses a second capability shape:
some twins must retain the CPU option's name, alias, default, range and
`FEATURE_PARAM` bit so they derive the same collector key, but their current
kernel implements only the default value.

Nine live entries have that shape. Every CUDA, SYCL, HIP and Metal
`float_vif` twin declares `vif_kernelscale=0.1..4.0` but rejects anything
other than `1.0`; every `float_adm` twin declares `adm_csf_mode` but rejects
anything other than Watson-97 mode `0`; and Metal `integer_adm` declares the
CPU's `adm_csf_mode=0..3` range but rejects every nonzero mode. A model
requesting one of those valid non-default values therefore passed ADR-1183's
key check, selected the GPU twin, and failed later with `-EINVAL` instead of
using the capable CPU extractor.

The complete init-restriction audit also found motion options that no CPU twin
can honour and a context-dependent SSIM auto-scale restriction. Those are not
default-only capabilities and must not be mislabeled to make this dispatcher
accept them; [Research-2105](../research/2105-gpu-option-value-capability-fallback-2026-09-25.md)
records their separate dispositions.

## Decision

Add the internal `VMAF_OPT_FLAG_DEFAULT_ONLY` capability bit to `VmafOption`.
A GPU twin sets it only when its declared option schema mirrors the CPU for
collector-key compatibility and the CPU twin can execute every valid value,
but that GPU extractor can execute only the declared default. Model-driven
selection parses each supplied value through the existing option parser:
valid non-default values cause per-feature CPU fallback before GPU
initialization, while valid defaults keep the GPU twin. Unknown keys and
malformed values retain their existing parser failures. Mark the nine
confirmed entries and guard the complete inventory with a device-free test.

Explicitly naming a GPU extractor still means exactly that extractor; its
existing init-time `-EINVAL` remains the direct-call capability signal. This
decision changes only automatic model dispatch. The CPU-authoritative range is
never narrowed to hide a backend implementation gap.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Narrow or remove the GPU option entries | The existing name-only check would fall back without new metadata | Breaks CPU/GPU collector-key parity and makes one logical option validate differently by backend | Rejected: schema identity and execution capability are different facts |
| Special-case extractor and option names in `libvmaf.c` | Small local diff | A second hand-maintained registry would drift whenever a twin gains capability | Rejected: capability belongs beside the option descriptor |
| Port every missing kernel mode now | Keeps all affected work on the GPU | Requires three separate algorithm families and Apple hardware validation; it is not one correctness fix | Deferred as ordinary backend completion work |
| Add one default-only capability bit | Preserves canonical schema, keeps dispatch generic, and is device-free to test | Adds one internal metadata rule that every new restricted twin must set | **Chosen** |

## Consequences

- **Positive**: Models using a valid non-default `vif_kernelscale` or the
  affected `adm_csf_mode` configurations complete through the CPU reference
  instead of failing after GPU selection.
- **Positive**: Option names, aliases, ranges and derived feature keys remain
  identical across CPU and GPU twins.
- **Negative**: Those individual feature configurations lose GPU acceleration
  until the corresponding kernels implement the requested values.
- **Neutral / follow-ups**: Direct selection of a restricted GPU extractor
  retains its existing error contract. Adding support later removes the bit
  from that one option and extends hardware parity coverage. The SSIM
  auto-scale restriction needs a separate context-aware dispatch design.
  There is no public C ABI, CLI syntax, FFmpeg patch, dependency, score
  arithmetic, model, snapshot, or Netflix golden assertion change.

## References

- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md) — name-aware model option fallback.
- [ADR-1214](1214-float-adm-csf-scale-watson-mode-and-aliases.md) — mirrored option aliases and semantics.
- [ADR-1312](1312-gpu-option-alias-parity.md) — CPU-authoritative option aliases.
- [Research-2105](../research/2105-gpu-option-value-capability-fallback-2026-09-25.md) — full inventory, exclusions and red-cap evidence.
- Source: `req` — “we fix everything until we cant find anything anymore for now”.
