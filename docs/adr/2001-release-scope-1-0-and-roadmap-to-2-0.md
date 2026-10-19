<!-- markdownlint-disable MD013 MD060 -->
# ADR-2001: Release scope of 1.0.0 and the roadmap to 2.0

- **Status**: Accepted (partially superseded by [ADR-2350](2350-cloud-native-platform.md) for its RC4 / RC5 cloud-native rows (the platform core moves to RC4; the GPU pool arbiter stays in RC5))
- **Date**: 2026-10-06
- **Deciders**: maintainer
- **Tags**: release, rc, roadmap, process

## Context

[ADR-1868](1868-candidate-map-2026-10-05.md) and
[ADR-1880](1880-format-envelope-device-targets.md) folded the 2026-10-05 scope
into the candidates RC4 to RC9 of 1.0.0, and the post-1.0 milestones were still
the older sketch (and RC6 / RC7 named only the hardware the project owns): 1.1 new metrics, 1.2 cloud-native foundation, 1.3
cloud-native scale-out, 2.0 language modernisation, plus a post-1.0 embedding
milestone ([ADR-1685](1685-post-1-0-embedding-zero-copy-milestone.md)). On
2026-10-06 the maintainer asked for the cloud-native work to join 1.0.0, for the
post-1.0 milestones to be re-sorted and redesigned, whether 1.0.0 needs
anything for GStreamer or direct OBS Studio support, and declared this the last
change of milestones until 2.0, apart from bugs and findings made on the way.
A two-minor layout after 1.0 was discussed and rejected: later releases need
deduplication, testing, bug fixing, tuning and training again, and new metrics
with A/B testing, the best mix and more training data make the 1.x line
heavier than a two-minor layout can carry.

## Decision

The candidate numbers RC1 to RC9 do not change. Their scope grows as follows.

