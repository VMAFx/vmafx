# AGENTS.md per-package coverage audit (2026-05-09)

Scope: every code-bearing directory under `core/src/`, `tools/`, `ai/`,
`python/vmaf/`, `mcp-server/`, `scripts/`. Goal: confirm that every
package with rebase-sensitive invariants ships an `AGENTS.md` per
CLAUDE.md §12 r11 / [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md).

## Methodology

1. Enumerate all code-bearing directories (1+ `.c` / `.h` / `.cpp` /
   `.cu` / `.py` / `.sh` / `.comp` file at depth 1).
2. For each, classify as one of:
   - **EXISTS — current**: AGENTS.md present and meaningfully covers
     the package's invariants.
   - **EXISTS — stale**: AGENTS.md present but cites removed code or
     wrong invariants (none found in this audit).
   - **MISSING — needs backfill**: directory has rebase-sensitive code
     patterns (twin-update rules, upstream-mirror discipline,
     ADR-pinned algorithms, bit-exactness gates) but no AGENTS.md.
   - **N/A — no invariants**: pure fork-original docs / configs /
     generated artefacts, or fully covered by a parent AGENTS.md.
3. For MISSING entries, read 5–10 representative files in the package
   to extract the actual rebase-sensitive patterns before writing.
4. Quality > quantity: an AGENTS.md that just restates the parent
   document is worse than no file.

## Coverage matrix

| Package | Status | Action |
| --- | --- | --- |
| `/` | EXISTS | current |
| `.github/` | EXISTS | current |
| `libvmaf/` | EXISTS | current |
| `core/include/libvmaf/` | EXISTS | current |
| `core/src/` | N/A — covered by `libvmaf/AGENTS.md` (thread_pool, output, gpu_picture_pool, fuzz, MCP scaffold) | none |
| `core/src/arm/` | MISSING | backfill (SVE2 HWCAP2 fork-local fallback per T7-38) |
| `core/src/x86/` | N/A — verbatim from dav1d, no fork-local additions | none |
| `core/src/ext/x86/` | N/A — `x86inc.asm` is upstream-verbatim third-party | none |
| `core/src/compat/{gcc,win32}/` | N/A — single-shim toolchain compat headers | none |
| `core/src/cuda/` | EXISTS | current |
| `core/src/dnn/` | EXISTS | current |
| `core/src/feature/` | EXISTS | current (includes ssimulacra2, ms_ssim, motion_v2, cambi, fastdvdnet, transnet_v2 invariants) |
| `core/src/feature/x86/` | MISSING | backfill (AVX2 / AVX-512 twin-update rules per ADR-0138 / 0139 / 0143 / 0148 / 0159 / 0161 / 0162 / 0163 / 0252) |
| `core/src/feature/arm64/` | MISSING | backfill (NEON / SVE2 twin-update rules per ADR-0125 / 0139 / 0140 / 0145 / 0160 / 0161 / 0162 / 0163 / 0213 / 0252) |
| `core/src/feature/iqa/` | MISSING | backfill (third-party tdistler scalar reference; ADR-0148 reserved-id renames; ADR-0146 helper decomposition) |
| `core/src/feature/common/` | MISSING | backfill (ADR-0143 generalised AVX scanline helpers; `static` + `ptrdiff_t` invariants) |
| `core/src/feature/cuda/` | MISSING | backfill (feature-kernel host glue; mirror contract with upstream-mirror `cuda/` runtime + per-feature kernel-template fields) |
| `core/src/feature/sycl/` | MISSING | backfill (fp64-free contract per ADR-0220; `-fp-model=precise` via parent meson) |
| `core/src/feature/vulkan/` | MISSING | backfill (host-glue twin-of-shaders contract; descriptor pool sizing) |
| `core/src/feature/vulkan/shaders/` | MISSING | backfill (GLSL `precise` / `NoContraction` precision invariants per ADR-0264 / 0269) |
| `core/src/feature/hip/` | N/A — covered by `core/src/hip/AGENTS.md` (kernel-template + per-consumer mirror invariants) | none |
| `core/src/feature/third_party/xiph/` | MISSING | backfill (Xiph `psnr_hvs.c` is the bit-exact scalar reference for ADR-0159 / 0160 SIMD ports; do not edit without lockstep SIMD updates) |
| `core/src/hip/` | EXISTS | current |
| `core/src/mcp/` | MISSING | backfill (audit-first scaffold contract per ADR-0209; smoke pins `-ENOSYS` until T5-2b) |
| `core/src/sycl/` | EXISTS | current |
| `core/src/vulkan/` | EXISTS | current |
| `core/test/` | EXISTS | current |
| `core/tools/` | EXISTS | current |
| `tools/vmaf-roi-score/` | EXISTS | current |
| `tools/vmaf-tune/` | EXISTS | current |
| `tools/ensemble-training-kit/` | MISSING | backfill (ADR-0324 fork-original training kit; numbered-step contract + bundled-binary directory layout invariants) |
| `python/vmaf/` | EXISTS | current |
| `python/vmaf/{core,tools,script,matlab,resource,third_party}/` | N/A — covered by `python/vmaf/AGENTS.md` (golden-data gate, MEX purge, workspace relocation) | none |
| `ai/` | EXISTS | current (582 lines covering scripts/, src/, train/, tests/, configs/, knob-sweep corpus invariants) |
| `ai/{scripts,src,tests,train,configs,testdata,data}/` | N/A — covered by parent `ai/AGENTS.md` | none |
| `mcp-server/` | EXISTS | current |
| `mcp-server/vmaf-mcp/{src,tests}/` | N/A — covered by parent `mcp-server/AGENTS.md` | none |
| `scripts/ci/` | EXISTS | current |
| `scripts/{setup,dev,docs,release}/` | MISSING | backfill (single `scripts/AGENTS.md` covering ADR-0221 fragment-concat scripts + ONNX placeholder generators + setup dispatcher) |

