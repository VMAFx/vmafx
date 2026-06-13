<!-- markdownlint-disable MD013 MD060 -->
# ADR-0686: VMAFX Rebrand and Aggressive Modernization — Umbrella ADR

- **Status**: Proposed
- **Date**: 2026-05-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: rebrand, fork-policy, modernization, license, vmafx, build, ci, cli, docs

## Context

The fork has diverged from Netflix/vmaf in substantial ways: GPU backends
(CUDA, SYCL, HIP, Vulkan, Metal), AVX-512 / NEON SIMD paths, a tiny-AI
(ONNX Runtime) surface, an MCP server, and a vmaf-tune encoding optimizer.
Continuing to operate under the upstream `vmaf` project name creates two
friction points:

1. **Identity confusion.** Users and CI scripts encounter `vmaf` binaries
   and library names that behave differently from upstream Netflix/vmaf due
   to the fork's default-backend policy, CLI extensions, and modernized C
   standard target. The name gives no signal that this is an extended fork.

2. **Training-data ingestion is blocked by license asymmetry.** Several
   high-quality MOS corpora (including CHUG and KoNViD variants accessed
   through the tiny-AI pipeline) are available only under terms compatible
   with MIT but not unambiguously with BSD-3-Clause-Plus-Patent (the MobileSal
   CC-BY-NC-SA story documented in ADR-0257 is the clearest illustration of
   the practical cost of operating under a single-license regime). Dual-
   licensing fork-added code under `BSD-3-Clause-Plus-Patent OR MIT` enables
   training pipelines to elect the MIT path for corpus ingestion while
   downstream binary consumers continue to use the BSD-patent path.

The fork already maintains a strict partition between Netflix-inherited code
(frozen under the upstream BSD-3-Clause-Plus-Patent) and fork-added code.
The only hard tie to upstream is the numerical golden data: three CPU
reference test pairs and their hardcoded `assertAlmostEqual` assertions in
`python/test/`. All upstream feature implementations are cherry-picked rather
than wholesale merged; this makes the rebrand a naming and license policy
change with no algorithmic upstream breakage.

This umbrella ADR locks the top-level decisions. Individual follow-up ADRs
govern each sweep (C-header sweep, doc-site rebrand, CI matrix dedup, etc.).

## Decision

The following decisions are locked. Per-sweep ADRs will refine implementation
details within these bounds.

### Naming and identity

- The project rebrands to **VMAFX** (VMAF eXperimental / eXtended).
- The new CLI binary is `vmafx` — a thin wrapper over the existing `vmaf`
  binary that sets modernized defaults (backend auto-detect, C23 target,
  sanitizer gates).
- `libvmaf.so` and its public C API (`VmafContext`, `vmaf_init`, etc.) are
  **not renamed**. Downstream FFmpeg linkers and third-party consumers keep
  working without change.
- The FFmpeg `libvmaf` filter name is **not changed**. Existing FFmpeg filter
  pipelines continue to work.
- The version scheme `v3.x.y-lusoris.N` (tracking the Netflix upstream version
  with the fork suffix) is retained.
- `ai/` tool aliases: `vmafx-train`, `vmafx-tune`, `vmafx-mcp` symlinks are
  added alongside the existing `vmaf-train`, `vmaf-tune`, `vmaf-mcp` names.
  The existing names remain active; the `vmafx-*` names are the preferred
  forward reference in new documentation.
- The dev container is renamed to `vmafx-dev-mcp`.
- The mkdocs site title rebrands to VMAFX throughout documentation.

### C standard and coding standards

- **C standard target: C23.** All fork-new C files use `-std=c2x` (GCC) /
  `-std=c23` (Clang) from this point forward. Netflix-inherited C files are
  not mass-upgraded; they are touched only when a bug fix or fork-feature
  requires it, and the upgrade is scoped to the changed TU.
- The banned-function list is tightened by promoting two clang-tidy checks
  from advisory to required-on-every-PR: `cert-err33-c` (every non-void
  return value checked) and `bugprone-not-null-terminated-result`.
- ASan + UBSan + MSan are promoted from optional to **required gates** on
  every PR that touches C source.

### Legacy path removal (scope: aggressive)

The following build configurations are dropped:

