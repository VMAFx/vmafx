<!-- GENERATED (`make docs-fragments-write`); do not hand-edit. -->
# AGENTS.md — core/src/feature/arm64

Generated index. Match touched paths against `Touching`; read each matching
page first (none -> skip). Text above table binds every file. `Touching`
relative; leading `/` = repo root. New invariant -> `AGENTS.d/` page
([how](../../../../docs/development/agents-index.md)).

Agent orientation: NEON / SVE2 feature SIMD paths.
Parent: [../AGENTS.md](../AGENTS.md). Sister directory:
[`../x86/`](../x86/AGENTS.md).

## Topic pages

| Touching | Read first | Invariant |
| --- | --- | --- |
| `cambi_neon.*`, `*sve2*`, `ssimulacra2*` | [cambi-sve2-and-ssimulacra2](AGENTS.d/cambi-sve2-and-ssimulacra2.md) | CAMBI NEON scan/tail rules, SVE2 4-lane versus VLA raster reductions, and shared SSIMULACRA 2 ARM64 header. |
| `speed_neon.c`, `psnr_hvs_neon.c`, `/core/src/meson.build` | [fp-targets-speed-and-msvc](AGENTS.d/fp-targets-speed-and-msvc.md) | arm64_v8_fp strict-FP library placement, pointer accumulators, SpEED covariance, and MSVC ARM64 portability. |
| `**`, `/.github/workflows/lint-and-format.yml` | [scaffolding-lint-and-adrs](AGENTS.d/scaffolding-lint-and-adrs.md) | New NEON/SVE2 TU scaffolding, arm64-only clang-tidy ratchet measurement, upstream sync, and governing ADRs. |
| `*adm*neon*`, `*.h` | [scope-and-adm-neon](AGENTS.d/scope-and-adm-neon.md) | Self-contained SIMD headers, scalar bit-exactness, and integer/float ADM NEON non-contracting kernels. |
| `*.c`, `/core/src/feature/x86/*.c` | [twin-update-rules](AGENTS.d/twin-update-rules.md) | Lockstep twin-bundle updates across ARM64 NEON/SVE2, x86 AVX2/AVX-512, and scalar references. |
