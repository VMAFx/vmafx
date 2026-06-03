#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Export the *real* upstream TransNet V2 shot-boundary detector into the
fork's [1, 100, 3, 27, 48] -> [1, 100] ONNX contract (T6-3a-followup).

This script supersedes ``export_transnet_v2_placeholder.py`` (T6-3a),
which shipped a tiny randomly-initialised MLP purely to pin the
contract.  Upstream TransNet V2 (Soucek & Lokoc 2020) is published
under the MIT license at github.com/soCzech/TransNetV2 pinned to commit
``77498b8`` (the master tip on 2026-05-03); the trained weights ship
under ``inference/transnetv2-weights/`` as a TensorFlow SavedModel
(saved_model.pb + variables/, ~30 MiB through git-LFS).

The fork's C-side extractor (``core/src/feature/transnet_v2.c``,
ADR-0223) was scoped against an [N, T, C, H, W] = [1, 100, 3, 27, 48]
input contract (NTCHW) plus a [1, 100] logits output. Upstream's
SavedModel takes [N, T, H, W, C] = [1, 100, 27, 48, 3] (NTHWC) and
returns *two* outputs ``output_1`` (single-frame logits, [1, 100, 1])
and ``output_2`` (the auxiliary "many_hot" output, also [1, 100, 1]).

The wrapper layer here:

    1. transposes inputs from NTCHW -> NTHWC (C-side packs RGB along
       the channel axis after height/width; upstream packs along the
       last axis),
    2. selects only ``output_1`` (single-frame shot-boundary logits),
    3. squeezes the trailing singleton dim so downstream sees [1, 100].

After ``tf2onnx`` conversion, one rank-2 ``UnsortedSegmentSum`` node in
upstream's ColorHistograms branch is rewritten as an equivalent
``ScatterND`` reduction='add' subgraph (standard ONNX 17 doesn't ship
``SegmentSum`` and ``UnsortedSegmentSum`` lowers to a rank-1-only op
in ``tf2onnx``). Numerical parity vs the TF SavedModel: max-abs-diff
< 4e-6 across 3 random 0..255 input trials (see ``--verify`` below).

Six new ops join the libvmaf op allowlist with this PR
(``BitShift``, ``GatherND``, ``Pad``, ``Reciprocal``, ``ReduceProd``,
``ScatterND``); each is a deterministic standard ONNX op with bounded
runtime cost. Rationale in ADR-0261.

Provenance (license attribution required by upstream MIT):

    Copyright 2020-2024 Tomas Soucek <tomas.soucek@matfyz.cuni.cz>
    https://github.com/soCzech/TransNetV2/blob/master/LICENSE

Usage::

    # 1. fetch upstream weights (LFS-tracked)
    git clone --depth=1 https://github.com/soCzech/TransNetV2.git \\
        /tmp/transnetv2_upstream
    git -C /tmp/transnetv2_upstream lfs pull \\
        -I inference/transnetv2-weights

    # 2. export
    python3 ai/scripts/export_transnet_v2.py \\
        --upstream-dir /tmp/transnetv2_upstream/inference/transnetv2-weights

Re-running is idempotent; the script overwrites ``model/tiny/transnet_v2.onnx``
and refreshes the registry sha256.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
from pathlib import Path

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

# Pinned upstream provenance — bumping these is a deliberate weights swap.
UPSTREAM_REPO = "https://github.com/soCzech/TransNetV2"
UPSTREAM_COMMIT = "77498b8e4a6d61ed7c3d9bd56f4de2b29ab7f4db"
UPSTREAM_WEIGHTS_VARIABLES_SHA256 = (
    "b8c9dc3eb807583e6215cabee9ca61737b3eb1bceff68418b43bf71459669367"
)
UPSTREAM_SAVED_MODEL_PB_SHA256 = "8ac2a52c5719690d512805b6eaf5ce12097c1d8860b3d9de245dcbbc3100f554"

WINDOW = 100
CHANNELS = 3
HEIGHT = 27
WIDTH = 48
NUM_HISTOGRAM_BINS = 51_200  # 100 frames * 512 bins per frame (RGB cube 8x8x8)


