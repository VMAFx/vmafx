#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Export trained C2 + C3 Lightning checkpoints to ONNX and update
``model/tiny/registry.json``.

After ``ai/scripts/train_konvid.py --model both`` finishes, this script:

  1. loads each Lightning ``.ckpt``,
  2. calls ``vmaf_train.models.exports.export_to_onnx`` with the right
     input shape (matches the training-time tensor),
  3. writes the ONNX file under ``model/tiny/``,
  4. computes its SHA-256,
  5. patches ``model/tiny/registry.json`` to add (or update) the C2 / C3
     entries,
  6. writes the per-model sidecar JSON manifest used by the registry
     loader.

Idempotent — re-running overwrites the ONNX + sidecar JSON and updates
the registry row in place.
"""

from __future__ import annotations

import json
import sys
from argparse import Namespace
from pathlib import Path
from typing import Any

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH: Path = _SCRIPT_PATHS.script_path
REPO_ROOT: Path = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import sha256  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

# Guard the pytorch_lightning → torchmetrics → torchvision import chain.
# A stale venv with torchvision 0.26.0 against torch 2.12.0 raises
# ``RuntimeError: operator torchvision::nms does not exist`` here (not an
# ImportError), so a plain try/except ImportError is insufficient.  Upgrade:
# ``pip install -U 'torchvision>=0.27.0,<0.28.0'`` to fix the venv.
try:
    from vmaf_train.models import LearnedFilter, NRMetric
except Exception as _torchvision_err:  # pragma: no cover
    sys.exit(
        f"Failed to import vmaf_train.models: {_torchvision_err}\n"
        "This is usually a torch/torchvision ABI mismatch.  "
        "Run: pip install -U 'torchvision>=0.27.0,<0.28.0'"
    )

TINY_DIR: Path = REPO_ROOT / "model" / "tiny"
REGISTRY: Path = TINY_DIR / "registry.json"

C2_CKPT_DEFAULT = REPO_ROOT / "runs" / "c2_konvid" / "last.ckpt"
C3_CKPT_DEFAULT = REPO_ROOT / "runs" / "c3_konvid" / "last.ckpt"

C2_INPUT_HW = 224
C3_INPUT_HW = 224


def _load_lightning_ckpt(model_cls, ckpt: Path):  # type: ignore[no-untyped-def]
    import torch

    # nosec B614: Lightning checkpoints store hyper_parameters as plain
    # Python objects alongside tensors, so weights_only=True rejects them
    # with UnpicklingError. The trust boundary is "ckpt produced by our
    # own training runs under runs/" — same pattern as the export_vmaf_tiny_v{2,3,4}
    # scripts. Path is a CLI arg from the developer, not network input.
    state = torch.load(ckpt, map_location="cpu", weights_only=False)  # nosec B614
    hp = state.get("hyper_parameters", {}) or {}
    model = model_cls(**hp)
    model.load_state_dict(state["state_dict"])
    return model.eval()


def _export_one(  # type: ignore[no-untyped-def]
    *,
    model,
    onnx_path: Path,
    in_shape: tuple[int, ...],
    input_name: str,
    output_name: str,
) -> None:
    from vmaf_train.models.exports import export_to_onnx

    print(f"[export] {onnx_path.name} from in_shape={in_shape}")
    export_to_onnx(
        model,
        onnx_path,
        in_shape=in_shape,
        input_name=input_name,
        output_name=output_name,
        atol=1e-4,
    )


def _write_sidecar(
    model_id: str,
    onnx_path: Path,
    kind: str,
    notes: str,
    *,
    run_provenance: dict[str, Any] | None = None,
) -> Path:
    sidecar: Path = TINY_DIR / f"{model_id}.json"
    payload = {
        "id": model_id,
        "kind": kind,
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": sha256(onnx_path),
        "notes": notes,
    }
    if run_provenance is not None:
        payload["run_provenance"] = run_provenance
    write_manifest_json(sidecar, payload)
    return sidecar


def _update_registry(*entries: dict[str, Any]) -> None:
    if not REGISTRY.exists():
        sys.exit(f"missing {REGISTRY}")
    doc = json.loads(REGISTRY.read_text())
    by_id: dict[str, Any] = {m["id"]: m for m in doc.get("models", [])}
    for e in entries:
        by_id[str(e["id"])] = e
    doc["models"] = sorted(by_id.values(), key=lambda m: str(m["id"]))
    REGISTRY.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")


def _parse_args(raw_argv: list[str]) -> Namespace:
    parser = make_argument_parser(description=__doc__)
    parser.add_argument("--c2-ckpt", type=Path, default=C2_CKPT_DEFAULT)
    parser.add_argument("--c3-ckpt", type=Path, default=C3_CKPT_DEFAULT)
    parser.add_argument("--c2-id", default="nr_metric_v1")
    parser.add_argument("--c3-id", default="learned_filter_v1")
    return parser.parse_args(raw_argv)


def _export_c2(args: Namespace, raw_argv: list[str]) -> dict[str, Any] | None:
    if not args.c2_ckpt.exists():
        return None
    c2 = _load_lightning_ckpt(NRMetric, args.c2_ckpt)
    c2_onnx = TINY_DIR / f"{args.c2_id}.onnx"
    _export_one(
        model=c2,
        onnx_path=c2_onnx,
        in_shape=(1, 1, C2_INPUT_HW, C2_INPUT_HW),
        input_name="frame",
        output_name="mos",
    )
    _write_sidecar(
        args.c2_id,
        c2_onnx,
        kind="nr",
        notes=(
            "Tiny NR MobileNet (C2) — single luma frame → MOS scalar. "
            "Trained on KoNViD-1k middle-frames (1200 clips, "
            "~973 train / ~106 val) at 224×224 grayscale. "
            "Exported via ai/scripts/export_tiny_models.py."
        ),
        run_provenance=build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={"c2_checkpoint": args.c2_ckpt},
            outputs={
                "onnx": c2_onnx,
                "sidecar": TINY_DIR / f"{args.c2_id}.json",
                "registry": REGISTRY,
            },
        ),
    )
    return {
        "id": args.c2_id,
        "kind": "nr",
        "notes": (
            "Tiny NR MobileNet baseline trained on KoNViD-1k "
            "(CC BY 4.0; not redistributed). 224×224 grayscale "
            "input; ~19K params; opset 17. See "
            "docs/adr/0168-tinyai-konvid-baselines.md."
        ),
        "onnx": c2_onnx.name,
        "opset": 17,
        "sha256": sha256(c2_onnx),
    }


def _export_c3(args: Namespace, raw_argv: list[str]) -> dict[str, Any] | None:
    if not args.c3_ckpt.exists():
        return None
    c3 = _load_lightning_ckpt(LearnedFilter, args.c3_ckpt)
    c3_onnx = TINY_DIR / f"{args.c3_id}.onnx"
    _export_one(
        model=c3,
        onnx_path=c3_onnx,
        in_shape=(1, 1, C3_INPUT_HW, C3_INPUT_HW),
        input_name="degraded",
        output_name="filtered",
    )
    _write_sidecar(
        args.c3_id,
        c3_onnx,
        kind="filter",
        notes=(
            "Tiny residual filter (C3) — degraded → clean luma. "
            "Trained self-supervised on KoNViD-1k middle-frames + "
            "synthetic gaussian-blur σ=1.2 + JPEG-Q35 degradation. "
            "Exported via ai/scripts/export_tiny_models.py."
        ),
        run_provenance=build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={"c3_checkpoint": args.c3_ckpt},
            outputs={
                "onnx": c3_onnx,
                "sidecar": TINY_DIR / f"{args.c3_id}.json",
                "registry": REGISTRY,
            },
        ),
    )
    return {
        "id": args.c3_id,
        "kind": "filter",
        "notes": (
            "Tiny residual filter baseline for vmaf_pre — "
            "self-supervised on KoNViD-1k frames with synthetic "
            "blur+JPEG degradation. ~19K params; opset 17. See "
            "docs/adr/0168-tinyai-konvid-baselines.md."
        ),
        "onnx": c3_onnx.name,
        "opset": 17,
        "sha256": sha256(c3_onnx),
    }


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _parse_args(raw_argv)
    TINY_DIR.mkdir(parents=True, exist_ok=True)
    new_entries = [
        entry for entry in (_export_c2(args, raw_argv), _export_c3(args, raw_argv)) if entry
    ]

    if not new_entries:
        sys.exit("no checkpoints found — nothing to export")

    _update_registry(*new_entries)
    for e in new_entries:
        sha = str(e["sha256"])
        print(f"[registry] {e['id']} sha256={sha[:16]}…")
    return 0


if __name__ == "__main__":
    sys.exit(main())
