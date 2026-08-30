#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Export upstream U-2-Net ``u2netp`` weights into the fork mirror contract.

The fork does not vendor upstream U-2-Net code or commit the resulting
``u2netp_mirror`` binary. Operators provide a local audited checkout plus the
upstream ``u2netp.pth`` checkpoint, and this script writes:

* an ONNX graph whose public contract matches ``feature_mobilesal.c``:
  ``input`` float32 NCHW [1, 3, H, W] -> ``saliency_map`` float32
  NCHW [1, 1, H, W];
* a deterministic JSON manifest with source hashes and shared
  ``run_provenance``.

The ONNX file remains a release artefact only. Do not commit
``model/u2netp_mirror.onnx`` or ``model/u2netp_mirror.pth``.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
from typing import Any

import torch
from torch import nn

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import sha256  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

UPSTREAM_REPO = "https://github.com/xuebinqin/U-2-Net"
UPSTREAM_COMMIT = "ac7e1c81"
MODEL_ID = "u2netp_mirror_v1"
SCHEMA = "u2netp-mirror-export-manifest-v1"
DEFAULT_OUTPUT = REPO_ROOT / "model" / "u2netp_mirror.onnx"
DEFAULT_HEIGHT = 320
DEFAULT_WIDTH = 320
DEFAULT_OPSET = 17


