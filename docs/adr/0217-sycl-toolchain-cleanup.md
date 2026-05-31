<!-- markdownlint-disable MD013 MD060 -->
# ADR-0217: SYCL toolchain cleanup — multi-version recipe + icpx-aware clang-tidy wrapper

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: sycl, ci, clang-tidy, tooling, fork-local

## Context

The 2026-04-28 backlog audit (BACKLOG row T7-13) bundled two SYCL-toolchain
follow-ups left dangling after T7-7 (clang-tidy SYCL findings cleared) and
T7-8 (oneAPI 2025.0.4 → 2025.3.1 bump):

1. **Bench gap.** The Arch repo's `intel-oneapi-basekit` package (pinned
   2025.0.4 at the time of the audit) ships device images that no longer
   match the Level Zero loader shipped by `level-zero-loader` on rolling
   distros. Anyone running `vmaf_bench` against the older compiler hits
   silent host-fallback or `ze_init` failures unless they activate the
   newer 2025.3 install via `setvars.sh` and override `LD_LIBRARY_PATH`.
   The `oneapi-install.md` doc covered installing 2025.3 side-by-side but
   not the per-shell switch recipe; non-CI users had to reverse-engineer
   it from `setvars.sh` source.
2. **Lint gap.** The CI changed-file `clang-tidy` job in
   `.github/workflows/lint-and-format.yml` excludes `core/src/sycl/`
   and `core/src/feature/sycl/` because the CPU-only `build/` lacks
   `compile_commands.json` entries for those TUs. T7-7 left 4 residual
   `'sycl/sycl.hpp' file not found` errors when invoking stock
   `clang-tidy` against a `build-sycl/` tree — Intel's `icpx`
   ships `<sycl/sycl.hpp>` under
   `/opt/intel/oneapi/compiler/latest/linux/include/sycl/`, but
   `clang-tidy` (an LLVM-stock binary) doesn't pick that path up
   automatically the way `icpx` does.

Both items are tooling cleanup — no metric-engine surface, no
bit-exactness implication. They unblock SYCL files for the CI lint gate
required by CLAUDE.md §12 r12 (touched-file lint-clean rule).

## Decision

Land two thin shims:

1. **`scripts/ci/sycl-bench-env.sh <version>`** — sources the named
   oneAPI install's `setvars.sh` and prints the fully-resolved
   `LD_LIBRARY_PATH` / `CMPLR_ROOT` / `LIBRARY_PATH` so a developer can
   run `eval $(scripts/ci/sycl-bench-env.sh 2025.3)` and immediately
   target the newer compiler runtime regardless of which version
   `/opt/intel/oneapi/` defaults to. Document the recipe in
   `docs/development/oneapi-install.md` under a new "Multi-version
   coexistence" section.
2. **`scripts/ci/clang-tidy-sycl.sh`** — thin wrapper that forwards
   args to `clang-tidy` while injecting:
   - `-extra-arg-before=-isystem<icpx-sycl-include>` so
     `<sycl/sycl.hpp>` resolves.
   - `-extra-arg-before=-D__SYCL_DEVICE_ONLY__=0` so device-only code
     paths (which clang-tidy can't analyse meaningfully without the
     SYCL device compiler) are skipped.
   - `-extra-arg-before=-Wno-unknown-warning-option` to silence the
     handful of `icpx`-specific warning-name passthroughs that
     stock `clang-tidy` doesn't recognise.
3. Wire the wrapper into the lint workflow as a new
   `clang-tidy-sycl` job that scans only files under
   `core/src/sycl/` + `core/src/feature/sycl/` +
   `core/test/test_sycl*` against a SYCL build tree. Job stays
   advisory (`continue-on-error: true`) on this initial PR; the
   tightened gate flips after a green run on master proves the
   wrapper holds across the full SYCL TU set.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Shell wrapper for clang-tidy | Trivial — 30 LOC; no new build deps; matches existing `scripts/ci/*.sh` shape | Hard-codes the icpx include path; needs an env-var override for non-default oneAPI installs | **Chosen** — pragmatic and lowest risk |
