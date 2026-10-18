# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""End-to-end tests of the mini retrain (``ai/scripts/mini_retrain.py``).

These run the real extractor, trainers, exporters, validators and gate on the
generated fixture corpus, so they need the ``vmaf`` binary of the build under
test and the training stack (torch, onnxruntime, jsonschema). They are not in
``ai/tests`` because one full run takes about half a minute and that suite
runs under a 60 s per-test limit; the Tiny AI job and the nightly ``mini-retrain`` workflow run them.

A missing binary or a missing library is a failure, never a skip: the point of
the job is that the pipeline ran.
"""

from __future__ import annotations

import importlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any, TypeVar, cast

import pytest
from scripts.lib import vmaftest
from scripts.lib.safe_subprocess import signal_process_group

REPO = Path(__file__).resolve().parents[2]
DRIVER = REPO / "ai" / "scripts" / "mini_retrain.py"
sys.path.insert(0, str(REPO / "ai" / "scripts"))

from aiutils.pipeline import Stage, StageError, StageResult, run_pipeline  # noqa: E402

mini_retrain: Any = importlib.import_module("mini_retrain")
pd: Any = importlib.import_module("pandas")
F = TypeVar("F", bound=Callable[..., Any])


def _parametrize(*args: Any, **kwargs: Any) -> Callable[[F], F]:
    return cast(Callable[[F], F], pytest.mark.parametrize(*args, **kwargs))


def _module_fixture(func: F) -> F:
    return cast(F, pytest.fixture(scope="module")(func))


def _vmaf_bin() -> Path:
    found = vmaftest.find()
    if found is None:
        raise AssertionError(vmaftest.MISSING_MESSAGE)
    return Path(str(found))


def _run_driver(
    run_dir: Path, *, extra_env: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, **(extra_env or {})}
    return subprocess.run(
        [
            sys.executable,
            str(DRIVER),
            "run",
            "--run-dir",
            str(run_dir),
            "--vmaf-bin",
            str(_vmaf_bin()),
        ],
        capture_output=True,
        text=True,
        env=env,
        timeout=900,
        check=False,
    )


def _stages(run_dir: Path) -> list[Stage]:
    return cast(
        list[Stage],
        mini_retrain.build_stages(run_dir, vmaf_bin=_vmaf_bin(), profile="mini", seed=0),
    )


def _pipeline(run_dir: Path, **kw: Any) -> list[StageResult]:
    return run_pipeline(
        _stages(run_dir),
        run_dir,
        repo_root=REPO,
        lock_files=mini_retrain.LOCKS,
        log=lambda _m: None,
        **kw,
    )


@_module_fixture
def first_run(tmp_path_factory: pytest.TempPathFactory) -> Path:
    run_dir = Path(str(tmp_path_factory.mktemp("mini"))) / "run"
    done = _run_driver(run_dir)
    assert done.returncode == 0, done.stdout + done.stderr
    return run_dir


def _manifest(run_dir: Path, stage: str) -> dict[str, Any]:
    return cast(
        dict[str, Any], json.loads((run_dir / "stages" / f"{stage}.stage.json").read_text())
    )


def test_every_stage_completes_and_writes_a_manifest(first_run: Path) -> None:
    names = [s.name for s in _stages(first_run)]
    for name in names:
        m = _manifest(first_run, name)
        assert m["status"] == "complete", name
        assert m["environment"]["python"] and m["environment"]["git_revision"], name
        assert m["environment"]["locks"], name


def test_ends_in_registry_entries_and_a_passing_gate_report(first_run: Path) -> None:
    gate = json.loads((first_run / "reports" / "gate_report.json").read_text())
    assert gate["passed"] is True and gate["teacher_model"] == "vmaf_v1.0.16_3d0h"
    assert [m["model"] for m in gate["models"]] == [
        "vmaf_tiny_v2",
        "vmaf_tiny_v3",
        "vmaf_tiny_v4",
        "fr_regressor_v1",
    ]
    for m in gate["models"]:
        assert (
            m["verdict"] == "pass"
            and m["plcc"] is not None
            and m["rows"] == mini_retrain.EXPECT_ROWS
        )
    registry = json.loads((first_run / "models" / "registry.json").read_text())
    assert [m["id"] for m in registry["models"]] == ["fr_regressor_v1"]


def test_second_run_resumes_every_stage_without_redoing_work(first_run: Path) -> None:
    before = {s.name: _manifest(first_run, s.name)["finished"] for s in _stages(first_run)}
    results = _pipeline(first_run)
    assert {r.status for r in results} == {"resumed"}
    after = {s.name: _manifest(first_run, s.name)["finished"] for s in _stages(first_run)}
    assert before == after


def test_two_fresh_runs_give_identical_stable_outputs(first_run: Path, tmp_path: Path) -> None:
    other = tmp_path / "run"
    done = _run_driver(other)
    assert done.returncode == 0, done.stdout + done.stderr
    compared = 0
    for stage in _stages(first_run):
        a, b = _manifest(first_run, stage.name), _manifest(other, stage.name)
        assert a["stable"].keys() == b["stable"].keys(), stage.name
        for rel, digest in a["stable"].items():
            assert b["stable"][rel] == digest, f"{stage.name}: {rel} differs between two runs"
            compared += 1
    assert compared >= 14  # corpus, features, 3 checkpoints, 3 models, registry, gate, ...


def test_a_killed_run_resumes_from_its_manifests(tmp_path: Path) -> None:
    run_dir = tmp_path / "run"
    proc = subprocess.Popen(
        [
            sys.executable,
            str(DRIVER),
            "run",
            "--run-dir",
            str(run_dir),
            "--vmaf-bin",
            str(_vmaf_bin()),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    target = run_dir / "stages" / "train_tiny_v2.stage.json"
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline and not target.exists():
        time.sleep(0.05)
    assert target.exists(), "driver never reached train_tiny_v2"
    signal_process_group(proc.pid, signal.SIGKILL)  # driver and the stage it is running
    proc.wait()
    extract_done = _manifest(run_dir, "extract")["finished"]
    done = _run_driver(run_dir)
    assert done.returncode == 0, done.stdout + done.stderr
    assert _manifest(run_dir, "extract")["finished"] == extract_done  # not redone
    assert _manifest(run_dir, "gate")["status"] == "complete"


def _rewrite(path: Path, fn: Callable[[Any], None]) -> None:
    df = pd.read_parquet(path)
    fn(df)
    df.to_parquet(path, index=False)


def _plant_after_extract(first_run: Path, run_dir: Path, mutate: Callable[[Any], None]) -> None:
    """Run the pipeline with a defect planted into the extracted table.

    The first run's per-clip feature cache is reused, so extraction takes a
    second; ``after_stage`` rewrites the table the moment ``extract`` is done,
    which is the state a faulty extractor would leave behind.
    """
    shutil.copytree(first_run / "cache", run_dir / "cache")

    def hook(name: str) -> None:
        if name == "extract":
            _rewrite(run_dir / "features" / "netflix.parquet", mutate)

    _pipeline(run_dir, after_stage=hook)


@_parametrize(
    ("label", "mutate", "needle"),
    [
        (
            "dropped column",
            lambda d: d.drop(columns=["vif_scale2"], inplace=True),
            "missing column(s) ['vif_scale2']",
        ),
        ("nan feature", lambda d: d.__setitem__("adm2", float("nan")), "column 'adm2' holds"),
        ("label scale 0-1", lambda d: d.__setitem__("vmaf", d["vmaf"] / 100.0), "0-1 scale"),
        (
            "wrong teacher",
            lambda d: d.__setitem__("teacher_model", "vmaf_v0.6.1"),
            "teacher_model ['vmaf_v0.6.1']",
        ),
        ("dead column", lambda d: d.__setitem__("ssimulacra2", float("nan")), "NaN in every row"),
        ("truncated table", lambda d: d.drop(d.index[:10], inplace=True), "134 rows, want 144"),
    ],
)
def test_planted_defect_fails_at_verify_features_with_a_named_error(
    first_run: Path, tmp_path: Path, label: str, mutate: Callable[[Any], None], needle: str
) -> None:
    run_dir = tmp_path / "run"
    with pytest.raises(StageError) as err:
        _plant_after_extract(first_run, run_dir, mutate)
    assert err.value.stage == "verify_features", f"{label}: stopped at {err.value.stage}"
    assert needle in str(err.value), f"{label}: {err.value}"
    # The stages after the defect never started.
    assert not (run_dir / "stages" / "combine.stage.json").exists(), label


def _constant_model(path: Path) -> None:
    """An ONNX model with the tiny models' signature whose output is always 0."""
    import onnx
    from onnx import TensorProto, helper

    x = helper.make_tensor_value_info("features", TensorProto.FLOAT, ["N", 6])
    y = helper.make_tensor_value_info("vmaf", TensorProto.FLOAT, ["N"])
    zero = helper.make_tensor("zero", TensorProto.FLOAT, [], [0.0])
    axes = helper.make_tensor("axes", TensorProto.INT64, [1], [1])
    nodes = [
        helper.make_node("Mul", ["features", "zero"], ["scaled"]),
        helper.make_node("ReduceSum", ["scaled", "axes"], ["vmaf"], keepdims=0),
    ]
    graph = helper.make_graph(nodes, "const", [x], [y], initializer=[zero, axes])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 10
    onnx.save(model, str(path))


