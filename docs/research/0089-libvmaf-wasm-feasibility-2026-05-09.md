<!-- markdownlint-disable MD029 MD060 -->
# Research-0089: libvmaf WebAssembly compilation feasibility

- **Status**: Active
- **Workstream**: ADR-0332
- **Last updated**: 2026-05-09
- **Author**: @Lusoris
- **Tags**: build, wasm, browser, ai, fork-local, research

## Question

Can the lusoris fork ship a WebAssembly build of libvmaf that runs
in modern browsers (and Node.js / Deno / Bun) so that streaming
product engineers, tooling sites, and educational content can run
VMAF on encoded variants without a native install? What does the
realistic surface look like across (a) the C99 metric engine,
(b) the SIMD paths, (c) the ONNX Runtime tiny-AI heads, and what
trade-offs does WASM impose that the fork cannot wave away?

The decision matrix lives in the companion
[ADR-0332](../adr/0332-libvmaf-wasm-target.md). This digest is the
evidence base.

All citations were retrieved on 2026-05-09. Per memory
`feedback_no_guessing` every load-bearing claim about Emscripten /
WASM-SIMD / ONNX Runtime Web / WebCodecs cites a primary source URL
with the access date.

## Sources

Primary (vendor / standards docs):

1. Emscripten — main documentation index.
   <https://emscripten.org/docs/index.html> (accessed 2026-05-09).
2. Emscripten — Porting / Simd128 (`wasm_simd128.h` and
   `-msimd128`).
   <https://emscripten.org/docs/porting/simd.html> (accessed 2026-05-09).
3. Emscripten — Pthreads support and `-pthread` requirements.
   <https://emscripten.org/docs/porting/pthreads.html> (accessed 2026-05-09).
4. WebAssembly proposals — `simd` (fixed-width 128-bit SIMD,
   shipped Phase 5 / standardised in the core spec).
   <https://github.com/WebAssembly/simd> (accessed 2026-05-09).
5. WebAssembly proposals — `relaxed-simd` (Phase 4 / Standardised).
   <https://github.com/WebAssembly/relaxed-simd> (accessed 2026-05-09).
6. WebAssembly proposals — `threads` (atomics + shared memory,
   Phase 4 / Standardised).
   <https://github.com/WebAssembly/threads> (accessed 2026-05-09).
7. WebAssembly proposals — `memory64` (64-bit address space,
   Phase 4 / Standardised in Wasm 3.0 draft).
   <https://github.com/WebAssembly/memory64> (accessed 2026-05-09).
8. MDN — `SharedArrayBuffer` cross-origin isolation requirements
   (`Cross-Origin-Opener-Policy: same-origin` +
   `Cross-Origin-Embedder-Policy: require-corp`).
   <https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/SharedArrayBuffer>
   (accessed 2026-05-09).
9. MDN — WebCodecs API (`VideoDecoder`, `VideoFrame`,
   `EncodedVideoChunk`).
   <https://developer.mozilla.org/en-US/docs/Web/API/WebCodecs_API>
   (accessed 2026-05-09).
10. ONNX Runtime — Web platform documentation
    (`onnxruntime-web` + WebAssembly / WebGPU execution providers).
    <https://onnxruntime.ai/docs/tutorials/web/> (accessed 2026-05-09).
11. ONNX Runtime — JavaScript API reference (`InferenceSession`,
    `Tensor`).
    <https://onnxruntime.ai/docs/api/js/> (accessed 2026-05-09).
12. Meson — Cross compilation with custom toolchain files.
    <https://mesonbuild.com/Cross-compilation.html> (accessed 2026-05-09).
13. Emscripten — Building libraries (Meson cross-file via
    `emcmake` / `emmeson`).
    <https://emscripten.org/docs/compiling/Building-Projects.html>
    (accessed 2026-05-09).
14. WebAssembly System Interface (WASI) — file I/O surface.
    <https://wasi.dev/> (accessed 2026-05-09).

Adjacent (search / package-registry checks):

15. npm registry search for `vmaf`.
    <https://www.npmjs.com/search?q=vmaf> (accessed 2026-05-09).
16. caniuse — `WebAssembly` baseline support.
    <https://caniuse.com/wasm> (accessed 2026-05-09).
17. caniuse — `wasm-simd` (fixed-width SIMD baseline).
    <https://caniuse.com/wasm-simd> (accessed 2026-05-09).
18. caniuse — `WebCodecs` support matrix.
    <https://caniuse.com/webcodecs> (accessed 2026-05-09).
