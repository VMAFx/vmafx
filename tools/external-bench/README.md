<!-- markdownlint-disable MD060 -->
# `tools/external-bench/` — external-competitor benchmark harness

Side-by-side numerical comparison between the fork's perceptual-quality
predictors and two external open-source competitors:

| Competitor | Surface | Upstream | Upstream licence |
|---|---|---|---|
| `fork-fr-regressor` | `fr_regressor_v2_ensemble` (full-reference) | this repo | BSD-3-Clause-Plus-Patent |
| `fork-nr-metric`    | `nr_metric_v1` (no-reference)               | this repo | BSD-3-Clause-Plus-Patent |
| `x264-pvmaf`        | Synamedia/Quortex predicted-VMAF           | [quortex/x264-pVMAF](https://github.com/quortex/x264-pVMAF) (Nov 2024) | **GPL-2.0** |
| `dover-mobile`      | DOVER-Mobile no-reference quality predictor | [DOVER](https://github.com/QualityAssessment/DOVER) | Apache-2.0 (code) / CC-BY-NC-SA 4.0 (weights) |

## Licence boundary — wrapper-only architecture

The fork is BSD-3-Clause-Plus-Patent. **`x264-pVMAF` is GPL-2.0**, which is
incompatible with redistribution alongside permissive-licensed code.
[ADR-0332](../../docs/adr/0332-external-bench-wrapper-only.md) records the
mitigation: each external competitor lives in its own
`tools/external-bench/<competitor>/run.sh` that:

- invokes a **user-installed** external binary (path provided via env var);
- reads the binary's output, **re-shapes** it into the harness JSON schema;
- writes the normalised `output.json` to the path given by `--out`.

No GPL'd code is vendored, linked, or copied into this fork. Side-by-side
benchmarking is fine because the fork only invokes the binary as a
subprocess and reads its (factual) numerical output. Per
[ADR-0027 / ADR-0024](../../docs/adr/) the same boundary applies to any
future GPL'd competitor we want to compare against.

If you redistribute a built artefact that bundles the GPL'd binary, **that
is a separate downstream redistribution** and the GPL applies to it; the
fork itself never produces such an artefact.

## Schema

Every wrapper emits the same JSON shape so `compare.py` can aggregate
across them:

```json
{
  "frames": [
    {"frame_idx": 0, "predicted_vmaf_or_mos": 81.2, "runtime_ms": 1.4},
    {"frame_idx": 1, "predicted_vmaf_or_mos": 80.7, "runtime_ms": 1.4}
  ],
  "summary": {
    "competitor": "x264-pvmaf",
    "plcc": 0.91, "srocc": 0.89, "rmse": 3.4,
    "runtime_total_ms": 200.0,
    "params": 5000, "gflops": 2.0
  }
}
```

`predicted_vmaf_or_mos` covers both worlds: full-reference predictors
emit a VMAF-like score (0–100), no-reference predictors emit a MOS
score on whatever scale the model was trained on. The aggregation in
`compare.py` reports PLCC / SROCC / RMSE per competitor — units in the
score field do not need to match across competitors.

`summary.competitor` must exactly match the registry key passed through
`--competitors` (`fork-fr-regressor`, `fork-nr-metric`, `x264-pvmaf`, or
`dover-mobile`). The model/version detail belongs in sidecar metadata or
wrapper-local logs; changing this field to a descriptive label makes the
orchestrator reject the payload as an invalid wrapper schema.

`compare.py` validates every wrapper payload before aggregation. Missing
required keys, a `summary.competitor` that does not match the wrapper
name, non-object frames, and non-numeric metric fields are reported as
`wrapper <name> produced invalid schema: ...`; malformed JSON is
reported as `wrapper <name> produced invalid JSON: ...`. A bad wrapper
is skipped for that corpus item instead of crashing the whole run with
an aggregation-time `KeyError`.

## Operator install (external competitors only)

The fork ships **only** the wrappers. You install the external binaries
yourself and point the env vars at them.

### `x264-pVMAF` (GPL-2.0)

```bash
git clone https://github.com/quortex/x264-pVMAF.git ~/external/x264-pVMAF
cd ~/external/x264-pVMAF && make
export EXTERNAL_BENCH_X264_PVMAF=~/external/x264-pVMAF/x264-pvmaf
```

### `dover-mobile`

```bash
pipx install dover-mobile
export EXTERNAL_BENCH_DOVER_MOBILE="$(which dover-mobile)"
```

### Fork-side wrappers

These call the in-tree `vmaf-tune` CLI with the canonical model path
under `model/tiny/`. No env var is required when `vmaf-tune` is on
`PATH`; override with `EXTERNAL_BENCH_VMAF_TUNE=/absolute/path/vmaf-tune`.

## Corpus

The orchestrator defaults to combining a BVI-DVC test fold with the
Netflix Public Drop. Both are local-only (see [ADR-0310](../../docs/adr/0310-bvi-dvc-corpus-ingestion.md));
the fork ships neither corpus.

| Corpus | Default expected path | Override flag |
|---|---|---|
| BVI-DVC test fold | `~/.workingdir2/bvi-dvc/test/` (containing `<src>__ref.yuv` + `<src>__dis*.yuv`, geometry encoded as `..._WxH_...` in the stem) | `--bvi-dvc-root <DIR>` |
| Netflix Public Drop | `<repo>/.workingdir2/netflix/<src>/{ref,dis}/*.yuv` (per the local layout convention from ADR-0310 / `docs/state.md`) | `--netflix-public-root <DIR>` |

If neither corpus root exists at runtime, `compare.py` exits with code
4 and a message naming both expected paths so the operator can fix
the layout or pass the override flags.

## Running a comparison

```bash
# Smoke run against a small subset
python3 tools/external-bench/compare.py \
    --bvi-dvc-root ~/.workingdir2/bvi-dvc \
    --netflix-public-root .workingdir2/netflix \
    --limit 4 \
    --out-json /tmp/bench.json

# Full run, all four competitors
python3 tools/external-bench/compare.py --out-json /tmp/bench.json
```

Sample output:

```text
competitor          n   PLCC    SROCC   RMSE   runtime_ms  params  GFLOPs
------------------  --  ------  ------  -----  ----------  ------  ------
fork-fr-regressor   12  0.9912  0.9876  1.21       128     12345    0.05
fork-nr-metric      12  0.9650  0.9540  3.04        85      5678    0.03
x264-pvmaf          12  0.9120  0.8920  3.41      2000      5000    2.00
dover-mobile        12  0.9420  0.9210  2.85      4500     45000    8.00
```

Subset to a single comparison via `--competitors`:

```bash
python3 tools/external-bench/compare.py --competitors fork-nr-metric dover-mobile
```

## Tests

```bash
python3 -m pytest tools/external-bench/tests/ -q
```

The tests stub `subprocess.run` so they never depend on `x264-pVMAF`
or `dover-mobile` being installed.

## Reproducer (CI smoke)

```bash
python3 -m pytest tools/external-bench/tests/ -q
```

Expected: `7 passed`.
