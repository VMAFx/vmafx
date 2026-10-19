<!-- markdownlint-disable MD013 MD060 -->
# Research-2479: what replacing the C and C++ host code with Rust takes

- **Status**: Active
- **Workstream**: [ADR-2478](../adr/2478-rust-core-migration.md)
- **Last updated**: 2026-10-08

## Question

The maintainer decided that Rust replaces all host-side C and C++ through 3.0
(the public C ABI stays, exported from Rust), that GPU device code moves to
Rust only where a Rust toolchain reaches parity, and that upstream tracking
stays port-only. What is in the tree today, what has RC4 already built that the
migration reuses, and what do the Rust toolchains support today, so that the
phases, the exit criteria and the open choices can be written down?

## Method

Counts: `git ls-files | grep -E '\.(c|h|cpp|cc|cxx|hpp|cu|cuh|hip|mm|m|metal)$'`
then `wc -l` per file, grouped by directory, on master `a55b60f10` (2026-10-08).
Lines include comments and blanks. Rust facts: the installed toolchain
(`rustc 1.98.1 (48a229cea 2026-09-01)`, `cargo 1.98.1`), the rustc book and
the Unstable Book (nightly pages fetched 2026-10-08) and GitHub's repository API
(fetched 2026-10-08). Anything not read from one of those is marked
**unverified**.

## Findings

### 1. Inventory of C and C++ (tracked files, lines)

| Layer | Path | Files | Lines | Notes |
| --- | --- | ---: | ---: | --- |
| Public C headers | `core/include/` | 32 | 6802 | `core/include/vmafx/*.h` are generated from `core/api/vmafx.toml`; `libvmaf/*.h` is the compat surface |
| Engine, top level | `core/src/*.c`, `*.cpp`, `*.h` | 65 | 21903 | `libvmaf.c`, `picture*.c`, `predict.c`, `thread_pool.c`, `dict.cpp`, `log.c`, `model.c`, `svm.cpp`, `read_json_model.*`, `framesync.c`, `gpu_dispatch_*`, `opt.cpp`, `output.cpp` |
| VMAFx API implementation | `core/src/vmafx/` | 26 | 5026 | partly generated (`status_gen.c`, `compat_libvmaf_gen.c`) |
| Compat, interop, arch glue | `core/src/{compat,interop,x86,arm}/` | 21 | 2323 | win32 pthread shim, dmabuf / zero-copy import |
| MCP embedded server | `core/src/mcp/` | 9 | 6250 | `libvmaf_mcp.h` surface |
| Tiny-AI (ONNX Runtime) | `core/src/dnn/` | 15 | 5805 | |
| CPU feature extractors | `core/src/feature/*.c`, `*.cpp`, `*.h`, `common/`, `iqa/`, `third_party/` | 162 | 48951 | 45230 top level, 1559 `common`, 1645 `iqa`, 517 `third_party/xiph` |
| SIMD, x86 | `core/src/feature/x86/` | 72 | 15897 | AVX2 and AVX-512 |
| SIMD, arm64 | `core/src/feature/arm64/` | 42 | 6829 | NEON; SVE2 ports named in the rebase invariants |
| CLI and tools | `core/tools/` | 32 | 12313 | `vmaf` / `vmafx`, `vmaf_bench`, per-shot, roi, vpl |
| GPU host runtimes | `core/src/{cuda,hip,sycl,metal}/` | 49 | 11612 | 2875 + 1982 + 4780 + 1975 |
| GPU feature hosts | `core/src/feature/{cuda,hip,sycl,metal}/`, host files | 177 | 75230 | CUDA 15161, HIP 18692, SYCL 24830 (kernels are lambdas in the same `.cpp` files), Metal 16547 |
| GPU kernels, separate files | `.cu` `.cuh` `.hip` `.metal` | 61 | 20933 | CUDA 9176, HIP 6768, Metal 4989 |
| C unit and contract tests | `core/test/` | 488 | 171755 | 423 `.c`, 26 `.cpp`, 37 `.h`, 2 `.hip` |
| Node agent | `cmd/vmafx-node/` | 2 | 254 | |
| Python harness | `compat/python-vmaf/` | 146 | 12847 | 133 are MATLAB `.m`, 12 `.c`; not host C of the library |

All tracked C and C++ in the repository: 426797 lines in 1452 files (this
includes the 12847 harness lines above and 146 lines in `scripts/ci`). Excluding
tests and the harness: about 240000 lines, of which about 108000 are GPU
(host plus kernels) and about 22700 are SIMD.

