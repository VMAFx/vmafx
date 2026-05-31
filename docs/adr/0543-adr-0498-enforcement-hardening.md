<!-- markdownlint-disable MD013 MD060 -->
# ADR-0543: ADR-0498 enforcement hardening — distinct exit code + structured JSON error + per-feature gate

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Opus 4.7)
- **Tags**: `cli`, `libvmaf`, `bugfix`, `backend`, `exit-code`, `extends-adr-0498`

## Context

ADR-0498 introduced the explicit-backend gate: when the user passes
`--backend NAME` (anything other than `auto` / `cpu`), an init failure
for the requested backend must surface as a hard error rather than
silently falling back to CPU. The gate was wired into
`init_gpu_backends` in `core/tools/vmaf.c` and returned `-1` on
failure, which propagates to `main()`'s `return ret`. POSIX truncates
that return to an unsigned 8-bit exit status (255), which is
technically non-zero.

A targeted v4-cluster regression test (ADR-0501 / V4-A) pinned the
non-zero propagation for `--backend vulkan`, but three sharp edges
remained that a downstream consumer (CI gate, vmaf-tune compare, MCP
probe) had no clean way to handle:

1. **Exit code 255 collides with generic error returns.**
   `report_pooled_scores`, `vmaf_read_pictures`, model loaders, and
   feature loaders all return non-zero on failure too. A wrapper that
   wants to distinguish "you asked for SYCL but it isn't there" from
   "scoring blew up mid-frame" has to grep stderr for the ADR-0498
   marker string. That's brittle — a future i18n / log-format change
   silently breaks the predicate.

2. **The `--output X.json` file is empty (or 0-byte) on backend
   failure.** `vmaf_write_output_with_format` is only reached on the
   success path, so when `init_gpu_backends` fails the file is never
   written (if stat-ed before the run) or left at whatever previous
   content sat at the path. Tooling that expects a JSON descriptor
   sees an empty file and crashes with a JSON-parse error rather than
   a structured signal. The empty file is especially misleading when
   the path is freshly `touch`-ed by the wrapper before invocation:
   the wrapper sees a 0-byte JSON, can't decode anything, and reports
   a generic "vmaf failure" instead of "vmaf rejected your backend
   request".

3. **Per-feature pinning has no equivalent gate.** Features named
   `*_cuda` / `*_sycl` / `*_vulkan` / `*_hip` / `*_metal` are
   GPU-pinned variants of CPU features. If the user passes
   `--feature motion_hip` but the matching backend isn't active in
   this run (not compiled in, not requested, or failed to init),
   `vmaf_use_feature` silently falls back to the CPU twin. The
   resulting scores are bit-identical to the explicit-backend
   invocation, but were computed on the wrong silicon — exactly the
   bug ADR-0498 banned for `--backend NAME`. The asymmetry leaves a
   quiet path around the explicit-backend gate.

## Decision

Extend ADR-0498 with three orthogonal hardening points, all landed in
the same PR (`fix/adr-0498-enforcement`):

1. **Dedicated exit code 100 for explicit-backend init failure.**
   Define `VMAF_EXIT_BACKEND_INIT_FAILED = 100` in
   `core/tools/vmaf.c`. `init_gpu_backends` now returns the
   sentinel `VMAF_INIT_GPU_EXPLICIT_FAIL = -100` (instead of `-1`)
   for the explicit-backend failure path; `main()` maps the sentinel
   to `VMAF_EXIT_BACKEND_INIT_FAILED` for the binary's exit code,
   while keeping the generic `-1` path for non-explicit failures
   (e.g. `vmaf_*_import_state` errors after a successful
   `state_init`). The value 100 is chosen because it's distinct from
   any errno-range value the binary might naturally emit and mirrors
   the `EX_*` convention from `<sysexits.h>` (without pulling that
   non-portable header in).

2. **Structured JSON error descriptor when `--output` is set and
   format is JSON.** A new `write_backend_error_json` helper
   overwrites the `--output` path with a single-line JSON object:

   ```json
   {
     "error": "<human-readable reason>",
     "backend_requested": "<sycl|cuda|vulkan|hip|metal>",
     "errno": <int>,
     "adr": "ADR-0498",
     "exit_code": 100
   }
   ```

   Called from every explicit-backend failure path (not-compiled-in,
   `state_init` failed for SYCL / CUDA / Vulkan / HIP / Metal) and
   from the new per-feature gate. No-op when `--output` is unset or
   the requested format isn't JSON; non-JSON consumers still get the
   non-zero exit code.

