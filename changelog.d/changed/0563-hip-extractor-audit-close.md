# chore(hip): close HIP-05 audit — all 14 originally-scaffolded extractors are now real (ADR-0563)

Static-source audit of the 9 remaining HIP feature extractors that the
HIP-05 audit listed as scaffold-ENOSYS: `ciede_hip`, `float_moment_hip`,
`float_ansnr_hip`, `integer_motion_v2_hip`, `float_motion_hip`,
`float_adm_hip`, `ssimulacra2_hip`, `integer_adm_hip`, `float_vif_hip`.

All 9 are already real — the audit was stale after the kernel-promotion wave
(PRs #1303–#1307, ADR-0539, and earlier ADR-0372/0373/0375/0377/0468).
Every extractor has a `.hip` kernel source registered in `hip_kernel_sources`
and a real `#ifdef HAVE_HIPCC` init/submit/collect path. `hip_hsaco_stubs.c`
is now effectively empty (all weak fallbacks removed). No porting work is
required; the audit row is closed.
