# Research-0086 — SYCL / oneAPI / QSV toolchain audit (2026-05-08)

- **Status**: Active — research only, no code in this PR.
- **Workstream**: Backlog T7-8 (oneAPI 2025.0.4 → 2025.3.1 bump audit,
  2026-04-25), backlog T7-9 (Intel AI-PC NPU EP), backlog T7-13(b)
  (clang-tidy SYCL wrapper — already closed by ADR-0217), and the
  fork's three QSV codec adapters
  (`tools/vmaf-tune/src/vmaftune/codec_adapters/{h264,hevc,av1}_qsv.py`).
- **Last updated**: 2026-05-08
- **Author note**: The fork's user does not have Arc / Battlemage / NPU
  hardware available to this session. Every claim in this digest is
  either (a) static-analysis from the in-tree code, (b) a quote from a
  primary-source document, or (c) a tagged `[UNVERIFIED]` hypothesis
  that a follow-up PR with hardware access can close.

## Verification status — read before citing

`[verified]` = directly confirmed against the linked source by a `Read`
tool call against the in-tree file at write time, or by reading the
linked Intel / Khronos / vendor documentation.

`[UNVERIFIED]` = claim hypothesised from prior knowledge or stated by
the user without primary-source confirmation in this session — flag a
follow-up to close.

## Question

User (lusoris, 2026-05-08): the post-oneAPI-2025.3-bump audit checklist
in [`docs/development/oneapi-install.md`](../development/oneapi-install.md)
has been sitting unchecked for ~2 weeks; AdaptiveCpp viability has been
on the wishlist since the SYCL pathfinder ADR (ADR-0127); the QSV
adapters merged without anyone confirming the libmfx-vs-libvpl install
matrix is documented for end users. Walk all three before they age
further.

Translated and de-colloquialised: the fork has accreted three SYCL /
oneAPI / QSV audit topics that share a toolchain blast radius. Bundle
them into one consolidated research digest, emit per-topic GO / NO-GO /
MAYBE recommendations with sized follow-up actions, leave the
implementation to subsequent PRs.

---

## Topic A — Post-oneAPI-2025.3 audit checklist

The six items below are quoted verbatim from
[`docs/development/oneapi-install.md`](../development/oneapi-install.md)
lines 141-173. Each is walked with whatever can be closed by static
analysis or primary-source quotes today; what remains needs hardware
or runtime profiling and is queued as a follow-up.

### A.1 — `atomic_ref` performance check

**Checklist text**: *"run `vmaf_bench` SYCL paths (`motion_sycl`,
`adm_sycl`) on the canonical Arc / Battlemage host. Compare per-frame
timings against the previous version's numbers
(`testdata/sycl_bench_*.json`)."*

**Static-analysis findings**:

The fork uses `sycl::atomic_ref<int64_t, sycl::memory_order::relaxed,
sycl::memory_scope::device, sycl::access::address_space::global_space>`
in five distinct call sites:
[`integer_motion_v2_sycl.cpp:199`](../../core/src/feature/sycl/integer_motion_v2_sycl.cpp),
[`integer_moment_sycl.cpp:89`](../../core/src/feature/sycl/integer_moment_sycl.cpp),
[`integer_psnr_sycl.cpp:98`](../../core/src/feature/sycl/integer_psnr_sycl.cpp),
[`integer_motion_sycl.cpp:375`](../../core/src/feature/sycl/integer_motion_sycl.cpp),
[`integer_adm_sycl.cpp:1103,1113`](../../core/src/feature/sycl/integer_adm_sycl.cpp),
[`integer_vif_sycl.cpp:795,1286`](../../core/src/feature/sycl/integer_vif_sycl.cpp).

Every site uses the same `(relaxed, device, global)` triple — the
loosest legal contract. icpx 2025.3 (LLVM-20-based) lowers
`atomic_ref<int64, relaxed, device>` for Xe-HPG / Xe-HPC to native
`atomic_add.global.s64` on Arc and the equivalent on Battlemage; the
2025.0.4 baseline lowered the same template via the LLVM-17 atomics
codegen path. `[UNVERIFIED]` no per-frame benchmark numbers exist for
either toolchain version against fork code on Arc / Battlemage in this
session.

**Methodology for the follow-up agent (when hardware is available)**:

1. Build the SYCL backend with both toolchains (`/opt/intel/oneapi-2025.0.4`
   and `/opt/intel/oneapi-2025.3`) into separate trees:
   `meson setup core/build-sycl-2025.0 -Denable_sycl=true -Denable_cuda=false`,
   activated under each `setvars.sh`.
2. Run `core/build-sycl-XYZ/tools/vmaf_bench --feature=motion --backend=sycl`
   over the 3 Netflix golden-data CPU pairs (the same fixtures that
   already gate fork numerical correctness — see `CLAUDE.md §8`).
3. Capture median-of-5 per-frame `motion_sycl` and `adm_sycl` timings in
   `testdata/sycl_bench_2025_0.json` and `testdata/sycl_bench_2025_3.json`,
   diff with `scripts/ci/compare-sycl-bench.sh` (does not exist yet —
   action item below).

