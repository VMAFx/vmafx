#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Summarise in-tree training artefacts into discovery candidates.

The script reads committed sidecars and model cards only. It does not
open ONNX files, local corpora, or gitignored run directories, so the
output is reproducible on a clean checkout.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class PredictorCard:
    codec: str
    corpus_kind: str
    plcc: float
    srocc: float
    rmse: float
    path: Path

    @property
    def family(self) -> str:
        if "_" not in self.codec:
            return self.codec
        return self.codec.rsplit("_", 1)[-1]


def _load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _float_or_none(value: Any) -> float | None:
    if isinstance(value, int | float):
        return float(value)
    return None


def _extract_metric_table_value(text: str, metric: str) -> float:
    match = re.search(rf"^\|\s*{re.escape(metric)}\s*\|\s*([0-9.]+)", text, re.MULTILINE)
    if match is None:
        raise ValueError(f"missing {metric} metric")
    return float(match.group(1))


def _extract_bold_field(text: str, label: str) -> str:
    match = re.search(rf"^- \*\*{re.escape(label)}\*\*:\s*`?([^`\n]+)`?", text, re.MULTILINE)
    if match is None:
        raise ValueError(f"missing {label} field")
    return match.group(1).strip()


def load_predictor_cards(repo_root: Path) -> list[PredictorCard]:
    cards: list[PredictorCard] = []
    for path in sorted((repo_root / "model").glob("predictor_*_card.md")):
        text = path.read_text(encoding="utf-8")
        if "synthetic-stub model" in text:
            raise ValueError(
                f"{path.name}: synthetic-stub model cards are not allowed; "
                "train against a real corpus and regenerate the card."
            )
        cards.append(
            PredictorCard(
                codec=_extract_bold_field(text, "Codec adapter"),
                corpus_kind=_extract_bold_field(text, "Corpus kind"),
                plcc=_extract_metric_table_value(text, "PLCC"),
                srocc=_extract_metric_table_value(text, "SROCC"),
                rmse=_extract_metric_table_value(text, "RMSE"),
                path=path.relative_to(repo_root),
            )
        )
    return cards


def _markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    out = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    out.extend("| " + " | ".join(row) + " |" for row in rows)
    return "\n".join(out)


def _fmt(value: float | None, digits: int = 4) -> str:
    if value is None:
        return "-"
    return f"{value:.{digits}f}"


def _tiny_fr_rows(repo_root: Path) -> list[list[str]]:
    rows: list[list[str]] = []
    for name in ("fr_regressor_v2.json", "fr_regressor_v3.json"):
        path = repo_root / "model" / "tiny" / name
        data = _load_json(path)
        training = data.get("training", {})
        rows.append(
            [
                data["id"],
                str(training.get("n_rows", "-")),
                _fmt(
                    _float_or_none(training.get("loso_mean_plcc", training.get("in_sample_plcc")))
                ),
                _fmt(
                    _float_or_none(training.get("loso_mean_srocc", training.get("in_sample_srocc")))
                ),
                _fmt(
                    _float_or_none(training.get("loso_mean_rmse", training.get("in_sample_rmse")))
                ),
                "LOSO" if "loso_mean_plcc" in training else "in-sample",
            ]
        )
    promote = _load_json(
        repo_root / "model" / "tiny" / "fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json"
    )
    gate = promote["gate"]
    rows.append(
        [
            "fr_regressor_v2_ensemble_v1",
            "-",
            _fmt(_float_or_none(gate.get("mean_plcc"))),
            "-",
            "-",
            f"LOSO ensemble, spread={gate['plcc_spread']:.6f}",
        ]
    )
    return rows


def _saliency_rows(repo_root: Path) -> list[list[str]]:
    rows: list[list[str]] = []
    for name in ("saliency_student_v1.json", "saliency_student_v2.json"):
        path = repo_root / "model" / "tiny" / name
        data = _load_json(path)
        training = data["training"]
        rows.append(
            [
                name.removesuffix(".json"),
                _fmt(_float_or_none(training.get("best_val_iou"))),
                str(training.get("param_count", "-")),
                str(training.get("decoder_upsampler", "ConvTranspose decoder")),
            ]
        )
    return rows


def _predictor_rows(cards: list[PredictorCard]) -> list[list[str]]:
    sorted_cards = sorted(cards, key=lambda card: (card.family, card.codec))
    return [
        [
            card.codec,
            card.corpus_kind,
            _fmt(card.plcc),
            _fmt(card.srocc),
            _fmt(card.rmse),
            str(card.path),
        ]
        for card in sorted_cards
    ]


def _qsv_nvenc_delta_rows(cards: list[PredictorCard]) -> list[list[str]]:
    real_cards = {card.codec: card for card in cards}
    rows: list[list[str]] = []
    for codec in ("h264", "hevc", "av1"):
        nvenc = real_cards.get(f"{codec}_nvenc")
        qsv = real_cards.get(f"{codec}_qsv")
        if nvenc is None or qsv is None:
            continue
        rows.append(
            [
                codec,
                _fmt(nvenc.plcc),
                _fmt(qsv.plcc),
                f"{qsv.plcc - nvenc.plcc:+.4f}",
                _fmt(nvenc.rmse),
                _fmt(qsv.rmse),
            ]
        )
    return rows


def render_report(repo_root: Path) -> str:
    cards = load_predictor_cards(repo_root)
    parts = [
        "# Training Discovery Report",
        "",
        "Generated from committed model sidecars and model cards.",
        "",
        "## Tiny FR Regressors",
        "",
        _markdown_table(
            ["Model", "Rows", "PLCC", "SROCC", "RMSE", "Evidence"],
            _tiny_fr_rows(repo_root),
        ),
        "",
        "## Saliency Students",
        "",
        _markdown_table(
            ["Model", "Best val IoU", "Params", "Decoder"],
            _saliency_rows(repo_root),
        ),
        "",
        "## Real Hardware Predictor Cards",
        "",
        _markdown_table(
            ["Codec", "Corpus", "PLCC", "SROCC", "RMSE", "Card"],
            _predictor_rows(cards),
        ),
        "",
        "## QSV vs NVENC Predictor Delta",
        "",
        _markdown_table(
            ["Codec family", "NVENC PLCC", "QSV PLCC", "Delta", "NVENC RMSE", "QSV RMSE"],
            _qsv_nvenc_delta_rows(cards),
        ),
        "",
    ]
    return "\n".join(parts)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=REPO_ROOT,
        help=f"repository root (default: {REPO_ROOT})",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="write markdown report to this path instead of stdout",
    )
    args = parser.parse_args(argv)

    report = render_report(args.repo_root.resolve())
    if args.output is None:
        print(report, end="")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
