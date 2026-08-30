#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Export a tiny placeholder ONNX checkpoint for the FastDVDnet temporal
pre-filter (T6-7).

This script does NOT reproduce FastDVDnet's published weights. It produces
a small randomly-initialised CNN whose graph respects the 5-frame-window
contract (input ``[1, 5, 1, H, W]`` packed into ``[1, 5, H, W]`` along the
channel dim, output ``[1, 1, H, W]``). The output is a smoke-only model
shipped to exercise the load / session / per-frame-buffer plumbing that
the eventual real-weights drop in T6-7b will inherit.

Why a placeholder rather than upstream weights:

* upstream ``github.com/m-tassano/fastdvdnet`` distributes a PyTorch
  checkpoint under MIT-license-compatible terms but is not pinned to a
  release tag we can vendor reproducibly without manual download;
* training a fresh checkpoint locally takes hours and is out-of-scope for
  one PR (cost item ADR-0210 §Cost);
* shipping a placeholder unblocks every other piece of the integration
  (registry schema, C extractor, smoke test, docs, ADR) and hard-codes
  the contract that T6-7b only has to swap weights against.

Usage::

    python3 ai/scripts/export_fastdvdnet_pre_placeholder.py \
        --output model/tiny/fastdvdnet_pre.onnx \
        --height 64 --width 64

Re-running is idempotent.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import torch
from torch import nn

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root
TINY_DIR = REPO_ROOT / "model" / "tiny"
REGISTRY = TINY_DIR / "registry.json"

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.file_utils import sha256  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402


class FastDVDnetPlaceholder(nn.Module):
    """Tiny CNN with the FastDVDnet 5-frame window I/O contract.

    Input:  ``(1, 5, H, W)`` — five consecutive luma planes stacked along
            the channel dimension. Frame layout is
            ``[t-2, t-1, t, t+1, t+2]``; only frame ``t`` is denoised.
    Output: ``(1, 1, H, W)`` — denoised luma for frame ``t``, range [0, 1].

    The architecture is deliberately tiny (~3K params) because this is a
    smoke fixture, not a quality model. T6-7b will replace the weights
    against a real FastDVDnet checkpoint while keeping the same I/O shape.
    """

    def __init__(self) -> None:
        super().__init__()
        self.conv1 = nn.Conv2d(5, 8, kernel_size=3, padding=1)
        self.conv2 = nn.Conv2d(8, 8, kernel_size=3, padding=1)
        self.conv3 = nn.Conv2d(8, 1, kernel_size=3, padding=1)
        self.act = nn.ReLU(inplace=False)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # Residual connection on the centre frame so an untrained
        # placeholder still produces a sane output (≈ identity on
        # frame t, perturbed by the tiny CNN). This makes downstream
        # smoke assertions meaningful: the output is never NaN, never
        # constant, and stays in [0, 1] after clamp.
        centre = x[:, 2:3, :, :]
        h = self.act(self.conv1(x))
        h = self.act(self.conv2(h))
        residual = self.conv3(h)
        out = torch.clamp(centre + 0.05 * residual, 0.0, 1.0)
        return out


def _export(onnx_path: Path, height: int, width: int, opset: int) -> None:
    torch.manual_seed(0xFA57DEAD)
    model = FastDVDnetPlaceholder().eval()
    dummy = torch.zeros(1, 5, height, width, dtype=torch.float32)
    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    # Use the legacy TorchScript exporter (dynamo=False) so the resulting
    # graph is a single self-contained file under ONNX-Runtime's strict
    # op allowlist (`Conv`, `Relu`, `Slice`, `Mul`, `Add`, `Clip`).
    torch.onnx.export(
        model,
        dummy,
        str(onnx_path),
        input_names=["frames"],
        output_names=["denoised"],
        opset_version=opset,
        dynamic_axes={
            "frames": {2: "H", 3: "W"},
            "denoised": {2: "H", 3: "W"},
        },
        do_constant_folding=True,
        dynamo=False,
    )


def _write_sidecar(onnx_path: Path, *, run_provenance: dict[str, object] | None = None) -> Path:
    sidecar = onnx_path.with_suffix(".json")
    payload = {
        "id": "fastdvdnet_pre",
        "kind": "filter",
        "onnx": onnx_path.name,
        "opset": 17,
        "input_name": "frames",
        "output_name": "denoised",
        "frame_window": 5,
        "centre_index": 2,
        "smoke": True,
        "name": "vmaf_tiny_fastdvdnet_pre_placeholder_v0",
        "notes": (
            "Placeholder FastDVDnet temporal pre-filter weights "
            "(T6-7). 5-frame window [t-2, t-1, t, t+1, t+2] -> "
            "denoised frame t. SMOKE-ONLY: random init, not "
            "trained. Real upstream weights tracked under T6-7b. "
            "See docs/ai/models/fastdvdnet_pre.md."
        ),
    }
    if run_provenance is not None:
        payload["run_provenance"] = run_provenance
    write_manifest_json(sidecar, payload)
    return sidecar


def _update_registry(onnx_path: Path) -> None:
    if not REGISTRY.exists():
        sys.exit(f"missing {REGISTRY}")
    doc = json.loads(REGISTRY.read_text())
    models: list[dict] = doc.get("models", [])
    by_id = {m["id"]: m for m in models}
    digest = sha256(onnx_path)
    entry = {
        "id": "fastdvdnet_pre",
        "kind": "filter",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "smoke": True,
        "notes": (
            "Placeholder FastDVDnet temporal pre-filter (T6-7). 5-frame "
            "window [t-2, t-1, t, t+1, t+2] -> denoised frame t. "
            "SMOKE-ONLY: random init, not trained. Real upstream weights "
            "tracked under T6-7b. See docs/adr/0210-fastdvdnet-pre-filter.md "
            "and docs/ai/models/fastdvdnet_pre.md."
        ),
    }
    by_id["fastdvdnet_pre"] = entry
    # Sort models by id for stable diffs (matches existing layout).
    models = sorted(by_id.values(), key=lambda m: m["id"])
    doc["models"] = models
    REGISTRY.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")


def main(argv: list[str] | None = None) -> None:
    parser = make_argument_parser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=TINY_DIR / "fastdvdnet_pre.onnx",
        help="ONNX output path (default: model/tiny/fastdvdnet_pre.onnx)",
    )
    parser.add_argument("--height", type=int, default=64)
    parser.add_argument("--width", type=int, default=64)
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument(
        "--no-registry",
        action="store_true",
        help="Skip registry.json + sidecar update (dry-run)",
    )
    raw_argv = collect_cli_argv(argv)
    args = parser.parse_args(raw_argv)

    if args.no_registry:
        return
    _export(args.output, args.height, args.width, args.opset)
    print(f"[export] wrote {args.output} ({args.output.stat().st_size} bytes)")
    sidecar = _write_sidecar(
        args.output,
        run_provenance=build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            outputs={
                "onnx": args.output,
                "sidecar": args.output.with_suffix(".json"),
                "registry": REGISTRY,
            },
        ),
    )
    print(f"[export] wrote {sidecar}")
    _update_registry(args.output)
    print(f"[export] updated {REGISTRY}")


if __name__ == "__main__":
    main()