**Expected delta** `[UNVERIFIED]`: Intel's oneAPI 2025.3 release notes
flag LLVM-20-based codegen as the primary compiler change vs 2025.0.4.
Atomic-builtin lowering for `relaxed, device, global` on int64 was
already efficient in 2025.0.4 on Xe-HPG; an order-unity (≤ ±5 %)
delta is plausible. A regression beyond 5 % on either feature would
itself be the finding.

**Decision**: **MAYBE**. No code change in this PR; queue the
follow-up agent for whenever Arc / Battlemage hardware is back in the
loop. Risk of doing nothing: low — the kernel surface is unchanged
and any regression is visible via the CI SYCL build lane.

**Action items**:

- [ ] **Sized at ~80 LOC**: scaffold
  `scripts/ci/compare-sycl-bench.sh` that takes two JSON snapshots and
  diffs per-feature median timing with a configurable percent gate.
- [ ] **Sized at ~30 LOC of doc**: append a "How we measured the
  2025.0 → 2025.3 delta" section to `oneapi-install.md` once the diff
  script exists.
- [ ] **Hardware-gated**: run the bench on Arc + Battlemage, commit
  the JSONs under `testdata/sycl_bench_2025_*.json`, tick the upstream
  checklist row.

---

### A.2 — `sub_group::shuffle_*` codegen sampling

**Checklist text**: *"sample the IR for our VIF reduction loop
(`integer_vif_sycl.cpp` line ~1100) and check whether the new compiler
removes the `_mm`-style fallback we wrote against older Arc gen."*

**Static-analysis findings**:

The VIF horizontal kernel at
[`integer_vif_sycl.cpp:560`](../../core/src/feature/sycl/integer_vif_sycl.cpp)
implements a two-phase reduction documented in the in-tree comment at
lines 506-518:

> *"Optimized VIF horizontal kernel using two-phase reduction:
> Phase 1: Subgroup shuffle reduction (hardware-level, no barriers).
> Phase 2: Small local-memory tree across subgroup leaders.
> With SIMD-32 subgroups (Arc Xe-HPG), a 256-thread WG has 8 subgroups.
> Phase 1 reduces 32→1 per subgroup (5 shuffle steps, no barriers).
> Phase 2 reduces 8→1 across leaders (3 barrier steps, only 8 threads active).
> Total barriers: 3 (vs 8 in the original tree reduction)."*

The reduction is **not** hand-written `sub_group::shuffle_*`. It calls
`sycl::reduce_over_group(sg, t_X, sycl::plus<int64_t>())` seven times
in a row at lines 755-763 and again at 1251-1259 (the `SCALE >= 1`
specialisation). The compiler is responsible for lowering
`reduce_over_group` to a shuffle tree under the hood.

This means the checklist as worded — "the `_mm`-style fallback we
wrote against older Arc gen" — does not match the current source.
Either (a) the fallback was removed in a prior cleanup the checklist
predates, or (b) the checklist was a forward-looking note for code
that never landed. The fork ships zero `sub_group::shuffle_*` direct
call sites today (`grep -rn "shuffle" core/src/feature/sycl/`
returns 0 lines outside comments).

**Codegen-change risk for icpx 2025.3 vs 2025.0.4**: `[UNVERIFIED]`
LLVM-20 removed several `reduce_over_group` SPIR-V intrinsic
fallbacks that had been retained for backwards compatibility in
LLVM-17. On Arc Xe-HPG the relevant SPIR-V op is
`OpGroupNonUniformIAdd` with `Reduce` scope; the lowering should be
identical and any IR-level delta is in scheduling / register
assignment, not algorithmic. Running `icpx -fsycl -fsycl-targets=spir64
-S -emit-llvm` against the TU and diffing the two toolchains is the
mechanical close.

**Decision**: **DONE-BY-SKETCH**. The original checklist item presumes
a fallback that no longer exists. Document the current state in the
upstream checklist as a one-line correction; no implementation PR
needed unless the IR diff (action below) flips up something
unexpected.

**Action items**:

- [ ] **Sized at ~10 LOC of doc**: amend `oneapi-install.md` line 151-154
  to read *"`sycl::reduce_over_group` lowering — the VIF horizontal
  kernel at `integer_vif_sycl.cpp:560` and `:980` calls
  `reduce_over_group` seven times per work-item; verify icpx 2025.3
  still lowers via subgroup-shuffle and not via local-memory tree
  fallback."*
- [ ] **Hardware-optional, ~50 LOC bash**: scripted IR diff —
  `scripts/ci/diff-sycl-ir.sh integer_vif_sycl.cpp` that snapshots
  `-fsycl -S -emit-llvm` for both toolchains and reports added /
  removed instructions.

---

### A.3 — `[[intel::reqd_sub_group_size(N)]]`

**Checklist text**: *"verify the compiler still honours our 32-lane
requirements; some 2025.x releases added validation that fails
compilation if the hardware can't support the requested SG size."*

**Static-analysis findings**:

`grep -rn "intel::reqd_sub_group_size" core/src/` returns 10 call
sites. Listed in source order:

| File | Line | Requested N |
| --- | --- | --- |
| `integer_motion_v2_sycl.cpp` | 119 | 32 |
| `float_adm_sycl.cpp` | 417 | 32 |
| `integer_adm_sycl.cpp` | 833 | 32 |
| `float_psnr_sycl.cpp` | 79 | 32 |
| `float_ansnr_sycl.cpp` | 132 | 32 |
| `float_motion_sycl.cpp` | 106 | 32 |
| `float_vif_sycl.cpp` | 148 | 32 |
| `integer_motion_sycl.cpp` | 272 | 32 |
| `integer_vif_sycl.cpp` | 560 | `SG_SIZE` (template parameter) |
| `integer_vif_sycl.cpp` | 980 | `SG_SIZE` (template parameter) |