def _verify_upstream(upstream_dir: Path) -> None:
    """Sanity-check that the upstream weights match the pinned hashes."""
    saved_pb = upstream_dir / "saved_model.pb"
    var_data = upstream_dir / "variables" / "variables.data-00000-of-00001"
    if not saved_pb.is_file() or not var_data.is_file():
        sys.exit(
            f"upstream-dir {upstream_dir} is missing saved_model.pb / variables/.\n"
            "Did you run `git lfs pull -I inference/transnetv2-weights` after cloning?"
        )
    pb_sha = sha256(saved_pb)
    var_sha = sha256(var_data)
    if pb_sha != UPSTREAM_SAVED_MODEL_PB_SHA256:
        sys.exit(
            f"saved_model.pb sha256 mismatch:\n"
            f"  expected: {UPSTREAM_SAVED_MODEL_PB_SHA256}\n"
            f"  got:      {pb_sha}\n"
            f"Upstream commit may have moved; bump UPSTREAM_COMMIT after review."
        )
    if var_sha != UPSTREAM_WEIGHTS_VARIABLES_SHA256:
        sys.exit(
            f"variables.data sha256 mismatch:\n"
            f"  expected: {UPSTREAM_WEIGHTS_VARIABLES_SHA256}\n"
            f"  got:      {var_sha}"
        )


def _wrap_to_savedmodel(upstream_dir: Path, wrapped_dir: Path) -> None:
    """Wrap the upstream SavedModel: transpose NTCHW->NTHWC inputs and
    return only output_1 (single-frame shot logits) squeezed to [1, 100].

    Saves a fresh SavedModel under ``wrapped_dir`` so tf2onnx can pick
    up only this signature.
    """
    import shutil

    import tensorflow as tf  # local import — tensorflow is heavy

    base_model = tf.saved_model.load(str(upstream_dir))

    class Wrapper(tf.Module):
        def __init__(self, base):
            super().__init__()
            self.base = base

        @tf.function(
            input_signature=[
                tf.TensorSpec(
                    shape=[1, WINDOW, CHANNELS, HEIGHT, WIDTH],
                    dtype=tf.float32,
                    name="frames",
                )
            ]
        )
        def __call__(self, frames):
            # NTCHW -> NTHWC: axes 0,1,2,3,4 -> 0,1,3,4,2
            x = tf.transpose(frames, perm=[0, 1, 3, 4, 2])
            out = self.base.signatures["serving_default"](input_1=x)
            logits = out["output_1"]  # (1, 100, 1)
            return tf.squeeze(logits, axis=-1)  # (1, 100)

    if wrapped_dir.exists():
        shutil.rmtree(wrapped_dir)
    tf.saved_model.save(Wrapper(base_model), str(wrapped_dir))


def _convert_to_onnx(wrapped_dir: Path, onnx_path: Path, opset: int) -> None:
    """Run tf2onnx on the wrapped SavedModel.

    Uses ``--continue_on_error`` to skip the rank-2 SegmentSum node that
    tf2onnx can't lower; we splice an equivalent ScatterND subgraph in
    afterwards via ``_replace_segmentsum``.
    """
    import subprocess

    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        "-m",
        "tf2onnx.convert",
        "--saved-model",
        str(wrapped_dir),
        "--output",
        str(onnx_path),
        "--opset",
        str(opset),
        "--continue_on_error",
    ]
    env = dict(os.environ)
    env.setdefault("CUDA_VISIBLE_DEVICES", "-1")
    env.setdefault("TF_CPP_MIN_LOG_LEVEL", "3")
    subprocess.run(cmd, check=True, env=env)