Callers of the C library: the Go package `pkg/libvmaf` (4 files with `import "C"`,
so a cgo consumer), the Python `ctypes` binding (`bindings/python/vmafx/_api.py`,
generated), the Rust crates `vmafx-sys` and `vmafx` (`bindings/rust/`), the
FFmpeg patch series (`ffmpeg-patches/`, 25 files, 9099 lines of patch), the
`vmaf` CLI and the MCP server. Every consumer goes through the C ABI, which is
why the ABI is the stable seam.

### 2. What RC4 already built

- `vmafx-fex` (`core/src/rust/fex`, 1433 lines): the `Extractor` trait, the
  plane views, the ABI structs ([ADR-1713](../adr/1713-rc4-rust-extractor-framework.md)).
- Merged on master: the framework (#2086), `cambi_rust` (#2090, 2482 lines in
  `core/src/rust/feature/cambi`), `speed_chroma` (#2097, 2211 lines in
  `feature/speed`). `gh pr view` on 2026-10-08: #2085 (Rust predictor), #2096
  (`motion_rust`) and #2099 (`adm_rust`) are **open**, not merged.
  `core/src/rust/feature/psnr` (306 lines) is on master.
- Total Rust under `core/src/rust` plus the TAD pilot and the bindings: about
  9500 lines in 55 `.rs` files, against about 240000 lines of C and C++ to
  replace (about 4 percent).
- Shape: Rust twins are registered as `<c name>_rust` by the C shim
  `core/src/rust/shim/rust_twins.cpp`, which copies the C extractor's descriptor
  and routes scores through the C collector. Rust never sees a libvmaf struct.
  Selection is `VMAF_FEATURE_IMPL=rust` ([ADR-1713](../adr/1713-rc4-rust-extractor-framework.md)).
  The differential harness is `scripts/ci/rust_twin_diff.py`.
- Consequence for the migration: today the C engine owns descriptors, options,
  the collector and the thread pool and Rust only supplies callbacks. Moving the
  engine to Rust removes the shim; moving extractors first needs no engine change.

### 3. Build integration today

- Meson is the build; Rust is optional behind `enable_rust_features`
  (`core/meson_options.txt:152`), default off, and without `cargo` it degrades
  with a warning (`core/meson.build`, `is_rust_enabled`).
- `core/src/rust/build_staticlib.py` runs `cargo build --release --locked
  --offline -p vmafx-core-rs` against `core/src/rust/Cargo.toml`, a separate
  workspace whose lockfile holds no external crate; `core/src/meson.build:2313`
  onward links `libvmafx_core_rs.a` with `-Wl,--exclude-libs` where supported.
- The root workspace (`Cargo.toml`) holds `bindings/rust/vmafx-sys` and
  `bindings/rust/vmafx` (bindgen 0.72, [ADR-1002](../adr/1002-rust-edition-2024-bindgen-072.md));
  edition 2024; no `rust-version` (MSRV) is declared in any manifest
  (`grep -rn rust-version --include=Cargo.toml` is empty). Release profile:
  `opt-level = 3`, `lto = true`, `codegen-units = 1`, `panic = "abort"`.
- The dev container has no Rust toolchain yet ([ADR-1713](../adr/1713-rc4-rust-extractor-framework.md)
  Consequences), and Windows linking of the Rust archive is not covered there.
- CI: `.github/workflows/rust-ci.yml` (fmt, clippy `-D warnings`, build, tests,
  golden gate for `vmafx-sys`), `sanitizers.yml`, `fuzz.yml` (harness
  `core/test/fuzz/`).

### 4. C ABI generation

`core/api/vmafx.toml` (1707 lines, schema 2, `abi_version` 0.1.3) defines 14
headers, 78 functions, 16 structs, 8 handles, 7 enums, 3 flag sets, 13 status
codes, 4 compat entries and 2 callbacks. `scripts/codegen/vmafx-api.py`
generates `core/include/vmafx/*.h`, the linker version script
`core/src/vmafx.map`, the Windows export list `vmafx.def`, `vmafx_symbols.txt`,
`status_gen.c`, `compat_libvmaf_gen.c`, `core/test/test_vmafx_abi_layout.c` and
the Python binding (`docs/development/api-generation.md`, section Outputs).
Gates: `test_vmafx_api_generated_current`, `test_vmafx_abi_layout`,
`test_vmafx_api_abi_append_only`, `check_exported_symbols`. There is no Rust
emitter yet: the Rust export surface would be new output of the same
generator, with cbindgen (`core/src/rust/cbindgen.toml`, used for the extractor
ABI header) as the precedent for the C side only.
Implication: the definition stays the single source; a Rust emitter produces
the `extern "C"` function shells and the `#[repr(C)]` structs, so the layout
test continues to compare the compiler's layout with the definition.

