<!-- markdownlint-disable MD060 -->
# ADR-0596: Delete orphan and duplicate HIP/CUDA translation units

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `cuda`, `build`, `cleanup`

## Context

A deep audit of `core/src/feature/hip/` and `core/src/feature/cuda/`
identified six translation units that were either not compiled into the build
(orphans absent from every `meson.build`) or defined the same linker symbol as
a compiled TU (duplicates that would cause a multiple-definition link error if
both were ever included).

**HIP orphans — duplicate symbol, not in `hip/meson.build`:**

- `feature/hip/integer_ciede_hip.c`: defined `vmaf_fex_ciede_hip`, the same
  symbol exported by the compiled `feature/hip/ciede_hip.c`.
- `feature/hip/integer_moment_hip.c`: defined `vmaf_fex_float_moment_hip`, the
  same symbol exported by the compiled `feature/hip/float_moment_hip.c`.

**CUDA orphan — duplicate symbol, not in `core/src/meson.build`:**

- `feature/cuda/float_ssim_cuda.c`: defined `vmaf_fex_float_ssim_cuda`, the
  same symbol exported by the compiled `feature/cuda/integer_ssim_cuda.c`.
  The compiled file is the newer, more complete version (adds `enable_chroma`
  and other improvements). `float_ssim_cuda.c` itself noted it "supersedes
  `integer_ssim_cuda.c`", but the meson.build was never updated to swap them,
  leaving the old file stranded.

**HIP plumbing stubs — compiled but zero callers, misleading availability:**

- `feature/hip/adm_hip.c`: defined `vmaf_hip_adm_{init,run,destroy}`. `init`
  returned 0 (success), `run` returned -ENOSYS. No `VmafFeatureExtractor`
  registration; no callers anywhere in the repo. The "init succeeds, run fails"
  posture is a misleading-availability pattern: it signals to any hypothetical
  dispatcher that the feature is available at init time, only to fail silently
  on the first frame.
- `feature/hip/motion_hip.c`: same pattern for `vmaf_hip_motion_{init,run,destroy}`.
- `feature/hip/vif_hip.c`: same pattern for `vmaf_hip_vif_{init,run,destroy}`;
  `init` itself also returned -ENOSYS in a later revision, inconsistent with
  the other two.

These three were wired into `hip/meson.build` under ADR-0212/T7-10 as
scaffolding for a dispatch shim that was never built. The actual HIP ADM, VIF,
and motion feature extractors are `integer_adm_hip.c`, `integer_vif_hip.c` /
`float_vif_hip.c`, and `integer_motion_hip.c` / `float_motion_hip.c` —
all of which carry proper `VmafFeatureExtractor` registrations and are correctly
wired in the registry.

**Shared header:**

- `feature/hip/feature_hip.h`: declared only the three plumbing-stub triplets
  above. With the stubs gone there are no consumers; the header is removed with
  them.

Pre-deletion verification confirmed zero callers of any of these symbols outside
the files themselves (`git grep` across `libvmaf/`, `tools/`, `python/`, `ai/`,
`mcp-server/`). No in-flight branches on `origin/feat/hip-*` were found that
could be resurrecting these TUs.

## Decision

Delete all six translation units and the orphaned header. Update
`hip/meson.build` to remove the three plumbing-stub entries. Add tombstone notes
to `feature/hip/AGENTS.md` and `feature/cuda/AGENTS.md` so future agents do not
re-add the files without understanding the history.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wire the orphan duplicates into meson.build | They become compiled | Immediate multiple-definition link error; canonical TUs are already correct | Hard build failure |
| Keep the plumbing stubs; add -ENOSYS to `init` | Consistent scaffold posture | Still zero callers; still not feature-extractor-registered; dead weight in every build | Preserving dead code adds cognitive overhead with zero benefit |
| Rename stubs to implement real extractors | Would produce real HIP ADM/VIF/motion | Out of scope; real extractors already exist under different names | Wrong fix for an audit-cleanup task |

## Consequences

- **Positive**: No more risk of accidentally including both a duplicate and its
  canonical TU in a future meson.build edit (which would silently produce a
  multiple-definition error at link time). Build artifact size reduced by
  removing three compiled-but-never-called object files from the HIP archive.
  The "init=0, run=-ENOSYS" misleading-availability pattern is eliminated.
- **Negative**: None. All deleted symbols had zero callers.
- **Neutral / follow-ups**: `float_ssim_cuda.c`'s self-referencing "This file
  supersedes `integer_ssim_cuda.c`" comment in `integer_ssim_cuda.c` (line 20)
  is now historically inaccurate — the supersession never completed because the
  meson wiring was not updated. The comment has been left in place as an
  audit trail; a follow-up can clean it up if desired.

## References

- ADR-0212 / T7-10: original HIP scaffold (introduced the plumbing stubs)
- ADR-0533: HIP all-extractors registration sweep (identified `integer_ciede_hip.c`
  and `integer_moment_hip.c` as stale duplicate-scaffold TUs)
- `docs/state.md` row `T-HIP-DEAD-CODE-EXTRACTORS-2026-05-18` (pre-existing documentation
  of the duplicate-scaffold issue)
- req: "Deep-audit found dead/orphan HIP and CUDA translation units. Delete them
  and update tests + docs."