- MinGW64 Windows cross-compile target.
- i686 (32-bit x86) and no-asm (assembly-disabled) build variants.
- Static library + DNN combined build (static libvmaf + ONNX Runtime
  static link was never production-supported and causes symbol conflicts).
- The legacy `python/vmaf` classic training harness build wheel (the Netflix
  golden test subset that calls into it is retained; only the `pip install`
  wheel-build path is dropped).

Rationale: these paths are not covered by the GPU parity CI matrix, were
already fragile (see ADR-0664 Windows CUDA toolkit installer), and their
maintenance cost exceeds demonstrated demand.

### Backend auto-detection

The `vmafx` binary default backend order: **CUDA > Vulkan > SYCL > CPU**.
The existing `vmaf` binary default (CPU) is unchanged.

### CI matrix

The CI matrix is deduplicated so every meaningful configuration is covered
once. Specific changes are governed by a follow-up ADR. The invariant: every
merged PR must pass the Netflix CPU golden gate, the GPU parity matrix, and
the fast test suite on at least one Linux configuration.

### Models

No model files are dropped in this PR. Per-model decisions are deferred to
follow-up ADRs.

### Find/replace policy

- **Code**: only new fork-added code uses the `vmafx` name in identifiers,
  binary names, and Meson targets. Existing `VmafContext`, `vmaf_init`,
  `libvmaf/`, `vmaf-tune`, etc. are not mass-renamed.
- **Documentation**: all documentation under `docs/` globally rebrands to
  use "VMAFX" as the project/brand name. The following names are explicitly
  excluded from naive find/replace: `libvmaf` (library name), `vmaf-tune`
  (tool name), `VmafContext` (C type), `vmaf_v0.6.1` (Netflix model id).

### License

- Fork-added code is dual-licensed: `BSD-3-Clause-Plus-Patent OR MIT`.
- Netflix-inherited code remains `BSD-3-Clause-Plus-Patent` only.
- The repo root `LICENSE` file is updated to note the dual-license policy
  for fork-added files while preserving the Netflix BSD-3-Clause-Plus-Patent
  body intact.
- A new `LICENSE-MIT` file is added at the repo root.
- Per-file SPDX headers in fork-added `.c` / `.h` / `.py` files are updated
  to reflect the dual-license identifier. This sweep is deferred to a
  follow-up PR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Name: "Lusoris VMAF" | Clearly attributed to fork operator | Awkward as a binary name; still starts with someone else's acronym | User rejected: compound portmanteau VMAFX is cleaner |
| Name: "VQEx" (Video Quality eXtended) | Fully breaks from Netflix branding | Loses VMAF recognition; MOS-model consumers would not associate it | User rejected: VMAFX retains the VMAF lineage signal |
| ABI break: rename `libvmafx.so` | Cleaner long-term fork identity | Breaks every downstream FFmpeg build, packaging script, and pkg-config consumer immediately | Deferred indefinitely; ABI compatibility is non-negotiable near-term |
| Hard rename of all identifiers | Maximum clarity | Enormous blast radius, diverges from upstream cherry-pick track, requires coordinated downstream changes | Rejected; find/replace policy above is the controlled approach |
| C17 instead of C23 | More toolchain support | Misses `_BitInt`, improved type-generic macros, and `nullptr` | User chose C23; toolchain support is sufficient for the target platforms |
| Drop feature extractors now | Smaller codebase immediately | Breaks any consumer that depends on those extractors; no data-driven decision | Deferred to per-feature ADRs after corpus signal is available |
| Single license (keep BSD-3-Plus-Patent only) | Simpler legal model | Continues to block CC-BY-NC-SA and related corpus ingestion use cases | Rejected; dual-license resolves the MobileSal-class blocker (ADR-0257) |
| Dual license from day one on ALL files | Maximum ingestion flexibility | Retroactively dual-licensing Netflix-inherited code is not in scope — their license is fixed | Rejected; only fork-added code is dual-licensed |

## Consequences

### Positive

- The project has a distinct identity that reflects its extended capabilities
  (GPU backends, SIMD, tiny-AI, MCP server).
- Training pipelines can elect the MIT path for corpus ingestion, unblocking
  the ADR-0257 class of license compatibility issues.
