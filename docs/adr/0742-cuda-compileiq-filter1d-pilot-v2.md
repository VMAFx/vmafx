# ADR-0742: Abandon CompileIQ Kernel Auto-Tuning Pilot — Tool Not Available on PyPI

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: cuda, performance, tooling, kernel-tuning, fork-local

## Context

The fork has an ongoing interest in automated CUDA kernel parameter search for
`core/src/feature/cuda/integer_vif/filter1d.cu` — the hottest kernel in the
integer VIF extractor. A pilot using a tool called "CompileIQ" was proposed and
attempted twice.

Pilot v1 (PR #66) was abandoned because the `vmaf-dev-mcp` container at the
time ran Python 3.14, and the pilot brief assumed CompileIQ required Python
`<3.14`.

Pilot v2 (this ADR) was initiated with the new `vmaf-dev-mcp:cuda13.3` image
(Ubuntu 26.04 LTS, CUDA 13.3) and a plan to install Python 3.13 via a sub-venv
to work around the version constraint.

Investigation during v2 revealed that the `compileiq` package on PyPI
(`pypi.org/project/compileiq`) is version `0.0.0a0` — an empty placeholder
with no module contents, no CLI entry point, and no functional code. The
package description reads `"Empty package compileiq."` The Python version issue
from v1 was a false lead; the actual blocker is that the tool does not exist as
a published package on PyPI.

## Decision

Abandon the CompileIQ auto-tuning pilots. Do not pursue further attempts until
evidence of a functional, published CompileIQ release is available from a
primary source (the tool's authors or a peer-reviewed reference). Use
`/profile-hotpath` with `ncu` for immediate profiling needs; design a grid
search wrapper using `subprocess.run` + meson reconfigure if systematic
block-size tuning is required.

## Alternatives Considered

| Option | Pros | Cons | Why Not Chosen |
|---|---|---|---|
| Retry with Python 3.13 via deadsnakes PPA | Python 3.13 is available for Ubuntu 26.04 via PPA | Still installs the same empty `compileiq 0.0.0a0` package — Python version is irrelevant | Does not fix the root cause |
| Use KTT (Kernel Tuning Toolkit) | Active open-source C++ auto-tuner; handles CUDA block sizes + register pressure + unroll | Requires adding a C++ dependency and writing a KTT host harness for filter1d kernels; ~3–5 days effort | Viable future option; not the immediate priority |
| Grid search via `subprocess.run` + meson reconfigure | Zero new dependencies; fully fork-controlled | Manual implementation required; ~1–2 days | Best next option if systematic tuning is desired |
| `/profile-hotpath` + `ncu` | Available today; identifies register pressure / occupancy / bank conflicts without recompiling | Not an auto-tuner; requires human interpretation | Recommended next step for `filter1d.cu` |
| Accept current hand-tuned parameters | No effort required | May leave performance on the table | Acceptable for now; filter1d already has smem tiling (win #1 + #4) |

## Consequences

- **Positive**: No dead code, no ACF files, no conditional build plumbing
  committed for a non-functional tool. Pilot cost capped at investigation time.
- **Negative**: Auto-tuning of `filter1d.cu` block parameters remains a manual
  or future task.
- **Neutral / Follow-ups**:
  - `/profile-hotpath cuda vif` is the recommended next action for
    `filter1d.cu`.
  - If CompileIQ is ever published as a functional package, the pilot can be
    reopened; the objective function design from the brief is still valid.
  - PR #66 is closed by the DRAFT PR that accompanies this ADR.

## References

- `req`: "CompileIQ pilot v2 on `core/src/feature/cuda/integer_vif/filter1d.cu`"
  (user request, 2026-05-28 session).
- Research digest: [docs/research/research-0742-cuda-compileiq-filter1d-pilot-v2.md](../research/research-0742-cuda-compileiq-filter1d-pilot-v2.md)
- PR #66 (pilot v1, abandoned)
- `pypi.org/project/compileiq` — inspected 2026-05-28; v0.0.0a0, empty package
