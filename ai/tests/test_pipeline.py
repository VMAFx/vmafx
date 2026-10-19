# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for ``aiutils.pipeline``: fail named and early, resume, record identity.

Every case runs real subprocesses with ``python -c`` bodies, so the stage
runner is exercised exactly as the retrain driver uses it.
"""

from __future__ import annotations

import json
import signal
import subprocess
import sys
import textwrap
from pathlib import Path
from typing import Any

import pytest

from aiutils.pipeline import (
    Stage,
    StageContext,
    StageError,
    StageResult,
    path_digest,
    preflight,
    run_pipeline,
)

REPO = Path(__file__).resolve().parents[2]


def _py(body: str, *args: str) -> tuple[str, ...]:
    return (sys.executable, "-c", textwrap.dedent(body), *args)


def _write_stage(name: str, src: Path, dst: Path, counter: Path) -> Stage:
    """Copy ``src`` to ``dst`` upper-cased and append a line to ``counter``."""
    body = """
        import sys
        from pathlib import Path
        src, dst, counter = map(Path, sys.argv[1:4])
        dst.write_text(src.read_text().upper())
        with counter.open("a") as fh:
            fh.write("ran\\n")
    """
    return Stage(
        name,
        _py(body, str(src), str(dst), str(counter)),
        inputs=(src,),
        outputs=(dst,),
        stable=(dst,),
    )


def _runs(counter: Path) -> int:
    return len(counter.read_text().splitlines()) if counter.exists() else 0


def _run(stages: list[Stage], run_dir: Path, **kw: Any) -> list[StageResult]:
    return run_pipeline(stages, run_dir, repo_root=REPO, log=lambda _m: None, **kw)


def test_missing_input_fails_named_before_any_stage_runs(tmp_path: Path) -> None:
    first = tmp_path / "first.txt"
    first.write_text("a")
    counter = tmp_path / "count"
    stages = [
        _write_stage("one", first, tmp_path / "one.txt", counter),
        _write_stage("two", tmp_path / "absent.txt", tmp_path / "two.txt", counter),
    ]
    with pytest.raises(StageError) as err:
        _run(stages, tmp_path / "run")
    assert err.value.stage == "two"
    assert "input missing" in str(err.value) and "absent.txt" in str(err.value)
    # Early: stage "one" never started although it could have.
    assert _runs(counter) == 0


def test_empty_input_fails_named(tmp_path: Path) -> None:
    empty = tmp_path / "empty.txt"
    empty.write_bytes(b"")
    stage = _write_stage("only", empty, tmp_path / "out.txt", tmp_path / "count")
    with pytest.raises(StageError, match=r"stage 'only': input is empty"):
        preflight([stage])


def test_input_directory_without_files_fails_named(tmp_path: Path) -> None:
    (tmp_path / "corpus").mkdir()
    stage = Stage("extract", _py("pass"), inputs=(tmp_path / "corpus",))
    with pytest.raises(StageError, match=r"stage 'extract': input directory holds no file"):
        preflight([stage])


def test_failing_stage_is_named_with_log_tail(tmp_path: Path) -> None:
    stage = Stage(
        "boom", _py("import sys; print('first line'); print('why', file=sys.stderr); sys.exit(3)")
    )
    with pytest.raises(StageError) as err:
        _run([stage], tmp_path / "run")
    assert err.value.stage == "boom"
    assert "exited 3" in err.value.reason and "why" in err.value.reason
    manifest = json.loads((tmp_path / "run/stages/boom.stage.json").read_text())
    assert manifest["status"] == "failed" and manifest["returncode"] == 3


def test_stage_that_writes_no_output_fails_named(tmp_path: Path) -> None:
    stage = Stage("lazy", _py("pass"), outputs=(tmp_path / "never.txt",))
    with pytest.raises(StageError, match=r"stage 'lazy': output missing"):
        _run([stage], tmp_path / "run")


def test_stage_that_writes_empty_output_fails_named(tmp_path: Path) -> None:
    out = tmp_path / "empty.bin"
    stage = Stage(
        "hollow", _py("import sys; open(sys.argv[1], 'wb').close()", str(out)), outputs=(out,)
    )
    with pytest.raises(StageError, match=r"stage 'hollow': output is empty"):
        _run([stage], tmp_path / "run")


def test_check_failure_is_named_and_leaves_manifest_failed(tmp_path: Path) -> None:
    out = tmp_path / "o.txt"

    def check(_ctx: StageContext) -> None:
        raise StageError("checked", "column 'adm2' is all-NaN")

    stage = Stage(
        "checked",
        _py("import sys; open(sys.argv[1], 'w').write('x')", str(out)),
        outputs=(out,),
        check=check,
    )
    with pytest.raises(StageError, match="column 'adm2' is all-NaN"):
        _run([stage], tmp_path / "run")
    manifest = json.loads((tmp_path / "run/stages/checked.stage.json").read_text())
    assert manifest["status"] == "failed"


def test_second_run_resumes_every_finished_stage(tmp_path: Path) -> None:
    src = tmp_path / "src.txt"
    src.write_text("abc")
    counter = tmp_path / "count"
    stages = [
        _write_stage("one", src, tmp_path / "one.txt", counter),
        _write_stage("two", tmp_path / "one.txt", tmp_path / "two.txt", counter),
    ]
    first = _run(stages, tmp_path / "run")
    assert [r.status for r in first] == ["complete", "complete"]
    second = _run(stages, tmp_path / "run")
    assert [r.status for r in second] == ["resumed", "resumed"]
    assert _runs(counter) == 2  # nothing ran again


def test_changed_input_reruns_that_stage_and_its_dependants_only(tmp_path: Path) -> None:
    a, b = tmp_path / "a.txt", tmp_path / "b.txt"
    a.write_text("a")
    b.write_text("b")
    counter_a, counter_b = tmp_path / "ca", tmp_path / "cb"
    stages = [
        _write_stage("sa", a, tmp_path / "A.txt", counter_a),
        _write_stage("sb", b, tmp_path / "B.txt", counter_b),
        _write_stage("sa2", tmp_path / "A.txt", tmp_path / "A2.txt", counter_a),
    ]
    _run(stages, tmp_path / "run")
    a.write_text("changed")
    result = {r.name: r.status for r in _run(stages, tmp_path / "run")}
    assert result == {"sa": "complete", "sb": "resumed", "sa2": "complete"}
    assert (tmp_path / "A2.txt").read_text() == "CHANGED"


def test_modified_output_reruns_the_stage(tmp_path: Path) -> None:
    src = tmp_path / "src.txt"
    src.write_text("abc")
    counter = tmp_path / "count"
    stage = _write_stage("one", src, tmp_path / "one.txt", counter)
    _run([stage], tmp_path / "run")
    (tmp_path / "one.txt").write_text("tampered")
    assert _run([stage], tmp_path / "run")[0].status == "complete"
    assert (tmp_path / "one.txt").read_text() == "ABC"


def test_killed_run_resumes_without_redoing_finished_stages(tmp_path: Path) -> None:
    """SIGKILL the runner while stage two is running; the next run finishes it only."""
    script = tmp_path / "driver.py"
    script.write_text(textwrap.dedent(f"""
            import os, signal, sys
            from pathlib import Path
            sys.path.insert(0, {str(REPO / "ai/src")!r})
            from aiutils.pipeline import Stage, run_pipeline
            t = Path({str(tmp_path)!r})
            die = os.environ.get("DIE") == "1"
            body_ok = "import sys; from pathlib import Path; Path(sys.argv[1]).write_text('x'); open(sys.argv[2], 'a').write('r\\\\n')"
            kill = ("import os, signal, sys; open(sys.argv[2], 'a').write('r\\\\n'); "
                    "os.kill(os.getppid(), signal.SIGKILL); import time; time.sleep(30)")
            stages = [
                Stage("s1", (sys.executable, "-c", body_ok, str(t / "o1"), str(t / "c1")),
                      outputs=(t / "o1",)),
                Stage("s2", (sys.executable, "-c", kill if die else body_ok, str(t / "o2"), str(t / "c2")),
                      outputs=(t / "o2",)),
            ]
            run_pipeline(stages, t / "run", repo_root=Path({str(REPO)!r}), log=print)
            """))
    killed = subprocess.run(
        [sys.executable, str(script)],
        env={"DIE": "1", "PATH": "/usr/bin:/bin"},
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert killed.returncode == -signal.SIGKILL
    s2 = json.loads((tmp_path / "run/stages/s2.stage.json").read_text())
    assert s2["status"] == "running"  # left mid-stage by the kill
    assert (tmp_path / "run/stages/s1.stage.json").is_file()
    resumed = subprocess.run(
        [sys.executable, str(script)],
        env={"DIE": "0", "PATH": "/usr/bin:/bin"},
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    assert "s1: resumed" in resumed.stdout
    assert "s2: running" in resumed.stdout
    assert _runs(tmp_path / "c1") == 1  # finished work was not redone
    assert _runs(tmp_path / "c2") == 2  # the killed stage ran again, once
    assert json.loads((tmp_path / "run/stages/s2.stage.json").read_text())["status"] == "complete"


def test_corrupted_output_of_an_earlier_stage_is_refused_by_the_next(tmp_path: Path) -> None:
    src = tmp_path / "src.txt"
    src.write_text("abc")
    counter = tmp_path / "count"
    stages = [
        _write_stage("one", src, tmp_path / "one.txt", counter),
        _write_stage("two", tmp_path / "one.txt", tmp_path / "two.txt", counter),
    ]

    def corrupt(name: str) -> None:
        if name == "one":
            (tmp_path / "one.txt").write_bytes(b"")

    with pytest.raises(StageError) as err:
        _run(stages, tmp_path / "run", after_stage=corrupt)
    assert err.value.stage == "two" and "input is empty" in str(err.value)


def test_manifest_records_identity_seed_resources_and_stable_digests(tmp_path: Path) -> None:
    src = tmp_path / "src.txt"
    src.write_text("abc")
    lock = tmp_path / "lock.txt"
    lock.write_text("torch==1\n")
    stage = _write_stage("one", src, tmp_path / "one.txt", tmp_path / "count")
    stage = Stage(**{**stage.__dict__, "seed": 7})
    _run([stage], tmp_path / "run", lock_files=[lock])
    m = json.loads((tmp_path / "run/stages/one.stage.json").read_text())
    assert m["schema"] == "retrain-stage-manifest-v1" and m["status"] == "complete"
    assert m["seed"] == 7
    env = m["environment"]
    assert env["python"] and env["container"] and "torch" in env["libraries"]
    assert env["locks"][str(lock)] == path_digest(lock)
    assert m["resources"]["wall_s"] >= 0
    assert m["stable"] == {str(tmp_path / "one.txt"): path_digest(tmp_path / "one.txt")}
    assert m["inputs"][str(src)] == path_digest(src)


def test_container_identity_comes_from_the_environment(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("VMAFX_CONTAINER_IMAGE_ID", "sha256:feedface")
    stage = Stage("one", _py("pass"))
    _run([stage], tmp_path / "run")
    m = json.loads((tmp_path / "run/stages/one.stage.json").read_text())
    assert m["environment"]["container"] == "sha256:feedface"


def test_seed_change_reruns_the_stage(tmp_path: Path) -> None:
    out = tmp_path / "o"
    base = Stage(
        "s", _py("import sys; open(sys.argv[1], 'w').write('x')", str(out)), outputs=(out,), seed=1
    )
    _run([base], tmp_path / "run")
    assert _run([base], tmp_path / "run")[0].status == "resumed"
    other = Stage(**{**base.__dict__, "seed": 2})
    assert _run([other], tmp_path / "run")[0].status == "complete"


def test_duplicate_stage_names_are_refused(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="duplicate stage names"):
        _run([Stage("a", _py("pass")), Stage("a", _py("pass"))], tmp_path / "run")


def test_timeout_kills_the_stage_and_names_it(tmp_path: Path) -> None:
    stage = Stage("slow", _py("import time; time.sleep(30)"), timeout_s=0.5)
    with pytest.raises(StageError, match=r"stage 'slow': exited 124"):
        _run([stage], tmp_path / "run")


def test_copies_seed_an_in_place_output(tmp_path: Path) -> None:
    seed, work = tmp_path / "seed.json", tmp_path / "work.json"
    seed.write_text("[1]")
    body = "import sys, json; p = sys.argv[1]; d = json.load(open(p)); d.append(2); json.dump(d, open(p, 'w'))"
    stage = Stage(
        "grow", _py(body, str(work)), inputs=(seed,), outputs=(work,), copies=((seed, work),)
    )
    _run([stage], tmp_path / "run")
    assert json.loads(work.read_text()) == [1, 2]
    assert json.loads(seed.read_text()) == [1]
    assert _run([stage], tmp_path / "run")[0].status == "resumed"


def test_a_running_manifest_never_resumes_even_with_matching_key_and_outputs(
    tmp_path: Path,
) -> None:
    from aiutils import pipeline

    out = tmp_path / "o"
    out.write_text("x")
    stage = Stage("s", _py("pass"), outputs=(out,))
    manifest = {"status": "running", "key": "k", "outputs": {str(out): path_digest(out)}}
    assert pipeline._can_resume(stage, manifest, "k") is False
    assert pipeline._can_resume(stage, {**manifest, "status": "complete"}, "k") is True
