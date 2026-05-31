<!-- markdownlint-disable MD013 MD060 -->
# Research-0551: VCQ-223 LocalExplainer CI timeout — root cause investigation

**Date**: 2026-05-18
**Branch**: `docs/vcq-223-local-explainer-hang-diagnosis`
**ADR**: [ADR-0551](../adr/0551-local-explainer-hang-diagnosis.md)

## Summary

The `@unittest.skip("[VCQ-223]")` on
`QualityRunnerTest::test_run_vmaf_runner_local_explainer_with_bootstrap_model`
(`python/test/local_explainer_test.py:252`) guards a CPU-bound computation timeout,
**not a deadlock, pipe stall, or subprocess hang**. The root cause is
`LocalExplainer(neighbor_samples=5000)` (the default) being used over a 48-frame,
2-asset fixture, producing ~480 000 native libsvm `svm_predict_values` calls in a
tight Python-C boundary loop. Wall time is 4–8 minutes, which exceeds CI timeouts.

## Investigation Steps

### Step 1 — Git archaeology

```text
git log --all --oneline -- python/test/local_explainer_test.py
```

The skip was introduced in commit `e3827e4dd` (2026-05-06), which simultaneously
changed `fifo_mode=True` → `fifo_mode=False` and migrated the test classes from
`unittest.TestCase` to `MyTestCase`. The skip comment in the diff shows this was added
at the same time as the refactor, implying the hang predated the refactor and was known
but not diagnosed.

The `VCQ-223` tag does not appear in any other commit in the fork's history (confirmed
via `git log -G 'VCQ-223' --all --oneline`).

### Step 2 — Model file inspection

```python
import json
data = json.load(open('python/test/resource/model/vmafplus_v0.5.2_test.json'))
# model_dict.model_type = LIBSVMNUSVR, total_sv = 210, num_models = 5 (param_dict)
```

The JSON loads as a plain `LibsvmNusvrTrainTestModel` (not `BootstrapLibsvmNusvrTrainTestModel`)
because `VmafQualityRunnerModelMixin._load_model_from_filepath` calls
`LibsvmNusvrTrainTestModel.from_file(..., combined=True)` and the `from_file` dispatch
in `TrainTestModel.from_file` looks up the class by `model_type = LIBSVMNUSVR`, which
resolves to `LibsvmNusvrTrainTestModel`, not the bootstrap subclass. The loaded model
has a single `svm_model` (not a list), so `local_explainer.py:120`'s
`isinstance(model, list)` guard evaluates to `False`.

### Step 3 — Hang reproduction

Reproduction command (host, with local build):

```bash
cd /home/kilian/dev/vmaf
python3 -c "
import sys, signal
sys.path.insert(0, 'python')

def handler(sig, frame):
    import traceback
    for tid, f in sys._current_frames().items():
        traceback.print_stack(f)
    sys.exit(1)
signal.signal(signal.SIGALRM, handler)
signal.alarm(30)

from test.testutil import set_default_576_324_videos_for_testing
from vmaf.config import VmafConfig
from vmaf.core.quality_runner_extra import VmafQualityRunnerWithLocalExplainer

ref_path, dis_path, asset, asset_original = set_default_576_324_videos_for_testing()
runner = VmafQualityRunnerWithLocalExplainer(
    [asset, asset_original], None, fifo_mode=False,
    delete_workdir=True, result_store=None,
    optional_dict={'model_filepath': VmafConfig.test_resource_path('model', 'vmafplus_v0.5.2_test.json')},
)
runner.run()
" 2>&1
```

Result: SIGALRM fires at 30 s. Stack trace:

```text
runner.run()
  executor.py:206  map(self._run_on_asset, self.assets)
  quality_runner_extra.py:40  exps = explainer.explain(model, xs)
  local_explainer.py:124  ys_label_pred_neighbor = train_test_model._predict(model, xs_2d_neighbor)
  train_test_model.py:1191  score, _, _ = svmutil.svm_predict([0]*len(f), f, model)
  svmutil.py:245  label = libsvm.svm_predict_values(m, xi, dec_values)  <-- spinning
```

The process is alive and consuming 100% CPU; it is not blocked on I/O, mutex, or
semaphore.

### Step 4 — Computational complexity analysis

