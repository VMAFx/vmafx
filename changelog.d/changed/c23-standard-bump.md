- The C standard moves from C11 to **C23** (`c_std=c23` in `core/meson.build`),
  finally implementing [ADR-0692](docs/adr/0692-vmafx-c23-bump.md), which was
  accepted on 2026-05-28 but never landed — the ADR names `libvmaf/meson.build`,
  a path that predates the ADR-0700 `libvmaf/` → `core/` rename, so the change
  was almost certainly dropped during that move and went unnoticed because C11
  kept compiling. All three parts of that decision now ship together: the
  standard bump, the `-Wimplicit-fallthrough` warning (JPL Rule 24), and the
  `set_meta` prototype fix in `core/test/test_propagate_metadata.c`. That last
  one is load-bearing under C23: an empty parameter list `()` means `(void)`
  rather than "unprototyped", so `void set_meta()` assigned to
  `void (*)(void *, VmafMetadata *)` becomes a genuine type mismatch. No C23
  language features are adopted by this change — it only unlocks `typeof`,
  `ckd_add`/`ckd_mul` and `[[fallthrough]]` for later work.