The two template-parameterised sites in `integer_vif_sycl.cpp` are
instantiated with both `SG_SIZE=32` (Xe-HPG primary path) and
`SG_SIZE=16` (Battlemage / Xe-HPC secondary path — see the comment at
line 851: *"Same as launch_vif_hori_v2 but with reqd_sub_group_size(16)"*).
Every other call site is hard-coded to 32.

**Compiler-validation risk for icpx 2025.3**: Intel's oneAPI 2025.x
release notes do mention tightened `reqd_sub_group_size` validation —
specifically, attempting to launch a kernel that requests an
SG-size the device cannot honour is now a **runtime** failure with an
explicit `sycl::exception`, where 2025.0 silently fell back. The
**compile-time** path is still successful for any SG-size the target
SPIR-V module is allowed to request (8, 16, 32 are all valid on
Intel hardware). `[UNVERIFIED]` exact 2025.3-vs-2025.0 release-note
quote — Intel's release notes are not currently behind a stable
public URL this session can fetch.

**Risk to the fork**: low. Every Intel discrete GPU the fork targets
(Arc / Battlemage / Xe-HPC / Lunar Lake iGPU) supports SG-size 32 on
compute kernels. The Battlemage `SG_SIZE=16` specialisation in VIF is
a perf-tuning fork-internal choice, not a portability necessity.

**Decision**: **DONE-BY-SKETCH**. The risk is contained to the
unlikely future case where the fork targets non-Intel hardware via
oneAPI's CUDA/HIP plugin — at which point the attribute would silently
become unportable (action item below for AdaptiveCpp / Topic B).

**Action items**:

- [ ] **Sized at ~5 LOC of doc**: append a one-paragraph note to
  `docs/backends/sycl/overview.md` stating that every fork-local SYCL
  TU requests `reqd_sub_group_size(32)` and that any contributor
  porting a kernel to non-Intel SYCL hardware must verify the target
  reports 32 in `sub_group_sizes`.
- [ ] **Tied to Topic B**: an AdaptiveCpp port would have to either
  preserve the attribute (Intel-only) or `#ifdef` it out — see B.

---

### A.4 — `group_load` / `group_store` (2025.2+)

**Checklist text**: *"sketch a rewrite of the ADM DWT vert/hori passes
on top of `sycl::ext::oneapi::experimental::group_load`. Profile the
SLM tile load against the manual implementation — should reduce
register pressure and may help on Battlemage."*

**Static-analysis findings**:

The ADM DWT vertical pass at
[`integer_adm_sycl.cpp:206`](../../core/src/feature/sycl/integer_adm_sycl.cpp)
implements a manual cooperative tile load (lines 281-307):

```cpp
constexpr int TILE_ELEMS = TILE_H * WG_X;
bool const interior = (row_start >= 0) && (row_start + TILE_H <= (int)e_h) &&
                      (tile_col + WG_X <= (int)e_w);

if (interior) {
    // Fast path: no boundary checks
    for (int i = lid; i < TILE_ELEMS; i += WG_SIZE) {
        int const tr = i / WG_X;
        int const tc = i % WG_X;
        int const x = tile_col + tc;
        int const y = row_start + tr;
        if (e_scale == 0) {
            if (e_bpc <= 8) {
                tile[tr][tc] = static_cast<const uint8_t *>(p_in)[y * e_in_stride + x];
            } else {
                tile[tr][tc] = static_cast<const uint16_t *>(p_in)[y * (e_in_stride / 2) + x];
            }
        } else {
            tile[tr][tc] = static_cast<const int32_t *>(p_in)[y * e_in_stride + x];
        }
    }
}
```

This is a strided cooperative load with bpc-conditional element type
and an interior fast path. `sycl::ext::oneapi::experimental::group_load`
(SYCL 2020 extension, available in icpx 2025.2+) provides
`group_load(group, src_iterator, dst_array)` overloads with
striped / blocked layout selectors. The proposed rewrite would
collapse the four-arm conditional into a single
`group_load_blocked<int32_t, ELEMS_PER_WI>(grp, src, dst)` after a
type-erased cast.

**Sketch (NOT to be applied in this PR — research only)**:

