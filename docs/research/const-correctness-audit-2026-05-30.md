<!-- markdownlint-disable MD018 MD060 -->
# Const-correctness audit — fork-added C / C++ — 2026-05-30

Sweep of fork-added C / C++ translation units for pointer-parameter
const-correctness gaps (MISRA C 8.13, CERT EXP05-C, SEI CERT C++ Con01).
Branch `chore/const-correctness-sweep` off `origin/master` at
`bbcaa8d127`.

---

## Scope

"Fork-added" = files whose header carries a `Copyright 2026 Lusoris …`
line (the convention adopted in CLAUDE.md §12 rule 7 — replacing the
older "and Claude (Anthropic)" co-authoring per the 2026-05-27
decision, see project memory `project_copyright_lusoris_only`).

Upstream-mirrored sources (Netflix copyright header, vendored Daala
`y4m_input.c`, vendored libsvm / cJSON / pdjson, vendored Xiph
helpers under `core/src/feature/third_party/xiph/`,
`core/src/feature/iqa/`) are explicitly **excluded** by the project
rebase-parity invariant — identifier-shape changes there would break
upstream-port mechanical merges.

In-flight PRs touching `core/src` / `core/tools` / `core/include` C
sources were also excluded to avoid merge-base churn (file-level
overlap check against PRs #308, #316, #317, #319, #338, #343, #352,
#355, #356, #357, #359, #360 at audit time).

| Surface | Files in tree | Files audited | Reason others skipped |
|---------|---------------|---------------|-----------------------|
| `core/src/dnn/` | 7 `.c` + 8 `.h` | 7 `.c` + 8 `.h` | All buildable in CPU-only meson configuration |
| `core/src/mcp/` | 6 `.c` + 1 `.h` (excl. cJSON vendored) | 3 `.c` | 4 `.c` + 1 `.h` in-flight on PR #359 |
| `core/src/feature/` (fork-added) | 9 `.c` | 9 `.c` | All buildable in CPU-only |
| `core/src/feature/x86/` (fork-added) | 9 `.c` | 9 `.c` | All buildable on x86_64 host |
| `core/src/feature/common/` (fork-added) | 1 `.c` (`convolution_avx512.c`) | 1 `.c` | Buildable |
| `core/src/feature/arm64/` (fork-added) | 12 `.c` | 0 (deferred) | Cross-compile only on x86_64 host; tracked for follow-up |
| `core/src/hip/`, `core/src/metal/`, `core/src/sycl/` | 18 source files | dispatch_strategy.c (Metal) only | HIP/Metal/SYCL stage headers not in CPU build; partially in-flight on #343, #356 |
| `core/src/` root (fork-added) | 5 `.c` (gpu_dispatch_env, fex_ctx_vector, framesync, etc.) | 4 `.c` | `gpu_picture_pool.c` in-flight on #317 |
| `core/tools/` (fork-added) | 2 `.c` (`vmaf_roi.c`, `vmaf_per_shot.c`) | 2 `.c` | Buildable |

Total in-scope: **28 buildable fork-added `.c` files** + headers they
expose. The full fork-added population is 227 `.c` / `.cpp` / `.h` /
`.hpp` files (61 of those under `core/test/`); the gap is the
HIP / Metal / SYCL backends and ARM64 NEON paths that need backend
toolchains not present on the host.

## Method

Two-pass with `clang-tidy --checks='-*,readability-non-const-parameter'`
(the canonical MISRA C 8.13 / CERT EXP05-C check) against the CPU-only
`build-const/compile_commands.json`:

```bash
meson setup build-const core -Denable_cuda=false -Denable_sycl=false
ninja -C build-const
clang-tidy -p build-const --checks='-*,readability-non-const-parameter' \
  $(cat /tmp/buildable_fork_c.txt)
```

Cross-check with `--checks='-*,readability-non-const-parameter,readability-avoid-const-params-in-decls,misc-const-correctness'`
produced no additional findings beyond the single result below.

A spot-check manual sweep was also done over `static` helpers in each
file (grep-and-read) to catch cases the checker misses when a function
is consumed only via function-pointer table (`VmafFeatureExtractor`
vtable callbacks). Every such helper either already declares its
read-only inputs `const`, or has a vtable-fixed signature where const
is structurally impossible without changing every dispatcher slot.

## Findings

**Result: 0 actionable const-correctness gaps in fork-added,
buildable C / C++ surfaces.**

The audit produced exactly one `readability-non-const-parameter`
warning, and it is a NOLINT-placement bug, not a missing `const`:

```text
core/src/dnn/dnn_api.c:324:87: warning: pointer parameter 'out'
  can be pointer to const [readability-non-const-parameter]
```

The function `vmaf_dnn_session_run_luma8` is a `-ENOSYS` stub whose
signature must match the real ONNX-Runtime entry point declared in
`core/include/libvmaf/dnn.h` (ADR-0374). The intentional NOLINT is
present, but on physical line 325 instead of line 324 where the
parameter token sits:

```c
int vmaf_dnn_session_run_luma8(
    VmafDnnSession *sess, const uint8_t *in, size_t in_stride, int w, int h, uint8_t *out,
    size_t out_stride) // NOLINT(readability-non-const-parameter) — stub contract per ADR-0374
```

The sibling stub at line 339 (`vmaf_dnn_session_run_plane16`) has the
NOLINT inline on the same physical line as the parameter and does not
warn. This NOLINT-attribution drift is in the scope of the in-flight
PR #353 ("NOLINT inventory audit — close 16 ADR-cite-form gaps") and
is best fixed there to avoid touching the same file from two
concurrent PRs.

## Why the surface is already clean

Three structural reasons the fork-added C surface holds up to the
const-correctness check unaided:

1. **`.clang-tidy` enables `readability-non-const-parameter` in the
   project default check list** (line 21 of the root config). The
   check has been running on every PR that builds against
   `compile_commands.json`, so any new fork-added function that
   accepts a non-const pointer it only reads from gets a warning
   before merge.
2. **The DNN / MCP / GPU-runtime style established in ADRs 0374 (DNN
   API contracts), 0461 (gpu_dispatch_env), 0485 (lifecycle struct
   zero), and 0550 (tensor_io auto-resize) consistently writes
   read-only inputs as `const T *` from the first commit.** Code
   review caught the patterns early.
3. **Feature-extractor vtable callbacks (`VmafFeatureExtractor.init` /
   `.extract` / `.close`) carry pointer parameters whose const-ness
   is fixed by the public header.** Calling sites cast appropriately,
   so individual feature `.c` files inherit the contract rather than
   re-declaring it.

## Files where the checker did fire (out of scope)

For completeness, the same check on upstream-mirrored Netflix C produced
warnings that this audit **deliberately did not act on**:

| File | Warnings | Provenance | Reason not fixed |
|------|---------:|------------|------------------|
| `core/src/feature/cambi.c` | 1 (`scores_per_scale`, line 1402) | Netflix upstream | Identifier-shape preserved for rebase parity (CLAUDE.md §10) |
| `core/src/feature/speed.c` | 11 (various `v` / `A` / `x` / `d` / `sd` parameters in private helpers) | Netflix upstream | Identifier-shape preserved for rebase parity |
| `core/tools/y4m_input.c` | 5 (various `_aux` / `_dst` parameters) | Vendored Daala (Xiph) | Vendored upstream license + diffable-with-Daala-master invariant |

These should be addressed only as part of a coordinated upstream-port
sweep (`/port-upstream-commit` or `/sync-upstream`), not opportunistically.

## Follow-ups (not blocked on this sweep)

1. **HIP / Metal / SYCL audit re-run inside `vmaf-dev-mcp` container.**
   The host CPU-only build cannot resolve `hip/hip_runtime_api.h`,
   Metal Objective-C++ headers, or the oneAPI SYCL headers. A
   container-side re-run of the same `clang-tidy` invocation against
   `build-cuda` / `build-all` configurations would cover the 18
   backend-specific source files this audit skipped. The
   `vmaf-dev-mcp` container has every backend's toolchain (CLAUDE.md
   §12 rule 15).
