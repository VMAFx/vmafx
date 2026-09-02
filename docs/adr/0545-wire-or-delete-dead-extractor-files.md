<!-- markdownlint-disable MD013 MD060 -->
# ADR-0545: Wire or delete dead Vulkan/Metal feature-extractor source files

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `metal`, `build`, `housekeeping`, `dead-code`

## Context

A deep audit (Finding 24 in `.workingdir/bbb_reports/DEEP_AUDIT_2026_05_18.md`)
flagged ~3500 LOC of feature-extractor source files under
`core/src/feature/vulkan/` and `core/src/feature/metal/` that were defined
on disk but never listed in the corresponding `core/src/{vulkan,metal}/meson.build`
source list. Several of them defined `VmafFeatureExtractor vmaf_fex_*` symbols that
either (a) duplicated an already-wired sibling, (b) had no `extern` declaration in
`feature_extractor.c` (pure orphans), or (c) were referenced as `extern` and even
registered in `feature_extractor_list[]` while the defining TU was missing from the
build — a latent link-time bomb on the affected backend.

The user direction was unambiguous: "no stubs anywhere." Dead, undisclosed scaffold
files compound context-cost on every later audit and risk silent re-resurrection by
a copy-pasta refactor.

## Decision

For every Vulkan / Metal feature TU not listed in its backend's meson source list,
apply one of three actions:

1. **Wire** — when the file is a real, complete implementation whose extractor
   symbol is already registered in `feature_extractor_list[]` and an Accepted ADR
   has claimed its existence.
2. **Delete** — when the file is a duplicate of the canonical wired sibling, has
   no `extern` declaration, or is otherwise an abandoned WIP scaffold with no
   downstream consumer.
3. **Retain as `*_legacy`** — when there is an explicit upstream-port reference
   reason. *(Not exercised in this PR — no files met the bar.)*

The applied per-file dispositions are recorded in the PR body's decision table.

In summary:

- **Wired**: `core/src/feature/metal/float_ms_ssim_metal.mm` +
  `float_ms_ssim.metal` (ADR-0490 was Accepted but the meson wiring was missed).
- **Deleted**: 7 Vulkan `.c` files + 7 orphan `.comp` shaders, 11 Metal `.mm`
  files + 11 paired `.metal` kernel files, and one dead `extern` declaration
  (`vmaf_fex_integer_adm_metal`) in `feature_extractor.c`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Wire all dead files into meson | Preserves the most code | Several files are duplicates that would cause multiple-definition link errors; many have no kernel pairs or no extern + registry entry, so wiring them in is meaningless work | Most files are not actually wire-ready; the user's "no stubs" rule favours deletion |
| Delete everything including ADR-0490's `float_ms_ssim_metal.mm` | Maximum LOC removal | Would silently regress an Accepted ADR (0490) and leave a link-time bomb in the `feature_extractor_list[]` registry on macOS | The registry entry must be honoured by a real definition |
| Rename all dead files to `*_legacy.disabled` and skip them | Preserves history in-tree | User direction: "no stubs anywhere" — half-disabled scaffold sources are still scaffolds | Rejected on user direction |
| Defer to a follow-up PR per backend (one Vulkan, one Metal) | Smaller PRs | Same audit overhead twice, and the symbol-collision risks pull in the same registry edit either way | Bundle saves one round-trip with no added risk |

## Consequences

- **Positive**: ~3500 LOC of dead scaffold sources removed; the Metal link-time
  bomb for `float_ms_ssim_metal` is closed by the wire; one orphan `extern` is
  removed from `feature_extractor.c`. The `cambi_spatial_mask.comp` shader (only
  referenced by the deleted `integer_cambi_vulkan.c`) and similar orphan
  `.comp` / `.metal` files are gone, so the shader-build set is no longer a
  superset of what gets used.
- **Negative**: `git log` for the deleted files now requires `--follow` (which
  some tools omit) to traverse the deletion commit. Anyone resurrecting one of
  these scaffolds will need to consult this ADR for the rationale.
- **Neutral / follow-ups**:
  - `vmaf_fex_integer_adm_vulkan_legacy` (in `adm_vulkan.c`, the shim per
    ADR-0468 / `0468-integer-adm-vulkan-canonical-rename.md`) is **out of
    scope** for this PR — it is documented and intentionally retained.
  - Two ADRs still reference deleted files in their decision text:
    `0468-metal-cambi-real-kernel.md` (`integer_cambi_metal.mm`) and
    `0271-cuda-drain-batch-ms-ssim.md` (`integer_ms_ssim_vulkan.c`). Per the
    ADR immutability rule for Accepted ADRs, these text bodies are not edited
    here; the state-of-the-world is captured in this ADR and in `docs/state.md`.
  - `docs/state.md` row T-VULKAN-METAL-DEAD-SCAFFOLDS records the closure.

## References

- req: "wire or delete dead Vulkan/Metal extractor sources; no stubs anywhere"
  (paraphrased; deep-audit Finding 24, 2026-05-18)
- [ADR-0490](0490-float-ms-ssim-metal-port.md) — `float_ms_ssim` Metal port
  (the Accepted ADR whose meson wiring this PR completes)
- [ADR-0468 (metal-cambi)](0468-metal-cambi-real-kernel.md) — superseded for the
  Metal path by this deletion (no consumer ever materialised; kernel never wired)
- [ADR-0468 (integer-adm-vulkan-canonical-rename)](0468-integer-adm-vulkan-canonical-rename.md)
  — explains why `adm_vulkan.c` (legacy shim) is retained while the dead
  `integer_*_vulkan.c` copies elsewhere are removed
- [ADR-0421](0421-metal-first-kernel-motion-v2.md) — Metal kernel batch contract
- `.workingdir/bbb_reports/DEEP_AUDIT_2026_05_18.md` — Finding 24