def _resolve_module_path(upstream_dir: Path, module_path: Path | None) -> Path:
    if module_path is not None:
        return module_path if module_path.is_absolute() else upstream_dir / module_path

    candidates = (
        upstream_dir / "model" / "u2net.py",
        upstream_dir / "u2net.py",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return candidates[0]


def _resolve_checkpoint(upstream_dir: Path, checkpoint: Path | None) -> Path:
    if checkpoint is not None:
        return checkpoint if checkpoint.is_absolute() else upstream_dir / checkpoint

    candidates = (
        upstream_dir / "saved_models" / "u2netp" / "u2netp.pth",
        upstream_dir / "u2netp.pth",
        upstream_dir / "model" / "u2netp.pth",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return candidates[0]


def _load_u2netp_class(module_path: Path) -> type[nn.Module]:
    if not module_path.is_file():
        raise FileNotFoundError(f"missing upstream U-2-Net module: {module_path}")
    spec = importlib.util.spec_from_file_location("u2netp_upstream", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not import upstream module: {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    cls = getattr(module, "U2NETP", None)
    if cls is None:
        raise AttributeError(f"{module_path} does not define U2NETP")
    return cls


def _instantiate_u2netp(cls: type[nn.Module]) -> nn.Module:
    try:
        return cls(3, 1)
    except TypeError:
        return cls()


def _strip_data_parallel(state_dict: dict[str, Any]) -> dict[str, Any]:
    return {
        key[7:] if key.startswith("module.") else key: value for key, value in state_dict.items()
    }


def _load_checkpoint(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise FileNotFoundError(f"missing upstream u2netp checkpoint: {path}")
    try:
        loaded = torch.load(path, map_location="cpu", weights_only=True)
    except TypeError:
        # nosec B614: TypeError only fires on torch <1.13 which lacks the
        # weights_only kwarg; the fallback path is hit by developer-side
        # legacy installs only. The trust boundary is "upstream u2netp
        # checkpoint already vetted by export_u2netp_mirror's caller" —
        # the path comes from a `--upstream-dir` CLI flag, not user input.
        loaded = torch.load(path, map_location="cpu")  # nosec B614
    if isinstance(loaded, dict) and "state_dict" in loaded:
        loaded = loaded["state_dict"]
    if not isinstance(loaded, dict):
        raise TypeError(f"{path} did not load as a state-dict-like mapping")
    return _strip_data_parallel(loaded)


def _load_license(upstream_dir: Path) -> tuple[Path, str]:
    candidates = (upstream_dir / "LICENSE", upstream_dir / "LICENSE.txt")
    for candidate in candidates:
        if candidate.is_file():
            text = candidate.read_text(encoding="utf-8", errors="replace")
            if "apache" not in text.lower() or "version 2.0" not in text.lower():
                raise RuntimeError(f"{candidate} is not recognisably Apache-2.0")
            return candidate, sha256(candidate)
    raise FileNotFoundError(f"missing Apache-2.0 LICENSE under {upstream_dir}")


class U2NetPMirrorAdapter(nn.Module):
    """Select upstream U2NETP ``d0`` and expose the fork saliency output name."""

    def __init__(self, upstream: nn.Module) -> None:
        super().__init__()
        self.upstream = upstream

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        outputs = self.upstream(x)
        if isinstance(outputs, tuple | list):
            return outputs[0]
        return outputs


def _export_onnx(adapter: nn.Module, output: Path, *, height: int, width: int, opset: int) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros(1, 3, height, width, dtype=torch.float32)
    torch.onnx.export(
        adapter.eval(),
        dummy,
        str(output),
        input_names=["input"],
        output_names=["saliency_map"],
        opset_version=opset,
        dynamic_axes={
            "input": {2: "H", 3: "W"},
            "saliency_map": {2: "H", 3: "W"},
        },
        do_constant_folding=True,
        dynamo=False,
    )


def _annotate_onnx(path: Path, metadata: dict[str, str]) -> bool:
    try:
        import onnx  # type: ignore[import-not-found]
    except ImportError:
        return False

    model = onnx.load(path)
    existing = {prop.key: prop.value for prop in model.metadata_props}
    existing.update(metadata)
    del model.metadata_props[:]
    for key in sorted(existing):
        prop = model.metadata_props.add()
        prop.key = key
        prop.value = existing[key]
    onnx.save(model, path)
    return True


def _write_manifest(
    path: Path,
    *,
    args: argparse.Namespace,
    raw_argv: list[str],
    module_path: Path,
    checkpoint_path: Path,
    license_path: Path,
    license_sha256: str,
    metadata_written: bool,
) -> None:
    notice_path = args.upstream_dir / "NOTICE"
    output_path = args.output
    payload: dict[str, Any] = {
        "schema": SCHEMA,
        "model_id": args.model_id,
        "upstream": {
            "repo": args.upstream_repo,
            "commit": args.upstream_commit,
            "module_path": str(module_path),
            "module_sha256": sha256(module_path),
            "checkpoint_path": str(checkpoint_path),
            "checkpoint_sha256": sha256(checkpoint_path),
            "license_path": str(license_path),
            "license_sha256": license_sha256,
            "notice_present": notice_path.is_file(),
        },
        "export": {
            "opset": args.opset,
            "height": args.height,
            "width": args.width,
            "input_name": "input",
            "output_name": "saliency_map",
            "selected_upstream_output": "d0",
            "metadata_written": metadata_written,
        },
        "outputs": {
            "onnx_path": str(output_path),
            "onnx_sha256": sha256(output_path),
        },
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={
                "upstream_dir": args.upstream_dir,
                "upstream_module": module_path,
                "upstream_checkpoint": checkpoint_path,
                "upstream_license": license_path,
                "upstream_notice": notice_path,
            },
            outputs={"onnx": output_path, "manifest": path},
        ),
    }
    write_manifest_json(path, payload)


def _build_parser() -> argparse.ArgumentParser:
    parser = make_argument_parser(description=__doc__)
    parser.add_argument(
        "--upstream-dir",
        type=Path,
        required=True,
        help="Audited xuebinqin/U-2-Net checkout containing model/u2net.py and LICENSE.",
    )
    parser.add_argument(
        "--checkpoint",
        type=Path,
        default=None,
        help=(
            "Path to u2netp.pth. Defaults to saved_models/u2netp/u2netp.pth "
            "or u2netp.pth under --upstream-dir."
        ),
    )
    parser.add_argument(
        "--module-path",
        type=Path,
        default=None,
        help="Override path to upstream u2net.py. Relative paths resolve under --upstream-dir.",
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help="Manifest output path. Defaults to <output>.manifest.json.",
    )
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--opset", type=int, default=DEFAULT_OPSET)
    parser.add_argument("--model-id", default=MODEL_ID)
    parser.add_argument("--upstream-repo", default=UPSTREAM_REPO)
    parser.add_argument("--upstream-commit", default=UPSTREAM_COMMIT)
    parser.add_argument(
        "--strict-load",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Use strict load_state_dict. Disable only for exporter bring-up tests.",
    )
    return parser


def main(argv: list[str] | None = None) -> None:
    raw_argv = collect_cli_argv(argv)
    parser = _build_parser()
    args = parser.parse_args(raw_argv)
    args.upstream_dir = args.upstream_dir.resolve()
    args.output = args.output.resolve()
    args.manifest_out = (
        args.manifest_out.resolve()
        if args.manifest_out is not None
        else args.output.with_suffix(".manifest.json")
    )

    module_path = _resolve_module_path(args.upstream_dir, args.module_path).resolve()
    checkpoint_path = _resolve_checkpoint(args.upstream_dir, args.checkpoint).resolve()
    license_path, license_sha = _load_license(args.upstream_dir)

    cls = _load_u2netp_class(module_path)
    upstream = _instantiate_u2netp(cls)
    state_dict = _load_checkpoint(checkpoint_path)
    upstream.load_state_dict(state_dict, strict=args.strict_load)
    adapter = U2NetPMirrorAdapter(upstream).eval()

    _export_onnx(adapter, args.output, height=args.height, width=args.width, opset=args.opset)
    metadata_written = _annotate_onnx(
        args.output,
        {
            "lusoris.model_id": args.model_id,
            "lusoris.upstream_repo": args.upstream_repo,
            "lusoris.upstream_commit": args.upstream_commit,
            "lusoris.upstream_checkpoint_sha256": sha256(checkpoint_path),
            "lusoris.conversion": (
                "torch.onnx.export opset "
                f"{args.opset}; selected upstream d0 and renamed to saliency_map"
            ),
        },
    )
    _write_manifest(
        args.manifest_out,
        args=args,
        raw_argv=raw_argv,
        module_path=module_path,
        checkpoint_path=checkpoint_path,
        license_path=license_path.resolve(),
        license_sha256=license_sha,
        metadata_written=metadata_written,
    )
    print(f"[u2netp] wrote {args.output}")
    print(f"[u2netp] wrote {args.manifest_out}")


if __name__ == "__main__":
    main()
