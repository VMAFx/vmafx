## Metal init unwinds without goto (2026-10-29)

`refactor/metal-single-label-unwind`. The seven `core/src/feature/metal/*.mm`
init functions return through a `static int unwind_<init>(State *s, int err)`
helper on every failure; the helper runs the null-safe releases in reverse
order. A rebase or upstream sync keeps zero `goto` in these files: the praetor
engine's HISS-01 scan reports any cleanup `goto` because `.standards.yaml`
declares no `c_goto_cleanup` exception. `ssimulacra2_metal.mm` keeps the
`ss2m_yuv_coeffs`, `ss2m_combine_scale` and `ss2m_downsample_in_place` helpers
that hold `ss2m_picture_to_linear_rgb` and `submit_fex_metal` under 60 lines.
Each helper releases the kernel lifecycle and the Metal context only inside
`if (s->ctx != nullptr)` and returns the init error it was given; keep that
guard when resolving a conflict in a helper.
`core/test/test_metal_unwind_contract.py` (meson suite `fast`) fails on an
unguarded release or a lost init error. No score, C API or build change.
