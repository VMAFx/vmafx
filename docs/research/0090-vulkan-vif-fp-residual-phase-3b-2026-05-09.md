<!-- markdownlint-disable MD013 MD060 -->
# Research-0090: Vulkan VIF API-1.4 NVIDIA residual — Phase 3b stronger-fence experiments

- **Status**: Concluded — none of the three Phase-3b candidates closes the residual; deferral path documented.
- **Workstream**: [ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md),
  [ADR-0269](../adr/0269-vif-ciede-precise-step-a.md) Phase-3b status update,
  state.md row **T-VK-VIF-1.4-RESIDUAL-NVIDIA-DEFERRED** (replaces the
  Phase-3 row `T-VK-VIF-1.4-RESIDUAL-ARC` after the hardware-mapping correction below).
- **Last updated**: 2026-05-09
- **Predecessor**: [research-0089](0089-vulkan-vif-fp-residual-bisect-2026-05-08.md)
  (Phase 2 dynamic dump + Phase 3 status appendix that PR #511 cited).

## Question

PR #511 (`fix(vulkan-vif): shared-memory release-acquire for cross-subgroup
int64 reduction`) wrapped the three bare `barrier()` calls in `vif.comp`
with `memoryBarrierShared(); barrier();` pairs, lowering to SPIR-V
`OpControlBarrier(Workgroup, Workgroup, AcquireRelease|WorkgroupMemory)`.
PR #511's commit message reported NVIDIA RTX 4090 + driver 595.71.05 + API
1.4.341 closed to 0/48 5-run-deterministic, with a residual on Arc A380 +
Mesa-ANV that the commit message attributed to `--vulkan_device 0` and
opened as state.md row `T-VK-VIF-1.4-RESIDUAL-ARC`.

Phase 3b's brief was to try three stronger-fence variants on top of
PR #511 in order — `coherent`/`volatile` shared (C1),
`subgroupMemoryBarrierShared()` before the elected-thread write (C2),
device-scope `controlBarrier` (C3) — and stop at the first that closes
the residual to 0/48 5-run-deterministic at `places=4`.

## Hardware-mapping correction (load-bearing)

This session ran on a **multi-GPU host** with three Vulkan-capable
devices visible to the loader:

```text
device 0: type=DISCRETE_GPU score=100 name=NVIDIA GeForce RTX 4090
device 1: type=DISCRETE_GPU score=100 name=Intel(R) Arc(tm) A380 Graphics (DG2)
device 2: type=INTEGRATED_GPU score=50  name=AMD Ryzen 9 9950X3D 16-Core Processor (RADV RAPHAEL_MENDOCINO)
```

(Probed with a 30-line `vkEnumeratePhysicalDevices` + `devtype_score`
program that mirrors `core/src/vulkan/common.c`'s sort. `VK_API_VERSION_1_3`
and `VK_API_VERSION_1_4` enumerate in identical order on this host.)

The `enumerate_compute_devices` insertion sort in `common.c` is **stable**
inside one `devtype_score` bucket, so `--vulkan_device 0` deterministically
lands on the first device returned by `vkEnumeratePhysicalDevices`. On this
host that is **NVIDIA RTX 4090**, not Arc A380.

PR #511's commit message claimed the opposite mapping: that `--vulkan_device 0`
was Arc on this host, and that PR #511 closed NVIDIA. Re-baselining at API 1.4
with PR #511's `vif.comp` fix already in place:

| device | hardware                         | cross-backend `places=4` (48 frames) | 5-run determinism |
| ------ | -------------------------------- | ------------------------------------ | ----------------- |
| 0      | NVIDIA RTX 4090 + 595.71.05      | scale 0/1/3 OK; **scale 2: 45/48 FAIL, max_abs 1.527e-02** | **5 distinct `(num_scale2, den_scale2)` pairs**, magnitudes ~10^14, sign flips on `den` |
| 1      | Intel Arc A380 + Mesa-ANV 26.1.0 | 0/48 OK on every scale               | 5 identical pairs `(11664.0, 11664.0)` (frame-0 ground truth) |
| 2      | AMD RADV (CPU) + Mesa 26.1.0     | 0/48 OK on every scale               | deterministic |

The numbers cited in PR #511's commit message — "NVIDIA RTX 4090 +
driver 595.71.05 closed to 0/48 5-run-deterministic, Arc A380 still
fails 45/48" — are correct empirically; the **device-name attribution
is inverted on this host**. The bug-bearing device is NVIDIA, not Arc.

PR #511's existing `T-VK-VIF-1.4-RESIDUAL-ARC` row is therefore tracking
a phantom: Arc is already 0/48. The actual remaining residual is on
NVIDIA. Phase 3b retargets accordingly. (See §"Why the original device-map
claim looked plausible" below for how PR #511's report ended up inverted.)

## Reproducer

```bash
git checkout fix/vulkan-vif-arc-mesa-anv-int64-reduction  # this PR's branch

# Local API-1.4 bump (same 4 sites as research-0089).
sed -i 's/VK_API_VERSION_1_3/VK_API_VERSION_1_4/g' core/src/vulkan/common.c
sed -i 's/VMA_VULKAN_VERSION 1003000/VMA_VULKAN_VERSION 1004000/' core/src/vulkan/vma_impl.cpp

cd libvmaf && meson setup build -Denable_vulkan=enabled -Denable_cuda=false \
    -Denable_sycl=false -Denable_avx512=true && ninja -C build
cd ..

python3 scripts/ci/cross_backend_vif_diff.py \
    --vmaf-binary core/build/tools/vmaf \
    --reference testdata/ref_576x324_48f.yuv \
    --distorted testdata/dis_576x324_48f.yuv \
    --width 576 --height 324 \
    --feature vif --backend vulkan --device 0
# device 0 = NVIDIA RTX 4090 on this host. Reproduces 45/48 scale-2.

# Revert the API-1.4 bump for the shipping default.
sed -i 's/VK_API_VERSION_1_4/VK_API_VERSION_1_3/g' core/src/vulkan/common.c
sed -i 's/VMA_VULKAN_VERSION 1004000/VMA_VULKAN_VERSION 1003000/' core/src/vulkan/vma_impl.cpp
```

## Candidates tested

### C1: `shared coherent` / `shared volatile` qualifiers — NOT BUILDABLE

```glsl
shared coherent int64_t s_lmem[ACCUM_FIELDS * MAX_SUBGROUPS];   // proposed
shared volatile int64_t s_lmem[ACCUM_FIELDS * MAX_SUBGROUPS];   // alternate
```

`glslc 2026.1` (Vulkan SDK) rejects both with:

```text
vif.comp:227: error: '' : memory qualifiers cannot be used on this type
vif.comp:228: error: '' : memory qualifiers cannot be used on this type
vif.comp:234: error: '' : memory qualifiers cannot be used on this type
vif.comp:241: error: '' : memory qualifiers cannot be used on this type
4 errors generated.
```

Per the GLSL 4.50 specification §4.10 ("Memory Qualifiers") and the
GL_KHR_vulkan_glsl mapping, the `coherent` / `volatile` / `restrict` /
`readonly` / `writeonly` qualifiers apply to **buffer** and **image**
variables only. They are not legal on `shared` declarations because
the GLSL memory model already treats `shared` as workgroup-scoped
storage with no client-side cache layer to flush. The fix-brief in
the Phase-3b task description was therefore not directly representable
in GLSL on this compiler. C1 is **rejected at build time**, not at
runtime; no scores were collected.

### C2: `subgroupMemoryBarrierShared()` before the workgroup-scope pair — BUILDS, NO EFFECT

```glsl
if (subgroupElect()) {
    s_lmem[0 * MAX_SUBGROUPS + sg_id] = sg_x;
    /* ... 6 more fields ... */
    s_lmem[6 * MAX_SUBGROUPS + sg_id] = sg_den_nlog;
}
subgroupMemoryBarrierShared();   // C2 — subgroup-scope shared-memory release
memoryBarrierShared();
barrier();
```

Cross-backend gate on device 0 (NVIDIA) at API 1.4:

```text
metric                    max_abs_diff    mismatches
  integer_vif_scale0        1.000000e-06    0/48  OK
  integer_vif_scale1        1.000000e-06    0/48  OK
  integer_vif_scale2        1.526800e-02    45/48  FAIL
  integer_vif_scale3        2.000000e-06    0/48  OK
```

Identical to the PR #511 baseline. The subgroup-scope fence does not
reorder the elected-thread write any earlier than the workgroup-scope
release already does on this driver.

### C3: device-scope `controlBarrier` — BUILDS, NO EFFECT

```glsl
#extension GL_KHR_memory_scope_semantics : require
/* ... */
controlBarrier(gl_ScopeWorkgroup, gl_ScopeDevice,
               gl_StorageSemanticsShared,
               gl_SemanticsAcquireRelease);
```

The execution scope stays `Workgroup` (we do not need to synchronise
across workgroups; thread-0 of the same workgroup is the reader); the
**memory** scope is widened to `Device`, which forces a full L1+L2
cache flush on NVIDIA's hierarchy.

Cross-backend gate on device 0 (NVIDIA) at API 1.4:

```text
metric                    max_abs_diff    mismatches
  integer_vif_scale0        1.000000e-06    0/48  OK
  integer_vif_scale1        1.000000e-06    0/48  OK
  integer_vif_scale2        1.526800e-02    45/48  FAIL
  integer_vif_scale3        2.000000e-06    0/48  OK
```

5-run determinism on device 0 with C3:

```text
C3 run 1: scale2_num=  123171072105107.73, scale2_den=  -932360090756074.0
C3 run 2: scale2_num=  123171072105107.73, scale2_den= -1231427253580778.0
C3 run 3: scale2_num=  105578886056595.73, scale2_den= -1213835067532266.0
C3 run 4: scale2_num=  175964810086614.25, scale2_den= -1512885050487974.0
C3 run 5: scale2_num=  105578886056595.73, scale2_den=  -897175718659050.0
```

Same 5-run non-determinism, same magnitudes, same sign flips on `den`.

### C2 + C3 stacked — BUILDS, NO EFFECT

Belt-and-braces: subgroup-scope shared-memory fence followed by
device-scope `controlBarrier`. Same 45/48 FAIL, same magnitudes, same
5-run non-determinism. Listed for completeness; not separately
documented as a candidate because the brief enumerated three.

## Hypotheses

The empirical magnitudes (~10^14, sign flips on `den`) are not consistent
with the conventional "reading uninitialised lanes from shared memory"
failure mode that stronger fences would close — those would yield ~0 or
~previous-frame-value noise, not values 10 orders of magnitude above the
legitimate scale-2 sums (`+2.494358e+04`, `+2.522523e+04`).

Two remaining hypotheses, neither of which Phase 3b can test
in-session:

1. **NVIDIA driver int64-emulation bug in `subgroupAdd(int64_t)` for
   SCALE=2.** SCALE=2 has the smallest `valid` thread fraction (only
   even-coord threads contribute non-zero accumulator deltas in the
   `if (sigma12 >= 0)` branch). The subgroup `add` over a sparse-active
   int64 lane mask may hit a driver path that NVIDIA's CTS doesn't
   exercise, given that `GL_EXT_shader_subgroup_extended_types_int64` is
   an OpenGL extension layered over `VK_KHR_shader_subgroup_extended_types`
   and the NVIDIA Vulkan implementation may be lowering it to an
   emulation that mishandles inactive lanes.

2. **NVIDIA driver `OpAtomicIAdd` lowering bug for `subgroupAdd(int64_t)`.**
   Same hypothesis, alternative lowering: the `subgroupAdd` may
   internally lower to two `OpAtomicIAdd` invocations (lo + hi 32-bit
   halves) without proper synchronisation between them, producing the
   torn-write signature.

Both would be confirmed by replacing the `subgroupAdd(int64_t)` call
sites in `vif.comp` with a manual lane-by-lane reduction:

```glsl
// proposed fix, NOT applied in Phase 3b — needs a separate PR.
int64_t sg_reduce_int64(int64_t v) {
    int64_t r = v;
    for (uint mask = 1u; mask < gl_SubgroupSize; mask <<= 1u) {
        r += subgroupShuffleXor(r, mask);   // shuffleXor does work on int64.
    }
    return r;
}
```

This is out of scope for Phase 3b — the task brief enumerated the three
fence candidates and explicitly stopped after them — and would need
its own ADR + research digest. The hypothesis stays a hypothesis until
the manual-reduction patch is built and gated.

## Why the original device-map claim looked plausible

The device-map enumeration in `vmaf_vulkan_context_new` uses
`vkEnumeratePhysicalDevices` and an insertion sort by `devtype_score`
which is **stable inside the same score bucket**. Both NVIDIA RTX 4090
and Intel Arc A380 score `discrete = 100`. The order returned by the
loader (`vkEnumeratePhysicalDevices`) is determined by:

- Driver registration order in `/etc/vulkan/icd.d/`.
- Loader environment variables (`VK_LOADER_DRIVERS_SELECT`,
  `VK_LOADER_DRIVERS_DISABLE`, `VK_DRIVER_FILES`).
- The Mesa device-select layer (`VK_LAYER_MESA_device_select`) when
  loaded — it can re-order devices by `MESA_VK_DEVICE_SELECT` env var.

PR #511 was likely run on a host where one of those three knobs put
Arc first; this session's host puts NVIDIA first. **The host-side
order is not portable.** A future cross-backend gate that needs a
specific device should select by `deviceName` substring, not by index.
That is a separate hardening task tracked under
`T-VK-DEVICE-MAP-BY-NAME` in the backlog (out of scope for this PR).

## Conclusion + next step

- C1 not buildable; C2 / C3 buildable but ineffective on NVIDIA RTX 4090
  driver 595.71.05.
- Phase 3b is **concluded with deferral**. State.md row updated to
  `T-VK-VIF-1.4-RESIDUAL-NVIDIA-DEFERRED`. Per
  [`feedback_no_test_weakening`](../../.workingdir2/...) — the gate is
  not relaxed; the API-1.4 bump itself stays blocked until the
  manual-reduction patch lands or NVIDIA ships a driver fix.
- The fork's shipping default (API 1.3) gates **0/48 on every device**,
  so this row does not block any release.

## References

- `req` (the user's Phase 3b task brief): the three candidate-fix
  pseudo-code blocks and the stop-at-first-success process.
- [PR #511](https://github.com/VMAFx/vmafx/pull/511) — Phase 3 fix
  this digest builds on.
- [research-0089](0089-vulkan-vif-fp-residual-bisect-2026-05-08.md) —
  Phase 2 + Phase 3 dynamic dump that motivated PR #511.
- [ADR-0264](../adr/0264-vulkan-1-4-bump-blocked-on-fp-contraction.md)
  — parent decision (deferred bump, 2-step plan).
- [ADR-0269](../adr/0269-vif-ciede-precise-step-a.md) — Phase-3b
  status appendix added by this PR.
- GLSL 4.50 specification §4.10 "Memory Qualifiers" — basis for C1's
  build failure.
- SPV_KHR_vulkan_memory_model — basis for C3's `controlBarrier` lowering.
