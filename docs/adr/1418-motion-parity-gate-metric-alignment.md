<!-- markdownlint-disable MD013 MD060 -->
# ADR-1418: Motion parity cells compare what every twin emits; a missing metric is a cell error

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: ci, parity-gate, motion, sycl, cuda, rc3

## Context

A full-matrix run of the cross-backend parity gate
(`scripts/ci/cross_backend_parity_gate.py`, and the single-pair
`scripts/ci/cross_backend_vif_diff.py`) stopped at the `motion` cell with
`KeyError: 'integer_motion'`
(`T-CI-PARITY-GATE-MOTION-DEBUG-DEFAULT-2026-09-29`). Two defects met there:

1. **`motion_sycl` declared `debug` with default `true`.** The CPU extractor
   (`core/src/feature/integer_motion.c`), `motion_cuda` and `motion_hip` all
   default it to `false`. A default SYCL run therefore emitted `integer_motion`
   (the legacy unfixed score) next to `integer_motion2` and `integer_motion3`,
   and a default CPU run did not.
2. **The gate indexed every expected metric without checking it exists.** A
   metric that one run does not carry raised `KeyError` in `diff_frames()` and
   ended the whole matrix instead of failing one cell.

## Decision

1. `motion_sycl` defaults `debug` to `false`, like the CPU, CUDA and HIP
   extractors. A default run of any `motion` twin emits `integer_motion2` and
   `integer_motion3`. `core/test/test_sycl_twin_option_parity.c` compares the
   twin's declaration with the CPU's.
2. The gate has two motion cells. `motion` compares `integer_motion2` and
   `integer_motion3` on default runs. `motion_debug` is an alias for
   `motion` with `debug=true` on both sides and compares `integer_motion`,
   `integer_motion2` and `integer_motion3`, at the `motion` tolerance.
3. A metric that either run lacks on any frame makes the cell `ERROR`, with a
   note naming the backend and the metrics, and the matrix continues with the
   next cell. `cross_backend_vif_diff.py` prints the same finding and exits 1.
   The gate never compares a subset of a cell's metrics.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Compare the metrics both runs emit and record the rest in a note, cell stays `OK` | A matrix run always completes; no cell fails over an output-set difference | A twin that stops emitting a metric passes the gate; the report shows a maximum difference of 0 for a metric nobody compared | The gate exists to catch a twin that differs from the CPU; a dropped output is such a difference |
| Leave `motion_sycl` at `debug=true` and run the CPU side with `debug=true` in the `motion` cell | No change to the SYCL extractor's default output | The twin keeps a default that differs from the CPU, CUDA and HIP; the default CPU path is never compared | Option defaults of a twin follow the CPU extractor |
| Fix only the SYCL default | Smallest change | The next output-set difference ends a matrix run with a traceback again | One cell's failure must not hide the cells after it |
| **SYCL default follows the CPU, `motion_debug` cell, missing metric is a cell `ERROR` (chosen)** | Both motion paths are compared on every backend; a dropped metric fails its cell and names the backend; the matrix always completes | A default `motion_sycl` run no longer writes `integer_motion`; callers that read it pass `debug=true` | Chosen |

## Consequences

- A default `--feature motion_sycl` run no longer emits
  `VMAF_integer_feature_motion_score` (`integer_motion`). Pass `debug=true` to
  get it, as on every other backend. The VMAF models read `integer_motion2`
  and are unaffected.
- `--features motion` and `--features motion_debug` are both valid gate cells
  for CPU, CUDA, SYCL and HIP.
- A cell whose runs emit different metric sets reports `ERROR` and fails the
  gate; the remaining cells still run.

## References

- [ADR-0214](0214-gpu-parity-ci-gate.md): the parity gate and its tolerance tables.
- [ADR-1183](1183-model-options-gate-gpu-twin-selection.md): option declarations decide whether a twin may replace the CPU extractor.
- [Research-2125](../research/2125-windows-native-sycl-run.md): the native Windows SYCL run that hit the `KeyError`.
- State row `T-CI-PARITY-GATE-MOTION-DEBUG-DEFAULT-2026-09-29`; PR #1681.
- Source: `req` (paraphrased: correctness comes before speed; a wrong result must not pass).