3. **Per-feature backend gate.** New helpers
   `feature_backend_suffix` (returns the suffix-derived backend
   keyword for `*_cuda` / `*_sycl` / `*_vulkan` / `*_hip` /
   `*_metal` feature names) and `backend_active` (checks the
   relevant `*_active` flag for the named backend). The
   feature-loading loop in `main()` consults both before calling
   `vmaf_use_feature` and hard-fails with the same exit code + JSON
   descriptor when a GPU-pinned feature is requested but the matching
   backend isn't active. CUDA's active flag, previously local to
   `init_gpu_backends`, is now propagated to `main()` via a new
   `bool *cuda_active_out` parameter so the gate (and the existing
   `backend_used` JSON echo from ADR-0498) can see it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `return -1` (exit 255), consumers parse stderr | Zero new behaviour | Couples every consumer to the exact marker string; breaks on i18n | The dedicated exit code is a one-line change in the consumer |
| Use exit code 200 instead of 100 | Higher = "more error" intuition | Collides with reserved 200+ range in some shells | 100 already mirrors `<sysexits.h>` convention; nothing in libvmaf CLI uses it |
| Emit JSON descriptor at every failure path (XML / CSV / SUB too) | Uniform error shape | Breaks format contract for non-JSON consumers | Exit code already conveys the signal; format-switch on failure surprises tooling |
| Use libvmaf's own error-formatting helper | Single source of truth | Expands public C API for a CLI-level concern | Inline `fprintf` is ~5 lines, stays local to `vmaf.c` |
| Libvmaf-side feature-registry refusal | Closer to the root cause | Requires threading backend-active state through `vmaf_use_feature` (API change) | CLI-side gate catches the same bug at the layer where intent is unambiguous |

## Consequences

- **Positive**:
  - CI gates can `[[ $rc -eq 100 ]]` to distinguish backend failures
    from other errors without parsing stderr.
  - `vmaf-tune compare` / MCP probes get a structured JSON error to
    decode instead of an empty file — cleaner error reporting in
    downstream reports.
  - Per-feature gate closes the silent-fallback gap for users who pass
    `--feature *_hip` without `--backend hip`.
  - CUDA active flag is now visible in `main()`, simplifying the
    `backend_used` JSON echo (drops the gpumask + no-flags inference).

- **Negative**:
  - Exit code 100 is a new public contract; consumers that expected
    255 from explicit-backend failures see a behaviour change.
    Mitigated by the fact that the only known consumer
    (vmaf-tune compare) was already checking `rc != 0`, not
    `rc == 255`.
  - The `--output` JSON file is now overwritten unconditionally on
    backend-init failure. A wrapper that pre-populated the file with
    metadata loses that content. No known consumer does this today.

- **Neutral / follow-ups**:
  - The per-feature gate refuses some invocations that previously
    silently fell back to CPU. The behaviour change is fail-loud,
    not fail-silent, so no scoring drift can result.
  - The Vulkan-empty-JSON case (`--backend vulkan` succeeds at init,
    then frame loop / score writing emits a 0-byte JSON) is a
    separate root cause and is not addressed here. Tracked
    separately for follow-up — needs investigation into
    `vmaf_write_output_with_format`'s behaviour when scores are
    missing.

## References

- ADR-0498 (predecessor): [`0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md`](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md)
  — introduced the explicit-backend gate and the `backend_used` JSON
  echo. This ADR extends, not supersedes.
- ADR-0501 (V4-A): [`0501-vmaf-tune-bbb-e2e-v4-bug-cluster.md`](0501-vmaf-tune-bbb-e2e-v4-bug-cluster.md)
  — pinned the ADR-0498 strict-mode non-zero exit propagation as a
  regression test, providing the runway for the dedicated-exit-code
  work here.
- Source: `req` — user direction (2026-05-18, paraphrased to neutral
  English): "ADR-0498 says: when the user passes `--backend NAME`
  (not the default `auto`), an init failure for the requested
  backend must surface as a hard error. Today this is partially
  enforced … the run doesn't actually exit with an error code. The
  JSON output is empty too. Apply the same enforcement to all
  backends, with backend-specific exit codes (100 = backend init
  failed when explicitly requested), JSON error field, and a
  per-feature check for HIP-pinned feature names."