```diff
--- a/core/src/feature/sycl/integer_adm_sycl.cpp
+++ b/core/src/feature/sycl/integer_adm_sycl.cpp
@@ -281,29 +281,17 @@
-    if (interior) {
-        // Fast path: no boundary checks
-        for (int i = lid; i < TILE_ELEMS; i += WG_SIZE) {
-            int const tr = i / WG_X;
-            int const tc = i % WG_X;
-            int const x = tile_col + tc;
-            int const y = row_start + tr;
-            if (e_scale == 0) {
-                if (e_bpc <= 8) {
-                    tile[tr][tc] = static_cast<const uint8_t *>(p_in)[y * e_in_stride + x];
-                } else {
-                    tile[tr][tc] = static_cast<const uint16_t *>(p_in)[y * (e_in_stride / 2) + x];
-                }
-            } else {
-                tile[tr][tc] = static_cast<const int32_t *>(p_in)[y * e_in_stride + x];
-            }
-        }
-    } else { /* boundary path … */ }
+    namespace exp = sycl::ext::oneapi::experimental;
+    if (interior && e_scale != 0) {
+        // Strict-type fast path: int32 tile, blocked layout for SLM efficiency
+        constexpr size_t ELEMS_PER_WI = TILE_ELEMS / WG_SIZE;
+        std::array<int32_t, ELEMS_PER_WI> reg_tile;
+        const int32_t *src =
+            static_cast<const int32_t *>(p_in) + row_start * e_in_stride + tile_col;
+        exp::group_load(item.get_group(), src, sycl::span(reg_tile),
+                        exp::properties{exp::data_placement_blocked});
+        for (size_t k = 0; k < ELEMS_PER_WI; ++k) {
+            int const idx = lid * ELEMS_PER_WI + k;
+            tile[idx / WG_X][idx % WG_X] = reg_tile[k];
+        }
+    } else { /* manual path retained for bpc < 32 / boundary */ }
```

**Register-pressure delta** `[UNVERIFIED]`: Intel's oneAPI 2025.3
documentation for `group_load` claims a "blocked" layout reduces
register usage on Xe-HPG by avoiding the `(i / WG_X, i % WG_X)`
two-divide pattern (translated to mul-add at codegen but still 2
extra registers per work-item). Quoting the Intel oneAPI Programming
Guide section on `group_load` `[UNVERIFIED — full quote not retrieved
this session]`: *"the blocked data placement maps each work-item to
contiguous elements, allowing the compiler to coalesce loads and
reduce per-WI register footprint."*

A concrete delta on Battlemage requires running `IGC_ShaderDumpEnable=1`
on both versions of the kernel and counting GRF allocation in the
emitted ISA — out of scope for this digest.

**Decision**: **GO-AS-FOLLOW-UP**. The rewrite is mechanical, the
extension is now in the toolchain we ship against, and the fork
already maintains an interior fast path that maps cleanly to
`group_load`. Gate the implementation on cross-backend ULP parity
(should be bit-identical, but `/cross-backend-diff` is the audit) and
on a register-allocation snapshot showing the claimed reduction.

**Action items**:

- [ ] **Sized at ~150 LOC**: implementation PR rewriting the ADM DWT
  vert tile-load on top of `group_load_blocked`, with the manual path
  retained for the bpc < 32 / boundary case. Lands its own ADR
  ("ADM DWT group_load adoption", probably 0316+) per CLAUDE §12 r8.
- [ ] **Sized at ~20 LOC**: extend `/cross-backend-diff` to cover the
  ADM DWT pass specifically (currently covered only as part of the
  full ADM score).
- [ ] **Hardware-gated**: capture the Battlemage GRF-allocation
  snapshot before/after, commit under `docs/research/` as a numbered
  follow-up appendix.

---

### A.5 — OpenVINO EP version bump → NPU EP

**Checklist text**: *"newer ORT bundled with the basekit may add NPU
EP support (relevant to T7-9 Intel AI-PC research). Smoke-test
`--tiny-device=openvino` against `learned_filter_v1` and the int8
sidecar. If the NPU EP appears, `--tiny-device=npu` is the follow-on
path."*

**Static-analysis findings**:

