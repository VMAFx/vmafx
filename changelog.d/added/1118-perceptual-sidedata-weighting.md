Added Pelorus-driven perceptual spatial-pooling weighting (ADR-1118): a new
opt-in C-API (`vmaf_set_perceptual_weight_enabled`,
`vmaf_set_perceptual_weight_strength`, `vmaf_set_perceptual_sidedata`, in
`libvmaf/perceptual_weight.h`) lets libvmaf read the per-frame Pelorus side-data
blob (banding-risk / variance maps, via the vendored interop ABI from ADR-1113)
and perceptually re-weight VMAF's spatial pooling — frames whose regions carry
high banding risk count more in a pooled MEAN / HARMONIC_MEAN. The `vf_libvmaf`
filter gains a boolean `perceptual_weight` AVOption (default 0 = OFF,
ffmpeg-patch `0017-libvmaf-read-pelorus-sidedata.patch`). **Golden-gate
isolation:** the weighting is inert unless both the opt-in is set and a valid
Pelorus blob is present for the frame, so the default scoring path — and the
Netflix golden pairs, which carry no side-data — score byte-identical to before
(proven by `core/test/test_perceptual_weight.c`). Forward/back-compat (interop
R1–R6): `min(known_size, dir.size)` section reads, unknown bits ignored,
`grid == 0` degrades to a frame-level scalar, ABI-major mismatch rejected
(unweighted + log). CPU-only, no GPU. Docs: `docs/api/perceptual-weight.md`.
