<!-- markdownlint-disable MD060 -->
# ADR-0877: Error-code consistency audit — fork-added MS-SSIM decimate dispatcher

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: c, simd, error-handling, api-contract

## Context

libvmaf's internal C convention for status-returning functions is negative
POSIX errno values (`-EINVAL`, `-ENOMEM`, `-EIO`, `-ENODEV`, …). This is
already enforced consistently in the public C API entry points (see
`vmaf_hip_state_init`, `vmaf_dnn_session_open`, `vmaf_cuda_picture_alloc`)
where the convention is documented and load-bearing — callers branch on
specific errno values to differentiate retryable from fatal failures.

Fork-added code occasionally drifts. The fork-added `ms_ssim_decimate`
dispatcher and its three SIMD specialisations (AVX2 / AVX-512 / NEON,
all introduced 2026-04-20) return a bare `-1` on `malloc` failure
rather than `-ENOMEM`. The header docstring even hedged: "0 on success,
non-zero on allocation failure". That's correct but anaemic — it leaves
the caller unable to surface the actual cause to a higher layer that
consumes the libvmaf return code as a POSIX errno.

A wider audit of fork-added C/C++/Obj-C surfaces under `core/src/` (99
suspicious returns across 35 TUs) found that the remaining `return 1`
patterns in fork-added code are either framework-correct flush()
"drain complete" signals (the feature-extractor framework loops on
`while (!err)` and treats positive as termination), boolean predicate
returns (`vmaf_hip_available`, `vmaf_metal_dispatch_supports`), qsort
comparators, or live inside upstream-mirrored translation units
(`pdjson`, `predict.c`, `read_json_model.c`, `motion.c`, `ssim.c`,
`ms_ssim.c`, `mkdirp.c`, `psnr_tools.c`, `picture_cuda.c`,
`integer_{adm,vif}_cuda.c`) where the rebase contract pins the
existing return shape. Those are deliberately untouched.

## Decision

We narrow this PR to the four fork-added MS-SSIM decimate translation
units, converting their lone `return -1` (the `malloc`-failure path)
to `return -ENOMEM`, and tighten the header doc to match. All other
fork-added C surfaces audited in this pass already follow the
errno-style convention or use one of the legitimate non-errno return
patterns documented above.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Convert every `return -1` / `return 1` in fork-added C | Maximum sweep | Most of the matches are framework-correct (flush signals, qsort, boolean) — converting them would break the framework contract | Rejected |
| Tighten only the SIMD decimate paths but leave the scalar untouched | Smaller diff | Header doc applies to the scalar entry point; if the scalar still returns `-1` the doc is inconsistent | Rejected |
| Also rewrite `mkdirp.c` (`-1` on failure) | Aligns the directory helper too | Original MIT-licensed Stephen Mathieson code; the `-1` is the public contract documented in the header comment and matches the original C source | Rejected |
| Convert the orphan `mkdirp.cpp` (C++23 pilot, not currently in meson.build) | Consistency with the .c version | Orphan TU not actually built; touching it adds risk for zero runtime benefit | Rejected |

## Consequences

- **Positive**: callers that wrap libvmaf in higher-level error
  reporters (the Go controller, MCP server, ffmpeg filter) can now
  surface the actual cause (`-ENOMEM`) on the MS-SSIM decimate path
  instead of an opaque `-1`. Brings the four files in line with
  `ms_ssim_decimate.h`'s strengthened docstring.
- **Negative**: none — caller `ms_ssim.c:207-208` uses a truthy check
  (`ms_ssim_decimate(...) || ms_ssim_decimate(...)`) which works for
  any non-zero return.
- **Neutral**: bit-exactness across scalar / AVX2 / AVX-512 / NEON
  paths is preserved (only the malloc-failure branch is touched; the
  hot path is unchanged).

## References

- `core/src/feature/ms_ssim_decimate.{c,h}`
- `core/src/feature/x86/ms_ssim_decimate_avx2.c`
- `core/src/feature/x86/ms_ssim_decimate_avx512.c`
- `core/src/feature/arm64/ms_ssim_decimate_neon.c`
- Source: `req` — "Audit error-code return consistency in fork-added
  C code. The libvmaf convention is negative errno-style (`-EINVAL`,
  `-ENOMEM`, etc.) — every function that returns int as status should
  follow this."
- Related: ADR-0872 (EINTR + I/O error audit on MCP transports —
  same audit family, different surface).