### 5. Platforms and targets

Rust tier of the project's targets, from the rustc platform-support page
(fetched 2026-10-08): Tier 1 with host tools for `x86_64-unknown-linux-gnu`,
`aarch64-unknown-linux-gnu`, `x86_64-pc-windows-msvc`,
`aarch64-pc-windows-msvc` and `aarch64-apple-darwin`. All five project targets
are Tier 1.
Sanitizers (Unstable Book, fetched 2026-10-08): `-Zsanitizer=address` on
x86_64 and aarch64 Linux and aarch64 macOS, `thread` on the same, `memory` on
Linux; all need nightly (`-Zbuild-std` recommended). The page does not mention
UBSan; Rust has overflow and bounds checks, so the UBSan lane would be replaced
by `debug-assertions` plus Miri (unverified here). The existing fuzz harnesses
are C entry points; `cargo-fuzz` (libFuzzer) is the Rust equivalent
(unverified, not fetched). Coverage: `cargo-llvm-cov` or `-C instrument-coverage`
(unverified, not fetched); the repo's coverage gate
([ADR-0922](../adr/0922-coverage-ratchet-aggressive.md)) reads C coverage and
needs a Rust source.

### 6. SIMD

- `std::arch` intrinsics: stable. AVX-512 target features were stabilised in
  Rust 1.89.0, released 2025-08-07 ([releases.rs 1.89.0](https://releases.rs/docs/1.89.0/);
  the intrinsics list itself was not read from the official post: unverified).
  The installed stable is 1.98.1, so AVX-512 is available.
- `std::simd` (portable SIMD): nightly-only. The page for `std 1.101.0-nightly
  (8d1a76430 2026-10-06)` says "nightly-only experimental API", feature gate
  `portable_simd`, tracking issue
  [rust-lang/rust#86656](https://github.com/rust-lang/rust/issues/86656).
  Not usable on the stable toolchain the project ships with.
- Consequence: SIMD twins use `std::arch` per ISA (x86 AVX2 / AVX-512, aarch64
  NEON), mirroring the C structure; NEON and SVE2 intrinsic stability on the
  stable channel: **unverified**, to be read from `core::arch::aarch64` docs
  before the arm64 phase.
- Floating-point contraction: **unverified from a primary source**. The
  expectation is that rustc does not fuse `a * b + c` and that `mul_add` is the
  only explicit fusion; the first Rust twin of a fused C expression decides it,
  the project's strict-FP contract tests ([ADR-1461](../adr/1461-strict-fp-every-translation-unit.md))
  being the check.

### 7. Rust GPU toolchains (fetched 2026-10-08)

| Backend | Candidate | State | Source |
| --- | --- | --- | --- |
| CUDA | `nvptx64-nvidia-cuda` in rustc | Tier 2, `no_std`, nightly components (`llvm-tools`, `llvm-bitcode-linker`), `-Zbuild-std=core` for a target-cpu other than `sm_70`; Rust 1.97 raised the minimum to sm_70 / PTX 7.0; all crates share one `target-cpu`; maintainers @kjetilkjeka, @kulst | [rustc book, nvptx64-nvidia-cuda](https://doc.rust-lang.org/nightly/rustc/platform-support/nvptx64-nvidia-cuda.html) |
| CUDA | Rust-CUDA (`Rust-GPU/rust-cuda`) | Apache-2.0; pushed 2026-09-30; not archived; README: "no longer dormant and is being rebooted", "still in early development", "Expect bugs, safety issues, and things that don't work"; last GitHub release 0.3 on 2022-02-07; needs CUDA Toolkit 12.0 or later, LLVM 7.x by default (optional `llvm21` feature), a pinned nightly, libnvvm; Linux and Windows | [repository](https://github.com/Rust-GPU/rust-cuda), [guide](https://rust-gpu.github.io/rust-cuda/guide/getting_started.html) |
| HIP / ROCm | `amdgcn-amd-amdhsa` in rustc | Tier 3, `no_std`, nightly (`abi_gpu_kernel`), `-Zbuild-std=core`, no prebuilt `core`; launched through HIP or ROCR; one `target-cpu` per link; maintainer @Flakebi; page has no floating-point section | [rustc book, amdgcn-amd-amdhsa](https://doc.rust-lang.org/nightly/rustc/platform-support/amdgcn-amd-amdhsa.html) |
| SYCL / Level Zero | none found | `Rust-GPU/rust-gpu` v0.10.0 (2026-10-01, Apache-2.0, pushed 2026-10-07) emits Vulkan-style SPIR-V; Level Zero consumes `Kernel`-model SPIR-V, and a search found no route from rust-gpu to it (search result, not a primary statement: **unverified**). The rustc `std::offload` design lists NVPTX64 and AMDGCN and Intel as future | [rust-gpu releases](https://github.com/Rust-GPU/rust-gpu/releases), [dev guide, offload](https://rust.googlesource.com/rust-lang/rustc-dev-guide/+/cd16ee2686d1b4ca83eb5d8019edfa5c8a36fcb6/src/offload/internals.md) |
| Metal | none native | No rustc Metal target found. CubeCL emits MSL through wgpu | below |
| Cross-vendor | CubeCL (`tracel-ai/cubecl`) | Apache-2.0; v0.11.0 on 2026-10-06; README: "currently in alpha", public API "still evolving"; backends CUDA, HIP (ROCm), wgpu (MSL, SPIR-V, WGSL), CPU; says nothing on determinism or floating-point exactness; no Intel Level Zero / SYCL backend listed | [repository](https://github.com/tracel-ai/cubecl) |
| Upstream | `std::offload` (LLVM offload) | in development, not in a stable or standard nightly workflow (`gpu_offload` gate, custom toolchain); status date not found | [rustc-dev-guide](https://rust.googlesource.com/rust-lang/rustc-dev-guide/+/d06b322c315ca5515b6beab8223c3aa36e82b0cf/src/offload/internals.md) |

What the project's kernels need that no source above answers: control of
fused multiply-add per kernel (CUDA kernels build with `--fmad=false`,
[ADR-1403](../adr/1403-cuda-strict-fp-every-kernel.md); HIP with
`-ffp-contract=off`), correctly rounded division and square root, and a
scratch-free SYCL form ([ADR-1395](../adr/1395-sycl-kernels-no-scratch.md)).
These are measured properties, not documented ones, so the per-backend
evaluation of ADR-2478 tests them on a minimal kernel before any port.

### 8. FFI cost

No measurement exists in the tree for a Rust-to-C call crossing in the hot
path; ADR-1713 defers throughput of the Rust twins to its task 7. The twins
cross the boundary once per frame per extractor (the shim passes plane views),
so the expected cost is small against per-frame compute; this is a hypothesis,
to be measured with the RC8 harness on the first migrated extractor.
Removing the shim removes the crossing.

### 9. Edition and MSRV

Edition 2024 is set for all crates (`Cargo.toml`, `core/src/rust/Cargo.toml`).
No `rust-version` is declared. The container has no Rust toolchain and the
installed stable is 1.98.1. A pinned stable toolchain (`rust-toolchain.toml`)
plus an MSRV equal to that pin, updated by Renovate with the container pins
(HISS-11), is the minimum the migration needs; nightly is required only by the
sanitizers and by GPU device crates.

## Alternatives weighed

See ADR-2478 section "Alternatives considered".

## Decisions taken on these points (Q-217 to Q-236, Q-243)

1. SYCL kernels share files with host code: native device sources go on a named
   exception list, 3.0 means no host C or C++, and the SYCL split is evaluated
   in the GPU-host phase.
2. Phase order is leaf-first (extractors and SIMD before the engine).
3. Late-1.x groundwork: MSRV pin, a Rust toolchain in the dev container, a Rust
   emitter in the API generator, Rust sanitizer, fuzz and coverage lanes. The
   sanitizer, fuzz and coverage equivalents named in section 5 are unproven in
   this tree until those lanes exist.
4. Toolchain: stable 1.98.1 pinned, `rust-version` equal to the pin, nightly
   only for sanitizers and GPU device crates.
5. Phases carry no milestone names until the re-plan; C builds only in an
   oracle-only profile after a phase exit, after hashed per-frame fixtures, and
   is deleted in 3.0.
6. Later decisions (Q-223 to Q-236, Q-243): milestones 2.1 to 2.5 and 3.0,
   epic [#2567](https://github.com/VMAFx/vmafx/issues/2567) with children
   #2568 to #2579 and #2581, `libvmaf.h` removal stays in 2.0, out-of-tree host
   glue is Rust where the host allows it (exception list has two classes),
   scalar Rust plus a re-evaluation list for ISAs without stable intrinsics
   (RVV, VSX, LSX / LASX, SVE / SVE2; the NEON and SVE2 stability questions of
   section 6 are therefore settled for scheduling, not verified). See ADR-2478.