def _replace_segmentsum(onnx_path: Path) -> None:
    """Splice ColorHistograms/UnsortedSegmentSum -> ScatterND.

    Original semantics (rank-2 segment IDs, num_segments=51200):

        output[51200] = zeros
        for i in range(100):
            for j in range(1296):
                output[ids[i, j]] += data[i, j]

    Equivalent ONNX rewrite:

        flat_ids   = Reshape(ids,  [-1, 1])
        flat_data  = Reshape(data, [-1])
        zeros      = ConstantOfShape([51200])
        output     = ScatterND(zeros, flat_ids, flat_data, reduction='add')

    onnxruntime CPU EP supports ScatterND with ``reduction='add'`` since
    opset 16; we target opset 17 here.
    """
    import numpy as np
    import onnx
    from onnx import TensorProto, helper, numpy_helper

    m = onnx.load(str(onnx_path))
    g = m.graph

    seg_node = None
    seg_idx = -1
    for i, n in enumerate(g.node):
        if n.op_type == "SegmentSum":
            seg_node = n
            seg_idx = i
            break
    if seg_node is None:
        # Already rewritten; idempotent re-run.
        return

    data_in, ids_in, num_seg_in = seg_node.input
    out_name = seg_node.output[0]

    num_seg = None
    for init in g.initializer:
        if init.name == num_seg_in:
            num_seg = int(numpy_helper.to_array(init))
            break
    if num_seg is None:
        sys.exit(f"SegmentSum num_segments {num_seg_in!r} not in initializers")
    if num_seg != NUM_HISTOGRAM_BINS:
        sys.exit(
            f"unexpected num_segments {num_seg}; "
            f"expected {NUM_HISTOGRAM_BINS} (100 frames * 512 bins)"
        )

    prefix = "fork_segmentsum_"
    new_inits = [
        numpy_helper.from_array(np.array([-1], dtype=np.int64), name=prefix + "neg1"),
        numpy_helper.from_array(np.array([-1, 1], dtype=np.int64), name=prefix + "neg1_1"),
        numpy_helper.from_array(np.array([num_seg], dtype=np.int64), name=prefix + "zeros_shape"),
    ]
    zero_int32_value = helper.make_tensor(prefix + "zero_int32_value", TensorProto.INT32, [1], [0])

    new_nodes = [
        helper.make_node(
            "Reshape",
            inputs=[data_in, prefix + "neg1"],
            outputs=[prefix + "flat_data"],
            name=prefix + "flat_data_node",
        ),
        helper.make_node(
            "Reshape",
            inputs=[ids_in, prefix + "neg1_1"],
            outputs=[prefix + "flat_ids"],
            name=prefix + "flat_ids_node",
        ),
        helper.make_node(
            "Cast",
            inputs=[prefix + "flat_ids"],
            outputs=[prefix + "flat_ids_i64"],
            to=TensorProto.INT64,
            name=prefix + "flat_ids_cast",
        ),
        helper.make_node(
            "ConstantOfShape",
            inputs=[prefix + "zeros_shape"],
            outputs=[prefix + "zeros"],
            value=zero_int32_value,
            name=prefix + "zeros_node",
        ),
        helper.make_node(
            "ScatterND",
            inputs=[prefix + "zeros", prefix + "flat_ids_i64", prefix + "flat_data"],
            outputs=[out_name],
            reduction="add",
            name=prefix + "scatter",
        ),
    ]

    final_nodes = list(g.node)
    final_nodes.pop(seg_idx)
    final_nodes[seg_idx:seg_idx] = new_nodes

    new_graph = helper.make_graph(
        final_nodes,
        g.name,
        list(g.input),
        list(g.output),
        list(g.initializer) + new_inits,
        value_info=list(g.value_info),
    )
    new_model = helper.make_model(
        new_graph,
        opset_imports=list(m.opset_import),
        producer_name="vmaf-fork-transnet-v2-export",
    )
    new_model.ir_version = m.ir_version
    onnx.checker.check_model(new_model)
    onnx.save(new_model, str(onnx_path))


def _verify_op_allowlist(onnx_path: Path) -> None:
    """Cross-check the exported graph against libvmaf's op allowlist."""
    from vmaf_train.op_allowlist import check_model  # type: ignore

    report = check_model(onnx_path)
    if not report.ok:
        sys.exit(f"op-allowlist check failed: {report.pretty()}")
    print(f"[verify] op-allowlist OK ({len(report.used)} distinct ops)")


def _verify_parity(onnx_path: Path, wrapped_sm_dir: Path, *, trials: int = 3) -> None:
    """Run ONNX vs TF and assert max-abs-diff < 1e-4 across `trials` random
    inputs (paper notes 3D conv accumulation > 1e-6 is realistic on FP32)."""
    import numpy as np
    import onnxruntime as ort
    import tensorflow as tf

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    loaded = tf.saved_model.load(str(wrapped_sm_dir))
    sig = loaded.signatures["serving_default"]

    rng = np.random.default_rng(seed=0xABCDEF)
    worst = 0.0
    for trial in range(trials):
        inp = (rng.random((1, WINDOW, CHANNELS, HEIGHT, WIDTH)) * 255.0).astype(np.float32)
        onnx_out = sess.run(None, {"frames": inp})[0]
        tf_out = sig(frames=tf.constant(inp))["output_0"].numpy()
        diff = float(np.max(np.abs(onnx_out - tf_out)))
        worst = max(worst, diff)
        print(f"[parity] trial {trial}: max-abs-diff = {diff:.3e}")
    if worst >= 1e-4:
        sys.exit(f"parity check FAILED: worst max-abs-diff {worst:.3e} >= 1e-4")
    print(f"[parity] worst max-abs-diff {worst:.3e} < 1e-4 -> OK")