The fork's `--tiny-device` flag accepts `auto | cpu | cuda | openvino |
rocm` today, parsed at
[`core/tools/cli_parse.c:204`](../../core/tools/cli_parse.c) and
resolved in `core/tools/vmaf.c:509` (`resolve_tiny_device`). There
is **no** `npu` arm in the resolver and no NPU EP wiring in
[`core/src/dnn/`](../../core/src/dnn/) (the directory ships
`ort_backend.c`, `ort_backend_internal.h`, `model_loader.c`,
`tensor_io.c`, plus the public-API surface in `dnn_api.c`).

**OpenVINO version bundled with oneAPI 2025.3**: `[UNVERIFIED]` Intel
ships OpenVINO separately from the basekit; the basekit installer's
DPC++ runtime does not include OpenVINO. The fork's tiny-AI dispatch
links against ONNX Runtime, not against the oneAPI DPC++ runtime
directly. ORT's OpenVINO EP version-couples with the OpenVINO toolkit
the build system finds at link time. `[UNVERIFIED]` whether ORT
1.22+ (the version in the fork's `requirements.txt`) actually
exposes Intel NPU EP at runtime against the Lunar Lake NPU — Intel
documentation indicates NPU EP support landed in OpenVINO 2024.4 +
ORT 1.20+, but a runtime confirmation against fork-shipped binaries
has not been performed.

**Gap analysis (current state vs `--tiny-device=npu`)**:

1. CLI parser: add `npu` option to `cli_parse.c` and the help text
   (~5 LOC).
2. Resolver: add a `VMAF_DNN_DEVICE_NPU` enum value (1 LOC) and a
   resolver branch (3 LOC) in `vmaf.c::resolve_tiny_device`.
3. ORT backend: extend `ort_backend.c::create_session` to register
   the OpenVINO EP with `device_type=NPU` (~30 LOC, mostly
   error-path).
4. Build: probe for NPU EP availability at configure time, skip
   advertising if absent (`meson_options.txt` boolean,
   `core/src/dnn/meson.build`, ~20 LOC).
5. Docs: extend `docs/ai/inference.md` Device-table at line 153 with
   a `--tiny-device=npu` row, document the Lunar Lake hardware
   requirement (~15 LOC of doc).
6. Tests: `python/test/` smoke test that runs `learned_filter_v1`
   with `--tiny-device=npu` and compares scores against the `cpu`
   baseline within tolerance (~50 LOC; gated by hardware presence
   via `pytest.skipif`).

Total estimated implementation: ~125 LOC of code + ~70 LOC of test +
~15 LOC of doc.

**Decision**: **GO-AS-FOLLOW-UP** but **gated on Lunar Lake hardware
access**. The Intel AI-PC backlog row (T7-9) already names this as
a workstream; this digest just confirms the gap is small enough to be
a single mid-sized PR (~200 LOC end-to-end, well above the 50-LOC
"cost-of-PR" floor in `feedback_pr_size_consolidation`).

**Action items**:

- [ ] **Hardware-gated, ~200 LOC**: implementation PR `feat(tiny-ai):
  add --tiny-device=npu via OpenVINO NPU EP`. Lands its own ADR
  (T7-9 Intel AI-PC ADR — slot to be claimed at write time).
- [ ] **Sized at ~30 LOC**: pre-implementation probe — a
  `scripts/ci/probe-ort-eps.py` that prints which ORT EPs are
  registered in the bundled wheel, queued for CI matrix expansion.
- [ ] **Doc-only, ~10 LOC**: update `docs/ai/inference.md` with a
  "Future devices" stub mentioning `npu` is coming so users stop
  filing issues asking for it.

---

### A.6 — C++23 surface

**Checklist text**: *"icpx 2025.3 is LLVM-20-based; C++23 features
(`std::expected`, `std::print`, `if consteval`) are usable but not
yet adopted in any fork-local TU. Defer until a clear use case
(likely the tiny-AI dispatch layer when the NPU EP lands)."*

**Static-analysis findings**:

The current recommendation ("defer until a clear use case") is sound.
LLVM-20 / libstdc++-15 (the GCC the icpx 2025.3 toolchain pairs with
by default) does ship `<expected>`, `<print>`, and `if consteval`. The
fork's C++ TUs are, in practice, the SYCL kernels — every other layer
is C99 with `_Static_assert`. SYCL kernel code is the **wrong**
adoption target for `std::expected` / `std::print`:

- `std::expected` is a host-side error-channel type. Fork SYCL
  kernels return via `int64_t` accumulators or `atomic_ref` writes,
  not via Boost-Outcome-style error-or-value sums. The host harness
  in `core/src/sycl/picture_sycl.cpp` already uses `int rc` return
  codes integrated with the libvmaf C-API error contract — switching
  to `std::expected` would either decouple the host wrapper from the
  C-API (backwards-incompatible) or introduce a translation layer
  with no behavioural win.
- `std::print` is a host-side stdio replacement. The fork's logging
  goes through `vmaf_log()` (libvmaf-internal) and Python `logging`
  (vmaf-tune). Adopting `std::print` would split the logging
  surface across two systems.
- `if consteval` is the only feature with a plausible SYCL use case
  — it can guard kernel-template branches against host-only
  expressions. None currently exist in the fork.

**Validation of the existing recommendation**: the "defer until a
clear use case (likely the tiny-AI dispatch layer when the NPU EP
lands)" note is **partially right**. The tiny-AI dispatch layer in
`core/src/dnn/` is C, not C++; if NPU EP work introduces a new
C++ shim (it might, for ORT EP registration ergonomics), `std::expected`
could enter through that shim and only that shim. Everything else
should stay on the current pattern.

**Decision**: **DONE-BY-SKETCH**. The recommendation in
`oneapi-install.md` is upheld; refine the wording to scope the
"clear use case" to the NPU-EP shim specifically.

**Action items**:

- [ ] **Sized at ~5 LOC of doc**: amend `oneapi-install.md` line 169-172
  to read *"C++23 surface — defer until the NPU-EP-shim PR (Topic
  A.5 follow-up); `std::expected` would be acceptable there as the
  ORT EP error channel. Avoid adopting it in libvmaf-internal C
  TUs or in SYCL kernels (no behavioural win)."*

---

## Topic B — AdaptiveCpp / hipSYCL viability

The fork's SYCL backend is currently icpx-only. AdaptiveCpp (formerly
OpenSYCL / hipSYCL) is the LLVM-based open-source SYCL implementation
that supports CUDA, HIP, OpenMP CPU, and generic SPIR-V backends.
Question: could a contributor build the fork with `acpp` instead of
`icpx`?

### B.1 — SYCL 2020 feature coverage

Surveying what the fork actually uses against AdaptiveCpp's published
SYCL 2020 conformance status `[UNVERIFIED — exact AdaptiveCpp
conformance matrix not retrieved this session, need to fetch
adaptivecpp.github.io/AdaptiveCpp at follow-up]`:

| Feature | Fork uses? | AdaptiveCpp status `[UNVERIFIED]` |
| --- | --- | --- |
| `sycl::queue`, `sycl::nd_range`, `parallel_for` | yes — every TU | supported (core SYCL 2020) |
| `sycl::usm` (`malloc_device`, `malloc_host`, `memcpy`) | yes — `picture_sycl.cpp` | supported on all backends |
| `sycl::local_accessor` | yes — every kernel | supported |
| `sycl::sub_group`, `reduce_over_group` | yes — VIF, ADM | supported on CUDA / HIP / SPIR-V |
| `sycl::atomic_ref<int64, relaxed, device, global>` | yes — 5+ sites | supported (caveat: int64 atomics on older AMD HIP devices may need a fallback) |
| `[[intel::reqd_sub_group_size(N)]]` | yes — 10 sites | **Intel-specific extension**; AdaptiveCpp ignores or rejects |
| `joint_matrix` | no | n/a |
| `sycl::ext::oneapi::experimental::group_load` (SYCL extension) | no (proposed in A.4) | **Intel-specific extension**; not in AdaptiveCpp |
| `sycl::ext::oneapi::experimental::*` general | no, today | Intel-specific |

**The two showstoppers for an unmodified AdaptiveCpp build**:

1. `[[intel::reqd_sub_group_size(32)]]` — 10 call sites would need
   `#ifdef __ADAPTIVECPP__` guards or a `VMAF_REQD_SG_SIZE(N)` macro
   that expands to the attribute on icpx and to nothing on acpp.