## Counts

- EXISTS — current: 18
- EXISTS — stale: 0
- MISSING — backfilled: 13
- N/A — no invariants / parent-covered: 13

## Findings

1. **No genuinely stale AGENTS.md found.** Every existing file cites
   real code and real ADRs. The fork's discipline of updating AGENTS.md
   in the same PR as the code change has held up across the last ~50
   PRs.
2. **The biggest gap was under `core/src/feature/`.** The parent
   `feature/AGENTS.md` is comprehensive (655 lines) and captures the
   per-extractor invariants, but the SIMD-twin / GPU-twin / scalar-
   reference tree split underneath it had no per-package orientation.
   A new contributor opening `feature/x86/ssim_avx2.c` cold had to
   read the parent file end-to-end to find the ADR-0138 / 0139
   bit-exactness contract that governs the file.
3. **The third-party trees inherit the most rebase risk.** Both
   `feature/iqa/` (tdistler 2011) and `feature/third_party/xiph/`
   (Xiph psnr_hvs) are scalar references that a half-dozen SIMD
   ports depend on byte-for-byte. The fork has accumulated several
   load-bearing modifications (ADR-0146 / 0148 helper splits and
   reserved-id renames in `iqa/`; ADR-0159 / 0160 lockstep contract
   for Xiph). Without an AGENTS.md these were only documented in
   the parent file and the ADRs themselves — easy to miss on a
   surface-level rebase.
4. **Vulkan has a 2-tier package**: `feature/vulkan/*.c` (host glue)
   vs `feature/vulkan/shaders/*.comp` (GLSL). They have *different*
   rebase-sensitive surfaces (host: descriptor pool sizing,
   plane-loop semantics; shaders: `precise` / `NoContraction`
   placement, ADR-0264 driver-bug carve-out). Two AGENTS.md files
   make sense here.
5. **`scripts/{setup,dev,docs,release}/` were a coverage gap.** The
   ADR-0221 fragment-concat scripts (`concat-changelog-fragments.sh`,
   `concat-adr-index.sh`) are the source of truth for two
   high-traffic CI-gated files (`CHANGELOG.md`, `docs/adr/README.md`).
   A rename or signature change in either silently breaks every
   subsequent PR. This deserved a written invariant note.

## Follow-ups

- None opened. The 13 MISSING entries are all addressed by this PR.
  No state.md row is opened — the audit confirms the discipline is
  holding, the gaps are filled, and there is no follow-up
  meta-coverage work outstanding.

## References

- CLAUDE.md §12 r11 — six deep-dive deliverables; AGENTS.md
  invariant note is one of them.
- [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) — the
  governing rule.
- [ADR-0028](../adr/0028-adr-maintenance-rule.md) — ADR
  immutability does NOT apply to AGENTS.md (refresh freely).
