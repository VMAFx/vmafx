<!-- markdownlint-disable MD013 MD060 -->
# ADR-2342: Record the scope decisions of 2026-10-06 and 2026-10-07 in the candidate map

- **Status**: Accepted
- **Date**: 2026-10-07
- **Deciders**: maintainer
- **Tags**: release, rc, roadmap, process

## Context

[ADR-1868](1868-candidate-map-2026-10-05.md) and
[ADR-2001](2001-release-scope-1-0-and-roadmap-to-2-0.md) fixed what RC4 to RC9
own. [ADR-2001](2001-release-scope-1-0-and-roadmap-to-2-0.md) declared itself
the last change of the milestone map until 2.0, apart from bugs and findings;
the candidate numbers and the milestones do not change here either. Since then
the maintainer decided a series of scope items that live only in the decision
ledger, in issue comments and in work-package briefs: new RC4 work packages,
new RC5 tool-consolidation work, and the placement of many new issues in
1.1 to 1.3. An audit of 2026-10-07 found that `docs/roadmap.md`,
`docs/development/release.md`, the RC epics (#1723, #1724) and the tool epics
(#1249, #1250) still describe the map as of 2026-10-06, so a reader of those
pages cannot tell what RC4 and RC5 contain. This ADR records the decisions in
one place and gives the pages and epics one source to follow. It decides
nothing new.

## Decision

The candidate numbers, their order and the phase rules of
[ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) and
[ADR-1490](1490-rc3-rc9-candidate-map-cpu-capability.md) stand. The scope of
RC4 and RC5 is amended as follows, and the post-1.0 placements below are
recorded as decided.

### RC4 adds

| Item | Scope | Decision |
|---|---|---|
| WP10, FFmpeg series redesign | The whole FFmpeg patch series is rebuilt in one PR from `0001`, grouped by purpose, every patch re-audited under WP15, no numbering gaps; ADR-2166 is rewritten to supersede ADR-0860's numbering; draft #2381 is folded in. The redesigned series is the one HandBrake's patch set reuses in RC5. | Q-092, Q-095 |
| WP9, encode-time GPU pipeline | The fftools loopback-decoder hardware-acceleration patch is carried in `ffmpeg-patches/` only and refreshed with every FFmpeg release; Dolby Vision profile 5 sources are converted outside the pipeline, with no DV-specific work. | Q-047, Q-048 |
| WP3, Vulkan import | FFmpeg Vulkan hardware frames are imported zero-copy into CUDA, SYCL and HIP (import only, no Vulkan compute in RC4; ADR-2152). | Q-011, Q-089 |
| WP6, compat conformance | The SVT Encore VMAF profile options run as cases of the upstream-FFmpeg job on `libvmaf.so.3` ([#2364](https://github.com/VMAFx/vmafx/issues/2364)). | Q-059 |
| WP13, input formats | Semi-planar and packed host layouts, RGB / RGBA / BGRA only with a stated matrix, range and transfer, odd bit depths 9 to 15 with the twin matrix, and the CLI gaps; the filter and element format lists are generated from the definition (PRs #2376, #2378). Until then the library refuses a bit depth other than 8, 10, 12 and 16 by name. | Q-049, Q-050, Q-063 |
| WP14, engineering principles | `docs/principles.md` is rewritten per language with an applicability matrix; warnings are errors in every language; missing gates land in RC4, each proven with a planted defect. | Q-060, Q-061 |
| WP15, FFmpeg audit | A measured audit of the patch series and of every FFmpeg use in tools, AI scripts, containers and docs; RC4 implements cleanup, zero warnings, no deprecated API and correctness fixes; tuning findings become RC7 rows (PRs #2380, #2381). | Q-064 |
| WP16, observability | The observability package of [#2430](https://github.com/VMAFx/vmafx/issues/2430): metrics on every long-running binary, generated dashboards, alerts with runbooks, logs and traces. | Q-109, Q-110, Q-111 |
| WP17, cloud-native platform | The core cloud-native features of [#2431](https://github.com/VMAFx/vmafx/issues/2431): state out of the processes (PostgreSQL through CloudNativePG, a River queue, a two-tier cache, S3-compatible object storage, OCI artifact export, a time-series store), disposable horizontally scalable services, queue-driven scaling, an outbox with CloudEvents, CRDs, proto, OpenAPI and the Helm schema generated from the RC4 definition, workload identity and mTLS, a standalone SQLite profile kept. It supersedes the queue decision of [ADR-1119](1119-golusoris-go-framework-adoption.md). The line between this package and [#1252](https://github.com/VMAFx/vmafx/issues/1252) / [#1253](https://github.com/VMAFx/vmafx/issues/1253) is drawn by its architecture ADR; until that ADR is accepted those two issues keep their phase from ADR-2001. rclone stays as an opt-in mover for outputs and artifacts (backends without S3 or OCI) besides opt-in input mounts. | Q-112, Q-136 |
| Incremental motion | `motion2` and `motion3` are derived frame by frame, every twin kept exact, so live window scores ([#2138](https://github.com/VMAFx/vmafx/issues/2138), [#2238](https://github.com/VMAFx/vmafx/issues/2238)) complete before the flush (ADR-2090, PR #2290). The Rust twin implements the same hook. | Q-038, Q-088, Q-093 |
| Colour on the API | Input colorimetry is a per-context function on the library; the RC4 API carries colour per frame; `VmafPicture` is unchanged (ADR-2093). | Q-040 |
| WP12, packaging | Linux arm64 assets, an older glibc floor and the package-manager channels (PyPI, Homebrew, crates, conda and the others of [#2314](https://github.com/VMAFx/vmafx/issues/2314) and [#2319](https://github.com/VMAFx/vmafx/issues/2319), both moved to 1.0.0) follow in RC4 (WP12, [#2437](https://github.com/VMAFx/vmafx/issues/2437)), on top of the assets rc.3 ships. | Q-042, Q-130 |

### RC5 adds

| Item | Scope | Decision |
|---|---|---|
| Tool consolidation, phase | [#1249](https://github.com/VMAFx/vmafx/issues/1249) (Go sunset of `mcp-server/vmaf-mcp` and `tools/vmaf-tune`, per ADR-0703 and ADR-0704) and [#1250](https://github.com/VMAFx/vmafx/issues/1250) belong to RC5 as ADR-1868 says; their bodies' "after 1.0.0" sentence is stale. | ADR-1868 |
| Live alignment | [#2354](https://github.com/VMAFx/vmafx/issues/2354): two live or recorded feeds are aligned before scoring; timecode (MXF, SEI, ST 2110-40, VITC) is an alignment source and timeline key, and a constant frame-rate mismatch has a stated temporal mapping (nearest by timestamp, or refusal). | Q-052, Q-056 |
| Interlaced video | [#2361](https://github.com/VMAFx/vmafx/issues/2361): field order read or stated, interlaced input refused by name unless a policy is chosen, field-aware scoring. | Q-055 |
| Region masks | [#2362](https://github.com/VMAFx/vmafx/issues/2362): static and per-segment masks honoured by every extractor's pooling and every twin, exact, with the excluded area reported. | Q-057 |
| Container input | [#2363](https://github.com/VMAFx/vmafx/issues/2363): the CLI reads MXF, IMF, MPEG-TS and MP4 through optional FFmpeg-library input. | Q-058 |
| Bits per pixel and BD-rate | [#2284](https://github.com/VMAFx/vmafx/issues/2284): bits per pixel in every report with a known encoded size, BD-rate ported once into the Go tools. | Q-031 |
| HandBrake | [#2407](https://github.com/VMAFx/vmafx/issues/2407): recipes, a `vmaf-tune` HandBrakeCLI driver and a carried HandBrake patch set on the WP10 series. | Q-096 |
| Device-targeted scoring | [#2276](https://github.com/VMAFx/vmafx/issues/2276) as ADR-1880 set it; listed because the RC5 epic only carried it as prose. | ADR-1880 |
| Run-result timeline | The minimal common run-result timeline (one report format: per-frame and window scores, events, provenance) is built in RC5, because alignment ([#2354](https://github.com/VMAFx/vmafx/issues/2354)) and bits per pixel report on it. The richer parts of [#2311](https://github.com/VMAFx/vmafx/issues/2311) stay in 1.1. | Q-116 |
| Reusable provenance workflow | One reusable build-and-provenance workflow for every image, closing the SLSA level 3 gap of the operator and node images (HISS-11). | Q-114 |
| Vulkan compute experiment | A small experiment on `libgpudispatch` (psnr, motion, ssim) with a written verdict on whether SPIR-V kernels can be bit-identical; not a backend. A full Vulkan backend is decided in 1.2 with the Android GPU study ([#2247](https://github.com/VMAFx/vmafx/issues/2247)). It needs its own ADR before code. | Q-011 |
| Proposed ADRs finished | ADR-0709 finishes its end-to-end breadth in RC5; ADR-0781, ADR-0783 and ADR-0907 stay Proposed under declared exceptions that RC5 and RC8 finish. | Q-019, Q-023 |

### Post-1.0 placements

| Release | Placement | Decision |
|---|---|---|
| 1.1 | No-reference mode in the filter, element and OBS plugin, starting with UVQ ([#2413](https://github.com/VMAFx/vmafx/issues/2413)); TensorRT and DirectML providers ([#2414](https://github.com/VMAFx/vmafx/issues/2414)); live P.1204.1 / P.1204.2 monitor ([#2417](https://github.com/VMAFx/vmafx/issues/2417)); ComfyUI node, Diffusers callback and multimodal scorers ([#2418](https://github.com/VMAFx/vmafx/issues/2418)); native P.1203 ([#2280](https://github.com/VMAFx/vmafx/issues/2280)); WebRTC analyzer ([#2355](https://github.com/VMAFx/vmafx/issues/2355)); streaming outputs ([#2356](https://github.com/VMAFx/vmafx/issues/2356), now with ST 2110 and MPEG-TS over UDP inputs); the cookbook with GPAC, IMF / MXF and GStreamer-mixer recipes ([#2330](https://github.com/VMAFx/vmafx/issues/2330)); the libVLC sample ([#2357](https://github.com/VMAFx/vmafx/issues/2357)) | Q-104, Q-105, Q-106, Q-029, Q-053, Q-054, Q-059, Q-051 |
| 1.2 | Encoder-side predictors VQM4HAS and an in-loop VMAF predictor ([#2416](https://github.com/VMAFx/vmafx/issues/2416)); the VLC plugin ([#2358](https://github.com/VMAFx/vmafx/issues/2358)); the WebAssembly work ([#2248](https://github.com/VMAFx/vmafx/issues/2248)) and the Vulkan backend decision | Q-105, Q-051, Q-022, Q-011 |
| 1.3 | Own no-reference models FasterVQA, DOVER(-Mobile) and FGSVQA ([#2415](https://github.com/VMAFx/vmafx/issues/2415)); foveated scoring with our own FOVQA ([#2419](https://github.com/VMAFx/vmafx/issues/2419)) | Q-104, Q-107 |
| 1.4 | The `vmaf-tune` research ADR chain ([#2261](https://github.com/VMAFx/vmafx/issues/2261)), BVI-CC data scope ([#2241](https://github.com/VMAFx/vmafx/issues/2241)) | Q-021, Q-022 |

Where a model or metric has no code, or no usable code, we implement it
ourselves from its paper (clean room, with a patent check first where the
source is a Recommendation).

### Pages and epics that follow

`docs/roadmap.md`, `docs/development/release.md`, the epics #1723 (retitled to
cover all of RC4), #1724, #1249, #1250 and #1885, and the 1.0.0 milestone text
follow this ADR in the PR that adds it or in the issue edits that accompany
it.

## Alternatives considered

| Option | Pros | Cons | Outcome |
|---|---|---|---|
| One amending ADR that lists every decision with its ledger id (**chosen**) | Pages and epics get one source; a reader can trace each line to a decision; no new numbers | A long table | Chosen |
| One ADR per decision | Smaller records | More than thirty ADRs for decisions that were already made and sit in the ledger; the map would still be spread over them | Not chosen |
| Edit ADR-1868 and ADR-2001 in place | One page per map revision | Both are Accepted: their bodies are frozen (`docs/adr/README.md`) | Not chosen |
| Update only the issues and leave the pages | Cheapest | The public pages keep describing a map the project no longer follows | Not chosen |

## Consequences

- **Positive**: `docs/roadmap.md`, `docs/development/release.md` and the RC
  epics state the same scope; the RC4 work packages WP7 and WP10 to WP17 and
  the RC5 additions each name the decision behind them.
- **Negative**: RC4 and RC5 carry more work and more exit evidence.
- **Neutral / follow-ups**: the architecture ADR of WP17 states the split with
  #1252 / #1253 and carries a status note on ADR-1119. ADR-2343 records the
  reference-exact default for RC7. The milestone map of ADR-2001 is unchanged.

## References

- Decision ledger ids (the maintainer's popup answers): Q-011, Q-019, Q-021,
  Q-022, Q-023, Q-029, Q-031, Q-038, Q-040, Q-042, Q-047, Q-048, Q-049, Q-050,
  Q-051, Q-052, Q-053, Q-054, Q-055, Q-056, Q-057, Q-058, Q-059, Q-060, Q-061,
  Q-063, Q-064, Q-087, Q-088, Q-089, Q-092, Q-093, Q-095, Q-096, Q-104,
  Q-105, Q-106, Q-107, Q-109, Q-110, Q-111, Q-112, Q-114, Q-116, Q-130, Q-136.
- `Q-112`: "Re-plan + build in RC4"
- `Q-095`: "Redesign in WP10, one PR"
- `Q-116`: "Minimal timeline into RC5"
- `Q-096`: "Everything in 1.0 (RC5)"
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md), [ADR-1421](1421-rc3-rc8-candidate-map.md), [ADR-1490](1490-rc3-rc9-candidate-map-cpu-capability.md), [ADR-1829](1829-rc4-zero-copy-import.md), [ADR-1852](1852-vmafx-api-redesign.md), [ADR-1868](1868-candidate-map-2026-10-05.md), [ADR-1880](1880-format-envelope-device-targets.md), [ADR-2001](2001-release-scope-1-0-and-roadmap-to-2-0.md)
- Issues [#1723](https://github.com/VMAFx/vmafx/issues/1723), [#1724](https://github.com/VMAFx/vmafx/issues/1724), [#1249](https://github.com/VMAFx/vmafx/issues/1249), [#1250](https://github.com/VMAFx/vmafx/issues/1250), [#2430](https://github.com/VMAFx/vmafx/issues/2430), [#2431](https://github.com/VMAFx/vmafx/issues/2431)