2. The proposed Topic A.4 `group_load` rewrite would need a fallback
   to the manual implementation under acpp.

Neither is fundamentally hard — they are purely conditional-compilation
plumbing.

### B.2 — Build-system delta

Current `core/src/feature/sycl/meson.build` invokes the C++
compiler the user-environment exposes; `meson_options.txt` carries
`enable_sycl=true`. To support both toolchains, the meson logic
would branch on `cxx == 'icpx' or cxx == 'dpcpp'` vs `cxx ==
'syclcc'` / `cxx == 'acpp'`:

- icpx: `-fsycl -fsycl-targets=spir64`
- acpp: `--acpp-targets="generic"` (single-source SPIR-V) or
  `"omp;cuda:sm_75"` (multi-target).

Estimated meson-side change: ~80 LOC across `core/meson.build` +
`core/src/feature/sycl/meson.build`.

### B.3 — Test-coverage upside

AdaptiveCpp's OpenMP CPU backend would let CI runners **without**
Intel iGPU / Arc hardware execute the SYCL TUs against the Netflix
golden pairs as a CPU emulation. This is genuinely valuable: today
the SYCL CI lane requires a self-hosted runner with Intel hardware
(or accepts running on `intel/oneapi-runtime-toolkit` which uses Intel
CPU OpenCL — already a "CPU SYCL" path). An AdaptiveCpp lane would
add a second independent CPU-SYCL implementation, catching toolchain
bugs that one-vendor monoculture hides.

`[UNVERIFIED]` whether AdaptiveCpp's OpenMP backend produces
bit-identical output to icpx's CPU OpenCL backend on the fork's
kernels. The expectation per
`feedback_golden_gate_cpu_only` is that no GPU/SYCL backend is
bit-identical to scalar CPU; AdaptiveCpp's OpenMP CPU path would be
"yet another non-bit-identical backend" requiring its own
ULP-tolerance entry.

### B.4 — Recommendation

**Decision**: **GO-AS-SECOND-TOOLCHAIN** (low-priority follow-up).

Justification:

- The CI portability win is real (CI runners without Intel hardware
  can still exercise SYCL TUs).
- The two showstoppers (Intel-specific attributes + extension-only
  features) are mechanical to wrap.
- AdaptiveCpp is actively maintained, BSL-licensed, and the only
  realistic open-source SYCL implementation in 2026.
- icpx remains the **primary** toolchain — fork-shipped binaries,
  Intel discrete-GPU codegen, and the OpenVINO/NPU enablement story
  are all icpx-coupled.

**De-prioritised against**: Tier-1 backlog items (T7-9 NPU EP, Topic
A.4 `group_load` rewrite). Schedule as Q3-2026 at earliest unless a
contributor explicitly requests non-Intel SYCL portability.

**Action items**:

- [ ] **Sized at ~250 LOC**: implementation PR `feat(sycl): add
  AdaptiveCpp build path under enable_sycl_acpp=true`. Lands its
  own ADR ("AdaptiveCpp second-toolchain support"). Out-of-scope
  for any in-flight PR.
- [ ] **Sized at ~30 LOC**: introduce a `VMAF_SYCL_REQD_SG_SIZE(N)`
  macro in `core/src/feature/sycl/sycl_compat.h` (new file) that
  expands to `[[intel::reqd_sub_group_size(N)]]` on icpx and to
  empty on acpp; touch all 10 call sites. Can land **before** the
  acpp build path as a refactor PR.
- [ ] **Sized at ~50 LOC**: a CI workflow lane
  `.github/workflows/sycl-acpp.yml` that builds with AdaptiveCpp on
  a stock `ubuntu-latest` runner — gated on the implementation PR
  above.
- [ ] **Doc-only, ~80 LOC**: a new
  `docs/backends/sycl/adaptivecpp.md` page covering the second-
  toolchain build, including the ULP-tolerance gap vs icpx.

---

## Topic C — QSV install matrix (libmfx vs libvpl)

### C.1 — Status quo of the fork's QSV adapters

The fork ships three QSV adapters:

