<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1113: Vendor the Pelorus interop ABI as a pinned read-only mirror

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: interop, abi, vendoring, pelorus, build, ffmpeg, fork-local

## Context

Pelorus (`VMAFx/pelorus`) and vmafx are two independently-built repositories
that must agree, byte-for-byte, on a shared data-plane interop ABI: a versioned
per-frame side-data blob that Pelorus's `vf_pelorus_*` FFmpeg filters write and
vmafx's scoring path reads (Pelorus ADR-0103 freezes the layout). The ABI is a
small, dependency-free, CPU-only C surface — a flat byte image plus a
pack/parse pair (`interop.c`) and two contract helpers (`deband_params.c`,
`version.c`) — and is licensed BSD+Patent, the same as vmafx.

vmafx needs this surface in-tree to compile a reader and to run a conformance
test, but the ABI is owned by Pelorus: there must be exactly one source of
truth, and any divergence between the two repos' parsers is a latent
interop bug. The forces: (a) keep a single source of truth in Pelorus;
(b) avoid coupling vmafx's build to Pelorus's (Pelorus pulls in Vulkan / shader
toolchains vmafx deliberately does not want — vmafx stays Vulkan-free per the
integration plan); (c) make divergence loud and mechanical to detect, not a
silent runtime mismatch.

## Decision

We vendor the Pelorus interop ABI into vmafx as a **pinned, read-only,
append-only mirror** of `VMAFx/pelorus@835e097`: the three ABI `.c` files under
`core/src/interop/pelorus_*.c`, the three public headers under
`core/include/libvmaf/pelorus/`, and the shared conformance fixture as
`core/test/test_pelorus_interop.c`. Each vendored file is byte-identical to its
Pelorus origin except for a `VENDORED FROM … DO NOT EDIT` banner and an
include-path rewrite (`pelorus/<x>.h` → `libvmaf/pelorus/<x>.h`). A pinned drift
guard (`scripts/sync-pelorus-interop.sh`, reading the pinned commit's git tree
object) fails CI on any divergence, and the conformance fixture (the same seven
vectors Pelorus runs) proves the vendored parser is byte-compatible with
Pelorus's writer. The vendored files are excluded from the fork's lint/format
gates (dir-local `.clang-tidy`, cppcheck suppressions, format-target path
filters, assertion-density skip) so the touched-file rule never forces a
fork-local edit that would break byte-identity.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Pinned read-only vendor + sync guard (chosen)** | Single source of truth stays in Pelorus; vmafx build is self-contained and Vulkan-free; drift is mechanically detected; conformance fixture proves byte-compat | A re-sync step on every ABI-minor bump; vendored files need lint-exclusion plumbing | Best balance: zero build coupling, loud drift detection, provable byte-compat |
| **git submodule of pelorus** | No copy; always in lock-step | Pulls Pelorus's full build (Vulkan/shader deps vmafx rejects); submodule UX is poor in CI / release tarballs / worktrees; ABI is a tiny slice of a large repo | Drags in the exact toolchain coupling the integration plan forbids |
| **meson subproject (wrap)** | Meson-native; builds only what's referenced | Still couples to Pelorus's meson graph + its option matrix; network fetch at configure time unless vendored-as-tarball; heavier than a 6-file mirror | Overkill for a flat, dependency-free ABI; configure-time fetch is a release-pipeline liability |
| **Hand-reimplement the parser in vmafx** | No vendoring at all | Two diverging implementations of a frozen wire format = guaranteed eventual interop bug; defeats single-source-of-truth | Exactly the failure mode the ABI freeze exists to prevent |

## Consequences

- **Positive**: vmafx builds the interop ABI with no Pelorus build dependency
  and no Vulkan. Divergence from the pinned Pelorus commit is a CI failure, not
  a runtime surprise. The shared conformance fixture is a byte-compat contract
  test that both repos run identically.
- **Negative**: A deliberate Pelorus ABI change (new section bit / appended
  field → `PELORUS_ABI_MINOR` bump) requires a manual re-sync: bump
  `PELORUS_VENDOR_SHA` in the script, the banners, the doc, and this ADR, then
  `--update` and re-run. The vendored files carry lint-exclusion plumbing that a
  future reviewer must understand (it is documented inline at each exclusion
  site).
- **Neutral / follow-ups**: The *reader* (perceptual weighting in pooling) and
  the *writer* coupling through `vf_libvmaf` are separate workstreams (plan B/C)
  and are out of scope here. A future ABI-minor re-sync is itself a follow-up
  ADR event. The deband AVOption control-plane contract is frozen on the
  Pelorus side (Pelorus ADR-0109) and consumed by a later vmafx autotune
  workstream (plan D).

## Supply-chain impact

- **New dependencies**: none. The vendored files are CPU-only, dependency-free
  C (BSD+Patent, same license as vmafx) compiled into the existing `libvmaf`
  target. No new runtime, build, or test dependency is introduced.
- **Removed dependencies**: none.
- **Build-time fetches**: none. The mirror is committed in-tree; the sync guard
  reads a *local* Pelorus checkout only when explicitly run, never during a
  normal build.
- **CVE surface delta**: negligible — adds a small flat-buffer parser with
  explicit bounds checks (`pel_blob_find_section` validates framing, offsets,
  and sizes before dereferencing) and no network/IO surface.

## References

- Integration plan: `.workingdir2/rc/pelorus/PLAN.md` — workstream A
  ("vmafx: vendor ABI"; A1 copy interop.c + headers with DO-NOT-EDIT banner,
  A2 sync guard + `_Static_assert` CI guard, A3 vendor `interop_test.c` as the
  shared 7-vector gate). The plan records the user decision: "BOTH repos, FULL
  both seams, PARALLEL", with "copy-vendor (default yes)" as the chosen
  vendoring posture.
- Pelorus ADR-0103 — `interop.h` ABI freeze (48-byte `PelorusSideData` header,
  five append-only section bits, magic `PELOR1\0\0`, UUID
  `e1d7c4a2-6b93-4f08-9a55-0f3c2db17e64`, ABI 1.0; pack/parse in `interop.c`,
  dependency-free, BSD+Patent, vendorable) — the single source of truth this
  ADR mirrors.
- Pinned source: `VMAFx/pelorus@835e097`, `libpelorus/{include/pelorus,src,test}`.
