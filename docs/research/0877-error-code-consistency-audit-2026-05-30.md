<!-- markdownlint-disable MD013 MD060 -->
# Research-0877: Error-code consistency audit — fork-added C surfaces

- **Date**: 2026-05-30
- **Author**: lusoris (agent-dispatched)
- **Scope**: `core/src/**` C/C++/Obj-C TUs added by the fork (≥ 2026-04-15)
- **Related ADR**: [ADR-0877](../adr/0877-error-code-consistency-audit.md)

## Question

libvmaf's internal convention for status-returning C functions is negative
POSIX errno (`-EINVAL`, `-ENOMEM`, `-EIO`, `-ENODEV`, `-ENOSYS`, …). Are
fork-added C surfaces consistent with this, or have some drifted to bare
`-1` / wrong-sign `+1` returns?

## Method

1. Enumerated every match of `return -?[12345];` under `core/src/` (filter
   out `third_party/`, `iqa/`, vendored cJSON, libsvm). Initial hit set:
   99 lines across 35 TUs.
2. Classified each file by first-commit date (`git log --all --pretty='%ad'
   --reverse -- libvmaf/src/<path>` to traverse the ADR-0700 rename):
   pre-2026 = upstream-mirror, ≥ 2026-04-15 = fork-added.
3. For each fork-added match, read the surrounding function to determine
   whether the return value is:
   - a true status code (candidate for errno conversion), OR
   - a framework signal (e.g. `flush()` "drain complete" → positive
     terminates the framework's `while (!err)` loop), OR
   - a boolean predicate (`vmaf_hip_available`, `vmaf_metal_dispatch_supports`,
     `vmaf_dnn_available`), OR
   - a qsort comparator (`predict.c::score_compare`).
4. Cross-referenced the call sites for each candidate to verify the change
   would not break the caller's branch logic.
5. Checked in-flight PR file lists (`gh pr list --json files`) to avoid
   colliding with #358 (eintr-and-io-error-audit — touches MCP transports),
   #359 (magic-number-audit — touches `mcp/mcp.c`, `transport_sse.c`), and
   others.

## Findings

### Upstream-mirror — out of scope per CLAUDE.md §12 r7

These TUs predate the fork (Netflix-origin) and their return shape is the
upstream contract. Touching them risks rebase conflicts and golden-data
drift:

| TU | First commit |
|---|---|
| `core/src/feature/ssim.c` | 2019-10-31 |
| `core/src/feature/motion.c` | 2019-10-31 |
| `core/src/feature/ms_ssim.c` | 2019-10-31 |
| `core/src/feature/psnr_tools.c` | 2019-10-31 |
| `core/src/feature/common/blur_array.c` | 2019-10-31 |
| `core/src/feature/mkdirp.c` | 2021-12-13 (Stephen Mathieson MIT) |
| `core/src/predict.c` | 2019-12-09 (qsort comparator) |
| `core/src/read_json_model.c` | 2020-11-10 (positive = "skip key") |
| `core/src/pdjson.c` | 2020-11-10 (vendored JSON parser) |
| `core/src/cuda/picture_cuda.c` | 2022-07-16 |
| `core/src/cuda/common.c` | 2022-07-16 |
| `core/src/feature/cuda/integer_adm_cuda.c` | 2022-12-13 (Netflix CUDA port) |
| `core/src/feature/cuda/integer_vif_cuda.c` | 2022-12-13 |

### Fork-added — framework-correct (intentionally non-errno)

- **Feature-extractor `flush()` callbacks** return positive on "drain complete";
  the framework's `vmaf_feature_extractor_context_flush` loops on
  `while (!(err = fex->flush(...)))` — positive terminates the loop, negative
  surfaces an error. This applies to ALL `return 1` at the end of `flush()` in:
  - `core/src/feature/integer_motion_v2.c:430,487`
  - `core/src/feature/hip/integer_motion_hip.c:629`
  - `core/src/feature/hip/integer_motion_v2_hip.c:379,392,417`
  - `core/src/feature/hip/integer_adm_hip.c:1280`
  - `core/src/feature/hip/float_motion_hip.c:536,542`
  - `core/src/feature/hip/integer_vif_hip.c:651`
  - `core/src/feature/cuda/integer_motion_v2_cuda.c:282,307`
  - `core/src/feature/sycl/{integer_motion_v2,integer_adm,integer_vif,float_motion}_sycl.cpp` (4 sites)
  - `core/src/feature/metal/{integer_motion,integer_motion_v2,float_motion}_metal.mm` (3 sites)
  - `core/src/feature/common/blur_array.c:63` (close callback, same convention)
  - `core/src/mcp/mcp.c:48` (transport loop terminator)
- **Boolean availability/predicate returns** (NOT errors):
  - `core/src/hip/common.c:96` — `vmaf_hip_available()` returns 1 when HIP is compiled in
  - `core/src/dnn/dnn_api.c:35` — `vmaf_dnn_available()` returns 1 when ONNX Runtime is linked
  - `core/src/metal/dispatch_strategy.c:49` — `vmaf_metal_dispatch_supports()` returns 1 when feature matches
  - `core/src/metal/common.mm:166,176,269` — Apple7+ probe and `vmaf_metal_available()`
- **Documented `-1` API contract** (callers test `== -1`):
  - `core/src/dnn/model_loader.c:116,123` — `vmaf_dnn_sniff_kind()` returns
    `VMAF_MODEL_KIND_*` enum (0/1/2/3) or `-1` for "unknown"; documented in
    `model_loader.h:131-132` and asserted in `test_model_loader.c:32-33`.

### Fork-added — in-flight PR overlap (skipped to avoid conflicts)

| TU | In-flight PR |
|---|---|
| `core/src/mcp/transport_stdio.c` | #358 (eintr-audit) |
| `core/src/mcp/transport_uds.c` | #358 |
| `core/src/mcp/transport_sse.c` | #359 (magic-number-audit) |
| `core/src/mcp/mcp.c` | #359 |

These will benefit from the same audit in a follow-up once the in-flight PRs
land.

### Fork-added — actually drifted (THIS PR's scope)

Four fork-added TUs (all introduced 2026-04-20 by the MS-SSIM SIMD pilot)
return bare `-1` on `malloc` failure where `-ENOMEM` is the right answer:

| TU | Line | Before | After |
|---|---|---|---|
| `core/src/feature/ms_ssim_decimate.c` | 139 | `return -1;` | `return -ENOMEM;` |
| `core/src/feature/x86/ms_ssim_decimate_avx2.c` | 216 | `return -1;` | `return -ENOMEM;` |
| `core/src/feature/x86/ms_ssim_decimate_avx512.c` | 198 | `return -1;` | `return -ENOMEM;` |
| `core/src/feature/arm64/ms_ssim_decimate_neon.c` | 197 | `return -1;` | `return -ENOMEM;` |

Plus a header doc tightening: `core/src/feature/ms_ssim_decimate.h:54`
"non-zero on allocation failure" → "-ENOMEM on allocation failure".

## Caller-impact verification

The only direct caller is `core/src/feature/ms_ssim.c:207-208`:

```c
if (ms_ssim_decimate(ref_imgs[idx - 1], cur_w, cur_h, ref_imgs[idx], 0, 0) ||
    ms_ssim_decimate(cmp_imgs[idx - 1], cur_w, cur_h, cmp_imgs[idx], &cur_w, &cur_h)) {
```

Truthy check — works for any non-zero. No call sites compare against `-1`
specifically. Replacement strengthens the surface without breaking anything.

## Recommendation

Land the four-TU + one-header change in this PR. Follow-up audits on the MCP
transports (`transport_stdio.c`, `transport_uds.c`, `transport_sse.c`,
`mcp.c`) once PRs #358 / #359 merge.

## References

- `req` — "Audit error-code return consistency in fork-added C code. The
  libvmaf convention is negative errno-style (`-EINVAL`, `-ENOMEM`, etc.)
  — every function that returns int as status should follow this."
- `core/src/feature/feature_extractor.c:660-665` — framework's flush loop
  contract (positive = terminate).
- ADR-0700 — `libvmaf/` → `core/` rename (used to classify file ages via
  pre-rename `git log`).
- ADR-0872 — sibling I/O-error / EINTR audit on MCP transports.