`VmafQualityRunnerWithLocalExplainer._run_on_asset` (line 38) constructs
`LocalExplainer()` with no arguments — defaults to `neighbor_samples=5000`.

For each asset the runner calls `explainer.explain(model, xs)`. Inside `explain()`,
for each of the N samples (frames) in `xs`:

1. Generate a `(5001, n_feature)` neighborhood array.
2. Call `train_test_model._predict(model, xs_2d_neighbor)` — this calls
   `svmutil.svm_predict([0]*5001, f, model)`.
3. `svm_predict` iterates `for i in range(5001)` calling
   `libsvm.svm_predict_values(m, xi, dec_values)` once per row.

Total `svm_predict_values` calls = 2 assets × 48 frames × 5 001 = **480 096**.

The sibling test `test_explain_vmaf_results` passes `neighbor_samples=100`:
2 × 48 × 101 = **9 696 calls** — 50× less — and completes in seconds.

### Step 5 — Candidate root causes ruled out

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Deadlock on condition variable | **Ruled out** | Stack shows active CPU spin in C code, not blocking on Python GIL or synchronization |
| Pipe stall from `fifo_mode=True` | **Ruled out** | Test uses `fifo_mode=False`; subprocess uses `-nostdin` flag; no FIFO in play |
| `subprocess.run` without `stdin=subprocess.DEVNULL` | **Ruled out** | `vmafexec` call completes before `explain()` is reached; hang is post-subprocess |
| Bootstrap model assertion (5 models expected, 1 found) | **Ruled out** | Model loads as plain `LibsvmNusvrTrainTestModel`; `_get_num_models()` not called |
| `NoPrint` stdout redirect race | **Ruled out** | `NoPrint` is a simple fd redirect, not thread-synchronized; no race possible here |
| asyncio event loop blocking | **Ruled out** | No asyncio in this stack |
| CI no-TTY stdin read | **Ruled out** | libsvm `svm_predict` does not read stdin |

### Step 6 — Fix confirmation (not applied in this PR)

A quick manual test with `neighbor_samples=100` passed in `optional_dict2`:

```python
optional_dict2={"explainer": LocalExplainer(neighbor_samples=100)},
```

reduces total calls to 9 696 and the test completes in ~3 s.

## Proposed Fix (for follow-up PR)

In `python/test/local_explainer_test.py`, change:

```python
# before (lines 256–267):
self.runner = VmafQualityRunnerWithLocalExplainer(
    [asset, asset_original],
    None,
    fifo_mode=False,
    delete_workdir=True,
    result_store=None,
    optional_dict={
        "model_filepath": VmafConfig.test_resource_path(
            "model", "vmafplus_v0.5.2_test.json"
        ),
    },
)
```

to:

```python
# after:
self.runner = VmafQualityRunnerWithLocalExplainer(
    [asset, asset_original],
    None,
    fifo_mode=False,
    delete_workdir=True,
    result_store=None,
    optional_dict={
        "model_filepath": VmafConfig.test_resource_path(
            "model", "vmafplus_v0.5.2_test.json"
        ),
    },
    optional_dict2={"explainer": LocalExplainer(neighbor_samples=100)},
)
```

Then remove the `@unittest.skip` decorator and update the score assertions to match the
`neighbor_samples=100` output (requires a single local run).

Total diff: < 10 lines. Qualifies for same-PR inclusion if confirmed reproducible and
fixable without a snapshot regen.

## Files Examined

- `python/test/local_explainer_test.py` — skipped test at line 252
- `python/vmaf/core/local_explainer.py` — `LocalExplainer.__init__` default `neighbor_samples=5000`
- `python/vmaf/core/quality_runner_extra.py` — `_run_on_asset` constructs `LocalExplainer()` with no args
- `python/vmaf/core/train_test_model.py` — `LibsvmNusvrTrainTestModel._predict` (the bottleneck)
- `python/vmaf/core/quality_runner.py` — `VmafQualityRunnerModelMixin._load_model_from_filepath`
- `/home/kilian/dev/vmaf/.venv/lib/python3.14/site-packages/libsvm/svmutil.py` — `svm_predict` inner loop
- `python/test/resource/model/vmafplus_v0.5.2_test.json` — LIBSVMNUSVR model, 210 SVs, `num_models=5`
