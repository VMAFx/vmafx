"""Gather all verifiable facts about a shipped tiny-AI model.

The LLM writes the prose, but it is not allowed to invent facts. This
module produces the structured "Collected facts" block that
`/dev-llm-modelcard` hands to the model — if a field is absent here, it
must not appear in the card.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass, field
from pathlib import Path


FEATURE_COLUMNS = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)


@dataclass
class ModelFacts:
    name: str
    onnx_path: str
    sidecar_path: str | None
    sha256: str
    byte_size: int
    schema_version: int | None = None
    kind: str | None = None
    input_names: list[str] = field(default_factory=list)
    output_names: list[str] = field(default_factory=list)
    input_shape: list[int | str] = field(default_factory=list)
    opset: int | None = None
    train_commit: str | None = None
    train_config: str | None = None
    manifest: str | None = None
    dataset: str | None = None
    license: str | None = None
    normalization: dict | None = None
    feature_columns: list[str] = field(default_factory=list)
    feature_contract_ok: bool | None = None
    op_allowlist_status: str | None = None
    forbidden_ops: list[str] = field(default_factory=list)
    cross_backend_status: str | None = None
    cross_backend_max_err: float | None = None
    eval_report: dict | None = None

    def to_markdown(self) -> str:
        """Render the dataclass as a compact key: value block for the LLM."""
        d = asdict(self)
        lines: list[str] = []
        for k, v in d.items():
            if v is None or v == [] or v == {}:
                continue
            if isinstance(v, (list, dict)):
                lines.append(f"{k}: {json.dumps(v)}")
            else:
                lines.append(f"{k}: {v}")
        return "\n".join(lines)


def _hash(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _locate_sidecar(onnx_path: Path) -> Path | None:
    sidecar = onnx_path.with_suffix(".json")
    return sidecar if sidecar.is_file() else None


def _parse_onnx(onnx_path: Path, facts: ModelFacts) -> None:
    try:
        import onnx  # type: ignore[import-not-found]
    except ImportError:
        return
    model = onnx.load(str(onnx_path))
    g = model.graph
    facts.input_names = [i.name for i in g.input]
    facts.output_names = [o.name for o in g.output]
    if g.input:
        dims = []
        for d in g.input[0].type.tensor_type.shape.dim:
            dims.append(d.dim_value if d.dim_value else (d.dim_param or "?"))
        facts.input_shape = dims
    if model.opset_import:
        facts.opset = int(model.opset_import[0].version)


def _parse_sidecar(sidecar: Path, facts: ModelFacts) -> None:
    meta = json.loads(sidecar.read_text())
    facts.schema_version = meta.get("schema_version")
    facts.kind = meta.get("kind")
    facts.train_commit = meta.get("train_commit")
    facts.train_config = meta.get("train_config")
    facts.manifest = meta.get("manifest")
    facts.dataset = meta.get("dataset")
    facts.license = meta.get("license")
    norm = meta.get("normalization") or {}
    if norm.get("mean") and norm.get("std"):
        facts.normalization = {"mean": norm["mean"], "std": norm["std"]}
    # Sidecar may carry declared input/output names; prefer the real graph values
    # over the sidecar's declaration but record them if the graph didn't give us
    # any (e.g., ONNX not installed in this env).
    if not facts.input_names and meta.get("input_names"):
        facts.input_names = list(meta["input_names"])
    if not facts.output_names and meta.get("output_names"):
        facts.output_names = list(meta["output_names"])


def _check_feature_contract(facts: ModelFacts) -> None:
    if facts.kind != "fr":
        return
    # FR input is (N, F). Find the feature dim.
    if len(facts.input_shape) >= 2:
        feat_dim = facts.input_shape[-1]
        if isinstance(feat_dim, int):
            facts.feature_columns = list(FEATURE_COLUMNS[:feat_dim])
            facts.feature_contract_ok = feat_dim == len(FEATURE_COLUMNS)


def _check_op_allowlist(onnx_path: Path, facts: ModelFacts, repo_root: Path) -> None:
    allow_src = repo_root / "libvmaf" / "src" / "dnn" / "op_allowlist.c"
    if not allow_src.is_file():
        return
    try:
        import onnx  # type: ignore[import-not-found]
    except ImportError:
        return
    import re

    text = allow_src.read_text()
    # Match both `allowed_ops[]` and upstream's `ALLOWED_OPS[]`.
    m = re.search(
        r"(?i)[A-Z_]*ALLOWED_OPS[A-Z_]*\s*\[\s*\]\s*=\s*\{([^}]*)\}", text, re.S
    )
    if not m:
        return
    allowed = set(re.findall(r'"([A-Za-z][A-Za-z0-9_]*)"', m.group(1)))
    model = onnx.load(str(onnx_path))
    used = {n.op_type for n in model.graph.node}
    forbidden = sorted(used - allowed)
    facts.forbidden_ops = forbidden
    facts.op_allowlist_status = "ok" if not forbidden else "forbidden ops present"


def _run_eval(
    onnx_path: Path, parquet: Path, facts: ModelFacts, split: str = "test"
) -> None:
    try:
        import numpy as np
        import onnxruntime as ort
        import pandas as pd
        from scipy.stats import pearsonr, spearmanr
    except ImportError:
        return
    df = pd.read_parquet(parquet)
    if "mos" not in df.columns:
        return
    if "key" in df.columns and split != "all":
        # Same hash scheme as vmaf_train.data.splits
        import hashlib as _h

        def bucket(k: str) -> float:
            h = _h.sha256(f"vmaf-train-splits-v1:{k}".encode()).digest()
            return int.from_bytes(h[:8], "big") / (1 << 64)

        which = df["key"].astype(str).map(
            lambda k: "test" if bucket(k) < 0.1 else "val" if bucket(k) < 0.2 else "train"
        )
        df = df[which == split]
    cols = [c for c in FEATURE_COLUMNS if c in df.columns]
    if not cols or len(df) < 2:
        return
    x = df[cols].to_numpy(dtype=np.float32)
    y = df["mos"].to_numpy(dtype=np.float32)
    input_name = facts.input_names[0] if facts.input_names else "features"
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    pred = np.asarray(sess.run(None, {input_name: x})[0]).reshape(-1)
    if pred.shape != y.shape:
        return
    facts.eval_report = {
        "split": split,
        "n": int(len(x)),
        "plcc": float(pearsonr(pred, y).statistic),
        "srocc": float(spearmanr(pred, y).statistic),
        "rmse": float(np.sqrt(((pred - y) ** 2).mean())),
    }


def collect_facts(
    onnx_path: Path,
    repo_root: Path | None = None,
    features: Path | None = None,
    split: str = "test",
) -> ModelFacts:
    """Gather every verifiable fact about @p onnx_path into one struct.

    Optional dependencies (onnx, onnxruntime, pandas, scipy) are handled
    per-step: each check no-ops if its backing library isn't available,
    so the generator still works in a minimal install.
    """
    facts = ModelFacts(
        name=onnx_path.stem,
        onnx_path=str(onnx_path),
        sidecar_path=None,
        sha256=_hash(onnx_path),
        byte_size=onnx_path.stat().st_size,
    )
    sidecar = _locate_sidecar(onnx_path)
    if sidecar is not None:
        facts.sidecar_path = str(sidecar)
        _parse_sidecar(sidecar, facts)
    _parse_onnx(onnx_path, facts)
    _check_feature_contract(facts)
    if repo_root is not None:
        _check_op_allowlist(onnx_path, facts, repo_root)
    if features is not None:
        _run_eval(onnx_path, features, facts, split=split)
    return facts
