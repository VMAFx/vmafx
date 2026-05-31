<!-- markdownlint-disable MD049 MD060 -->
# Research-0024 — VIF kernelscale: fork-vs-upstream divergence and port strategy

_Updated: 2026-04-28._

## Question

Netflix upstream has shipped a chain of recent commits that materially
re-shape the `float_vif` extractor's option surface: arbitrary-value
`vif_kernelscale`, frame pre-scaling (`vif_prescale` +
`vif_prescale_method`), per-scale minimum-value clamps. The fork's
`float_vif` already diverges from upstream — it carries precomputed
`vif_filter1d_table_s` tables indexed by `vif_kernelscale_enum`,
runtime-resolved by `resolve_kernelscale_index` in
`core/src/feature/vif.c:61`.

**Can the fork port the upstream chain (`4ad6e0ea` → `41d42c9e` →
`bc744aa3` → `8c645ce3`) without breaking the Netflix golden-data
gate? If not, what part of upstream do we keep and what do we
deliberately diverge on?**

## Background — fork state

[`vif_tools.h`](../../core/src/feature/vif_tools.h) carries a
**closed enum** of 11 supported kernelscale values:

```c
enum vif_kernelscale_enum {
    vif_kernelscale_1 = 0,        // 1.0
    vif_kernelscale_1o2 = 1,      // 0.5
    vif_kernelscale_3o2 = 2,      // 1.5
    vif_kernelscale_2 = 3,        // 2.0
    vif_kernelscale_2o3 = 4,      // 0.666...
    vif_kernelscale_24o10 = 5,    // 2.4
    vif_kernelscale_360o97 = 6,   // 3.7113...
    vif_kernelscale_4o3 = 7,      // 1.333...
    vif_kernelscale_3d5o3 = 8,    // 1.166...
    vif_kernelscale_3d75o3 = 9,   // 1.25
    vif_kernelscale_4d25o3 = 10,  // 1.4166...
};
extern const float vif_filter1d_table_s[11][4][65];
extern const int   vif_filter1d_width[11][4];
```

Each `(kernelscale, scale)` pair has a **precomputed Gaussian kernel**
of up to 65 taps. Kernel values are **frozen as `const float`
literals** — the SIMD paths
([`vif_avx2.c`](../../core/src/feature/x86/vif_avx2.c),
[`vif_avx512.c`](../../core/src/feature/x86/vif_avx512.c),
[`vif_neon.c`](../../core/src/feature/arm64/vif_neon.c)) consume
those exact floats; the Netflix golden goldens at
`python/test/feature_extractor_test.py::test_vmafexec_vif_v0_6_1`
encode the resulting per-frame VIF scores at `places=4`.

The fork explicitly chose this design as part of:

- **ADR-0138** (widen-then-add SIMD reduction)
- **ADR-0139** (per-lane scalar-double reduction)
- **ADR-0142** (port Netflix `18e8f1c5` `vif_sigma_nsq` with
  fork-specific `(float)` cast preserving ADR-0138/0139 discipline)
- **ADR-0143** (port Netflix `f3a628b4` generalized AVX convolution —
  drops the hard-coded fwidth whitelist in `vif_tools.c` but keeps
  the precomputed table)

The fork's precomputed tables exist because runtime-computed Gaussians
on x86 don't always match NEON Gaussians don't always match scalar
Gaussians at the same precision — `expf(...)` is implementation-
defined. Freezing the float literals at build time gives bit-identical
SIMD paths; the alternative (runtime computation) reintroduces the
pre-ADR-0138 cross-ISA drift.

## Background — upstream state

