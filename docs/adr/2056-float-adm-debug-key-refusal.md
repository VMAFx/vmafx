<!-- markdownlint-disable MD013 MD060 -->
# ADR-2056: `float_adm` files its debug ratio unsuffixed; a second debug instance is refused

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer
- **Tags**: feature-extractor, float-adm, gpu, parity, cli

## Context

`float_adm` with `debug=true` writes the ratio `adm` next to `adm_num` and `adm_den`.
Every other output of a non-default instance is filed under the option suffix
(`adm_num_egl_1.2`), but `adm` is not in the extractor's `provided_features` (the list
names `adm_scale0`, which is never emitted), so the feature-name dictionary has no entry
for it and the key stays unsuffixed. The Netflix golden tests read
`VMAF_feature_adm_score` under non-default options (`python/test/feature_extractor_test.py`),
so the unsuffixed key is a contract. Two consequences: `vmaf --feature float_adm=debug=true
--feature float_adm=debug=true:adm_enhn_gain_limit=1.2` ended with `problem with feature
extractor "float_adm"` at the first frame (the key is written twice), and the CUDA, SYCL and
HIP twins listed `adm` and suffixed it, so their debug key differed from the CPU's under any
option (the parity tests left it out). Row `T-FLOAT-ADM-DEBUG-KEY-UNSUFFIXED-2026-10-01`.

## Decision

The unsuffixed `adm` key stays. A second `float_adm` instance with `debug=true` is refused
when it is registered (`feature_extractor_vector_append()`, so `vmaf_use_feature()` returns
`-EINVAL`) with an error naming the key and the instance that holds it. An extractor declares
such a key with `VmafFeatureExtractor::unsuffixed_debug_key`; the CPU extractor and the CUDA,
SYCL, HIP and Metal twins declare `adm`. The twins list `adm_scale0` as the CPU does, so they
file the ratio unsuffixed, and the twin parity cases compare it under the options.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Named refusal at registration, align the twins (**chosen**) | Keeps the Netflix contract; the failure moves from frame 0 to the command line with the cause named; one key on CPU and twins | A run cannot hold two debug instances | Chosen |
| Suffix the debug key (`adm` to `adm_<suffix>`) | Two instances coexist | Renames the key the golden tests read; those tests are never changed | Rejected, tried and withdrawn |
| File the ratio under both keys, a second instance only suffixed | Both work | Two names for one number, a second code path in four backends | Not chosen |
| Keep the failure at the first frame | No code | The message ("problem reading pictures") names nothing | Not chosen |

## Consequences

- **Positive**: the failure is early and says why; CPU and twins agree on the key, and
  `test_cuda_float_adm_parity` and the shared `float_adm_twin_parity.h` compare it under every option.
- **Negative**: no two `debug=true` instances in one run.
- **Neutral / follow-ups**: `core/test/test_float_adm_debug_key_refusal.c` guards the refusal;
  the Metal twin carries the declaration but its device check waits for an Apple device.

## References

- `Q`: "Named refusal + align twins (Recommended)" (praetor question ledger Q-015).
- [ADR-0024](0024-netflix-golden-preserved.md), [ADR-1420](1420-cuda-float-adm-cpu-arithmetic.md).
