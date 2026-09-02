Scaffold audit P2: nine half-finished implementation and tracking gaps resolved.
(1) Expose `adm_p_norm` option on integer ADM extractor to match float_adm interface;
document SIMD path gap as T-INTEGER-ADM-P-NORM-SIMD-GAP. (2) Gate `float_vif_hip`
auto-dispatch behind `enable_float_vif_hip_autodispatch` Meson option (default OFF)
pending T7-10c picture-pool plumbing. (3) File T-SYCL-CLANG-TIDY-DISABLED with
reactivation criterion in state.md. (4) Add `docker run --rm vmaf --version` smoke step
to Docker image CI job; file T-DOCKER-SMOKE. (5) File T-VULKAN-MOTION-LAVAPIPE-INIT
with closure criterion. (6) Add stability start date 2026-05-19 to GPU coverage gate;
file T-GPU-COVERAGE-STABLE-WEEKS, promotion target 2026-06-02. (7) Fix stale
`.workingdir2/` → `.corpus/` paths in konvid_mos_head_v1.md (ADR-0547 migration).
(8) Add forward-declaration banner to u2netp_mirror_card.md citing ADR-0265.
(9) Rename docs/ai/models/lpips_sq.md → lpips_sq_v1.md to match registry id.