Netflix master (as of 2026-04-28 `9dac0a59`) ships
[`vif_tools.c`](https://github.com/Netflix/vmaf/blob/9dac0a59/core/src/feature/vif_tools.c)
with **runtime-computed** filters:

```c
int vif_get_filter_size(int scale, float kernelscale);
void vif_get_filter(float *out, int scale, float kernelscale);
```

These compute a Gaussian PDF on the fly from `(scale, kernelscale)` —
arbitrary float `kernelscale ∈ [0.1, 4.0]` works. Plus a frame
pre-scaler with four interpolation methods:

```c
enum vif_scaling_method { NEAREST, BILINEAR, BICUBIC, LANCZOS4 };
void vif_scale_frame_s(enum vif_scaling_method, ...);
```

The new options (`8c645ce3`) wire those helpers into
`float_vif.c::extract`:

```c
+    if (s->vif_prescale != 1.0) {
+        vif_scale_frame_s(scaling_method, ref, ref_scaled, ...);
+        ref = ref_scaled; w = scaled_w; h = scaled_h;
+    }
+    // ... same for dist
+    compute_vif(...) with scaled buffers
+    out_score = MAX(out_score, s->vif_scaleN_min_val);
```

The upstream chain also ports two bug-fixes:

- **`41d42c9e`** — vif edge-mirror reflection (fixes a 1-pixel error
  at frame edges on scale ≥ 1)
- **`bc744aa3`** — loosen Netflix's own python-test `places` from 4
  to 3 to absorb the resulting score drift (~`1e-3` per frame on
  small fixtures)

The mirror bugfix is the load-bearing constraint: it shifts every
reported VIF score by ~`1e-4` to `1e-3` even on `vif_kernelscale=1.0`
with `vif_prescale=1.0` (the defaults).

## Decision matrix

| Strategy | Fork goldens | New upstream options | SIMD paths | LoC delta |
|---|---|---|---|---|
| **(A) Cherry-pick verbatim** | **BREAKS** at `places=4` (mirror fixed) | All work | Need re-derivation against runtime Gaussian | +600 incl. helpers; net  +400 after dropping table |
| **(B) Replace fork's table with runtime helpers + accept golden shift** | Goldens move (need `places=3` like upstream did) | All work | Same as (A) | +100 net (table goes away) |
| **(C) Port options as **opt-in only**, keep fork's table as the default** | UNCHANGED at default settings | New options work; default behaviour bit-identical | Existing SIMD untouched | +700 (helpers + parallel code path) |
| **(D) Port only the helpers (`4ad6e0ea`); skip the option-surface commits** | UNCHANGED | None of the new options exposed | Untouched | +350 (helpers as dead code) |
| **(E) Skip the chain entirely; stamp ADR-X documenting deliberate divergence** | UNCHANGED | None | Untouched | 0 |

### Strategy A — Cherry-pick verbatim

Replicates upstream exactly. **Forces the Netflix golden gate to
`places=3`** because of the mirror bugfix — that means relaxing the
fork's `places=4` contract that ADR-0006 / ADR-0024 explicitly froze.
ADR-0142 (Netflix-authority carve-out) allows this *only when Netflix
themselves loosen their python tests* — which `bc744aa3` does. So
strictly speaking it's permitted, but **the fork's `places=4` contract
in `python/test/feature_extractor_test.py` would need a paired
loosening**.

Risk: the SIMD bit-exactness guarantee dies. Runtime Gaussians on AVX2
vs NEON vs scalar can drift at the float-mantissa level depending on
implementation of `expf`. The fork's precomputed tables exist
*specifically* to make AVX2 == NEON == scalar bit-for-bit. Replacing
them re-opens cross-ISA drift bugs that ADR-0138 / 0139 closed.

### Strategy B — Runtime helpers + golden shift

Same risk as A. Net code is smaller because we delete
`vif_filter1d_table_s` + the resolve-by-enum logic. But this loses the
precomputed-table investment for nothing.

### Strategy C — Opt-in only, defaults unchanged

Keeps `vif_kernelscale=1.0` + `vif_prescale=1.0` on the precomputed
table path (bit-identical to today). When the user passes a custom
value, **drops to upstream's runtime path with a documented warning**
("non-default vif_kernelscale uses runtime-computed filters; SIMD bit-
exactness no longer guaranteed cross-ISA"). The new options
(`vif_prescale*`, `vif_scaleN_min_val`) are pure additions and don't
disturb the default path.

This **preserves the Netflix golden gate at `places=4`** because
defaults are untouched. It **gains the new options for users who want
them**. It **costs ~700 lines** of duplicated code (precomputed-table
path + runtime-path coexisting) and a non-trivial dispatch.

### Strategy D — Helpers only

Pure-addition cherry-pick of `4ad6e0ea`. No behavioural change because
nothing calls the new helpers. Pointless code growth (~350 dead
lines). Only useful as scaffolding for Strategy C in a follow-up.

### Strategy E — Skip + document

Stamp an ADR explicitly recording that the fork stays on the
precomputed-table flow, and **does not adopt** the upstream
prescale / mirror-bugfix / per-scale-min-val features. Rationale: the
fork's bit-exact SIMD discipline (ADR-0138/9) is more valuable than
upstream's flexibility add. Costs **0 lines**; loses **the option to
ever upgrade** without revisiting.

## Recommendation

**Strategy E** for v1, with the option to escalate to **Strategy C**
if a concrete use case for `vif_prescale` lands.

**Why E over C right now:**

1. **No active demand.** No issue, no user request, no benchmark
   call-out. The new options exist upstream because Netflix's internal
   training pipelines wanted them; the fork's training pipeline
   (ADR-0203, tiny-AI) doesn't use `vif_prescale` and doesn't need
   per-scale `min_val` clamps.
2. **The fork's bit-exact SIMD discipline is load-bearing.** ADR-0138
   / 0139 / 0142 / 0143 chain represents weeks of investment. Replacing
   the precomputed table without strong motivation throws that work
   away.
3. **Strategy C is reversible.** If a user does want `vif_prescale`,
   we add it as the dual-path on top of the existing table. Easier to
   land later than to maintain a parallel option-surface forever.
4. **The mirror bugfix is the only upstream change with broad impact**,
   and it can be ported as a pure-bugfix targeted PR (without the
   prescale options) — but only if we also relax `places=4` paired
   with the C-side fix per ADR-0142's Netflix-authority precedent.
   That's a **separate decision** from the option-surface port.

## Recommended PR scope (after this digest)

1. **PR-α (this PR)** — research digest only (no code).
2. **PR-β (deferred)** — port `41d42c9e` mirror bugfix + paired
   `places=4 → places=3` golden loosening. Standalone, single-purpose
   PR. Requires ADR-0142-style Netflix-authority justification.
3. **PR-γ (deferred indefinitely, no T-number until concrete user
   demand)** — Strategy C dual-path port of `4ad6e0ea` +
   `8c645ce3` if a use case for `vif_prescale` ever materialises.

## Same divergence test for motion + float_adm

The motion (`b949cebf`) and float_adm (`4dcc2f7c`) chains have
**different divergence profiles**:

- **motion** — fork already absorbed most of the integer_motion option
  set (`motion_blend_factor`, `motion_max_val`, etc. exist in
  `integer_motion.c`); only the float_motion side is unported.
  float_motion is a far smaller code path than float_vif and doesn't
  carry the same precomputed-table investment. **Strategy A
  (verbatim) is plausible** for float_motion but requires re-running
  the cross-backend gate to confirm `places=4` still holds.

- **float_adm** — adds 12 new parameters to `compute_adm` and a new
  `score_aim` output. The fork's `compute_adm` signature is already
  fork-modified (per ADR-0142 / ADR-0143 work). Porting this requires
  threading 12 new parameters through the SIMD paths
  (`adm_avx2.c` / `adm_avx512.c` / `adm_neon.c`) AND through the GPU
  twins (`adm_vulkan.c` / `adm_cuda.c` / `adm_sycl.cpp`). That's a
  multi-day effort in itself, and the new `aim` feature requires
  alignment with the Netflix `aim` golden values that don't yet exist
  in our fork's golden tables. **Recommend Strategy E for adm too**
  until `aim` becomes a concrete user requirement.

Net: **only the motion-options port is straightforwardly worth
doing now** (Strategy A on `b949cebf` float_motion side). Vif and adm
chains stay deferred behind ADRs that document the divergence.

## Decision matrix (chains-level)

| Chain | Recommended strategy | Reason |
|---|---|---|
| **vif** (`4ad6e0ea` / `41d42c9e` / `bc744aa3` / `8c645ce3`) | E (skip + document) | Precomputed-table SIMD discipline > flexibility |
| **motion** (`a44e5e61` / `62f47d59` / `b949cebf`) | A (verbatim) for `b949cebf` only; mirror bugfix `a44e5e61` already in fork? — verify | float_motion has no precomputed-table issue; cheap to port |
| **float_adm** (`966be8d5` / `f8fb7b48` / `4dcc2f7c`) | E (skip + document) | 12-param signature change cascades to SIMD + 3 GPU backends |
| **alias map** (`9dac0a59`) | Skip until A motion + E adm decision lands; aliases reference features that only exist if the chains land | Dead aliases otherwise |

## Open questions

- **Q1.** Does Netflix consider the precomputed-table approach
  obsolete? If they have a future commit that actively removes runtime
  fallbacks, divergence becomes a maintenance burden.
- **Q2.** Is the mirror bugfix (`41d42c9e`) load-bearing on any
  shipped VMAF model? ADR-0142 worked through this for `vif_sigma_nsq`
  by checking `model/vmaf_v0.6.1.json` — same exercise needed here
  before PR-β.
- **Q3.** Does the fork's tiny-AI training (PR #180 combined trainer)
  benefit from any of the new options? Specifically:
  `vif_scaleN_min_val` could clip outliers in the feature distribution
  fed to `mlp_small`. If yes, that's user demand for Strategy C.

## References

- **`req`** (popup, 2026-04-28): user direction *"All-in: do all
  three chains in sequence as one big PR train"* — superseded by
  *"Pause vif chain, write research digest first"* same popup,
  later answer.
- ADR-0006 — Netflix golden tests preserved verbatim as required gate.
- ADR-0024 — Golden-gate immutability rule.
- ADR-0138 — VIF AVX-512 widen-then-add reduction (bit-exact).
- ADR-0139 — VIF per-lane scalar-double reduction (bit-exact).
- ADR-0140 — Two-part SIMD DX framework.
- ADR-0142 — Port Netflix `18e8f1c5` (`vif_sigma_nsq`).
- ADR-0143 — Port Netflix `f3a628b4` (generalized AVX convolution).
- Netflix upstream commits surveyed: `4ad6e0ea`, `41d42c9e`,
  `18e8f1c5`, `bc744aa3`, `8c645ce3`, `b949cebf`, `4dcc2f7c`,
  `9dac0a59`, `966be8d5`, `a44e5e61`, `62f47d59`, `f8fb7b48`.
