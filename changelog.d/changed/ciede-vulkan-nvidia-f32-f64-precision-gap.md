- **NVIDIA-Vulkan ciede2000 places=4 5/48 mismatch root-caused as f32/f64 fork debt (ADR-0273)** —
  closes the deferred follow-up reserved by PR #346 ("vif + ciede
  shaders — precise decorations") for the residual 5/48
  NVIDIA-Vulkan ciede2000 mismatch (max abs `8.9e-05`, 1.78× the
  places=4 threshold of `5.0e-05`) on the highest-ΔE frames of the
  576×324 fixture. Investigation in
  [`docs/research/0055-ciede-vulkan-nvidia-f32-f64-root-cause.md`](docs/research/0055-ciede-vulkan-nvidia-f32-f64-root-cause.md)
  triangulates the unmodified double-CPU output, an experimental
  float-CPU output (one-off diagnostic patch — not committed —
  rebuilds `ciede.c::get_lab_color` and helpers in `float`
  throughout, mirroring the Vulkan shader's precision contract), and
  the NVIDIA RTX 4090 + driver 595.71.05 Vulkan output. Result:
  float-CPU and NVIDIA-GPU agree to ~6e-7 on the 5 frames that fail
  double-CPU vs NVIDIA-GPU at places=4 — the residual gap is the
  irreducible f32-vs-f64 precision delta on the highest-ΔE pixels,
  amplified by per-pixel ΔE summation. Three mitigations are
  rejected by [ADR-0273](docs/adr/0273-ciede-vulkan-nvidia-f32-f64-precision-gap.md):
  promoting the shader to f64 (RTX 4090 runs f64 at 1/64 fp32
  throughput; SPIR-V f64 transcendentals are not bit-mandated),
  f32-narrowing the CPU reference (changes Netflix golden ground
  truth), and matched-polynomial transcendental approximations
  (cost-benefit fails). The 5/48 is accepted as documented fork debt
  via the new `T-VK-CIEDE-F32-F64` row in
  [`docs/state.md`](docs/state.md) Open bugs. CI's lavapipe parity
  gate (places=4, currently 0/48) remains authoritative; NVIDIA
  hardware validation stays a manual local gate. No code changes —
  ships docs only (ADR-0273, research-0055, state.md row, CHANGELOG,
  rebase-notes, vulkan-backend doc note).
