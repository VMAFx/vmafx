<!-- markdownlint-disable MD013 MD018 MD060 -->
# Research-0063: T7-5 NOLINT sweep — citation audit + closeout

- **Date**: 2026-05-04
- **Owner**: Lusoris, Claude (Anthropic)
- **Tags**: lint, audit, t7-5, touched-file-rule
- **Companion ADR**: [ADR-0278](../adr/0278-t7-5-nolint-sweep.md)

## Question

Backlog item **T7-5** (per [ADR-0141](../adr/0141-touched-file-cleanup-rule.md)
§Historical debt) targets the pre-2026-04-21 baseline of ~18
`readability-function-size` NOLINTs plus the upstream `_iqa_*`
reserved-identifier suppressions. Three predecessor PRs (#82 / ADR-0146,
#293, #327) progressively chipped the backlog down. **What is left?**

## Inventory before the sweep

Programmatic walk of every `NOLINT(readability-function-size)` site in
`core/src/` and `core/tools/`, with the preceding contiguous
comment-only block (up to 14 lines), classifying each by whether the
comment cites an `ADR-NNNN` or `Research-NNNN` identifier explicitly.

| Cluster | Sites | Already cite ADR/Research | Missing explicit cite |
|---|---:|---:|---:|
| `cuda/ssimulacra2_cuda.c` | 5 | 1 (line 277, ADR-0141 carve-out) | 4 (lines 378, 680, 770, 834) |
| `vulkan/ssimulacra2_vulkan.c` | 3 | 0 | 3 (lines 340, 444, 1068) |
| `vulkan/cambi_vulkan.c` | 1 | 0 | 1 (line 1148) |
| `sycl/integer_adm_sycl.cpp` | 7 | 1 (the `: see comment block above` form) | 6 |
| `sycl/integer_motion_sycl.cpp` | 2 | 0 | 2 |
| `sycl/integer_vif_sycl.cpp` | 5 | 1 (`see comment block above` form) | 4 |
| `integer_adm.c` | 14 (incl. NOLINTBEGIN blocks) | 13 (already cite ADR-0141) | 1 (line 988 sub-block) |
| `tools/vmaf.c` | 3 | 0 (prose-only) | 3 |
| `float_motion.c` | 3 | 3 (cite Research-0024 Strategy A) | 0 |
| `arm64/ssimulacra2_neon.c` | 6 | 6 (cite ADR-0138/0139) | 0 |
| `arm64/ssimulacra2_sve2.c` | 6 | 6 (cite ADR-0138/0139) | 0 |
| `arm64/psnr_hvs_neon.c` | 1 | 1 (cite ADR-0159 + ADR-0141) | 0 |
| `arm64/ssimulacra2_host_neon.c` | 1 | 1 (cite ADR-0242 carve-out) | 0 |
| `x86/ssimulacra2_avx2.c` | 5 | 5 (cite ADR-0138/0139) | 0 |
| `x86/ssimulacra2_avx512.c` | 5 | 5 (cite ADR-0138/0139) | 0 |
| `x86/psnr_hvs_avx2.c` | 1 | 1 (ADR-0141 audit invariant) | 0 |
| `x86/ssimulacra2_host_avx2.c` | 1 | 1 (ADR-0141 carve-out) | 0 |
| `ssimulacra2.c` | 3 | 3 (cite ADR-0141) | 0 |
| `third_party/xiph/psnr_hvs.c` | 1 NOLINTBEGIN block | upstream-vendored block, scoped | 0 |
| **Total** | **~75 sites** | **~53** | **22** |

`_iqa_*` reserved-identifier NOLINTs in `core/src/`: **zero** (cleared
by ADR-0146 / PR #82 already; the `iqa/` directory is fully lint-clean
and the renamed helpers — `iqa_convolve_horizontal_pass`,
`iqa_convolve_vertical_pass`, `ssim_compute_stats`, etc. — sit in named
helpers without reserved-identifier prefixes).

## Decision rationale per cluster

### Upstream-mirror parity (8 sites — `integer_adm.c`, `cuda/ssimulacra2_cuda.c`, `vulkan/ssimulacra2_vulkan.c`, `vulkan/cambi_vulkan.c`)

**Refactor or cite-only?** → cite-only.

Each site is a verbatim port (or near-verbatim port) of an upstream
function: `adm_decouple_s123` ports Netflix `966be8d5`'s
`adm_decouple_s123` block; the four `ss2c_*` / `ss2v_*` functions port
`ssimulacra2.c`'s `setup_gaussian`, `picture_to_linear_rgb`,
`run_scale`, and the per-scale combine step; `cambi_vk_extract`
mirrors `cambi.c::cambi_score`. The whole point of these helpers is
that a future reviewer can hold them next to the CPU scalar reference
and diff line-for-line. ADR-0146 made the same call for the IQA
helpers when it kept those refactor-with-extracted-helpers but
preserved the outer dispatch shape; here we go further and skip the
refactor entirely because the GPU-vs-CPU diff is the load-bearing
invariant and helper-extraction would force matching extraction in
the CPU file (which would re-open SIMD parity audits).

### SYCL kernel-launch pattern (12 sites — `sycl/integer_adm_sycl.cpp`, `sycl/integer_motion_sycl.cpp`, `sycl/integer_vif_sycl.cpp`)

**Refactor or cite-only?** → cite-only.

Each of these functions is a `cl::sycl::queue::submit(handler)` call
where the handler body is a single `parallel_for` lambda. The
function-size warning fires because the accessor declarations + the
lambda body together exceed the 60-line threshold; but the lambda
body is the kernel, and SYCL's compilation model requires the kernel
to be a single lexical entity that the offline compiler can fold into
a device-side specialisation. Splitting via macro doesn't reduce the
lexical kernel; splitting via free function defeats inlining and
breaks USM-pointer capture. The fork shares this pattern across every
SYCL TU; ADR-0146 didn't reach the SYCL files (CPU-only-built) and
this PR leaves it consistent with the existing prose justification.

### `tools/vmaf.c` (3 sites — `copy_picture_data`, `init_gpu_backends`, `main`)

**Refactor or cite-only?** → cite-only.

- `copy_picture_data`: The four bit-depth × component branches are
  the structure of YUV; folding through a function pointer would cost
  a per-row indirect call on every frame.
- `init_gpu_backends`: Three `#ifdef`-guarded backend stanzas
  (SYCL > CUDA > Vulkan priority chain). Splitting per-backend would
  multiply the `#if defined(HAVE_X)` decoration without making the
  priority chain clearer.
- `main`: Already swept by ADR-0146 / PR #327 — eight helpers
  extracted, the residual body is the cleanup-ownership spine
  inter-step glue.

All three already had multi-line prose comments documenting the
invariant; the sweep adds an explicit ADR reference.

## Outcome

- After-sweep audit: **75 sites, 0 missing ADR/Research citation.**
- Function-body refactors: **0** — every site's invariant is documented
  as a load-bearing reason not to split.
- New ADR-0138 / ADR-0139 / ADR-0141 references: **22**.
- New ADR-0278 cross-references: **22** (one per touched site).
- Behavioural delta: **none**.

## Verification command

```bash
# 1. Programmatic audit (Python; same script as in ADR-0278 §Verification)
python3 - <<'PY'
import re, os
paths = [os.path.join(r, f) for r, _, fs in os.walk('libvmaf/src')
         for f in fs if f.endswith(('.c','.cpp','.h'))]
paths.append('core/tools/vmaf.c')
miss = total = 0
for p in paths:
    with open(p) as fh:
        ls = fh.readlines()
    for i, line in enumerate(ls):
        if 'NOLINT' in line and 'readability-function-size' in line and 'NOLINTEND' not in line:
            total += 1
            ctx = [line]; j = i - 1
            while j >= 0 and j > i - 14:
                s = ls[j].strip()
                if not s: break
                if s.startswith(('//','/*','*')):
                    ctx.insert(0, ls[j]); j -= 1
                else: break
            buf = ''.join(ctx)
            if 'ADR-' not in buf and not re.search(r'[Rr]esearch-?\d', buf):
                miss += 1
print(f"sites={total} missing={miss}")
PY

# 2. Build + tests (CPU-only)
meson setup build -Denable_cuda=false -Denable_sycl=false
ninja -C build
meson test -C build

# 3. Netflix golden gate
make test-netflix-golden
```

Expected: `sites=75 missing=0`, all tests pass, golden gate green.

## References

- [ADR-0141](../adr/0141-touched-file-cleanup-rule.md) §2 +
  §Historical debt.
- [ADR-0146](../adr/0146-nolint-sweep-function-size.md) — Sweep A
  (parent).
- PR #82 (Sweep A), PR #293 (Sweep B+C), PR #327 (follow-up).
- [ADR-0278](../adr/0278-t7-5-nolint-sweep.md) — this sweep.
