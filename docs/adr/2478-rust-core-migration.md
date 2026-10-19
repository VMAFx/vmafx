<!-- markdownlint-disable MD013 MD060 -->
# ADR-2478: Rust replaces the host-side C and C++ through 3.0, behind the unchanged C ABI

- **Status**: Accepted (2026-10-08, Q-209 to Q-212, Q-217 to Q-236, Q-243)
- **Date**: 2026-10-08
- **Deciders**: maintainer (Q-209 to Q-212, Q-217 to Q-236, Q-243); lusoris
- **Tags**: `rust`, `abi`, `build`, `gpu`, `simd`, `roadmap`, `upstream-sync`

## Context

[ADR-1713](1713-rc4-rust-extractor-framework.md) put Rust twins next to the C
extractors for the RC4 metric (framework, `cambi_rust` and `speed_chroma`
merged; the predictor, `motion_rust` and `adm_rust` open on 2026-10-08). That
is about 9500 lines of Rust against about 240000 lines of host C and C++
([Research-2479](../research/2479-rust-core-migration.md)). The 2.0 plan
listed "C++23 core internals with the VMAFx C ABI preserved" (#2537). The
maintainer decided to replace that plan with a Rust core: all host-side C and
C++ moves to Rust over the 2.x releases and is removed in 3.0, the public C ABI
stays, GPU device code moves only where a Rust toolchain reaches parity, and
upstream Netflix tracking stays port-only into the Rust implementation.

## Decision

**End state (3.0).** The engine, the CPU feature extractors, the SIMD paths,
model loading, prediction and pooling, the `vmaf` / `vmafx` CLI and tools, the
GPU host runtimes and dispatch, and the integration glue the project owns are
Rust. The public C ABI (`core/include/vmafx/*.h`, `libvmafx`) is exported from
Rust. The `libvmaf.h` compatibility layer stays until its removal in 2.0
([#2536](https://github.com/VMAFx/vmafx/issues/2536), Q-227: 2.0 is the
breaking-changes-only release, Q-231); the 3.0 phase deletes host C and C++ only. No
host C or C++ file remains in the tree outside the exception list. The list has
two classes, each entry naming file, rule, reason, re-evaluation trigger and
expiry: native GPU device sources a Rust toolchain does not reach (see GPU), and
out-of-tree host glue for hosts that offer no supported Rust route (Q-228: FFmpeg
filters, VLC, OBS where no supported route exists). Glue is rewritten in Rust
where the host offers a supported route (GStreamer through its Rust bindings,
VapourSynth if supported bindings exist); [#2581](https://github.com/VMAFx/vmafx/issues/2581)
tracks it.

**ABI export.** `core/api/vmafx.toml` stays the single definition. The
generator (`scripts/codegen/vmafx-api.py`) gains a Rust emitter for the
`#[repr(C)]` structs and the `extern "C"` shells; `test_vmafx_abi_layout`,
`test_vmafx_api_abi_append_only` and `check_exported_symbols` keep running
against the built library. Headers stay generated from the definition, not by
cbindgen (cbindgen remains for the extractor ABI until the shim is gone). The
exported symbol set and version nodes do not change in any phase.

**Exactness and oracle contract.** The C implementation of a layer is the
differential oracle until that layer's C is deleted. A Rust layer is accepted
when it is bit-identical to the C layer (`--precision max`, doubles equal on
every metric of every frame) on the three Netflix golden pairs, the 1080p
checkerboard pairs and `testdata/bbb`, passes the cross-backend parity gate
unchanged, keeps every twin either bit-identical or at its measured tolerance
with its ADR, and stays inside the RC8 harness bounds (ms/frame median of 3,
same configuration). Netflix golden assertions never change. Its per-frame outputs on the corpus are recorded as hashed fixtures before the
layer leaves the default build, so differential testing continues from recorded
data after the deletion.

**When C stops building.** At a phase's exit the layer's default flips to
Rust and its C is built only in an oracle-only Meson profile (off in published
artifacts, on in CI), after the hashed per-frame fixtures of that layer are
recorded; the C is deleted in 3.0. A layer is never both default implementations (HISS-19, RC5).

**Phases.** Leaf-first, because RC4 already built the extractor framework and a
leaf moves without touching the engine; the engine then absorbs the Rust
descriptors and the C shim is deleted.

| Phase | Milestone | Issue | Content | Exit |
| --- | --- | --- | --- | --- |
| P0a | 1.0.0 (RC4) | [#2568](https://github.com/VMAFx/vmafx/issues/2568) | Stable toolchain pin and cargo in the dev container | container builds the Rust crates |
| P0b | 1.2 | [#2569](https://github.com/VMAFx/vmafx/issues/2569) | Rust emitter in the API generator | layout, append-only and symbol gates run on the emitted Rust, each shown failing on a planted defect |
| P0c | 1.2 | [#2570](https://github.com/VMAFx/vmafx/issues/2570) | Sanitizer, fuzz and coverage lanes for the Rust crates (nightly for sanitizers only) | lanes green and shown failing |
| P0d | 1.2 | [#2571](https://github.com/VMAFx/vmafx/issues/2571) | Registration of Rust-only extractors with no C twin | a Rust-only extractor runs through the CLI |
| P0e | 1.2 | [#2572](https://github.com/VMAFx/vmafx/issues/2572) | Hashed per-frame oracle fixtures recorded from the C path | fixtures replay in CI |
| P1a | 2.1 Rust P1a | [#2573](https://github.com/VMAFx/vmafx/issues/2573) | Default-model extractors (adm, motion, cambi, speed_chroma) Rust-default with SIMD; CPU tuning rows with their measured targets | bit-identical to C and scalar; targets met |
| P1b | 2.2 Rust P1b | [#2574](https://github.com/VMAFx/vmafx/issues/2574) | Remaining CPU extractors and SIMD (`std::arch`: AVX2, AVX-512, NEON); new-ISA kernels in Rust only; CPU SIMD of the new-metric twins; scalar Rust for ISAs without stable intrinsics | every extractor Rust-default |
| P2 | 2.3 Rust P2 | [#2575](https://github.com/VMAFx/vmafx/issues/2575) | Engine core (collector, pictures, thread pool, log, options, dict), model loading, predict and pooling, generated API shells; the shim is removed | Rust engine behind the generated ABI; C engine in the oracle profile only |
| P3 | 2.4 Rust P3 | [#2576](https://github.com/VMAFx/vmafx/issues/2576), [#2581](https://github.com/VMAFx/vmafx/issues/2581) | `vmaf` / `vmafx` CLI, tools, MCP server, ONNX Runtime host; host-integration glue where the host allows Rust | CLI differential on all documented flags |
| P4 | 2.5 Rust P4+P5 | [#2577](https://github.com/VMAFx/vmafx/issues/2577) | GPU host runtimes and dispatch per backend; `libgpudispatch` rewritten in Rust behind the C ABI RC5 extracted it in; evaluation of the SYCL device/host split | per-backend parity gate and perf bounds; kernels unchanged |
| P5 | 2.5 Rust P4+P5 | [#2578](https://github.com/VMAFx/vmafx/issues/2578) | GPU kernel verdict per backend (input: the SPIR-V experiment [#2466](https://github.com/VMAFx/vmafx/issues/2466), RC5) and the exception-list gate | per backend: moved, or on the list |
| P6 | 3.0 Host C/C++ removed | [#2579](https://github.com/VMAFx/vmafx/issues/2579) | Host C and C++ deleted outside the exception list; oracle fixtures recorded | no host C or C++ outside the list; a planted `.c` file fails the gate |

Epic: [#2567](https://github.com/VMAFx/vmafx/issues/2567) (Q-243).

Other decided scope (Q-223 to Q-236): RC7 delivers the SIMD capability table,
drift check, disassembly audit and emulated matrix for the existing kernels;
kernels for a new ISA are written once, in Rust, in P1b (Q-223). ISAs without
stable Rust intrinsics (RVV, VSX, LSX / LASX, SVE / SVE2) get a scalar Rust
fallback and a re-evaluation list, trigger: a stable `core::arch` module exists
(Q-235). CPU tuning rows fold into P1a / P1b with their measured targets as
acceptance; GPU tuning rows stay in RC8 (Q-224). GPU twins of new metrics stay
in RC5, their CPU SIMD goes to P1b (Q-225). `libgpudispatch` is extracted in C
for RC5 and rewritten in Rust in P4 behind the same C ABI (Q-226). The SPIR-V
experiment stays in RC5 and its verdict feeds P5 (Q-229). New host components
from 1.3 on are Rust-first after P0d, with a C twin only for an existing C-only
consumer (Q-233); the extractor SDK ([#2337](https://github.com/VMAFx/vmafx/issues/2337))
builds on the Rust framework ABI (Q-234). The clang-tidy ratchet stays (no new
findings) and the burn-down stops in C files whose layer has a scheduled Rust
phase (Q-236).
Each release after 1.0.0 runs its own candidate cycle as ADR-2001 sets. Phases may merge or split when the evidence says so; the entry
criteria are the previous phase's exit criteria plus a measured FFI and build
cost, and a phase does not start while its dependency (RC5 dedup for P4) is
open.

**GPU device code (Q-211).** A backend's kernels move to Rust only when a Rust
toolchain passes all of: bit-identical to the CPU reference on the exact
twins under the project's contracts (per-kernel control of fused multiply-add,
correctly rounded division and square root, no scratch memory on SYCL),
performance inside the RC8 bounds, and a toolchain that builds in the pinned
container on every supported OS for that backend. The evaluation is a minimal
kernel test per backend, recorded as a digest. State on 2026-10-08
([Research-2479](../research/2479-rust-core-migration.md) section 7):

| Backend | Candidate | Verdict now | Re-evaluation trigger |
| --- | --- | --- | --- |
| CUDA | `nvptx64-nvidia-cuda` (Tier 2, nightly components); Rust-CUDA (early development, no release since 2022) | evaluate in P5; host in P4 either way | stable `build-std`-free target or a Rust-CUDA release |
| HIP | `amdgcn-amd-amdhsa` (Tier 3, nightly, `abi_gpu_kernel`) | kernels native until the target leaves Tier 3; host in P4 | Tier 2 promotion or `gpu-kernel` stabilisation |
| SYCL | none found (rust-gpu emits Vulkan-style SPIR-V; Level Zero needs `Kernel` SPIR-V) | kernels native; host through Level Zero in P4 only after the kernels are separate device sources | a Rust route to `Kernel`-model SPIR-V |
| Metal | none native (CubeCL via wgpu, alpha) | kernels stay MSL; host in P4 | a rustc Metal target or CubeCL exactness evidence |

**Upstream (Q-212).** Tracking stays port-only: Netflix commits are ported
into the Rust implementation. The `sync-upstream` and `port-upstream-commit`
skills are retargeted to Rust paths in the phase that moves the ported layer;
until then they target the C path, which is the oracle. The upstream-parity
audit is kept.

**Toolchain.** The stable compiler is pinned (1.98.1 on 2026-10-08) and every
manifest sets `rust-version` to the pin; nightly is allowed only for the
sanitizer lanes and for GPU device crates.

**3.0 means no host C or C++.** Device sources a Rust toolchain does not reach
(`.cu`, `.hip`, `.metal`, the SYCL device source) stay, each on a named
exception list (file, rule, reason, re-evaluation trigger, expiry). SYCL kernels
share `.cpp` files with host code today; whether to split them into device
sources with a Rust host through Level Zero is evaluated in P4. Host glue for
C-only hosts is the second class of the exception list (above).

**New work.** After this ADR, new host-side implementation work lands in Rust
unless the phase table above says the layer is still C; bug fixes in a C layer
that has a Rust successor land in both.

**Supersedes.** The C++23 part of the 2.0 plan ([#2537](https://github.com/VMAFx/vmafx/issues/2537)).
[ADR-1713](1713-rc4-rust-extractor-framework.md)'s fallback to C becomes
temporary: it holds until each layer's C removal in P6.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Rust replaces host C and C++ across 2.x, removed in 3.0 (chosen)** | One language for the host; memory safety; reuses RC4; oracle until deletion | Long dual-implementation period; build and CI cost; SIMD and GPU gaps | The maintainer's decision (Q-209, Q-210) |
| Keep C and move to C++23 (the old #2537) | Smaller step; one toolchain family | Keeps unsafe patterns; no Rust twin story; two languages stay | Superseded by Q-209 |
| All in 2.0 | One breaking release; shorter dual period | Too large for one candidate cycle; the old 2.0 holds only breaking changes | Q-210: the migration spans 2.x |
| Staged inside 1.x (groundwork only) | Starts earlier | Competes with RC3-RC9 correctness work and exit rules; Rust twins already are the groundwork | Q-210 keeps 1.x on C plus the RC4 twins; groundwork is an open question to the maintainer |
| Kernels native for ever | No GPU toolchain risk | Leaves C, C++ and CUDA dialects in the tree | Q-211: evaluate per backend, move where parity is reached |
| Rust-only API (drop the C ABI) | Cleaner types | Breaks every consumer: Go (cgo), Python, FFmpeg, the CLI | Q-209: the C ABI stays, exported from Rust |
| Engine-first order | Removes the shim early | The leaf work is ready now; the engine move blocks on the generated API shells | Q-217: leaf-first |
| Claim 3.0 as no C, C++ or CUDA dialect at all | Simple statement | Forces device code onto toolchains that fail the exactness rule | Q-218: native device sources on an exception list |
| Split SYCL into device and host sources at once, or keep SYCL whole for ever | Removes or accepts the mixed files now | Needs a Level Zero host before it is measured / leaves C++ unreviewed | Q-218: evaluated in the GPU-host phase |
| No late-1.x groundwork, or only the MSRV pin | Keeps 1.x on correctness | Phases P1 and P2 would start without the lanes that prove them | Q-219: pin, container toolchain, emitter and sanitizer / fuzz / coverage lanes |
| Nightly for everything, or stable only | One channel / no nightly | Nightly pins the whole build / loses sanitizers and GPU device crates | Q-220: stable pin, nightly for sanitizer lanes and device crates |
| Delete C at each phase exit | Shortest dual period | Loses the oracle | Q-222: oracle-only profile, deleted in 3.0 |
| Move #2536 (libvmaf.h removal) to 3.0, or ship a shim crate | One removal release | Mixes a breaking API release with the host rewrite | Q-227: stays in 2.0 |
| Out-of-tree glue as one exception class, or separate repositories | Simple / no glue in this tree | Keeps Rust-capable hosts on C / splits ownership | Q-228: Rust where the host allows, C-only hosts on the list |
| Full SIMD ladder in C now, or nightly for new ISAs | New ISAs sooner | Writes kernels twice / nightly in the build | Q-223, Q-235: new-ISA kernels in Rust, scalar fallback and re-evaluation list |
| CPU tuning rows in RC8 in C | One tuning phase | Tunes code about to be replaced | Q-224: fold into P1a / P1b |
| One 2.x milestone, or P4 and P5 apart | Fewer milestones / finer gates | Less structure / more overhead | Q-230: 2.1 to 2.5 and 3.0 as listed |
| Epic per milestone, or one issue | Local ownership / no tracking | Fragmented / opaque | Q-243: one epic with phase children |

## Consequences

- **Positive**: one host language; a single definition drives the ABI; the
  oracle keeps correctness measurable until deletion.
- **Negative**: for several releases both implementations build in CI; MSRV,
  nightly (sanitizers, GPU device crates) and a Rust toolchain in the container
  are new pins (HISS-11); SYCL files mix kernel and host code.
- **Neutral / follow-ups**: the epic and phase trackers, a Rust emitter in the API generator, `rust-version` in the
  manifests, Rust sanitizer, fuzz and coverage lanes, retargeted upstream
  skills, an exception list for native device code.

## References

- Q-209, Q-210, Q-211, Q-212, Q-217, Q-218, Q-219, Q-220, Q-221, Q-222,
  Q-223, Q-224, Q-225, Q-226, Q-227, Q-228, Q-229, Q-230, Q-231, Q-232,
  Q-233, Q-234, Q-235, Q-236, Q-243 (maintainer decisions, praetor ledger,
  2026-10-08).
- Epic [#2567](https://github.com/VMAFx/vmafx/issues/2567); milestones 2.1 Rust
  P1a, 2.2 Rust P1b, 2.3 Rust P2, 2.4 Rust P3, 2.5 Rust P4+P5, 3.0 Host C/C++
  removed (Q-230).
- [Research-2479](../research/2479-rust-core-migration.md).
- [ADR-1713](1713-rc4-rust-extractor-framework.md),
  [ADR-1852](1852-vmafx-api-redesign.md),
  [ADR-0706](0706-vmafx-rust-sys-bindings.md),
  [ADR-1421](1421-rc3-rc8-candidate-map.md),
  [ADR-2001](2001-release-scope-1-0-and-roadmap-to-2-0.md),
  [ADR-1395](1395-sycl-kernels-no-scratch.md),
  [ADR-1403](1403-cuda-strict-fp-every-kernel.md).