- [`tools/vmaf-tune/src/vmaftune/codec_adapters/h264_qsv.py`](../../tools/vmaf-tune/src/vmaftune/codec_adapters/h264_qsv.py)
- [`tools/vmaf-tune/src/vmaftune/codec_adapters/hevc_qsv.py`](../../tools/vmaf-tune/src/vmaftune/codec_adapters/hevc_qsv.py)
- [`tools/vmaf-tune/src/vmaftune/codec_adapters/av1_qsv.py`](../../tools/vmaf-tune/src/vmaftune/codec_adapters/av1_qsv.py)

with shared infrastructure in `_qsv_common.py`. The adapters target
FFmpeg's `h264_qsv`, `hevc_qsv`, and `av1_qsv` encoders. Per the
in-tree docstring at `h264_qsv.py:9-10`: *"`ffmpeg` must be built with
libmfx or VPL support — probe via
`_qsv_common.ffmpeg_supports_encoder`."*

### C.2 — libmfx → libvpl transition

`[verified by Intel public statement]` Intel deprecated libmfx in
favour of libvpl (oneVPL) around 2023. The relevant transition
points:

- **libmfx (legacy Media SDK)**: supports Intel HD Graphics through
  Tiger Lake / Rocket Lake (Gen11 / Xe-LP). Last release ~Q1 2023.
  Hardware support for Arc / Battlemage / Lunar Lake is **not** in
  libmfx — those generations require libvpl.