| Candidate | Adds |
|---|---|
| RC4 | The versioned scoring API contract ([#2155](https://github.com/VMAFx/vmafx/issues/2155)) and server mode with observability ([#1251](https://github.com/VMAFx/vmafx/issues/1251)), both generated from the same definition as every other surface (`core/api/vmafx.toml`, work package 8). A native `vmafx` GStreamer element ([#2236](https://github.com/VMAFx/vmafx/issues/2236)) and CI conformance of the `vmaf` element of upstream's gst-plugins-bad on the compatibility `libvmaf.so.3` ([#2237](https://github.com/VMAFx/vmafx/issues/2237)). An API ready for OBS Studio ([#2238](https://github.com/VMAFx/vmafx/issues/2238)): texture import including OpenGL interop, asynchronous window scores, bounded queues. Real-time FFmpeg GPU scoring with `n_stats` ([#2138](https://github.com/VMAFx/vmafx/issues/2138), work package 9) |
| RC5 | Containers, Helm chart and a kind plus kuttl test setup ([#1252](https://github.com/VMAFx/vmafx/issues/1252)); the operator, the controller / node split and the GPU pool arbiter in `libgpudispatch` ([#1253](https://github.com/VMAFx/vmafx/issues/1253)) |
| RC6 | Legacy GPU build variants: CUDA 12.x builds for Maxwell, Pascal and Volta (sm_50 to sm_72; CUDA 13.4 starts at compute_75), the Intel legacy compute runtime for Gen9 to Gen11 iGPUs, and every AMD target the pinned ROCm compiler still emits. Each is bit-exact and listed in the GPU capability table ([#1725](https://github.com/VMAFx/vmafx/issues/1725)) |
| RC7 | The full CPU SIMD ladder: every extractor gets a bit-exact kernel at every useful ISA level with runtime dispatch, on x86-64 (SSE2, SSSE3, SSE4.1, AVX, AVX2, AVX-512, AVX-512 ICL, AVX10), AArch64 (NEON, dotprod/i8mm, SVE, SVE2), RISC-V RVV 1.0, POWER VSX and LoongArch LSX/LASX; qemu-user CI for architectures without hardware, the golden gate on each ([#1885](https://github.com/VMAFx/vmafx/issues/1885)) |
| RC8 | Distributed throughput: measured scale-out across nodes, next to throughput per resolution |

The OBS Studio plugin itself is not part of 1.0.0; it ships in 1.1 on the API
of RC4.

After 1.0.0 every minor release runs its own candidate cycle: features first,
then deduplication, tests and bug fixing, then capability tables with
benchmarks and tuning, then training where models change, then the release.

| Release | Theme | Issues |
|---|---|---|
| 1.1 | Integrations and live quality: the OBS Studio plugin and live-quality work | [#2239](https://github.com/VMAFx/vmafx/issues/2239), [#2148](https://github.com/VMAFx/vmafx/issues/2148), [#2147](https://github.com/VMAFx/vmafx/issues/2147), [#2144](https://github.com/VMAFx/vmafx/issues/2144), [#2146](https://github.com/VMAFx/vmafx/issues/2146), [#2159](https://github.com/VMAFx/vmafx/issues/2159) |
| 1.2 | Encoder feedback, embedding and platforms: the rest of the embedding epic, Windows and macOS libraries, CMake package | [#2067](https://github.com/VMAFx/vmafx/issues/2067) (rest), [#2164](https://github.com/VMAFx/vmafx/issues/2164), [#2156](https://github.com/VMAFx/vmafx/issues/2156) |
| 1.3 | New metrics with exact twins | [#2165](https://github.com/VMAFx/vmafx/issues/2165), [#2167](https://github.com/VMAFx/vmafx/issues/2167), [#2166](https://github.com/VMAFx/vmafx/issues/2166), picks from [#2168](https://github.com/VMAFx/vmafx/issues/2168) |
| 1.4 | Metric A/B comparison, the best current mix and more training data | [#2240](https://github.com/VMAFx/vmafx/issues/2240), [#2241](https://github.com/VMAFx/vmafx/issues/2241) |
| 1.5 | The next model generation | [#2242](https://github.com/VMAFx/vmafx/issues/2242) |
| 2.0 | Breaking changes only: the `libvmaf.h` compatibility library removed (ADR-1852 decision D7), the C++23 core, the rest of [#1254](https://github.com/VMAFx/vmafx/issues/1254) | |

The rolling milestones "Models & benchmarks" and "Code health & deduplication"
stay. This ADR amends the scope statements of
[ADR-1868](1868-candidate-map-2026-10-05.md),
[ADR-1880](1880-format-envelope-device-targets.md) and
[ADR-1829](1829-rc4-zero-copy-import.md) (ADR-1829 left the rest of the
embedding epic after 1.0.0; that remainder is 1.2) and supersedes the
post-1.0 milestone sketch in `docs/roadmap.md` and the old 1.1 to 1.3
milestones. The phase rules of
[ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md) and
[ADR-1490](1490-rc3-rc9-candidate-map-cpu-capability.md) stand: no performance
claim before RC8, no training before RC8 evidence is accepted. The same order
applies inside each post-1.0 cycle.

## Alternatives considered

| Option | Pros | Cons | Outcome |
|---|---|---|---|
| Fold cloud-native into RC4 / RC5 / RC8, GStreamer in RC4, OBS-ready API in 1.0.0 with the plugin in 1.1 (**chosen**) | No renumbering; the server, container and operator work uses the generated API surface and `libgpudispatch` from the start; the API shape is fixed once, before the first stable release | RC4 and RC5 grow again; more is promised for 1.0.0 | Chosen |
| Keep cloud-native in 1.2 / 1.3 | Smaller 1.0.0 | The scoring API contract would change after the first stable release or be designed twice; the arbiter would be built outside `libgpudispatch` | Not chosen |
| New candidates for cloud-native | Isolated stage | Renumbers tags every document and ADR cites | Not chosen |
| OBS Studio plugin in 1.0.0 | Integration ships earlier | Plugin packaging and its own test surface in the stage that proves exactness and the API | Not chosen |
| Nothing for OBS Studio in 1.0.0 | Smallest scope | The texture import and asynchronous score API would need a breaking change later | Not chosen |
| Two post-1.0 minors (1.1, 1.2) then 2.0 | Fewer cycles | Each release needs deduplication, tests, bug fixing, tuning and training again, and new metrics, A/B testing, the best mix and more data make the 1.x line heavier | Not chosen |
| Themed 1.1 to 1.5, each with its own candidate cycle (**chosen**) | Each release is verifiable on its own; a theme never waits for another | More release cycles | Chosen |

## Consequences

- **Positive**: one scoring API contract, generated, covers the C library, the
  server, the GStreamer element and the OBS-ready surface at 1.0.0; cloud
  deployment is tested in the same candidates as the code it deploys; later
  releases have a fixed place for each kind of work.
- **Negative**: RC4 to RC8 carry more work (RC7 needs qemu-user lanes for four architectures, RC6 needs an older CUDA toolchain); five post-1.0 cycles each
  repeat the candidate sequence.
- **Neutral / follow-ups**: this is the last change of the milestone map until
  2.0 apart from bugs and findings. The GitHub milestones and the issues named
  above follow this ADR; `AGENTS.md` section 11 and its compiled projections,
  `docs/roadmap.md`, `docs/development/release.md` and the dispositions table
  of `docs/state.md` are updated in the same change.

## References

- `Q`: "Fold into RC4/RC5/RC8 (Recommended)"
- `Q`: "Compat test + native element (Recommended)"
- `Q`: "1.1 plugin, API ready in 1.0 (Recommended)"
- `Q`: "Themed 1.1-1.5, own RC cycle each (Recommended)"
- `Q`: "RISC-V Vector (RVV 1.0)", "POWER VSX (ppc64le)", "LoongArch LSX/LASX" (multi-select)
- `Q`: "Legacy build variants (Recommended)"
- `req`: "well who cares, we want the best and correct ones on all cpu (thats a cpu module part though) because only because industry adopts fast doesnt mean that all the cpu and gpu's suddenly arent used anymore"
- `req`: "okay, one last rc change, I want the cloudnative stuff in 1.0.0 as well and then we need to properly resort/redesign the post 1.0.0  and do we need anything in 1.0.0 for gstreamer? or even better, direct obs studio support? and thats now the last change of milestones, i guess (okay without bugs or things we find on the way) for up until 2"
- `req`: "well i guess from experience, 2 milestones wont do, need deduplication, testing, bugfixing, tuning traing etc. again later? and I assume noe metrics and a/b testing them and finding the best current mix and more training data etc. will make 1-2 far more interesting and loaded"
- [ADR-1341](1341-rc-correctness-benchmark-retrain-sequence.md), [ADR-1490](1490-rc3-rc9-candidate-map-cpu-capability.md), [ADR-1685](1685-post-1-0-embedding-zero-copy-milestone.md), [ADR-1829](1829-rc4-zero-copy-import.md), [ADR-1852](1852-vmafx-api-redesign.md), [ADR-1868](1868-candidate-map-2026-10-05.md), [ADR-1880](1880-format-envelope-device-targets.md)
