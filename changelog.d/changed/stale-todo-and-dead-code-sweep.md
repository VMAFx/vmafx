## chore(hip): re-phrase stale TODO in `hip/dispatch_strategy`

Replaced the "TODO: walk a feature-name → hip-kernel registry once
kernels exist" comment in `core/src/hip/dispatch_strategy.c` (and the
matching "stub: returns 0 unconditionally until the kernels land"
docstring in `dispatch_strategy.h`) with an ADR-0533-citing explanation
that reflects how HIP dispatch actually works today.

The TODO dated from the T7-10 audit-first scaffold (ADR-0212), before
real HIP kernels existed. Since ADR-0533 (HIP all-extractors
registration sweep), HIP routing happens via
`VMAF_FEATURE_EXTRACTOR_HIP` in `compute_fex_flags()` (`libvmaf.c`),
not via the dispatch-supports predicate — which currently has no
callsites and is kept only for symmetry with the
Vulkan/SYCL/CUDA/Metal `_dispatch_supports` siblings (so a future HIP
smoke twin can grow without adding a new symbol).

No behavioural change. Sweep ruled the only other in-scope candidates
(cJSON `FIXME`, libsvm `XXX`, `y4m_input.c` `#if 0` block) out of
scope as vendored / upstream-mirrored code where preserving the
upstream wording matters for rebase parity.
