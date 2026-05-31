<!-- markdownlint-disable MD060 -->
# ADR-0561 — Widen HIP `gfx_targets` hardcoded fallback

| Field | Value |
|---|---|
| **Status** | Accepted |
| **Date** | 2026-05-18 |
| **Deciders** | lusoris, Claude (Anthropic) |
| **Tags** | hip, build, gfx, rocm, meson, fork-local |

## Context

`core/src/meson.build` resolves the list of AMD GPU ISA targets for `hipcc
--genco` in this order:

1. The `-Dhip_gfx_targets=<csv>` Meson option (explicit operator override).
2. `rocm_agent_enumerator` — filters to `gfx*` lines, comma-joined.
3. `hipconfig --amdgpu-target`.
4. A hardcoded fallback when all three probes fail (the typical no-GPU build
   sandbox case: BuildKit CI, clean-room containers without a GPU bind-mount).

Until this ADR, step 4 fell back to `gfx90a` only — a single CDNA2 server
target. The fork's primary development host is an AMD Raphael APU with an
integrated GPU presenting as `gfx1036`. Raphael's `hsa_init()` required
`HSA_OVERRIDE_GFX_VERSION=10.3.0` to satisfy ROCm's allowlist; at runtime the
loader looked for an HSACO blob compiled for `gfx1030` (the override target).
Any `libvmaf.so` compiled in a build sandbox used the `gfx90a`-only fallback
and produced an HSACO fat binary that contained no compatible object for
`gfx1030`. The dynamic loader emitted:

```text
hip_fatbin.cpp: No compatible code objects found for: gfx1030
```

and the HIP backend failed to initialise at runtime even though the host had a
functioning GPU and a full ROCm stack.

The RDNA2 desktop GPU (`gfx1030`), AMD Raphael APU iGPU (`gfx1036`), and RDNA3
desktop GPU (`gfx1100`) represent the three most common consumer AMD GPUs in the
target demographic. Including them in the fallback ensures that a container or
CI-built `libvmaf.so` runs on all three without operator intervention.

## Decision

Widen the hardcoded fallback from `gfx90a` to
`gfx90a,gfx1030,gfx1036,gfx1100`.

- `gfx90a` — AMD Instinct MI200 (CDNA2); the original fallback; kept for server
  / cloud GPU compatibility.
- `gfx1030` — RDNA2 desktop (RX 6000 series) and the effective target for the
  AMD Raphael APU iGPU via `HSA_OVERRIDE_GFX_VERSION=10.3.0`.
- `gfx1036` — AMD Raphael APU iGPU (RX 680M / Radeon 680M); the fork's primary
  HIP test device.
- `gfx1100` — RDNA3 desktop (RX 7000 series); forward-compatibility for the
  most common RDNA3 target.

The `docs/backends/hip/overview.md` gain a new `### -Dhip_gfx_targets` section
documenting the four-step resolution order and the operator override syntax.

## Alternatives considered

| Option | Notes | Decision |
|--------|-------|----------|
| Keep `gfx90a` only | Requires every operator to pass `-Dhip_gfx_targets=...` explicitly; error-prone and not documented at build time. The fork broke silently on first use after a container rebuild — the build succeeded but runtime failed. | Rejected |
| Query GPU at configure time only | `rocm_agent_enumerator` / `hipconfig` already fill steps 2–3; no new logic needed. The fix is purely step 4. | Redundant — steps 2–3 already do this |
| Add `gfx1035` (another Raphael variant) | The `HSA_OVERRIDE_GFX_VERSION=10.3.0` already maps the iGPU to `gfx1030`; a native `gfx1035` blob is not needed. | Rejected — not a production target in this fork |
| Auto-detect via `hipcc --print-targets` | Not available in ROCm toolchains before 6.0; adds a configure-time hipcc dependency even for no-GPU builds. | Rejected — fragile |

## Consequences

- HSACO fat binaries built in no-GPU sandboxes cover four targets instead of
  one. Fat binary size increases by approximately 3× (measured: `vif_statistics`
  HSACO grows from ~180 KB to ~620 KB). Total `libvmaf.so` size increase:
  < 4 MB — acceptable.
- Operators who need a smaller binary can pin via `-Dhip_gfx_targets=gfx1036`.
- No change to the behaviour of builds where `rocm_agent_enumerator` or
  `hipconfig` succeeds; step 4 is only reached in no-GPU sandboxes.

## References

- Build failure: `hip_fatbin.cpp: No compatible code objects found for: gfx1030`
  (surfaced on fork dev host running AMD Raphael APU `gfx1036` / ROCm 6.4)
- `docs/backends/hip/overview.md` — `-Dhip_gfx_targets` section (this PR)
- ADR-0537 — HIP integer VIF kernel crash fixes (context: first real HIP VIF run)
- ADR-0552 — HIP VIF deterministic wavefront reduction (sibling fix)