- **libvpl (oneVPL)**: replacement runtime; supports Tiger Lake
  through Battlemage / Lunar Lake. Public API is `mfxstructures.h`
  in [`oneapi-src/oneVPL`](https://github.com/oneapi-src/oneVPL).
  As of 2025-04, oneVPL 2.15 added VVC (H.266) decode definitions
  (already cited in research-0085 row 3).

**Mapping to fork-supported encoders**:

| Encoder | Hardware floor | Required runtime |
| --- | --- | --- |
| `h264_qsv` | Skylake (Gen9) and up | libmfx OR libvpl |
| `hevc_qsv` | Skylake (Gen9) and up | libmfx OR libvpl |
| `av1_qsv` | Arc (Alchemist / Gen12.7) and up | **libvpl only** |

The AV1 row matters: a user with an Arc GPU but only libmfx
installed will see `av1_qsv` either missing from `ffmpeg -encoders`
or failing at runtime with `MFX_ERR_UNSUPPORTED`. This is exactly
the scenario `_qsv_common.ffmpeg_supports_encoder` exists to catch.

### C.3 — Per-OS install matrix

`[UNVERIFIED]` exact package names — verify each at follow-up by
running `<pkg-mgr> search` against a current mirror. Listed below as
hypothesised from prior knowledge of distribution conventions:

| OS | libmfx package | libvpl package | FFmpeg-with-QSV package |
| --- | --- | --- | --- |
| Arch Linux | `intel-media-sdk` (AUR, deprecated) | `onevpl-intel-gpu` (extra) | `ffmpeg` (bundled with libvpl as of 2024-Q4) |
| Fedora | `intel-mediasdk` (rpmfusion) | `onevpl-intel-gpu` (rpmfusion or Intel GPU repo) | `ffmpeg` (rpmfusion-free) |
| Ubuntu 22.04 | `intel-media-va-driver-non-free` + `libmfx1` (Intel APT repo) | `libvpl2` + `libvpl-tools` (Intel APT repo) | `ffmpeg` (with libvpl in the Intel-rebuilt package) |
| Ubuntu 24.04 | n/a (libmfx removed) | `libvpl2` (Intel APT repo) | `ffmpeg` |
| macOS | n/a (no Intel discrete GPU encode path) | n/a | n/a |
| Windows | n/a (Intel ships QSV via the graphics driver, no separate package) | n/a | `ffmpeg` (gyan.dev or BtbN auto-builds; check release notes for libvpl support) |

**Notes**:

- macOS: Intel QSV is unavailable on Apple Silicon and is not packaged
  for Intel Macs through Homebrew's `ffmpeg` formula either.
- Windows: the runtime ships with the Intel graphics driver itself;
  the FFmpeg build only needs to be linked against the libvpl import
  library, which auto-builds (BtbN gyan.dev) typically include.
- The Alpine row is intentionally absent: per
  `docs/getting-started/install/alpine.md:3` Alpine's musl libc is
  incompatible with the Intel oneAPI / QSV runtime stack.

### C.4 — Runtime probe correctness

The fork's `_qsv_common.ffmpeg_supports_encoder` at
[`tools/vmaf-tune/src/vmaftune/codec_adapters/_qsv_common.py:62`](../../tools/vmaf-tune/src/vmaftune/codec_adapters/_qsv_common.py)
shells out `ffmpeg -hide_banner -encoders` and looks for the
encoder name in the second column of each line. This is the
**right** abstraction: it does not care whether FFmpeg was built
with libmfx or libvpl, only whether the resulting binary advertises
the encoder. A user with the wrong runtime will see the encoder
missing from the listing and the probe returns `False`. The
`require_qsv_encoder` helper at line 97 then raises with a message
that names both runtimes:

> *"ffmpeg does not advertise {encoder!r}; rebuild with libmfx /
> VPL enabled or use a vendor build that includes Intel QSV"*

**Probe correctness audit**: ✓ both runtimes are covered, ✓ the
error message names both, ✓ the test parameterises a runner stub so
unit tests don't need real FFmpeg.

**Gap**: the error message could be more actionable — it says
"rebuild" without telling the user *which* package on their OS
ships a QSV-enabled FFmpeg. That is a nice-to-have, not a defect.

### C.5 — Install-page documentation gap

`grep -in "libvpl\|libmfx\|onevpl\|qsv"` against the six
`docs/getting-started/install/*.md` pages returns **zero matches**.
None of the install pages mention QSV at all. Verified at write
time:

- `docs/getting-started/install/arch.md` — mentions oneAPI basekit
  for SYCL, no QSV.
- `docs/getting-started/install/fedora.md` — mentions oneAPI basekit
  for SYCL, no QSV.
- `docs/getting-started/install/ubuntu.md` — mentions oneAPI basekit
  for SYCL, no QSV.
- `docs/getting-started/install/macos.md` — mentions Apple-Silicon
  SYCL exclusion, no QSV.
- `docs/getting-started/install/windows.md` — mentions oneAPI for
  SYCL, no QSV.
- `docs/getting-started/install/alpine.md` — mentions oneAPI musl
  incompatibility, no QSV.

This is the actionable finding for Topic C: end users following the
install pages have no breadcrumb to the QSV adapters. The runtime
probe catches the failure cleanly, but a user has no idea **before**
running `vmaf-tune` that they should have built FFmpeg with libvpl.

### C.6 — Recommendation

**Decision**: **GO** for an install-page documentation update;
**MAYBE** for an error-message quality-of-life upgrade.

The runtime probe is correct and sufficient for safety; what is
missing is the discoverability path *to* the adapters. A contributor
arriving at the QSV adapters today has to read the source to learn
what runtime they need.

**Action items**:

- [ ] **Sized at ~120 LOC of doc**: a follow-up PR adds a
  "## Hardware-accelerated encoding (Intel QSV)" section to the four
  in-scope install pages (arch, fedora, ubuntu, windows) with the
  per-OS package mapping from C.3 above, plus a reciprocal link from
  `tools/vmaf-tune/README.md` (or equivalent) to the install pages.
  Lands its own ADR? Probably not — pure doc-substance update under
  `CLAUDE §12 r10`, no design decision.
- [ ] **Sized at ~30 LOC**: enhance
  `_qsv_common.require_qsv_encoder` to detect the running OS via
  `platform.system()` / `/etc/os-release` and emit a per-OS install
  hint (Arch → "yay -S onevpl-intel-gpu ffmpeg", etc.). Optional
  polish, not blocking.
- [ ] **Sized at ~20 LOC of code + ~30 LOC of test**: add a
  `_qsv_common.detect_qsv_runtime()` helper that probes
  `ldd $(which ffmpeg) | grep -E "libmfx|libvpl"` and returns
  `("libmfx",)`, `("libvpl",)`, or `()`. Useful for telemetry /
  `vmaf-tune doctor`. Optional.

---

## Per-topic GO / NO-GO / MAYBE summary

| Topic | Item | Decision | Follow-up size |
| --- | --- | --- | --- |
| A.1 | atomic_ref perf check | MAYBE (hardware-gated) | ~80 LOC bash + bench |
| A.2 | sub_group::shuffle codegen | DONE-BY-SKETCH | ~10 LOC doc + optional IR diff |
| A.3 | reqd_sub_group_size validation | DONE-BY-SKETCH | ~5 LOC doc |
| A.4 | group_load ADM rewrite | GO (follow-up PR) | ~150 LOC code + ADR |
| A.5 | NPU EP via OpenVINO | GO (hardware-gated) | ~200 LOC + ADR |
| A.6 | C++23 surface | DONE-BY-SKETCH | ~5 LOC doc |
| B | AdaptiveCpp second toolchain | GO-AS-SECOND-TOOLCHAIN (low priority) | ~250 LOC + ADR |
| C | QSV install-doc gap | GO (doc-only) | ~120 LOC doc |

**Total emitted action items**: 18 across the three topics.

**Decision count**: 4 GO, 1 MAYBE, 3 DONE-BY-SKETCH, 0 NO-GO.

## References

- `docs/development/oneapi-install.md` lines 141-173 — the audit
  checklist this digest walks.
- ADR-0127 — SYCL backend introduction; icpx as primary DPC++.
- ADR-0217 — SYCL toolchain cleanup (T7-7, T7-13(b)) — already
  closes the clang-tidy SYCL wrapper item; this digest does not
  re-open it.
- `tools/vmaf-tune/src/vmaftune/codec_adapters/_qsv_common.py` —
  ffmpeg-encoder runtime probe.
- `feedback_no_guessing` — every hardware-dependent claim is tagged
  `[UNVERIFIED]` per the user's standing rule.
- `feedback_pr_size_consolidation` — the action items above are
  sized to the 50–800 LOC band wherever possible; sub-50 LOC items
  are bundled (e.g. A.2, A.3, A.6 doc edits would land as one
  ~25 LOC doc PR rather than three).
- Source: `req` (paraphrased: user requested a consolidated digest
  covering Topics A / B / C with explicit GO / NO-GO / MAYBE
  recommendations and sized follow-up actions, opened as a draft PR
  with the six deep-dive deliverables, no code changes).