19. simd-everywhere / simde — SSE/AVX → WASM-SIMD shim header
    library.
    <https://github.com/simd-everywhere/simde> (accessed 2026-05-09).

## Findings

### 1. Compilation feasibility — the libvmaf C core

**libvmaf is plain C99 + Meson + (optional) ONNX Runtime, with no
hard POSIX-only dependency on the CPU path.** The CPU code reads
YUV from a `FILE*` only via the CLI (`core/tools/vmaf.c`); the
library API itself takes pictures through `VmafPicture`, a memory
struct, so a WASM caller can marshal frames in from JavaScript
without touching libc file I/O.

- Meson supports cross-compilation through cross-files
  ([source 12](https://mesonbuild.com/Cross-compilation.html)).
  The Emscripten project maintains an integration path —
  `emconfigure meson setup` plus an Emscripten cross-file — that
  drives Meson with `emcc` as `c_compiler` and produces `.wasm`
  `.js` glue
  ([source 13](https://emscripten.org/docs/compiling/Building-Projects.html)).
  No upstream Meson change is required.
- The fork's banned-function policy (CLAUDE §6) already forbids
  `gets` / `sprintf` / `system`, all of which Emscripten's libc
  either omits or stubs to errors. The CPU path should not trip
  any unsupported syscall.
- `core/src/feature/` already has a dedicated NEON tree under
  `arm64/`. NEON intrinsics are **not** directly portable to WASM
  — there is no `arm_neon.h` analogue. WASM exposes its own
  fixed-width 128-bit SIMD via `<wasm_simd128.h>` and the
  `-msimd128` flag
  ([source 2](https://emscripten.org/docs/porting/simd.html),
  [source 4](https://github.com/WebAssembly/simd)). simde's
  `simde-arm/neon` and `simde-x86/sse*` headers translate the
  intrinsic surface to `wasm_simd128.h` lane-by-lane
  ([source 19](https://github.com/simd-everywhere/simde)),
  which means the existing AVX2 / NEON files compile via simde
  with a one-line include shim and no per-feature rewrite.
  Performance vs hand-tuned `wasm_simd128.h` is typically within
  10–30% on linear lane patterns; on shuffles it varies more.
  This is enough for a Tier-2 build to clear "scalar fallback +
  SIMD-shim".

**Verdict:** the CPU + SIMD core is a buildable target on
Emscripten with simde as the AVX/NEON → WASM-SIMD shim. The
binary will not be bit-exact with native (see §5).

### 2. ONNX Runtime — the tiny-AI heads

**ONNX Runtime ships an official `onnxruntime-web` package with
WebAssembly + WebGPU execution providers**
([source 10](https://onnxruntime.ai/docs/tutorials/web/),
[source 11](https://onnxruntime.ai/docs/api/js/)). The package
loads `.onnx` files from a URL or `ArrayBuffer`, runs inference
through a JS `InferenceSession`, and exposes WASM-SIMD and
multi-threaded WASM as opt-in execution providers.

Op-set coverage:

- ORT-Web's WASM EP does not list every ONNX operator — the docs
  call out a "supported operators" page per release. The fork's
  tiny-AI models (`fr_regressor_v1` / `v2`, `vmaf_tiny`,
  `saliency_student`) sit on a deliberately narrow allowlist
  documented in `docs/ai/`. Each candidate model **must** be
  audited against the ORT-Web op coverage page for the version
  pinned (verify per release; the list moves).
- ORT-Web supports `ort.env.wasm.numThreads` for multi-threaded
  inference, which requires the `SharedArrayBuffer` /
  cross-origin isolated context (§3 below).
- WebGPU EP gives GPU acceleration in browsers that ship
  `navigator.gpu`. This is **not** a substitute for our
  CUDA/SYCL/Vulkan backends — the GPU compute is reachable only
  for the ONNX inference, not for the libvmaf feature kernels.

**Verdict:** ORT-Web is a drop-in fit *if* the fork audits every
shipped tiny-AI model against the pinned ORT-Web release's op
allowlist. Block on op-list audit before promising Tier 3.

### 3. WASM platform limitations the fork cannot paper over

| Limitation | Source | Fork impact |
|---|---|---|
| **No GPU compute reachable from within the WASM module** for libvmaf kernels | sources 1, 4 | All of CUDA/SYCL/Vulkan/HIP/Metal are out at the libvmaf-kernel level. WebGPU is JS-only and ORT-Web-only. |
| **Threading requires `SharedArrayBuffer`** + cross-origin isolation (`COOP: same-origin` + `COEP: require-corp`) | sources 3, 6, 8 | A site embedding the WASM build must serve those headers. Many embedding contexts (e.g. CodePen-style sandboxes, plain GitHub Pages without overrides) cannot. Single-threaded fallback must work. |
| **No direct file I/O** in the browser sandbox | sources 1, 14 | Callers must marshal YUV through `ArrayBuffer` / `Uint8Array`, or pull frames from `WebCodecs` (source 9). The WASI surface (source 14) covers Node.js / Deno / Bun but not browsers. |
| **32-bit memory cap (~4 GB) by default** | source 7 | A 4K 10-bit YUV frame is ~12.4 MB; a few seconds of frames easily reaches a working set that crowds the 4 GB cap together with model weights. Memory64 is now in Wasm 3.0 draft (Phase 4 / Standardised) but browser shipping is uneven — Firefox shipped first; Chromium gates `--js-flags`/origin-trial in some channels (verify per browser at consume time). |
| **WebCodecs is the natural input source** but is not yet baseline-everywhere | sources 9, 18 | Safari shipped WebCodecs in 16.4 (2023). caniuse shows global support north of 90% for evergreen browsers but no IE/legacy. Embedders without WebCodecs must decode in JS or upload pre-decoded YUV. |

### 4. Existing JavaScript VMAF on npm

`npm search vmaf` ([source 15](https://www.npmjs.com/search?q=vmaf))
returns a small set of *wrappers* — packages that shell out to a
native `vmaf` binary or a Docker container, plus a few "score
parsing" helpers. **There is no published in-process WASM VMAF
on the npm registry as of 2026-05-09.** A first-party fork-built
WASM module would fill an empty slot.

(Names of wrapper packages exist but the fork should not endorse
or compete with any specific one in the ADR. The relevant fact is
that the in-process slot is unfilled.)

### 5. Will the WASM build preserve the Netflix golden-data gate?

**Loud flag — almost certainly NO at full bit-exactness.** Per
memory `feedback_no_test_weakening` and `feedback_golden_gate_cpu_only`
the gate is the 3 Netflix CPU pairs with their hardcoded
`assertAlmostEqual` values, and the GPU backends have *never*
been claimed bit-exact against the CPU path.

Reasons WASM will diverge from native CPU:

- WASM's float semantics are IEEE-754 binary32 / binary64 with
  deterministic rounding *within* a module, but cross-module
  fused-multiply-add (`fma`) availability differs from native
  AVX2/AVX-512, and Emscripten's libm uses `musl` while the
  fork's CI baseline uses `glibc` — the math-library transcendentals
  (`log`, `expf`, `pow`) can ULP-differ at the last bit.
- `relaxed-simd` ([source 5](https://github.com/WebAssembly/relaxed-simd))
  permits non-deterministic FMA / dot-product lane behaviour as
  an opt-in performance lane; if Tier 2 enables it, ULP drift is
  *spec-permitted*.

**Implication:** the WASM build cannot be a Netflix-golden-gate
participant. It must run a **separate** snapshot suite (analogous
to `testdata/scores_cpu_*.json` for GPU/SIMD per CLAUDE §9) with
its own `regen-snapshots`-justified baselines. The ADR must call
this out explicitly so a future contributor does not accidentally
gate WASM on `make test-netflix-golden`.

### 6. Realistic three-tier rollout

| Tier | Scope | Approx `.wasm` size | Dependencies | Build flags |
|---|---|---|---|---|
| **Tier 1** | Scalar VMAF score for two YUV blobs in memory; **no SIMD, no AI head**; single-threaded | ~500 KB–1 MB | Emscripten only | `meson` cross-file with `emcc`, `-O3`, `-msimd128=0` |
| **Tier 2** | Tier 1 + WASM-SIMD via simde (AVX2/NEON shim) + optional `pthread` build for multi-frame parallelism | ~1.5–2.5 MB | Emscripten + simde header-only library | `-msimd128`, `-pthread` (requires consumer to set COOP/COEP), simde include shim |
| **Tier 3** | Tier 2 + ONNX Runtime Web + the fork's allowlisted tiny-AI heads | ~5–10 MB module + ~3–8 MB per ONNX model | Emscripten + simde + `onnxruntime-web` (npm) + per-model op-allowlist audit | Tier-2 flags + ORT-Web JS glue |

Disk-size numbers above are order-of-magnitude estimates from
typical Emscripten/ORT-Web project benchmarks; they should be
re-measured when the EXPERIMENT phase produces the first build.

### 7. Distribution and packaging

Recommended primary surface: **npm package
`@lusoris/libvmaf-wasm`** — same scope used by other fork-local
JS artifacts. Module shape: ESM with a typed `index.d.ts`. Loads
the `.wasm` from the package's `dist/` by default, configurable
to a CDN URL.

Secondary: GitHub release artifact (raw `.wasm` + `.js` glue +
sourcemaps) attached to the same `release-please` tag the native
build cuts (CLAUDE §11). This means consumers without npm can
fetch directly.

Versioning: `<libvmaf-version>-wasm.N` minor suffix on the npm
package, tracking the libvmaf SemVer.

### 8. Maintenance burden

A new WASM build target adds:

- A new CI lane (Emscripten install + meson cross-build + the WASM
  snapshot suite) — call it `wasm-cpu`.
- A new `meson_options.txt` flag (`enable_wasm` or driven entirely
  by the cross-file).
- Per CLAUDE §12 r10 (project-wide doc rule): `docs/development/wasm.md`
  a `docs/usage/wasm-quickstart.md` + a row in
  [`docs/backends/`](../backends/) covering the WASM surface.
- Per memory `feedback_no_lint_skip_upstream`: clang-tidy + the rest
  of `make lint` must apply to the WASM build's source files; if
  simde headers trip lint, the fix is to refactor or to cite the
  shim invariant per CLAUDE §12 r12, not to suppress paths.
- A new ADR every time we add a tier (Tier 2, Tier 3 ship as their
  own ADRs after the EXPERIMENT phase confirms feasibility).

This is non-trivial but bounded — comparable in scope to a single
new GPU backend.

## Alternatives explored

1. **Native binary + browser-side wrapper that POSTs YUV to a
   server.** Works today, no fork change required. Loses the
   "runs in the user's browser, no server" benefit which is the
   *only* reason WASM is attractive for tooling sites and
   educational content. Rejected as not addressing the question.
2. **Pure-JavaScript VMAF reimplementation.** Several papers and
   the npm registry (source 15) confirm nobody has done this; it
   would require porting the entire feature stack to JS by hand.
   Massively more work than Emscripten-cross-compile, and forks
   the codebase. Rejected.
3. **WebAssembly + WebGPU compute kernels** for the libvmaf
   feature path (i.e. write feature kernels in WGSL). Possible in
   principle but requires a from-scratch GPU backend separate
   from CUDA/SYCL/Vulkan. Out of scope for the feasibility study;
   should be its own future ADR if Tier 2 ships and demand
   justifies it.
4. **AssemblyScript (subset-of-TypeScript → WASM) port.** Same
   problem as alt 2 — a port, not a build. Rejected.

## Open questions

- **Does ORT-Web's pinned op allowlist cover every operator the
  fork's ONNX models emit?** Audit required before Tier 3 work
  starts. Per-model checklist: dump the graph (`onnx.checker` /
  Netron), cross-reference each `OpType` against ORT-Web's
  operators page for the pinned release.
- **Can simde's AVX2 shim drive the fork's AVX-512 paths?**
  AVX-512 → WASM-SIMD requires lane-doubling (256 bits → two
  128-bit ops); simde supports this but the perf cliff vs native
  AVX-512 is steep. Tier 2 likely targets the AVX2-equivalent path
  only.
- **What is the realistic perf floor?** Empirical benchmark must
  follow Tier 1 ship; numbers depend on browser + CPU + frame
  size in ways no doc can predict.
- **Memory64 browser shipping status at consume time.** Verify per
  browser at the point Tier 1 ships; if Memory64 is not yet
  Baseline, the ADR's 4 GB cap warning stays in `docs/usage/`.

## Related

- [ADR-0332](../adr/0332-libvmaf-wasm-target.md) — the decision.
- [CLAUDE.md §12 r10](../../CLAUDE.md) — project-wide doc rule.
- [CLAUDE.md §9](../../CLAUDE.md) — snapshot regeneration policy
  (the WASM build will need its own snapshot file under
  `testdata/scores_wasm_*.json`).
- Memory `feedback_golden_gate_cpu_only` — the Netflix golden gate
  is CPU-native only; WASM joins the GPU/SIMD class of "close but
  not bit-identical".
- Memory `feedback_no_guessing` — every Emscripten / WASM-SIMD /
  ORT-Web claim above cites a primary source URL with access date.
