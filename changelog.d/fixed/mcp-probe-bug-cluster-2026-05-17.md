- MCP server probe bug-fix cluster (5 defects found by the
  `vmaf-dev-mcp-stdio` probe on 2026-05-17, ADR-0495):
  - `vmaf_score` now probes the local `vmaf` binary's `--help` output
    and **refuses** any caller-requested backend that is not
    advertised, instead of silently falling back to CPU. The
    response gains `backend_requested` and `backend_used` fields so
    downstream cross-backend parity tests can assert against the
    binary's actual selection.
  - Tool-schema `backend` enum extended to `["auto", "cpu", "cuda",
    "sycl", "vulkan", "hip", "metal"]` on both `vmaf_score` and
    `describe_worst_frames`. The `_BACKEND_DISABLE` map already
    handled all seven backends; only the JSON-schema surface was
    drifted.
  - `run_benchmark` now detects the "non-zero exit + empty stdout +
    empty stderr" pattern (typically `set -euo pipefail` silent-abort)
    and populates a meaningful `error` field with the most common
    root-cause shortlist and a `bash -x` re-run hint. The
    `testdata/bench_all.sh` script itself now exits `2` with a clear
    stderr message when the vmaf binary is missing, eliminating the
    most common silent-abort path.
  - `vmaf_score` now emits a `mismatched_model_warning` field when
    the model's intended resolution preset (`hd` / `4k`) disagrees
    with the source frame size's bucket. Catches the foot-gun where
    `version=vmaf_4k_v0.6.1` on 576×324 input silently saturates at
    100 on every frame.
  - Documented Bug #4 (`vmaf_v0.6.1` ref==dis ≠ 100) as a model
    artefact rather than a code defect — the Netflix golden gate
    (ADR-0024 / CLAUDE.md §8) forbids modifying the model
    coefficients; users who need a 100-on-identical-pair pool
    should pick `vmaf_v0.6.1neg` (which clips).
