# Roadmap

VMAFx tracks its plan in GitHub, not in a document that drifts. This page is a
map of where that plan lives and how the releases are sequenced.

- **Board** — [VMAFx Roadmap](https://github.com/orgs/VMAFx/projects/1) (public)
- **Milestones** — [all milestones](https://github.com/VMAFx/vmafx/milestones)
- **Epics** — issues labelled
  [`epic`](https://github.com/VMAFx/vmafx/issues?q=is%3Aissue+is%3Aopen+label%3Aepic),
  each with a child task list

## Releases

| Milestone | Theme |
| --- | --- |
| [1.0.0](https://github.com/VMAFx/vmafx/milestone/1) | First release: RC1 correctness and tester reports, RC2 stabilisation, RC3 twin exactness, RC4 first full Rust metric, zero-copy import, the VMAFx API, the FFmpeg and GStreamer integrations and the cloud-native platform, RC5 deduplication, tool consolidation and new metrics, RC6 GPU capability table, RC7 CPU capability table, RC8 benchmarking and tuning, RC9 real model retraining, then final |
| [1.1](https://github.com/VMAFx/vmafx/milestone/2) | Integrations and live quality: the OBS Studio plugin ([#2239](https://github.com/VMAFx/vmafx/issues/2239)), [#2148](https://github.com/VMAFx/vmafx/issues/2148), [#2147](https://github.com/VMAFx/vmafx/issues/2147), [#2144](https://github.com/VMAFx/vmafx/issues/2144), [#2146](https://github.com/VMAFx/vmafx/issues/2146), [#2159](https://github.com/VMAFx/vmafx/issues/2159); no-reference mode in the plugins ([#2413](https://github.com/VMAFx/vmafx/issues/2413)), the live P.1204 monitor ([#2417](https://github.com/VMAFx/vmafx/issues/2417)), AI-generated video scoring ([#2418](https://github.com/VMAFx/vmafx/issues/2418)), WebRTC, streaming outputs, libVLC and cookbook recipes, and the rest of the milestone |
| [1.2](https://github.com/VMAFx/vmafx/milestone/3) | Encoder feedback, embedding and platforms: the rest of the embedding epic ([#2067](https://github.com/VMAFx/vmafx/issues/2067)), [#2164](https://github.com/VMAFx/vmafx/issues/2164), [#2156](https://github.com/VMAFx/vmafx/issues/2156); encoder-side predictors ([#2416](https://github.com/VMAFx/vmafx/issues/2416)), the VLC plugin ([#2358](https://github.com/VMAFx/vmafx/issues/2358)), the mobile and WebAssembly targets |
| [1.3](https://github.com/VMAFx/vmafx/milestone/4) | New metrics with exact twins: [#2165](https://github.com/VMAFx/vmafx/issues/2165), [#2167](https://github.com/VMAFx/vmafx/issues/2167), [#2166](https://github.com/VMAFx/vmafx/issues/2166), picks from [#2168](https://github.com/VMAFx/vmafx/issues/2168); our own no-reference models ([#2415](https://github.com/VMAFx/vmafx/issues/2415)) and foveated scoring ([#2419](https://github.com/VMAFx/vmafx/issues/2419)) |
| [1.4](https://github.com/VMAFx/vmafx/milestone/8) | Metric A/B comparison, the best current mix and more training data: [#2240](https://github.com/VMAFx/vmafx/issues/2240), [#2241](https://github.com/VMAFx/vmafx/issues/2241) |
| [1.5](https://github.com/VMAFx/vmafx/milestone/9) | The next model generation: [#2242](https://github.com/VMAFx/vmafx/issues/2242) |
| [2.0](https://github.com/VMAFx/vmafx/milestone/5) | Breaking changes only: the `libvmaf.h` compatibility library removed ([ADR-1852](adr/1852-vmafx-api-redesign.md)), the C++23 core, the rest of [#1254](https://github.com/VMAFx/vmafx/issues/1254) |

[ADR-2001](adr/2001-release-scope-1-0-and-roadmap-to-2-0.md) set this layout on
2026-10-06 and declared it the last change of the milestone map until 2.0,
apart from bugs and findings. The cloud-native work (scoring API contract,
server mode, observability, the cloud-native platform, containers, Helm,
operator, GPU pool arbiter) is part of 1.0.0.
Every release after 1.0.0 runs its own candidate cycle: features first, then
deduplication, tests and bug fixing, then capability tables with benchmarks and
tuning, then training if models change, then the release; the same phase rules
as for 1.0.0 apply inside each cycle.

Two milestones are deliberately **rolling** rather than tied to a release:

- [Models & benchmarks](https://github.com/VMAFx/vmafx/milestone/6) — retraining
  cadence, benchmark baselines, corpus work.
- [Code health & deduplication](https://github.com/VMAFx/vmafx/milestone/7) —
  the
  fork adds and changes a lot, so slimming it is recurring work, not a one-off.

## How 1.0.0 is gated

The fork's first release candidate, `v1.0.0-rc.1`, was published on 2026-09-27;
older tags are inherited upstream history.
[ADR-1341](adr/1341-rc-correctness-benchmark-retrain-sequence.md) gives each
first-release candidate one responsibility.
[ADR-1421](adr/1421-rc3-rc8-candidate-map.md) maps the stages to tags, so that
each stage number matches its `v1.0.0-rc.N` tag (it supersedes the mapping of
[ADR-1352](adr/1352-rc-phase-shift-plus-one.md)), and
[ADR-1490](adr/1490-rc3-rc9-candidate-map-cpu-capability.md) inserts the CPU
capability stage as RC7, which moves benchmarking to RC8 and retraining to RC9.
[ADR-1868](adr/1868-candidate-map-2026-10-05.md) folds the work that joined
1.0.0 on 2026-10-05 into those candidates without new numbers: the new API and
provenance into RC4, tool consolidation, new metrics and the Metal SpEED twins
into RC5, training readiness into RC8.
[ADR-1880](adr/1880-format-envelope-device-targets.md) adds the format envelope:
an overflow audit at 8K and 16K and 8K exactness in RC3, the supported
resolutions, bit depths and layouts per backend and device in the RC6 and RC7
tables, throughput per resolution in RC8; and device-targeted scoring (one
decode scored for several displays) in RC5.
[ADR-2001](adr/2001-release-scope-1-0-and-roadmap-to-2-0.md) moves the
cloud-native work into 1.0.0 without new numbers: the versioned scoring API
contract, server mode and observability in RC4, containers, Helm chart, operator
and GPU pool arbiter in RC5, distributed throughput in RC8; a native GStreamer
element, an API ready for OBS Studio and real-time FFmpeg GPU scoring in RC4.
The OBS Studio plugin follows in 1.1.
[ADR-2342](adr/2342-rc-map-amendment-2026-10.md) records the scope decisions of
2026-10-06 and 2026-10-07, again without new numbers: RC4 also holds the
FFmpeg series redesign, the input-format work, the engineering principles per
language, the FFmpeg audit, the observability package and the cloud-native
platform; RC5 also holds live alignment, interlaced video, region masks,
container input, bits per pixel and BD-rate, HandBrake support, the minimal
run-result timeline and the reusable provenance workflow.

### What this means for you

- **Use the newest candidate.** Each candidate keeps the Netflix golden scores;
  later candidates make GPU and SIMD results match the CPU and remove
  duplicated code.
- **No performance claims before RC8.** Candidates up to RC7 are about
  correctness; speed numbers are measured and tuned in RC8.
- **Model retraining comes last**, in RC9.
- **Help test.** Results from hardware the project does not own count as
  evidence: run the [tester image](usage/tester-image.md).

### The stages

| Stage | Theme | Tracking issue |
| --- | --- | --- |
| **RC1** | correctness and tester readiness | — |
| **RC2** | stabilisation and repair | — |
| **RC3** | twin exactness, overflow audit at 8K and 16K | [#1721](https://github.com/VMAFx/vmafx/issues/1721) |
| **RC4** | first full Rust metric, zero-copy device-frame import, new VMAFx API and its bindings, FFmpeg filters and series redesign, GStreamer element, provenance, input formats, scoring API contract, server mode, observability, cloud-native platform, engineering principles, OBS-ready API | [#1723](https://github.com/VMAFx/vmafx/issues/1723) |
| **RC5** | deduplication, tool consolidation (live alignment, interlaced video, region masks, container input, bits per pixel and BD-rate, HandBrake, run-result timeline), new metrics with exact twins, Metal SpEED twins, device-targeted scoring, containers, Helm, operator, GPU pool arbiter | [#1724](https://github.com/VMAFx/vmafx/issues/1724) |
| **RC6** | GPU capability source of truth, GPU format envelope, legacy GPU build variants | [#1725](https://github.com/VMAFx/vmafx/issues/1725) |
| **RC7** | CPU capability source of truth, CPU format envelope, full SIMD ladder on five architectures | [#1885](https://github.com/VMAFx/vmafx/issues/1885) |
| **RC8** | benchmark and tune, throughput per resolution, distributed throughput, training readiness | [#1245](https://github.com/VMAFx/vmafx/issues/1245) |
| **RC9** | real retraining | [#1246](https://github.com/VMAFx/vmafx/issues/1246), [#1242](https://github.com/VMAFx/vmafx/issues/1242) |
| **Final `v1.0.0`** | — | — |

#### RC1 — correctness and tester readiness

- **In scope:** Release-blocking correctness, reliability, security, build,
  packaging, backend usability, and a portable report path for outside hardware
- **Exit boundary:** The exact candidate head is green; no confirmed RC1 blocker
  or untriaged `docs/state.md` row remains; a tester can return
  artifact/environment identity, device and tool versions, backend availability,
  correctness/parity results, commands, logs, and failures

#### RC2 — stabilisation and repair

- **In scope:** The dependency updates and correctness fixes merged since rc.1,
  delivered to testers through the same report path; no benchmark or training
  work
- **Exit boundary:** The RC1 boundary, re-established on the rc.2 head

#### RC3 — twin exactness

- **In scope:** Every GPU and SIMD twin returns the CPU extractor's scores bit
  for bit, or carries a measured tolerance recorded in an ADR; no SYCL kernel
  uses scratch memory. Since 2026-10-05
  ([ADR-1880](adr/1880-format-envelope-device-targets.md)): an integer-overflow
  audit of every extractor and twin at 8K and 16K frame sizes with 16-bit
  samples
- **Exit boundary:** Per-twin parity measured at `--precision max` on the
  Netflix pairs, the 1080p checkerboard pairs, the 4K fixture and 8K cells;
  no accumulator overflows at 16K with 16-bit samples; Netflix golden
  assertions unchanged

#### RC4 — first full Rust metric, zero-copy import and the VMAFx API

Also in RC4 since 2026-10-05 (ADR-1868): provenance on every score
([#2142](https://github.com/VMAFx/vmafx/issues/2142)) through the new API.

- **In scope:** The whole `vmaf_v1.0.16_3d0h` path (cambi, speed_chroma, integer
  adm3, integer motion3, model prediction) in Rust; the C ABI is unchanged and
  the GPU twins stay CUDA, SYCL and HIP code. The whole device-memory import API
  ([ADR-1829](adr/1829-rc4-zero-copy-import.md)): additive import on
  `VmafPicture2` with fences in both directions for CUDA, SYCL, HIP and Metal,
  NV12 and P010 on the GPU, CUDA without its device-to-device copy, SYCL chroma
  import and D3D11, Metal IOSurface and MTLTexture bound without the CPU copy, a
  HIP import path, FFmpeg filters that take hardware frames. The new VMAFx C API
  (`vmafx/*.h`, `libvmafx.so.1`) generated with every other surface from
  `core/api/vmafx.toml`, `libvmaf.h` as a thin compatibility library on it, and
  the FFmpeg filters under VMAFx names (`vmafx`, `vmafx_tune`, `vmafx_pre`)
  ([ADR-1852](adr/1852-vmafx-api-redesign.md)). Since 2026-10-06
  ([ADR-2001](adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)): the versioned
  scoring API contract ([#2155](https://github.com/VMAFx/vmafx/issues/2155)) and
  server mode with observability
  ([#1251](https://github.com/VMAFx/vmafx/issues/1251)), generated from the same
  definition; a native `vmafx` GStreamer element
  ([#2236](https://github.com/VMAFx/vmafx/issues/2236)) and CI conformance of
  the `vmaf` element of upstream's gst-plugins-bad on the compatibility
  `libvmaf.so.3` ([#2237](https://github.com/VMAFx/vmafx/issues/2237)); an API
  ready for OBS Studio ([#2238](https://github.com/VMAFx/vmafx/issues/2238):
  texture import including OpenGL interop, asynchronous window scores, bounded
  queues); real-time FFmpeg GPU scoring with `n_stats`
  ([#2138](https://github.com/VMAFx/vmafx/issues/2138)). Since 2026-10-07
  ([ADR-2342](adr/2342-rc-map-amendment-2026-10.md)): language bindings
  generated from the definition (Rust, Go, Python); the FFmpeg patch series
  redesigned in one change from `0001`, grouped by purpose, after an audit of
  everything VMAFx uses in FFmpeg; host input of every semi-planar and packed
  layout, RGB with a stated matrix and every bit depth from 8 to 16;
  incremental `motion2` / `motion3` for live windows; Vulkan frame import;
  engineering principles per language with warnings as errors in every one;
  the observability package
  ([#2430](https://github.com/VMAFx/vmafx/issues/2430)); the cloud-native
  platform ([#2431](https://github.com/VMAFx/vmafx/issues/2431)): state out of
  the processes (PostgreSQL, a job queue, a two-tier cache, object storage, OCI
  artifacts), scaling on queue depth, and CRDs, proto, OpenAPI and the Helm
  schema generated from the definition; conformance of SVT Encore's VMAF
  options on the compatibility library ([#2364](https://github.com/VMAFx/vmafx/issues/2364))
- **Exit boundary:** The Rust path is bit-identical to the C path on the parity
  fixtures; an imported device frame scores bit-identically to the same frame
  uploaded from the host, with no host copy of pixel data and fence-ordering
  tests that fail when a fence is skipped; the golden-data gate passes through
  the compatibility library and every generated surface is checked against the
  definition

#### RC5 — deduplication

- **In scope:** One implementation per behaviour across GPU twins and host code,
  the Rust code included; `libgpudispatch` extracted, folding in the per-backend
  import code RC4 wrote ([#1455](https://github.com/VMAFx/vmafx/issues/1455)).
  One implementation per tool
  ([#1249](https://github.com/VMAFx/vmafx/issues/1249)), the `tools/` surface
  finished and the known unfinished surfaces closed
  ([#1250](https://github.com/VMAFx/vmafx/issues/1250),
  [#1270](https://github.com/VMAFx/vmafx/issues/1270),
  [#1272](https://github.com/VMAFx/vmafx/issues/1272)). The new metrics with
  their twins written once on `libgpudispatch`: ΔE-ITP, PU21, NIQE, BRISQUE,
  Y-FUNQUE+ ([#1247](https://github.com/VMAFx/vmafx/issues/1247),
  [#1248](https://github.com/VMAFx/vmafx/issues/1248)), HDR-SSIM and HDR-MS-SSIM
  ([#2161](https://github.com/VMAFx/vmafx/issues/2161)), XPSNR
  ([#2158](https://github.com/VMAFx/vmafx/issues/2158)); Metal twins of
  `speed_chroma` and `speed_temporal`
  ([#2160](https://github.com/VMAFx/vmafx/issues/2160)). Device-targeted scoring
  ([ADR-1880](adr/1880-format-envelope-device-targets.md)): device profiles
  (phone, tablet, laptop, TV, VR per eye, portrait included), each a target
  resolution, a scaling and a viewing distance per display height mapped onto
  the ADM options `nvd` and `rdh`; one decode scored for many targets; a short
  research pass first, the profile table generated by the RC6 / RC7 table
  machinery. Cloud-native deployment
  ([ADR-2001](adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)): containers,
  Helm chart and a kind plus kuttl test setup
  ([#1252](https://github.com/VMAFx/vmafx/issues/1252)); the operator, the
  controller / node split and the GPU pool arbiter in `libgpudispatch`
  ([#1253](https://github.com/VMAFx/vmafx/issues/1253)). Since 2026-10-07
  ([ADR-2342](adr/2342-rc-map-amendment-2026-10.md)) the tool consolidation
  also carries live alignment of two feeds with timecode
  ([#2354](https://github.com/VMAFx/vmafx/issues/2354)), interlaced video
  ([#2361](https://github.com/VMAFx/vmafx/issues/2361)), region masks
  ([#2362](https://github.com/VMAFx/vmafx/issues/2362)), container input
  ([#2363](https://github.com/VMAFx/vmafx/issues/2363)), bits per pixel and
  BD-rate ([#2284](https://github.com/VMAFx/vmafx/issues/2284)), HandBrake
  support ([#2407](https://github.com/VMAFx/vmafx/issues/2407)) and the minimal
  run-result timeline they report on; one reusable build-and-provenance
  workflow for every image; and a small Vulkan compute experiment with a
  written verdict. The Go tools replace the Python MCP server and `vmaf-tune`
  here, not after 1.0.0
- **Exit boundary:** Scores unchanged against the RC3 reference; duplicated code
  removed rather than moved; every new twin bit-identical to its CPU extractor
  or within a measured libm bound recorded in an ADR; a multi-target run scores
  each target as a separate run with that target's options would

#### RC6 — GPU capability source of truth

- **In scope:** A per-vendor capability table generated from
  `nvcc --list-gpu-arch`, `ocloc` and ROCm `llc -mcpu=help`, checked in with a
  CI drift check, covering the twins RC5 adds; dispatch and kernel parameters
  read it, with a generic fallback for unknown devices; every kernel compiled
  and statically audited for every target (scratch, spills, register ceiling,
  fp64). The table also declares the format envelope per backend and device
  ([ADR-1880](adr/1880-format-envelope-device-targets.md)): maximum resolution
  up to 16K with measured memory limits and tiling where needed, bit depths 8
  to 16, chroma layouts, odd and portrait sizes, each row backed by a test.
  Legacy build variants ([ADR-2001](adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)):
  CUDA 12.x builds for Maxwell, Pascal and Volta (sm_50 to sm_72; CUDA 13.4
  starts at compute_75), the Intel legacy compute runtime for Gen9 to Gen11
  iGPUs, and every AMD target the pinned ROCm compiler still emits, each
  bit-exact and listed in the table
- **Exit boundary:** Drift check green, the envelope included; audit clean for
  every listed target

#### RC7 — CPU capability source of truth

- **In scope:** The CPU twin of RC6: a checked-in table, generated by one
  script, of the CPU features each SIMD kernel needs (from the
  per-translation-unit compile flags in `core/src/meson.build` and the runtime
  gates in `core/src/x86/cpu.c` and `core/src/arm/cpu.c`), with a CI drift
  check; a per-function disassembly audit for x86 and aarch64; every dispatch
  level run bit-exact against scalar under emulation. The full SIMD ladder
  ([ADR-2001](adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)): every
  extractor gets a bit-exact kernel at every useful ISA level with runtime
  dispatch, on x86-64 (SSE2, SSSE3, SSE4.1, AVX, AVX2, AVX-512, AVX-512 ICL,
  AVX10), AArch64 (NEON, dotprod/i8mm, SVE, SVE2), RISC-V RVV 1.0, POWER VSX
  and LoongArch LSX/LASX; qemu-user CI for architectures without hardware,
  the golden gate on each. The CPU format envelope
  (resolution up to 16K with measured memory limits, bit depths 8 to 16, chroma
  layouts, odd and portrait sizes) in the same table, each row test-backed
  ([ADR-1880](adr/1880-format-envelope-device-targets.md)). A reference
  conformance column: every extractor with an original implementation is
  proven against it, the default becomes reference-exact and Netflix's
  behaviour a named compatibility mode that the golden gate runs in
  ([ADR-2343](adr/2343-reference-exact-default-compat-mode.md),
  [#2286](https://github.com/VMAFx/vmafx/issues/2286))
- **Exit boundary:** Drift check green; audit finds no instruction outside the
  feature set a gate guarantees; parity green under Intel SDE (AVX2-only model,
  Skylake-X, Ice Lake, Sapphire Rapids, the AMD AVX-512 set) and qemu (aarch64
  NEON, SVE2 at more than one vector length). Reports from real Xeon or Apple
  Silicon machines are extra evidence, not a requirement; timing is RC8

#### RC8 — benchmark and tune

- **In scope:** Comparable benchmark baselines, profiling, hardware-generation
  retuning, and measured performance fixes, including the speed RC3 gave up for
  exactness; throughput per resolution, 16K included (the envelope itself is RC6
  / RC7 evidence), and distributed throughput across nodes
  ([ADR-2001](adr/2001-release-scope-1-0-and-roadmap-to-2-0.md)). Training
  readiness: automatic temporal alignment and the HDR-input guard for SDR models
  ([#2163](https://github.com/VMAFx/vmafx/issues/2163),
  [#2157](https://github.com/VMAFx/vmafx/issues/2157)), the external-metric
  runner and estimator calibration
  ([#2162](https://github.com/VMAFx/vmafx/issues/2162),
  [#2143](https://github.com/VMAFx/vmafx/issues/2143)), the HDR conversion-check
  workflow ([#2145](https://github.com/VMAFx/vmafx/issues/2145)), the mini
  retrain in CI and the measured resource plan of
  [#1246](https://github.com/VMAFx/vmafx/issues/1246)
- **Exit boundary:** Results identify the exact artifact, fixtures, host,
  drivers and runtimes; accepted wins are re-measured and preserve
  correctness/parity; the mini retrain passes every stage

#### RC9 — real retraining

- **In scope:** The locked one-shot model retraining programme on the clean,
  tuned tree, started only when every precondition of
  [#1246](https://github.com/VMAFx/vmafx/issues/1246) holds (the RC4 to RC8
  items above included). The shipped v1 models read compatibility-mode
  features until this run; the retrain trains on reference-exact features
  ([ADR-2343](adr/2343-reference-exact-default-compat-mode.md))
- **Exit boundary:** Model-quality gates, model cards, registry/signing
  metadata, and unchanged Netflix golden assertions pass

#### Final `v1.0.0`

- **In scope:** Accepted RC9 output plus any required repair candidate
- **Exit boundary:** Publication preflight passes on the immutable final tag

“Done fixing” is deliberately bounded rather than a promise that no future bug
will be found. RC1 and RC2 are ready when there are no confirmed, actionable
release blockers and no untriaged rows. Performance-only findings belong to
RC8, real training belongs to RC9, and externally blocked work stays explicitly
deferred with its trigger and evidence.

If any later candidate exposes a correctness regression, fix it and rerun the
affected stage evidence before proceeding. Do not pull general benchmarking
into RC1 to RC7, or real training before RC8 evidence is accepted. Speed that
RC3 gives up for exactness is recorded as a tuning row and recovered in RC8; it
is not traded back for a tolerance. The RC1 and RC2 report envelope may run a
short correctness and device-engagement smoke; it does not make a performance
claim.

Ordinary Renovate and other version PRs remain mergeable throughout the
sequence when normal required checks, review, pinning, and component-specific
validation pass. Security updates are prioritised rather than being the only
allowed updates. Because evidence is exact-head, any later merge requires the
affected candidate checks or measurements to be rerun.

## Things that do not change

Some guarantees are load-bearing for downstream users and hold across every
milestone above, with one exception: 2.0 removes the `libvmaf.h` compatibility
library ([ADR-2001](adr/2001-release-scope-1-0-and-roadmap-to-2-0.md),
[ADR-1852](adr/1852-vmafx-api-redesign.md)). Until then:

- The **Netflix golden values** are never edited. They are the numerical
  ground truth; if scores drift, the code is wrong.
- The **`libvmaf.so` ABI** and the FFmpeg `libvmaf` filter name stay stable,
  even
  as the internals move to C++23 and parts of the tooling move to Go and Rust.
- The public **C API** under `core/include/libvmaf/` stays source-compatible.
- Release artifacts are **built in the container**, never from a host build.

## Contributing against the roadmap

Pick an epic, read its task list, and open a PR that closes one line of it.
Epics
are intentionally coarse — sub-tasks become their own issues when someone starts
them, so the tracker reflects work in progress rather than a wish list.
