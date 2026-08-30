- The build moves to the **current** language standards: C11 → **C23**
  (ISO/IEC 9899:2024) and C++23 → **C++26** (ISO/IEC 14882:2026, shipped
  2026-03-28). This finally implements [ADR-0692](docs/adr/0692-vmafx-c23-bump.md),
  accepted 2026-05-28 but never landed — the ADR names `libvmaf/meson.build`, a
  path that predates the ADR-0700 `libvmaf/` → `core/` rename, so the change was
  dropped during that move and went unnoticed because C11 kept compiling.
- All three parts of ADR-0692 ship together: the standard bump,
  `-Wimplicit-fallthrough` (JPL Rule 24), and the `set_meta` prototype fix in
  `core/test/test_propagate_metadata.c`. The last is load-bearing under C23: an
  empty parameter list means `(void)` rather than "unprototyped", so
  `void set_meta()` assigned to `void (*)(void *, VmafMetadata *)` becomes a
  genuine type mismatch.
- MSVC keeps `/std:c++latest` and the icx-cl-under-MSVC-ABI leg keeps its
  explicit `-Dcpp_std` override, so neither path changes. No C23 or C++26
  language features are adopted here — the bump only unlocks `typeof`,
  `ckd_add`/`ckd_mul` and `[[fallthrough]]` for later work.
