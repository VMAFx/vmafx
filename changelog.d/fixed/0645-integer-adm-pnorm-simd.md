Fixed `integer_adm:adm_p_norm` on CPU x86 SIMD dispatch by threading the
configured exponent through the scalar, AVX2, and AVX-512 contrast-measure
callbacks instead of retaining the hard-coded default `3.0` exponent.

Fixed the rule-enforcement workflow trigger so draft pull requests rerun the
ADR/docs/FFmpeg/state gates when promoted to ready-for-review, matching the
ADR-0331 draft-CI contract used by the other PR workflows.

Fixed the `test_feature_collector` Meson build graph so the generated
`vcs_version.h` header exists before the test compiles its direct
`libvmaf.c` include.

Removed the known-broken lavapipe `motion` / `motion_v2` advisory probes from
the named Vulkan VIF CI job so passing pull requests no longer surface
misleading `##[error]` annotations for a documented open Vulkan-motion issue.