- Dropping legacy build paths shrinks the CI matrix and eliminates maintenance
  drag from configurations that serve no production workloads.
- C23 and tighter clang-tidy gates reduce the class of undefined-behavior bugs
  that reach review.

### Negative

- Follow-up sweep PRs (doc rebrand, SPDX header sweep, CI matrix dedup,
  symlink additions) must be coordinated to avoid merge conflicts.
- The `vmafx` binary name adds a new user-visible surface that must be
  documented, packaged, and kept in sync with `vmaf`.

### Neutral / follow-ups

- A doc-site sweep PR rebrands all `docs/` references to VMAFX (excluding the
  four protected names above).
- A SPDX header sweep PR updates fork-added file headers to the dual-license
  identifier.
- A CI matrix dedup PR removes redundant matrix legs and adds the new
  mandatory sanitizer gates.
- A `vmafx` CLI wrapper PR adds the binary under `libvmaf/tools/`.
- Per-feature ADRs govern any extractor removals.

## Non-negotiables (preserved through all sweep PRs)

- [ ] Netflix CPU golden numbers remain bit-exact: the three reference test
  pairs (`src01`, checkerboard ×2) and their hardcoded `assertAlmostEqual`
  values in `python/test/quality_runner_test.py`,
  `python/test/vmafexec_test.py`, `python/test/feature_extractor_test.py`,
  and `python/test/result_test.py` are never modified. If scores drift, the
  code is fixed, not the assertions.
- [ ] Upstream Netflix feature implementations remain tracked: cherry-picks of
  `motion_v2`, `cambi`, `vif`, `adm` bug fixes, and new options continue as
  the standard upstream-sync mechanism.
- [ ] Netflix-inherited code remains BSD-3-Clause-Plus-Patent only: the dual-
  license applies exclusively to fork-added files.

## References

The following paraphrase the user's locked decisions (per CLAUDE.md user-quote
handling rule — rendered in neutral English):

- req: "Project rebrands to VMAFX (compound: VMAF + eXperimental/eXtended)."
- req: "ABI strategy: preserve libvmaf.so name."
- req: "Keep v3.x.y-lusoris.N versioning."
- req: "Keep libvmaf ffmpeg filter name."
- req: "Scope: aggressive — drop legacy paths, modernize C."
- req: "Backend default: auto-detect CUDA > Vulkan > SYCL > CPU."
- req: "C standard: C23."
- req: "Drop legacy build paths: MinGW64, i686, Static+DNN, legacy python/vmaf wheel build."
- req: "CI matrix: dedupe correctly — catch everything but don't run a hundred times."
- req: "vmafx binary: thin wrapper over vmaf."
- req: "Models: drop nothing now; per-model later."
- req: "Release suffix: keep -lusoris.N."
- req: "ADR strategy: one umbrella + per-sweep ADRs."
- req: "AI naming: add vmafx-* aliases now (symlinks)."
- req: "Doc site: rebrand both — VMAFX docs + vmafx-dev container."
- req: "Banned C functions: tighter clang-tidy (cert-err33-c, bugprone-not-null-terminated-result) AND sanitizers as required gates."
- req: "Mass find/replace: only NEW code uses vmafx; ALL docs globally rebrand to VMAFX."
- req: "Feature extractor cleanup: defer per-feature decisions."
- req: "License: dual-license fork-added code as BSD-3-Clause-Plus-Patent OR MIT."
- req: "Sweep timing: start NOW in parallel with merge train."

Related ADRs:

- [ADR-0024](0024-netflix-golden-preserved.md) — Netflix golden tests as required status check
- [ADR-0100](0100-project-wide-doc-substance-rule.md) — documentation substance rule
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — deep-dive deliverables rule
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file lint cleanup rule
- [ADR-0214](0214-gpu-parity-ci-gate.md) — GPU parity CI gate (places=2)
- [ADR-0257](0257-mobilesal-real-weights-deferred.md) — MobileSal CC-BY-NC-SA license incompatibility (dual-license motivation)
- [ADR-0386](0386-adr-numbering-collision-prevention.md) — ADR allocator
- [ADR-0514](0514-dev-container-full-backend-exposure.md) — dev-MCP container backend exposure
