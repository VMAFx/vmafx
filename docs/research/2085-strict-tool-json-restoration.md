# Research-2085: Strict tool JSON restoration

## Finding

BUG048 item A9 names two strict-JSON boundaries introduced by commit
`8c00f897c17e43c55ee489d9350dee4afb52253c` and removed 44 minutes later by
the unrelated repository-layout squash
`384d97d037f1537c934ff67c51ff07e19c3bda6b`:

1. `tools/external-bench/compare.py --out-json` converted non-finite aggregate
   floats to JSON `null` and encoded with `allow_nan=False`.
2. `vmaf-roi-score` converted a non-finite pooled score into a diagnostic and
   exit status 65 without writing a result file.

The clobber deleted both implementations and both regression tests. It left
the user guide, changelog fragment, and Research-0722 in place. Exact base
`4e6916d16ac57647105d14a47a6680117d6b5738` therefore documents strict output
while external-bench emits bare `NaN` and the ROI CLI raises an uncaught
`ValueError`.

This is a bug restoration, not a new architecture or policy decision. No new
ADR is needed; the public output contract and exit code were already selected,
documented, and shipped.

## Current-tree behavior

The current external-bench schema remains a flat `CompetitorAggregate` row.
An unavailable optional wrapper intentionally yields no summaries, so the
in-memory means and human-readable table use `nan`. Only the durable JSON
boundary should translate missing data to `null`. The restoration sanitizes
every float in that flat row, including future non-finite runtime or GFLOPs
values, then uses `allow_nan=False` as a fail-closed backstop.

The current ROI implementation is narrower than the May version:
`blend_scores()` already rejects non-finite inputs, while `main()` no longer
maps that validation failure to the CLI contract. Catching only its
`ValueError` preserves every subprocess exit status and mask error while
restoring status 65. `_emit()` also uses `allow_nan=False` so a future payload
field cannot silently bypass the finite-score guard.

## Alternatives considered

| Option | Result | Decision |
| --- | --- | --- |
| Leave Python's default JSON encoder | Bare `NaN`/`Infinity` remains non-standard and breaks strict consumers | Rejected |
| Reject an external-bench run when an optional wrapper is unavailable | Removes missing rows and changes the existing partial-report contract | Rejected |
| Import `vmaftune.jsonio` into both tools | Couples two independent entry points to another package not declared as their dependency | Rejected |
| Recursively sanitize arbitrary values as the original helper did | Restores behavior but violates the current no-recursion standard for a flat schema | Rejected |
| Sanitize the flat aggregate wire row and retain the ROI finite check | Strict output, unchanged schemas and in-memory math, smallest current-tree fix | Chosen |

## Red and green evidence

Tests were added before production changes. On the exact base:

```text
python3 -m pytest -q \
  tools/external-bench/tests/test_compare.py::test_main_out_json_is_strict_when_all_wrapper_rows_fail \
  tools/vmaf-roi-score/tests/test_combine.py::test_cli_rejects_nonfinite_vmaf_score
FFFF
```

The external report contained `"plcc_mean": NaN`; ROI raised from
`blend_scores()` for `NaN`, `+Inf`, and `-Inf`. After the restoration, the same
four cases pass. A direct serializer test additionally pins all six float
fields against all three non-finite classes.

Full package verification is hermetic: external binaries and the `vmaf`
process are stubbed by the suites. No Netflix golden assertion, score formula,
public libvmaf/FFmpeg surface, or JSON schema field changes.

## Related

- [Research-0722](0722-tool-report-strict-json.md) — original strict-output
  decision and validation.
- [ADR-1284](../adr/1284-silent-revert-detection-gate.md) — prevention for
  future stale-content clobbers; it cannot repair losses already on master.
