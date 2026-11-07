<!-- markdownlint-disable MD013 MD060 -->

# ADR-3402: Revive Vulkan as an opt-in, integer-first compute path (partial supersession of ADR-0726)

- **Status**: Proposed
- **Date**: 2026-11-07
- **Deciders**: lusoris (decision invited on #3302 comment 6691339657; tracked as
  [#3319](https://github.com/VMAFx/vmafx/issues/3319), milestone 1.1.0)
- **Tags**: `vulkan`, `gpu`, `backend`, `numerics`, `fork-local`

## Context

[ADR-0726](0726-drop-vulkan-backend.md) (2026-05-28, PR #47 / `e9d265657`) removed the
Vulkan compute backend entirely: 95 code files / 38 372 LOC (runtime 15 files, 24 feature
twins, 34 GLSL shaders, 8 test binaries), the `enable_vulkan` meson option, the public
`libvmaf_vulkan.h` header, and 3 CI jobs + 2 build-matrix rows. Its decisive argument was
coverage: under the container-first Kubernetes deployment model each vendor runs in its
own node pool and the native backend (CUDA, HIP, SYCL) is injected at pod-schedule time,
so "Vulkan provides no coverage that the native trio does not already provide on the same
hardware" (`0726-drop-vulkan-backend.md:10-18`). Cost (~30 000 LOC, the largest non-CUDA
CI footprint) and three open bugs were compounding reasons (`:22-35`).

Four things have changed since, each on the public record:

1. **One of the three blockers was misattributed and was already fixed before the ADR.**
   The "Intel Arc A380 / Mesa-ANV" VIF residual was re-attributed to NVIDIA by the fork's
   own merged research (`6baa9d751`, PR #512, 2026-05-09,
   [research-0090](../research/0090-vulkan-vif-fp-residual-phase-3b-2026-05-09.md)
   "Hardware-mapping correction": Arc 0/48, NVIDIA the bug-bearing device) and fixed by
   the `subgroupShuffleXor` butterfly in `b6d3ececc` (PR #787, 2026-05-14) — before
   ADR-0726 was written. The maintainer opened the erratum himself
   ([#3312](https://github.com/VMAFx/vmafx/issues/3312)); the drop decision stands.
2. **The two remaining blockers are now measured on current drivers.** On NVIDIA Windows
   (RTX 5080 `0x10de:0x2c02`, RTX 4060 Laptop `0x10de:0x28e0`, driver 621.257, API
   1.4.357) and Intel Windows (RaptorLake-S iGPU `0x8086:0xa78b`, driver 0.101.7092, API
   1.4.323): SPIR-V `NoContraction` is honored with **0 violations** on both vendors, and
   the `subgroupAdd(int64_t)` lowering returns the CPU-golden value on all three devices
   with the butterfly A/B-identical (#2466 comment 6689054122). These are single-driver,
   single-OS data points, not a general clearance.
3. **The integer-parity gate passed.** `integer_adm` computed by a Vulkan compute path
   reproduces the unmodified CPU extractor **bit-for-bit** — 48 frames × 11 fields, both
   gate fixtures (`576x324_48f`, `640x480_48f`), RTX 5080 and RTX 4060 — compared as
   exact `%a` hex-float strings, with a comparator self-test that catches a planted
   single-digit flip, committed `refs/` + `out/`, and a byte-for-byte reproducibility
   chain (shader → SPIR-V → outputs → CPU anchor at master `af5ec5c94`). Evidence:
   #3302 comment 6689093406; fork branch `poc/vk-integer-adm` @ `0bb98ee21`. The
   maintainers' verdict: "the gate passed, so you are welcome to open the in-tree
   proposal" — a draft PR plus a new ADR superseding ADR-0726, review starting after
   1.0.0, no merge promise (#3302 comment 6691339657, tracked as #3319).
4. **The porting-defect vs driver-inherent distinction is settled for the record.** The
   removed `integer_adm.comp` (`d69f5301:446`, `:614`) rounded with `+2^31` where every
   other twin uses the ADR-0155 `-2^31` — a tree defect, not a Vulkan property (#3311;
   #2466 comment 6689054122; #3312 work item 3). The PoC needed six such deviations from
   the removed snapshot to reach bit-exactness (listed in the Decision below); they are
   the finding, not incidental fixes.

One open item is carried by the evidence: on the Intel iGPU (`shaderFloat64=0`,
`shaderInt64=1`) all four device-capability checks pass, but the PoC's integer ADM —
which keeps the removed tree's fp64 angle test (`adm_angle_flag.h:85-91`, promotion
through the double literal `4096.0`) — dies with `VK_ERROR_DEVICE_LOST` at the first
fence wait. That is a porting defect, and the fp64-free reformulation
`adm_angle_flag_i64()` (`adm_angle_flag.h:219-262`,
[research-2030](../research/2030-adm-angle-flag-fp64-free.md)) is the fix direction
(#3319 work item 3). Separately, the Intel driver SIGSEGVs inside `vkCreateDevice` when
asked for an unreported feature (2/2 runs, #2466 comment 6689054122): a revival must
request only device-reported features.

## Answering ADR-0726's coverage argument

ADR-0726's coverage argument decides the *Kubernetes deployment model*, and the
maintainers have said so on the record: "That argument is scoped to the Kubernetes
deployment model. It says nothing about Android, which has no native vendor trio, so
ADR-0726 does not settle #2517 either way" (#3311 comment 6685708124). This ADR answers
that argument where it does not reach; it does not touch the k8s conclusion itself.

- **Android (#2517).** Android has no native vendor trio at all. ADR-0726 is silent on
  it, and #2517 is decided on its own evidence. This ADR does not decide Android either;
  it removes the "coverage is moot" objection as a universal and puts a measured,
  capability-gated candidate path on the table for that decision to evaluate.
- **Windows desktop.** The same scoping applies to the desktop, where there are no node
  pools. On the Windows desktop lane the tree's GPU compute surface is CUDA-only
  (NVIDIA): the only in-tree Windows GPU *import* face is the SYCL D3D11 staging import
  ([ADR-0103](0103-sycl-d3d11-surface-import.md)), which is explicitly not zero-copy;
  HIP-on-Windows is not provided; dmabuf import is refused on Windows by design; a
  Windows GPU kernel CI lane does not exist (ROUND18 tree scan:
  `windows-tester-bundle.yml:523-535`; no `T-HIP-WINDOWS-*` tracker row exists because
  Windows HIP is not shipped). For Intel/AMD on Windows desktop the coverage premise —
  "the native trio already covers the same hardware" — is simply false today. Vulkan
  compute is the only single-binary cross-vendor candidate for that lane, which is why
  the argument's author agreed it does not reach here.
- **The three bugs no longer carry the weight the ADR gave them.** One was structural
  (ciede f32/f64, ADR-0391) and stays out of scope by construction — the integer path
  never ports ciede. One was a tree defect fixed before the drop (the VIF residual,
  PR #787; #3312). One is driver-inherent but has a measured counter-data-point on two
  vendors (NoContraction honored, 0 violations) plus a design rule that does not depend
  on driver goodwill (`NoContraction` decoration; no subgroup int64 ops at all).
- **What remains of the cost argument is conceded, not disputed.** The 30 000-LOC
  backend and its CI footprint were a real reason. This ADR therefore does not restore
  the backend: it revives a *subset* (one integer feature, one shader, the runtime the
  subset needs) behind an off-by-default build option, with no CI rows and no public API
  surface. The other 200+ removed files stay removed.

## Decision

Adopt an **opt-in, integer-first, capability-gated Vulkan compute path**, superseding
ADR-0726 in this domain only. The ADR-0726 removal stands, unchanged, for everything
else: the floating-point shader pipeline, the CI lanes, the FFmpeg patch wiring, the CLI
surface, and the public `libvmaf_vulkan.h` API. Concretely:

- **Opt-in and off by default.** A `core/meson_options.txt` feature option
  (`enable_vulkan`, value `disabled`); nothing in CI; no required-check lane. CI wiring
  is a separate decision after the 1.0.0 review.
- **Integer-first.** Integer operators only. The floating-point twins stay out of scope —
  not deferred — because the fork's contract fixes the summation order and a per-block
  partial sum cannot reproduce a sequential sum (#3302 proposal, feasibility boundary).
- **Feature-by-feature parity-gated entry.** Each feature enters only when it reproduces
  the unmodified CPU extractor bit-for-bit under the parity-gate contract below. The
  first in-tree kernel is integer ADM (`adm2` only; AIM is a separate feature and is not
  included; VIF needs the 53-bit ratio rework and is future work).
- **No fp64 dependency at runtime.** The first in-tree shader must port the fp64-free
  `adm_angle_flag_i64()` angle test as a prerequisite (#3319 work item 3). No shader may
  require `shaderFloat64`.
- **The six deviations are the porting recipe, and one of them is mandatory here where
  the PoC could defer it.** An in-tree `integer_adm.comp` is written from the PoC shader
  with all six deviations, including ADR-1402 compliance: the scale-0 CM own-tap
  `int16_t` narrowing that the PoC kept (Netflix-style, matching the removed snapshot's
  target) is **dropped**, per [ADR-1402](1402-adm-cm-centre-tap-int32.md). The six:
  1. no `subgroupAdd(int64_t)` — shared-memory serial sum (addition is associative);
     subgroup int64 ops are banned outright regardless of current-driver health;
  2. per-site `int16_t` truncation replication (`wrap16`) at every CPU scale-0 int16
     site (`integer_adm.h:86-89`; `integer_adm_kernels.h:1351-1356`, `:1377-1383`,
     `:486-490`, `:316-318`, `:324-326`) — **except** the ADR-1402 site above;
  3. scale-0 diagonal rounding constant 65535, not `1 << 16`
     (`integer_adm_kernels.h:439` `adm_csf_shiftsadd = {16384, 16384, 65535}`, applied
     `:487`);
  4. the angle test ported fp64-free via `adm_angle_flag_i64()`
     (`adm_angle_flag.h:219-262`; the CPU's `(float)ot_dp / 4096.0` double promotion is
     at `:85-91`);
  5. `−2^31`, not `+2^31`, in the scale 1–3 CM rounding (`integer_adm_kernels.h:510-528`,
     ADR-0155 wrapped arithmetic; the removed shader's `+2^31` at `d69f5301:446`/`:614`
     is the recorded historical defect, #3312);
  6. host conclude details: `numden_limit` uses the base frame `w*h`
     (`integer_adm.c:906-909`); s>0 CM uses the CPU's `(float)(accum / final_shift)`
     float division (`integer_adm_kernels.h:1228-1235`); `powf_add` uses the CPU's
     double product (`:604`); per-band sums are left-associated `((h + v) + d)`
     (`:1084`, `:1243`).
- **Device hygiene, from the recorded failure classes.** Devices are selected and
  reported by `vendorID`/`deviceID`, never ordinals (research-0090's inverted-attribution
  lesson; the ADR-0176 errata). `vkCreateDevice` requests only features the device
  reports (the Intel SIGSEGV, #2466 comment 6689054122). A dispatch sentinel proves the
  kernel ran (the ADR-0176 errata's bogus-"ULP=0" lesson). DAZ (denormals flushed to
  zero by NVIDIA Windows drivers) is recorded as an input assumption, not a failure.
- **`NoContraction` decoration** (`precise`) on the load-bearing floating-point
  operations — the concluded divisions and any remaining fp steps — because the API-1.4
  contraction flip (ADR-0264) is driver-inherent even though current drivers honor the
  decoration.
- **No public ABI surface.** `VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` and
  `VMAF_FEATURE_EXTRACTOR_VULKAN` remain the reserved gaps ADR-0726 left; reusing them
  requires the explicit ABI-bump ADR that ADR-0726 itself demands. The draft PR adds
  none of it; the path is reachable only through the opt-in build and the extractor
  registry.

### Parity-gate contract

Every feature and every device claim under this ADR is governed by:

1. **Anchor.** The contract is the unmodified CPU extractor built from public master
   (pinned commit, recorded build), driven through the public API only. Another GPU
   backend is never the anchor. The PoC's dual-anchor cross-check (VMAFx CPU and the
   Netflix-style CPU the removed backend targeted produce byte-identical references on
   the gate fixtures) is re-run at merge time.
2. **Comparison.** Values are exchanged as C `%a` hex-float strings and compared as
   exact strings — tolerance 0, every bit recoverable. Per frame: `num/den` per scale
   0–3, frame totals, and the concluded score (11 fields for `integer_adm`).
3. **Non-vacuousness.** The comparator self-tests a planted single-digit flip and must
   catch and name it; parse degenerations exit non-zero. The dispatch sentinel asserts
   the kernel executed on the selected device. Committed `refs/` + `out/` let the
   comparison be re-checked without a GPU; the SPIR-V is byte-reproducible from the
   committed shader.
4. **Device identity.** Verdict rows are one per (check, device) with vendor ID, device
   ID, driver version, Vulkan API version, OS, result, and a cause column holding one of
   `driver`, `tree defect`, `structural`, `unknown`, plus a pointer to the run — the
   shape fixed by the maintainer in #3311 comment 6685708124. The gate refuses (exit
   non-zero, naming the adapters it saw) a device that does not match the requested
   identity.
5. **Entry rule.** A feature is declared on a device class only after exact parity on
   lavapipe and at least two real vendors (#3302 Phase-1 exit condition), each row
   committed. Until then the extractor answers `-ENOTSUP` on that class. The
   maintainers' own runner re-run — including a planted flip the gate must refuse — is
   the review gate after 1.0.0 (#3319 work item 2).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep ADR-0726 as-is (no revival) | Zero new surface; the record stays simple | The Windows-desktop and Android coverage gap has no candidate path at all; the parity evidence and the deviation map (an audit-grade map of every C truncation site) are discarded | Rejected: the maintainers invited the proposal after the gate passed (#3302 comment 6691339657), and the coverage argument is k8s-scoped by their own statement |
| Restore the full 232-file backend from the `816a5f33f` snapshot | Everything comes back at once | Restores 30 135 LOC of kernel+runtime plus the known-defective shaders (`+2^31`, `subgroupAdd(int64)`, fp64 angle test); re-opens the ciede structural gap; re-adds the CI footprint that motivated the drop | Rejected: even the canonical `integer_adm.comp` needed six repaired deviations to reach bit-exactness; the snapshot is an archaeology source, not a starting point |
| Revive the floating-point pipeline instead | Shortest path to full feature coverage | The fork's fixed summation order makes fp twins structurally un-reproducible on a GPU; ciede f32/f64 is a recorded structural gap (ADR-0391) with no shaderFloat64 path (rejected in ADR-0726) | Rejected: integer operators are the only arithmetic that reproduces the CPU contract exactly |
| Stay fork-external (PoC branch / libvmaf-fast integration) | Zero in-tree cost | Users cannot get it; the maintainers explicitly invited the in-tree proposal; the deviation map would remain unreviewed | Rejected: forks the contract instead of joining it |
| DX12 or OpenCL as the cross-vendor path | Avoids revisiting a removed backend | DX12: DXC fast-math offers only per-variable `precise`, systematically weaker than Vulkan's per-result `NoContraction` (rejected in #3302). OpenCL: belongs to #2517's decision matrix, not evaluated here | Rejected for this ADR; OpenCL remains #2517's to evaluate |

## Consequences

- **Positive**:
  - A measured answer to the coverage gap: the tree gains a cross-vendor GPU compute
    candidate for the lanes the k8s-scoped argument does not reach (Windows desktop,
    Android later via #2517), with device admission decided by the parity gate rather
    than by assertion.
  - The six-deviation map and the probe/gate machinery become durable assets: the probe
    (identity selection, contraction check, dispatch sentinel) is reusable evidence
    tooling for #2517's Android matrix, and the truncation-site map documents every
    place the CPU contract is width-sensitive.
  - The revival cannot silently regress anything: off by default, no CI rows, no public
    ABI, no changes to the CPU, CUDA, HIP, SYCL or Metal paths.
- **Negative**:
  - The maintenance surface returns in bounded form: one shader, one host TU, the
    runtime subset, and the gate tooling. The `volk` and `Vulkan-Memory-Allocator`
    meson wraps, removed after the drop, must return to `core/subprojects/`.
  - The evidence gaps are real and named, not hidden: **zero AMD rows** (no AMD device
    on hand), Intel is capability-PASS only until the i64 angle port lands and is
    re-gated, **no Linux/Mesa-ANV/RADV data at all**, and one driver version per device
    (#2466 comment 6689054122 "what these rows do and do not cover"). No perf claims:
    the PoC ran under ~100% SM load; quiet-window measurement is pending and dispatch
    overhead is the known first risk (research-0135).
  - Two build-time dependencies return (volk, VMA — both MIT) plus a GLSL compiler
    (glslc/shaderc or glslang) behind the off-by-default option.
- **Neutral / follow-ups**:
  - ADR-0726's Status line gains a partial-supersession note (the same one-line edit
    class as the #3312 erratum): the removal stands for the fp pipeline, CI lanes and
    public API; the opt-in integer path proceeds under this ADR.
  - `docs/state.md` rows: the three `T-VK-*` Recently closed rows (`:2716`–`:2718`) gain
    ADR-3402 notes (NoContraction measured; ciede stays structural-out; the ARC-row
    erratum per #3312); new open rows for the Intel fp64 DEVICE_LOST defect, the
    single-vendor evidence gap, and the unmeasured quiet-window performance.
  - `docs/backends/vulkan/overview.md` (kept on master as historical reference) is
    rewritten for the opt-in path; the ADR-0860 no-op FFmpeg shim patches stay as they
    are.
  - The maintainers re-run the parity check on their own runner, including a planted
    flip the gate must refuse, before any merge decision after 1.0.0 (#3319). No merge
    is promised; the proposal is judged on this ADR.

## Supply-chain impact

- **New build-time dependencies** (behind the disabled-by-default option): `volk`
  (MIT), `Vulkan-Memory-Allocator` (MIT) — re-added as meson wraps in
  `core/subprojects/`; a GLSL→SPIR-V compiler (`glslc` from shaderc, Apache-2.0, or
  standalone glslang — toolchain pin is an open item). Runtime: `libvulkan` loaded
  dynamically; no vendor SDK linkage.
- **SBOM delta**: two wrapped C/C++ libraries and one host loader, present only in
  builds that opt in. No build-time fetches in CI (no CI rows); no new network listeners
  or privileged calls beyond Vulkan device enumeration.

## References

- [ADR-0726](0726-drop-vulkan-backend.md) — the superseded decision (partially).
- [#3302](https://github.com/VMAFx/vmafx/issues/3302) — proposal, decision
  (comment 6665476984), parity evidence and six deviations (comment 6689093406),
  invitation (comment 6691339657).
- [#3319](https://github.com/VMAFx/vmafx/issues/3319) — tracking issue, milestone 1.1.0.
- [#3311](https://github.com/VMAFx/vmafx/issues/3311) + comment 6685708124 — blocker
  re-attribution, driver-inherent vs tree-defect table, coverage-argument scoping,
  verdict-table shape.
- [#2466](https://github.com/VMAFx/vmafx/issues/2466) comment 6689054122 — per-(check,
  device) rows: NoContraction 0 violations, subgroupAdd(int64) A/B, Intel fp64
  DEVICE_LOST, vkCreateDevice SIGSEGV, `+2^31` finding.
- [#3312](https://github.com/VMAFx/vmafx/issues/3312) — ADR-0726 erratum (third blocker
  misattributed; fixed by `b6d3ececc` / PR #787 before the drop; `+2^31` historical
  defect).
- [#2517](https://github.com/VMAFx/vmafx/issues/2517) — Android GPU compute study
  (unaffected; decided on its own evidence).
- Fork PoC branch `poc/vk-integer-adm` @ `0bb98ee21` — probe (Phase 0), gate (Phase 1),
  `refs/`/`out/`, comparator with planted-flip self-test; CPU anchor `af5ec5c94`.
- [ADR-0264](0264-vulkan-1-4-bump-blocked-on-fp-contraction.md),
  [research-0053](../research/0053-vulkan-1-4-nvidia-fp-contraction-regression.md) —
  T-VK-1.4-BUMP.
- [ADR-0391](0391-ciede-vulkan-nvidia-f32-f64-precision-gap.md),
  [research-0055](../research/0055-ciede-vulkan-nvidia-f32-f64-root-cause.md) —
  T-VK-CIEDE-F32-F64 (stays structural-out).
- [ADR-0269](0269-vif-ciede-precise-step-a.md),
  [research-0108](../research/0108-vulkan-vif-int64-subgroup-reduction-2026-05-14.md),
  [research-0090](../research/0090-vulkan-vif-fp-residual-phase-3b-2026-05-09.md) —
  int64 subgroup lowering and the device-mapping correction.
- [ADR-1402](1402-adm-cm-centre-tap-int32.md) — the wrap removal an in-tree port must
  follow; [ADR-0155](0155-adm-i4-rounding-deferred-netflix-955.md) — the `−2^31` wrapped
  arithmetic.
- [research-2030](../research/2030-adm-angle-flag-fp64-free.md) — the fp64-free angle
  test (`adm_angle_flag_i64()`) the first in-tree shader ports.
- [ADR-0176](0176-vulkan-vif-cross-backend-gate.md) — the gate/errata lessons (bogus
  baselines, device-selection lies).
- [ADR-0586](0586-integer-adm-vulkan-canonical-rename.md) — provenance of the canonical
  integer ADM shader name.
- [ADR-0165](0165-state-md-bug-tracking.md) — state.md row rules the PR follows.
- Snapshot provenance: `816a5f33f` (last complete wiring-alive state, parent of
  `e9d265657`); archaeology in the local `verify/vulkan-archaeology/removed-backend/`
  MANIFEST.
