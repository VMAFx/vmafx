# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The report's GPU section: one GPU backend measured on the tester's devices.

Backend-neutral. A backend (hw_sycl.py for Intel GPUs, hw_cuda.py for NVIDIA GPUs,
hw_hip.py for AMD GPUs) supplies a `GpuBackend`: how to find its devices and how
the image reaches them, the environment that pins a run to one device, and parsers
for test outputs it audits. Per device the section then holds:

- `twins`: every CPU extractor of the dispatch check run with `--backend <name>` at
  `--precision max` against the CPU scores of the same image, per fixture (counts,
  first difference) and per parity-gate feature (identical, within the gate's
  bound for that twin, or differing; a twin that fell back to the CPU is named);
- `gate`: the parity gate's cells of the backend on every fixture, held exact
  (hw_gate.py);
- `device_tests`: the device test executables of the build (image/gpu-tests.json);
- `audits`: what the backend's audit tests found (the SYCL scratch audit);
- `rows`: the state rows of docs/state.md this device's measurements close
  (hw_rows.py, from the backend's row map).

A device fails when a twin differs beyond its bound, a gate cell or a device test
fails, or an audit fails. No device is `no_device`, with the reason: not a
failure, and nothing measured.
"""

from __future__ import annotations

import dataclasses
import json
import os
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any

from .hw_equiv import FixtureRunError, Runner, Scores, compare_scores, run_fixture_meta
from .hw_gate import run_gate
from .hw_rows import evaluate_device_rows, load_row_map
from .hw_suites import run_unit_tests
from .safe_process import run_bounded

DEVICE_LIMIT = 4
FAILING_DEVICE = ("fail", "error")


@dataclasses.dataclass(frozen=True)
class GpuBackend:
    """What the generic section needs from one backend (see the module docstring)."""

    name: str
    discover: Callable[[Path, Runner], dict[str, Any]]
    device_env: Callable[[Mapping[str, Any]], dict[str, str]]
    audits: Mapping[str, Callable[[str], dict[str, Any]]]
    row_map: str
    tests_manifest: str = "gpu-tests.json"


@dataclasses.dataclass(frozen=True)
class Budget:
    """Time limits of one device's runs, in seconds."""

    fixture: float
    gate: float
    tests: float


def split_backends(meta: Mapping[str, Any], backend: str) -> tuple[list[str], list[str]]:
    """(extractors that ran on `backend`, extractors that ran elsewhere), sorted."""
    on_device: set[str] = set()
    elsewhere: set[str] = set()
    for entry in meta.get("feature_backends", []):
        target = on_device if entry.get("backend") == backend else elsewhere
        target.add(str(entry.get("extractor")))
    return sorted(on_device), sorted(elsewhere - on_device)


def twin_cell(
    vmaf: str,
    fixture: Mapping[str, Any],
    cpu: Scores,
    *,
    backend: str,
    timeout_seconds: float,
    runner: Runner = run_bounded,
    environment: Mapping[str, str] | None = None,
    keys: tuple[str, str] = ("extractors_on_device", "extractors_on_cpu"),
) -> dict[str, Any]:
    """One fixture run with `--backend <backend>`, compared with the CPU scores.
    Carries `scores` and `device_lines` for the caller; `no_device` on vmaf exit 100."""
    cell: dict[str, Any] = {"fixture": fixture["id"]}
    try:
        scores, meta = run_fixture_meta(
            vmaf, fixture, None, timeout_seconds=timeout_seconds, runner=runner,
            backend=backend, environment=environment,
        )  # fmt: skip
    except FixtureRunError as error:
        cell["error"] = str(error)
        cell["no_device"] = error.returncode == 100
        return cell
    except (TimeoutError, RuntimeError, ValueError) as error:
        cell["error"] = str(error)
        return cell
    on_device, elsewhere = split_backends(meta, backend)
    if not on_device:
        cell["error"] = f"no extractor ran on {backend} (silent CPU fallback)"
        return cell
    cell.update(compare_scores(cpu, scores))
    cell[keys[0]], cell[keys[1]] = on_device, elsewhere
    cell["scores"], cell["device_lines"] = scores, meta.get("device_lines", [])
    return cell


def _feature_values(
    spec: Mapping[str, Any], cells: Sequence[Mapping[str, Any]], cpu: Mapping[str, Scores]
) -> tuple[int, list[tuple[str, str, int, float | None, float | None]]]:
    """(values compared, every differing value as (fixture, metric, frame, cpu, device))."""
    compared = 0
    differing: list[tuple[str, str, int, float | None, float | None]] = []
    for cell in cells:
        device, base = cell.get("scores", {}), cpu.get(str(cell["fixture"]), {})
        for metric in spec["metrics"]:
            left, right = base.get(metric), device.get(metric)
            if left is None and right is None:
                continue
            if left is None or right is None or len(left) != len(right):
                compared += 1
                differing.append((str(cell["fixture"]), metric, 0, None, None))
                continue
            compared += len(left)
            differing += [(str(cell["fixture"]), metric, i, a, b)
                          for i, (a, b) in enumerate(zip(left, right, strict=True)) if a != b]  # fmt: skip
    return compared, differing


def _largest(differing: Sequence[tuple[str, str, int, float | None, float | None]]) -> float:
    gaps = [abs(a - b) for _, _, _, a, b in differing if a is not None and b is not None]
    absent = any(a is None or b is None for _, _, _, a, b in differing)
    return float("inf") if absent else max(gaps, default=0.0)


def feature_verdict(
    feature: str, spec: Mapping[str, Any], cells: Sequence[Mapping[str, Any]],
    cpu: Mapping[str, Scores],
) -> dict[str, Any]:  # fmt: skip
    """One parity-gate feature in the default-option run: its twin's values against
    the CPU's, judged at the gate's bound for that twin."""
    item: dict[str, Any] = {"feature": feature, "bound": spec["bound"], "source": spec["source"]}
    ran = [c for c in cells if "scores" in c]
    if spec.get("options"):
        return {**item, "status": "gate_only", "values": 0, "differing_values": 0}
    if not ran or not all(spec["extractor"] in c.get("extractors_on_device", []) for c in ran):
        on_cpu = any(spec["extractor"] in c.get("extractors_on_cpu", []) for c in ran)
        return {**item, "status": "not_on_device" if on_cpu else "not_in_run",
                "values": 0, "differing_values": 0}  # fmt: skip
    compared, differing = _feature_values(spec, ran, cpu)
    largest = _largest(differing)
    status = "identical" if not differing else (
        "within_bound" if largest <= float(spec["bound"]) else "differing")  # fmt: skip
    item.update({"status": status, "values": compared, "differing_values": len(differing),
                 "max_abs_diff": f"{largest:.17g}"})  # fmt: skip
    if differing:
        fixture, metric, frame, left, right = differing[0]
        item["first"] = {"fixture": fixture, "metric": metric, "frame": frame,
                         "cpu": "absent" if left is None else f"{left:.17g}",
                         "device": "absent" if right is None else f"{right:.17g}"}  # fmt: skip
    return item


def twins_status(cells: Sequence[Mapping[str, Any]], features: Sequence[Mapping[str, Any]]) -> str:
    """`error`, `differing`, `within_bound` or `identical` over cells and features."""
    if not cells or any("error" in cell for cell in cells):
        return "error"
    states = {item["status"] for item in features}
    if "differing" in states or "not_on_device" in states:
        return "differing"
    mapped = {m for item in features for m in item.get("metrics", ())}
    unmapped = [d for c in cells for d in c.get("details", []) if d["metric"] not in mapped]
    if unmapped:
        return "differing"
    return "within_bound" if "within_bound" in states else "identical"


def run_twins(
    vmaf: str,
    fixtures: Sequence[Mapping[str, Any]],
    cpu: Mapping[str, Scores],
    bounds: Mapping[str, Any],
    *,
    backend: str,
    environment: Mapping[str, str],
    timeout_seconds: float,
    runner: Runner = run_bounded,
) -> tuple[dict[str, Any], list[str]]:
    """The `twins` part of one device, and the device lines vmaf logged."""
    cells = [
        twin_cell(vmaf, fixture, cpu[str(fixture["id"])], backend=backend,
                  timeout_seconds=timeout_seconds, runner=runner, environment=environment)
        for fixture in fixtures if str(fixture["id"]) in cpu
    ]  # fmt: skip
    specs = bounds.get("features", {})
    features = [feature_verdict(name, spec, cells, cpu) for name, spec in sorted(specs.items())]
    status = twins_status(cells, [{**f, "metrics": specs[f["feature"]]["metrics"]}
                                  for f in features])  # fmt: skip
    lines = sorted({line for cell in cells for line in cell.get("device_lines", [])})
    public = [{k: v for k, v in c.items() if k not in ("scores", "device_lines", "no_device")}
              for c in cells]  # fmt: skip
    return {"status": status, "fixtures": public, "features": features}, lines


def run_device_tests(
    root: Path, backend: GpuBackend, environment: Mapping[str, str], budget: Budget,
    runner: Runner,
) -> tuple[dict[str, Any], dict[str, Any]]:  # fmt: skip
    """The device test executables, and what the backend's audits found in them."""
    outputs: dict[str, str] = {}

    def observe(name: str, output: str) -> None:
        if name in backend.audits:
            outputs[name] = output

    suite = run_unit_tests(
        root / "image" / backend.tests_manifest, timeout_seconds=budget.tests, runner=runner,
        environment=environment, observe=observe,
    )  # fmt: skip
    audits = {
        name: parse(outputs[name]) if name in outputs else {"status": "not_run"}
        for name, parse in sorted(backend.audits.items())
    }
    return suite, audits


def device_status(entry: Mapping[str, Any]) -> str:
    """`fail` when any measurement of the device failed, `pass` otherwise."""
    bad = (
        entry["twins"]["status"] in ("differing", "error")
        or entry["gate"]["status"] in ("fail", "error")
        or entry["device_tests"]["status"] != "pass"
        or any(audit["status"] in ("fail", "error") for audit in entry["audits"].values())
    )
    return "fail" if bad else "pass"


def run_device(
    root: Path,
    backend: GpuBackend,
    device: Mapping[str, Any],
    cpu: Mapping[str, Scores],
    context: Mapping[str, Any],
) -> dict[str, Any]:
    """Every measurement of one device."""
    selector, budget, runner = backend.device_env(device), context["budget"], context["runner"]
    env = {**os.environ, **selector}
    vmaf = str(root / "build" / "tools" / "vmaf")
    twins, lines = run_twins(
        vmaf, context["fixtures"], cpu, context["bounds"], backend=backend.name,
        environment=env, timeout_seconds=budget.fixture, runner=runner,
    )  # fmt: skip
    gate = run_gate(
        root, vmaf, context["fixtures"], context["gate_config"], backend=backend.name,
        timeout_seconds=budget.gate, runner=runner, environment=env,
    )  # fmt: skip
    tests, audits = run_device_tests(root, backend, selector, budget, runner)
    entry: dict[str, Any] = {
        "index": int(device["index"]),
        "selector": selector,
        "facts": dict(device["facts"]),
        "device_lines": lines,
        "twins": twins,
        "gate": gate,
        "device_tests": tests,
        "audits": audits,
    }
    entry["status"] = device_status(entry)
    entry["rows"] = evaluate_device_rows(context["row_map"], entry)
    return entry


def gate_config(bounds: Mapping[str, Any], row_map: Mapping[str, Any] | None) -> dict[str, Any]:
    """The gate run of every device: every gate feature, minus the row map's skips."""
    skip = list((row_map or {}).get("gate_skip", []))
    return {"features": sorted(bounds.get("features", {})), "skip": skip}


def load_json(path: Path) -> dict[str, Any] | None:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return document if isinstance(document, dict) else None


TWIN_KEYS = ("metrics", "extractor", "options", "bound", "source")


def load_bounds(path: Path) -> dict[str, Any] | None:
    """image/gpu-twins.json, or None when it is absent or lacks a field per feature."""
    document = load_json(path)
    features = (document or {}).get("features")
    if not isinstance(features, dict) or not features:
        return None
    if not all(isinstance(spec, dict) and all(k in spec for k in TWIN_KEYS)
               for spec in features.values()):  # fmt: skip
        return None
    return document


def section_status(devices: Sequence[Mapping[str, Any]]) -> str:
    if not devices:
        return "no_device"
    return "fail" if any(d["status"] in FAILING_DEVICE for d in devices) else "pass"


def run_gpu_section(
    root: Path,
    backend: GpuBackend,
    fixtures: Sequence[Mapping[str, Any]],
    cpu: Mapping[str, Scores],
    budget: Budget,
    runner: Runner = run_bounded,
) -> dict[str, Any]:
    """The report's `gpu` section for `backend` on every device it finds (at most four)."""
    found = backend.discover(root, runner)
    section: dict[str, Any] = {"backend": backend.name, "access": found["access"],
                               "runtime": found.get("runtime", {})}  # fmt: skip
    bounds = load_bounds(root / "image" / "gpu-twins.json") or {}
    row_map = load_row_map(root / "image" / backend.row_map)
    devices = found["devices"][:DEVICE_LIMIT]
    if not devices or not bounds:
        reason = found.get("reason") or "image/gpu-twins.json is absent or malformed"
        return {**section, "status": "no_device" if not devices else "error", "reason": reason,
                "devices": []}  # fmt: skip
    context = {"budget": budget, "runner": runner, "fixtures": list(fixtures), "bounds": bounds,
               "gate_config": gate_config(bounds, row_map), "row_map": row_map}  # fmt: skip
    entries = [run_device(root, backend, device, cpu, context) for device in devices]
    section.update({"status": section_status(entries), "devices": entries})
    if len(found["devices"]) > DEVICE_LIMIT:
        section["reason"] = f"{len(found['devices'])} devices found; the first {DEVICE_LIMIT} ran"
    return section


def gpu_not_exercised(section: Mapping[str, Any]) -> list[tuple[str, str]]:
    """(item, reason) pairs the GPU section adds to the report's `not_exercised`."""
    name = str(section.get("backend", "GPU")).upper()
    if section.get("status") in ("not_run", "not_applicable"):
        return []
    if section.get("status") in ("no_device", "error"):
        return [(f"{name} twins", str(section.get("reason", section.get("status"))))]
    items: list[tuple[str, str]] = []
    for device in section.get("devices", []):
        tests = device["device_tests"]
        for test in tests.get("left_out", []):
            items.append((f"{name} test {test['name']}", str(test["reason"])))
        if tests.get("skipped_tests"):
            items.append((f"{name} tests skipped on device {device['index']}",
                          ", ".join(tests["skipped_tests"])))  # fmt: skip
        gate_only = [
            f["feature"] for f in device["twins"]["features"] if f["status"] == "gate_only"
        ]
        if gate_only:
            items.append((f"{name} twins with options in the default-option run",
                          "measured by the gate only: " + ", ".join(gate_only)))  # fmt: skip
        break  # the lists are the image's, the same for every device
    return items


VERSION_FACTS = ("ip_version", "compute_capability", "gfx_target")


def device_version(facts: Mapping[str, Any]) -> str:
    """The backend's own version of a device: Intel GPU IP version, CUDA compute
    capability or AMD gfx target, whichever the backend reports."""
    return next((str(facts[key]) for key in VERSION_FACTS if key in facts), "?")


def summary_lines(section: Mapping[str, Any]) -> list[str]:
    """Human lines for stderr: the section's status and one line per device."""
    name = f" ({section['backend']})" if section.get("backend") else ""
    head = f"gpu{name}: {section['status']}"
    if section.get("access"):
        head += f", path {section['access'].get('path')}"
    if section.get("reason"):
        head += f" ({section['reason']})"
    lines = [head]
    for device in section.get("devices", []):
        facts, tests = device["facts"], device["device_tests"]
        audits = ", ".join(f"{k} {v['status']}" for k, v in device["audits"].items())
        counts = device["rows"].get("counts", {})
        lines.append(
            f"  device {device['index']} {facts.get('name', '?')} ({facts.get('family', '?')} "
            f"{device_version(facts)}): {device['status']}; twins "
            f"{device['twins']['status']}, gate {device['gate']['status']}, tests "
            f"{tests['status']} ({tests['passed']} passed, {tests['failed']} failed, "
            f"{tests['skipped']} skipped), {audits or 'no audit'}; state rows "
            f"{counts.get('pass', 0)} passing, {counts.get('fail', 0)} failing, "
            f"{counts.get('not_measured', 0)} not measured"
        )
    return lines
