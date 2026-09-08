# Research-1238: Predictor card G304 and missing Go merge gate

## Reproduction and evidence

At master `7bafbb8cc`, and PR heads `2422c6089` (#1394), `51fb602d3`
(#1396), and `e9a853a24` (#1413), the Go job completed the native build,
`go vet`, runner build, and real ONNX Runtime smoke. Each then failed
`gosec (exclude generated)` with one G304 finding at
`pkg/predictor/ortsession.go:69`, `os.ReadFile(cardPath)`. The later envtest
setup and `go test` steps were skipped.

| Revision | Job evidence |
| --- | --- |
| master `7bafbb8cc` | [102111920650](https://github.com/VMAFx/vmafx/actions/runs/34241320185/job/102111920650) |
| PR #1394 | [102112109677](https://github.com/VMAFx/vmafx/actions/runs/34241376759/job/102112109677) |
| PR #1396 | [102112067520](https://github.com/VMAFx/vmafx/actions/runs/34241363816/job/102112067520) |
| PR #1413 | [102112091359](https://github.com/VMAFx/vmafx/actions/runs/34241371215/job/102112091359) |

The read was introduced by `ffe453887` (#1415). Its
[Go job failed](https://github.com/VMAFx/vmafx/actions/runs/34219892071/job/102040270094)
at 11:22:40Z on 2026-09-08 while its
[aggregator passed](https://github.com/VMAFx/vmafx/actions/runs/34219892163/job/102040269789)
at 11:39:19Z. The Go check name was absent from the required array.

The CUDA-provider loader error in the smoke stderr is unrelated: inference
falls back to CPU and returns the exact expected `[66.13961791992188]`.

## Repair and checks

Go 1.27.1's installed `go doc os.Root` confirms that directory-rooted reads
reject symlinks escaping the root. Gosec v2.29.0's installed G304 rule
recommends this API; merely cleaning a string or suppressing G304 would
not enforce the desired sibling-card boundary.

The focused validation commands are:

```bash
go test ./pkg/predictor
go vet ./pkg/predictor
gosec -exclude-generated -quiet ./pkg/predictor
python3 scripts/ci/test_go_workflow_contract.py
python3 -m unittest scripts/ci/tests/test_ci_impact.py
scripts/ci/check-aggregator-names.sh
```

The predictor tests exercise actual temporary cards, classification
precedence, missing cards/directories, and inside/outside symlinks. The
workflow test runs the embedded aggregator JavaScript with a Go failure
among otherwise successful checks. Planner fixtures establish that docs
remain cheap while Go, native, model, and release inputs run Go validation.
Hosted completion remains a separate gate after the branch is published.
