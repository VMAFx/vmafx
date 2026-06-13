<!-- markdownlint-disable MD036 MD060 -->
# ADR-0126: SSIMULACRA 2 perceptual metric as a fork-local feature extractor

- **Status**: Accepted
- **Date**: 2026-04-20
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: metrics, feature-extractor, docs, agents

## Context

Traditional pixel-error metrics (PSNR, MSE) and the original SSIM family
were designed for codecs that produce blocking / ringing artifacts
(H.264, MPEG-2). Modern codecs — AV1, HEVC at low bitrates, VVC, and
AI-upscaled content — deliberately smooth and tone-map pixels to
optimise against PSNR and simple SSIM. The result is that a
high-PSNR / high-SSIM output can look visibly worse than a
low-PSNR output that preserves high-frequency structure.

[SSIMULACRA 2](https://github.com/cloudinary/ssimulacra2)
(Structural SIMilarity Unveiling Local And Compression Related
Artifacts, version 2; Jyrki Alakuijala / Cloudinary / libjxl) addresses
this gap by combining a multi-scale SSIM-like structural comparison
with an **asymmetric** penalty that punishes lost texture energy far
more heavily than added noise. It has become the de-facto modern-codec
quality metric in the JPEG XL and AV1 image-compression communities,
and is already cited in the Cloudflare image-codec comparison
harness. For the fork — whose positioning is "VMAF, but better for
modern codecs and heterogeneous compute" — shipping SSIMULACRA 2 as a
first-class feature extractor closes the most-requested metric gap.

The fork's feature-extractor surface is well-established:
[`core/src/feature/AGENTS.md`](../../core/src/feature/AGENTS.md)
documents the `VmafFeatureExtractor` contract and the
`add-feature-extractor` skill scaffolds the file layout. SSIMULACRA 2
fits that mould cleanly — it is a full-reference, per-frame metric
operating on the luma plane plus chroma-weighted contributions, with
a deterministic output score. No new build-system machinery is
required beyond registering the extractor.

The reference implementation is
[`tools/ssimulacra2.cc`](https://github.com/libjxl/libjxl/blob/main/tools/ssimulacra2.cc)
in the libjxl repository, under a BSD-3-Clause license that is
compatible with our BSD-3-Clause-Plus-Patent fork license.

## Decision

We will **port the libjxl C++ reference implementation to a new C
feature extractor** under
`core/src/feature/ssimulacra2.c`, matching libjxl's numerical output
on an agreed float tolerance. The extractor will register under the
name `ssimulacra2` and provide scores `ssimulacra2` (main score, 0–100
scale where 100 = pristine), plus the three per-scale MSSIM-like
sub-metrics libjxl exposes. The extractor:

- Ships scalar-first. AVX2 / AVX-512 / NEON SIMD paths are a
  follow-up workstream gated by a separate ADR and the
  [add-simd-path](../../.claude/skills/add-simd-path/SKILL.md) skill.
- Links against no new external dependency. The libjxl functions we
  need (XYB colour conversion, multi-scale Gaussian blur, asymmetric
  penalty) are copied in verbatim with attribution, not pulled via
  a `subprojects/` wrap.
- Validates bit-closeness (not bit-equality) against the libjxl
  reference via a new snapshot test under
  `testdata/scores_cpu_ssimulacra2.json`, regenerated through
  [`/regen-snapshots`](../../.claude/skills/regen-snapshots/SKILL.md)
  with the justification recorded in the first implementation commit.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port libjxl C++ → C (chosen) | Known-good reference; 1:1 math; small surface; no new build dependency | Manual port work; need to track libjxl upstream changes (slow-moving) | Best fit for our "pure C with bolt-on SIMD" pattern and the existing `add-feature-extractor` scaffold |
| Link libjxl as a `subprojects/` wrap | No port work; get updates for free | Drags in libjxl's huge dependency tree (highway SIMD lib, brotli, skcms); complicates CUDA/SYCL static linking; license-legal only, not license-ideal | Dependency bloat disqualifies it — libjxl is ~2 MB compiled for one extractor's worth of output |
| FFI to Rust `ssimulacra2_bin` | Mature, actively maintained port; good perf | Introduces Rust as a build-time dep on a pure-C/C++ library; breaks the Netflix-upstream-sync story (upstream is C, we stay C) | Adding Rust to the build matrix is a strictly larger decision than this one and would need its own ADR |
| Re-derive from the Alakuijala paper | Cleanest IP story | Highest engineering risk; no known-good reference to bit-compare against during port | Offers nothing the libjxl port doesn't, at much higher cost |

## Consequences

**Positive**

- Closes the biggest cited gap in "VMAF doesn't understand modern
  codecs"; gives the fork a distinctive feature in the crowded VQA
  landscape.
- New extractor follows the `add-feature-extractor` template — zero
  build-system / dispatcher surprises.
- Enables the follow-up VR/viewport workstream (ADR forthcoming) to
  compose SSIMULACRA 2 over an extracted viewport the same way we
  compose VMAF.
- Small, focused surface for the CI matrix: one new CPU-only feature,
  no GPU kernel required for shipping.

**Negative**

- Upstream drift: libjxl is unlikely to rewrite SSIMULACRA 2, but any
  fix or extension they ship has to be manually mirrored. Mitigated
  by pinning the port to a specific libjxl commit in the header and
  re-auditing on each `sync-upstream` cycle.
- Another feature in the `make test` matrix; another entry in
  `docs/metrics/features.md`; another snapshot JSON. Minor but
  compounding.
- Port-level rounding differences from libjxl are possible. We accept
  them if they fall within a documented tolerance; bit-equality is
  not a requirement (libjxl uses `-ffast-math` in some builds, which
  we do not).

**Neutral**

- No impact on the Netflix golden gate (CLAUDE.md §8) — it does not
  exercise SSIMULACRA 2.
- No change to the `libvmaf` public ABI. The extractor is
  discovered by name through the existing registry.

## References

- [req] User instruction round, AskUserQuestion popup answered
  2026-04-20: "SSIMULACRA 2 (Google)" selected as the next-priority
  perceptual metric; "Port JPEG XL C++ reference" selected as the
  port source.
- [github.com/libjxl/libjxl](https://github.com/libjxl/libjxl) —
  `tools/ssimulacra2.cc`, BSD-3-Clause license.
- [github.com/cloudinary/ssimulacra2](https://github.com/cloudinary/ssimulacra2)
  — the original Cloudinary reference (predates the libjxl merge).
- [J. Alakuijala et al., SSIMULACRA 2](https://arxiv.org/abs/2309.02960)
  — arXiv writeup.
- [ADR-0125](0125-ms-ssim-decimate-simd.md) — SIMD-for-existing-metric
  pattern to mirror when the follow-up SSIMULACRA 2 SIMD ADR is
  written.
- [Research-0003](../research/0003-ssimulacra2-port-sourcing.md) —
  source-selection digest and open questions.
- [CLAUDE.md §12 r10](../../CLAUDE.md) — per-surface docs rule
  (extractor gets an entry in `docs/metrics/features.md`).

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `core/src/feature/ssimulacra2.c` — present.
- `core/src/feature/ssimulacra2_eotf_lut.h` /
  `ssimulacra2_math.h` / `ssimulacra2_simd_common.h` — present.
- `core/src/meson.build` registers the extractor (lines 137 and
  the arm64 NEON sub-library at 350-360).
- ADR-0130 (Accepted) was the implementation closeout for this ADR;
  the SSIMU2 SIMD bit-exact follow-ups are tracked under
  ADR-0161/0162/0163.
- Verification command:
  `ls core/src/feature/ssimulacra2.c core/src/feature/ssimulacra2_*.h`.