| Python wrapper around `clang-tidy` | Easier to detect oneAPI install dynamically; richer error messages | Adds a Python entry to a fast-path lint job; runtime ~50 ms vs ~5 ms; more moving parts to keep ruff/black-clean | Rejected — overkill for a path-injection shim |
| Per-file detection in the existing `clang-tidy` job | Single workflow file; no new job | The existing job's `meson setup` is CPU-only — no SYCL `compile_commands.json` entries; switching it to the SYCL build doubles install time on every PR even when no SYCL file changes | Rejected — penalises the common case |
| Full SYCL config baked into the existing job | Removes the wrapper; uses a single `.clang-tidy-sycl` config | Same install-cost problem; clang-tidy-18 doesn't honour per-TU configs cleanly when invoked via `parallel` | Rejected — same reason |
| Multi-version recipe documented but no helper script | Docs-only; smallest diff | Leaves the burden of correctly-resolving `LD_LIBRARY_PATH` / `setvars.sh` quirks on every developer; recipe is non-trivial across `bash`/`zsh`/`fish` | Rejected — the helper script is 25 LOC and removes the footgun |
| Single-version-required (drop multi-version support) | Simpler — one path, one recipe | Forces every developer onto whatever `/opt/intel/oneapi/` resolves to; A/B compiler bench impossible | Rejected — A/B is the audit driver |

## Consequences

- **Positive**: SYCL files become eligible for the changed-file
  `clang-tidy` CI gate; CLAUDE.md §12 r12 (touched-file lint-clean) now
  enforceable on `core/src/sycl/**`. The 4 residual
  `'sycl/sycl.hpp' file not found` errors from T7-7 are cleared. Local
  developers can run `vmaf_bench` against either compiler version with
  a single `eval`.
- **Negative**: Two new CI shims to maintain. Wrapper hard-codes
  `/opt/intel/oneapi/compiler/latest/linux/include/sycl/`; if Intel
  reorganises the directory layout (last move was 2025.0 →
  `compiler/latest/linux/`) the wrapper needs updating. `ICPX_ROOT`
  env-var override absorbs the common deviations.
- **Neutral / follow-ups**: The `clang-tidy-sycl` job stays advisory
  (`continue-on-error: true`) on this PR. Once one green run lands on
  master across all current SYCL TUs the gate flips to required. Future
  oneAPI bumps (e.g. 2026.0 when it ships) test the wrapper as part of
  the bump validation; that audit checklist is already in
  `docs/development/oneapi-install.md` §"Post-bump audit checklist".

## References

- BACKLOG row T7-13 — "SYCL toolchain cleanup" (2026-04-28 audit).
- [ADR-0127](0127-vif-as-sycl-pathfinder.md) — SYCL backend; `icpx` selection.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file lint-clean rule.
- T7-7 (cleared SYCL clang-tidy findings); T7-8 (oneAPI 2025.0.4 → 2025.3.1 bump).
- `docs/development/oneapi-install.md` §"Verify SYCL clang-tidy still works" — the 4 residual errors closed by this ADR.
- Source: `req` (paraphrased: BACKLOG T7-13 bundles the bench recipe + clang-tidy wrapper as the next SYCL toolchain follow-up).

### Status update 2026-05-08: AdaptiveCpp added as a second toolchain

Per the ADR-0028 maintenance rule (status updates appended, body
frozen). The icpx-only SYCL toolchain assumption embedded in this
ADR's `## Decision` no longer holds:
[ADR-0335](0335-adaptivecpp-second-sycl-toolchain.md) adds
AdaptiveCpp (`acpp` / `syclcc`) as a supported alternative
toolchain. The `clang-tidy-sycl.sh` wrapper described above remains
icpx-coupled by design — it injects the icpx `<sycl/sycl.hpp>`
include path and Intel-specific suppressions; AdaptiveCpp builds do
not need that wrapper because `<sycl/sycl.hpp>` resolves naturally
under acpp's include layout. A separate AdaptiveCpp-specific lint
lane is not in scope for ADR-0335 and remains a follow-up item.
icpx stays the **primary** toolchain; the multi-version recipe and
clang-tidy wrapper described here continue to apply unchanged for
icpx builds.