def _write_sidecar(onnx_path: Path, *, run_provenance: dict[str, object] | None = None) -> Path:
    sidecar = onnx_path.with_suffix(".json")
    payload = {
        "id": "transnet_v2",
        "kind": "shot_detector",
        "onnx": onnx_path.name,
        "opset": 17,
        "input_name": "frames",
        "output_name": "boundary_logits",
        "frame_window": WINDOW,
        "thumbnail_h": HEIGHT,
        "thumbnail_w": WIDTH,
        "channels": CHANNELS,
        "boundary_threshold": 0.5,
        "smoke": False,
        "name": "vmaf_tiny_transnet_v2_v1",
        "license": "MIT",
        "license_url": (f"{UPSTREAM_REPO}/blob/{UPSTREAM_COMMIT}/LICENSE"),
        "upstream_repo": UPSTREAM_REPO,
        "upstream_commit": UPSTREAM_COMMIT,
        "notes": (
            "TransNet V2 shot-boundary detector (T6-3a-followup). "
            "100-frame window of 27x48 RGB thumbnails -> per-frame "
            "shot-boundary logits. Real upstream weights from "
            "Soucek & Lokoc 2020 (MIT). "
            "Wrapper transposes NTCHW->NTHWC and selects only the "
            "single-frame logits output; ColorHistograms's rank-2 "
            "UnsortedSegmentSum rewritten to ScatterND for ONNX "
            "opset-17 compatibility. See ADR-0261 + "
            "docs/ai/models/transnet_v2.md."
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
        "id": "transnet_v2",
        "kind": "nr",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "license": "MIT",
        "license_url": f"{UPSTREAM_REPO}/blob/{UPSTREAM_COMMIT}/LICENSE",
        "description": (
            "TransNet V2 shot-boundary detector (Soucek & Lokoc 2020) — "
            "100-frame window of 27x48 RGB thumbnails -> per-frame logits."
        ),
        "notes": (
            "TransNet V2 shot-boundary detector (T6-3a-followup). "
            "100-frame window of 27x48 RGB thumbnails -> per-frame "
            "shot-boundary logits. Real upstream weights from Soucek & "
            f"Lokoc 2020 (MIT, {UPSTREAM_REPO} commit "
            f"{UPSTREAM_COMMIT[:12]}). Wrapper transposes NTCHW->NTHWC and "
            "selects only the single-frame logits; ColorHistograms's "
            "rank-2 UnsortedSegmentSum rewritten as ScatterND for ONNX "
            "opset-17 compatibility. Per-shot CRF predictor is T6-3b. "
            "See docs/adr/0223-transnet-v2-shot-detector.md, "
            "docs/adr/0261-transnet-v2-real-weights.md, and "
            "docs/ai/models/transnet_v2.md."
        ),
    }
    by_id["transnet_v2"] = entry
    models = sorted(by_id.values(), key=lambda m: m["id"])
    doc["models"] = models
    REGISTRY.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n")


def main(argv: list[str] | None = None) -> None:
    parser = make_argument_parser(description=__doc__)
    parser.add_argument(
        "--upstream-dir",
        type=Path,
        required=True,
        help="Path to upstream transnetv2-weights/ (contains saved_model.pb + variables/)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=TINY_DIR / "transnet_v2.onnx",
        help="ONNX output path (default: model/tiny/transnet_v2.onnx)",
    )
    scratch_root = Path(os.environ.get("VMAF_TINY_AI_SCRATCH", tempfile.gettempdir()))
    parser.add_argument(
        "--wrapped-savedmodel",
        type=Path,
        default=scratch_root / "transnetv2_wrapped_sm",
        help=(
            "Scratch directory for the wrapped SavedModel "
            "(default: $VMAF_TINY_AI_SCRATCH/transnetv2_wrapped_sm)."
        ),
    )
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument(
        "--no-registry",
        action="store_true",
        help="Skip registry.json + sidecar update (dry-run)",
    )
    parser.add_argument(
        "--skip-verify",
        action="store_true",
        help="Skip op-allowlist + TF parity verification",
    )
    raw_argv = collect_cli_argv(argv)
    args = parser.parse_args(raw_argv)

    _verify_upstream(args.upstream_dir)
    print(f"[upstream] verified saved_model.pb + variables under {args.upstream_dir}")

    _wrap_to_savedmodel(args.upstream_dir, args.wrapped_savedmodel)
    print(f"[wrap] wrote {args.wrapped_savedmodel}")

    _convert_to_onnx(args.wrapped_savedmodel, args.output, args.opset)
    print(f"[convert] wrote {args.output}")

    _replace_segmentsum(args.output)
    print("[rewrite] spliced ScatterND in place of SegmentSum")

    if not args.skip_verify:
        _verify_op_allowlist(args.output)
        _verify_parity(args.output, args.wrapped_savedmodel)

    if args.no_registry:
        return

    sidecar = _write_sidecar(
        args.output,
        run_provenance=build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={
                "upstream_dir": args.upstream_dir,
                "saved_model_pb": args.upstream_dir / "saved_model.pb",
                "variables_data": args.upstream_dir / "variables" / "variables.data-00000-of-00001",
            },
            outputs={
                "wrapped_savedmodel": args.wrapped_savedmodel,
                "onnx": args.output,
                "sidecar": args.output.with_suffix(".json"),
                "registry": REGISTRY,
            },
        ),
    )
    print(f"[sidecar] wrote {sidecar}")
    _update_registry(args.output)
    print(f"[registry] updated {REGISTRY}")


if __name__ == "__main__":
    main()