def test_gate_names_a_model_that_predicts_a_constant(first_run: Path, tmp_path: Path) -> None:
    """A constant prediction has NaN PLCC; every `plcc < gate` check passes it, the gate must not."""
    run_dir = tmp_path / "run"
    shutil.copytree(first_run / "cache", run_dir / "cache")

    def hook(name: str) -> None:
        if name == "export_tiny_v3":
            _constant_model(run_dir / "models" / "vmaf_tiny_v3.onnx")

    with pytest.raises(StageError) as err:
        _pipeline(run_dir, after_stage=hook)
    # validate_vmaf_tiny_v3.py itself prints "PASS - PLCC nan >= gate" and exits 0
    # (T-AI-VALIDATORS-PASS-NAN-PLCC-2026-10-05); the gate is what stops the run.
    assert err.value.stage == "gate"
    assert "gate vmaf_tiny_v3: fail" in str(err.value)
    assert "PLCC is nan (not finite)" in str(err.value)
    assert "gate vmaf_tiny_v2: pass" in str(err.value)  # the other models are not blamed


def test_missing_vmaf_binary_fails_named_before_any_stage(tmp_path: Path) -> None:
    stages = mini_retrain.build_stages(
        tmp_path / "run", vmaf_bin=tmp_path / "no-such-vmaf", profile="mini", seed=0
    )
    with pytest.raises(StageError) as err:
        run_pipeline(stages, tmp_path / "run", repo_root=REPO, log=lambda _m: None)
    assert err.value.stage == "extract" and "input missing" in str(err.value)
    assert not (tmp_path / "run" / "corpus").exists()  # nothing ran
