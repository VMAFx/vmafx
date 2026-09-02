<!-- markdownlint-disable MD013 MD060 -->
# ADR-0692: Bump C standard to C23 (VMAFX rebrand Phase 1D)

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: build, c, standards, meson, fork-local, vmafx-rebrand

## Context

The fork has compiled under C11 since its initial divergence from Netflix/vmaf. The
C standard controls which language features and standard-library additions are available
to C source files. C23 (ISO/IEC 9899:2024) shipped in late 2024 and is fully supported
by GCC 13+, Clang 16+, and MSVC 19.34+ (partial). The dev machine and CI matrix both
run GCC 16 and Clang 22, well past the threshold.

Three language features from C23 are directly useful for the VMAFX fork:

- `typeof` — avoids the `-Wpedantic`-violating GCC `__typeof__` spelling in SIMD macros.
- `ckd_add` / `ckd_mul` (checked integer arithmetic) — safer replacements for the
  explicit bounds-check patterns required by SEI CERT INT30-C and INT32-C.
- `[[fallthrough]]` attribute syntax — cleaner than `__attribute__((fallthrough))` and
  self-documenting at call sites without a `/* FALLTHROUGH */` comment convention.

This ADR covers only the standard bump and the immediate prerequisite fix. C23
feature adoption happens in subsequent targeted PRs. This is Phase 1D of the VMAFX
rebrand plan (umbrella ADR-0686, PR #1546).

One latent correctness defect was uncovered during the smoke-build: `test_propagate_metadata.c`
defined `set_meta` with an empty parameter list (`void set_meta()`), then assigned it to a
`void (*)(void *, VmafMetadata *)` typed field. In C11 an empty parameter list is an
unspecified-argument declaration and the assignment is a warning; in C23 the semantics
changed so that `void f()` is equivalent to `void f(void)` (no arguments), making the
assignment an error. The fix is to give `set_meta` the correct prototype.

## Decision

We will set `c_std=c23` in `libvmaf/meson.build`'s `default_options`, fix the
`test_propagate_metadata.c` prototype mismatch, and add `-Wimplicit-fallthrough` to
`vmaf_cflags_common` to enforce the JPL Rule-24 fallthrough annotation requirement at
compile time.

No C23 language features are introduced in source code by this PR; the bump merely
unlocks them for subsequent PRs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Stay on C11 indefinitely | Zero migration cost | Blocks `typeof`, `ckd_*`, `[[fallthrough]]` adoption; `c2x` interim toggle was already acknowledged as a holdover | Rejected — VMAFX rebrand plan explicitly targets C23 |
| Bump to C17 first, C23 later | Smaller hop; C17 compiler support is universal | C17 adds nothing over C11 except a few defect fixes; no practical benefit over a direct C23 jump | Rejected — C17 intermediate step is pure overhead |
| Keep `c2x` alias | Works on older Meson | `c2x` was the pre-publication draft alias; `c23` is the standardised name supported by Meson ≥1.3.0 (fork has 1.11.0) | Rejected — `c23` is unambiguous and correctly supported |

## Consequences

- **Positive**: C23 language features (`typeof`, `ckd_*`, `[[fallthrough]]`,
  `#embed`, `nullptr`) are now available in all libvmaf C translation units.
  `-Wimplicit-fallthrough` catches missing fallthrough annotations at compile time,
  complementing JPL Rule-24 enforcement.
- **Negative**: Upstream Netflix/vmaf code ported via `port-upstream-commit` may carry
  C99/C11-era constructs that need testing under C23 (primarily empty-parameter-list
  function declarations like the one fixed here; see `docs/rebase-notes.md`).
- **Neutral / follow-ups**: Adopt `typeof` in SIMD macros, `ckd_*` in bounds-check
  hot paths, and `[[fallthrough]]` at switch fall-throughs as targeted follow-up PRs
  per the VMAFX rebrand roadmap.

## References

- Umbrella: ADR-0686 (VMAFX rebrand plan, PR #1546) — not yet filed as of this PR.
- `docs/principles.md` §1.2 rule 24 (fallthrough annotations).
- `libvmaf/meson.build` line 3 (`default_options`).
- `libvmaf/src/meson.build` `vmaf_cflags_common` block.
- `libvmaf/test/test_propagate_metadata.c` line 22 (prototype fix).
- Meson C23 support: <https://mesonbuild.com/Reference-tables.html#language-arguments-parameter-names>
- req: "Locked decision: C standard: C23 (typeof, ckd_*, embed, [[attribute]] syntax)."