2. **ARM64 NEON audit re-run on an ARM64 host or via QEMU + Clang
   cross-compile.** The 12 `core/src/feature/arm64/*.c` files compile
   only against NEON intrinsics that x86_64 Clang does not stub. A
   one-shot run on a Raspberry Pi 5 / Ampere Altra / M-series host
   would close the coverage gap.
3. **NOLINT-placement drift on stub signatures.** The
   `dnn_api.c:324` finding above is owned by in-flight PR #353. If
   #353 lands first, this audit becomes 100 % clean.

## Reproducer

```bash
# In /tmp/wt-const (or any worktree of master tip):
meson setup build-const core -Denable_cuda=false -Denable_sycl=false
ninja -C build-const
find core/src core/tools core/include -type f \( -name '*.c' -o -name '*.cpp' \) \
  | xargs grep -l "Copyright.*Lusoris\|Copyright 2026" 2>/dev/null > /tmp/all_fork_c.txt
python3 -c "
import json
data = json.load(open('build-const/compile_commands.json'))
build_files = {e['file'].replace('../', '') for e in data}
fork_files = open('/tmp/all_fork_c.txt').read().strip().split('\n')
buildable = [f for f in fork_files if f in build_files]
open('/tmp/buildable_fork_c.txt', 'w').write('\n'.join(buildable))
"
clang-tidy -p build-const --checks='-*,readability-non-const-parameter' --quiet \
  $(cat /tmp/buildable_fork_c.txt)
```

Expected output (post-PR-#353): no warnings. Pre-#353: one warning
on `dnn_api.c:324`.
