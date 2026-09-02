# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""x264 / x265 FFmpeg pass-1 stats-file parser.

Captures encoder-internal per-frame signal that x264 already emits during
a single ``--pass 1`` / ``-pass 1 -passlogfile <prefix>`` encode. The
stats file is the encoder's own rate-distortion ledger — predicted
bitrate, QP, motion-vector cost, texture cost, and macroblock-type
counts. These are exactly the signals the encoder considered when making
its rate-distortion decisions, so feeding them back into the predictor
closes the loop on what the encoder's *own* RC engine saw, not just on
what the input pixels look like (research digest 0086, 2026-05-08).

Formats (verified empirically against x264 0.165.r3214 and x265 4.1 via
``ffmpeg -pass 1 -passlogfile <prefix>`` / ``-x265-params
pass=1:stats=<path>`` on tiny ``testsrc`` clips). The on-disk files are
line-oriented UTF-8 with layouts like:

    #options: <space-separated key=value config record>
    in:0 out:0 type:I dur:2 cpbdur:2 q:25.23 aq:20.39 tex:14051 mv:1126 misc:5871 imb:80 pmb:0 smb:0 d:- ref:;
    in:1 out:1 type:P dur:2 cpbdur:2 q:25.23 aq:23.76 tex:2595 mv:151 misc:182 imb:0 pmb:33 smb:47 d:- ref:0 ;
    in:3 out:4 type:b dur:2 cpbdur:2 q:25.23 aq:14.02 tex:58   mv:3   misc:115 imb:0 pmb:1  smb:79 d:- ref:0 ;
    ...

    #options: <space-separated key=value config record>
    in:0 out:0 type:I q:27.83 q-aq:28.41 q-noVbv:27.83 q-Rceq:0.53 tex:11836 mv:1752 misc:129 icu:80.00 pcu:0.00 scu:0.00 sc:0 ;
    in:1 out:1 type:P q:27.83 q-aq:28.71 q-noVbv:27.83 q-Rceq:0.53 tex:1317  mv:200  misc:191 icu:0.53  pcu:12.00 scu:67.47 sc:0 ;
    ...

One frame per line; tokens are space-separated ``key:value`` pairs with
``key=value`` reserved for the ``#options`` header. Per x264's source
(`encoder/ratecontrol.c`, the ``parse_zone`` /
``rate_estimate_qscale`` / pass-1 writer paths), the meaning of each
token is:

    in       — input frame index (display order)
    out      — output frame index (coded order)
    type     — frame slice type. ``I`` = IDR, ``i`` = non-IDR I, ``P`` =
               P, ``B`` = reference B, ``b`` = non-reference B
    dur      — frame duration in timebase units
    cpbdur   — CPB-domain duration
    q        — quantizer (the ``QP`` the encoder picked, post-AQ)
    aq       — adaptive-quantization variance signal driving the
               per-macroblock QP offsets. x265 spells this ``q-aq``.
    tex      — texture bits. For I/i frames: intra-texture cost. For
               P/B/b frames: predicted-texture cost (post-MC residual)
    mv       — motion-vector bits (zero for I-frames)
    misc     — header / overhead bits
    imb      — intra macroblock count. x265 spells this ``icu`` and
               emits fractional CTU counts.
    pmb      — predicted (inter) macroblock count. x265: ``pcu``.
    smb      — skipped macroblock count. x265: ``scu``.
    d        — direct-mode flag (``-`` = N/A)
    ref      — reference list (semicolon-terminated)

Total per-frame bits ≈ ``tex + mv + misc``. The parser keeps the raw
fields in :class:`PerFrameStats` and exposes ten sweep-friendly scalar
aggregates via :func:`aggregate_stats`:

    enc_internal_qp_mean / qp_std        — mean / std QP across frames
    enc_internal_bits_mean / bits_std    — mean / std (tex+mv+misc) bits
    enc_internal_mv_mean / mv_std        — mean / std MV-bit cost
    enc_internal_itex_mean               — mean intra-texture cost (I/i)
    enc_internal_ptex_mean               — mean predicted-texture cost (P/B/b)
    enc_internal_intra_ratio             — fraction of MBs coded as intra
    enc_internal_skip_ratio              — fraction of MBs coded as skip

The ``itex`` / ``ptex`` split mirrors the task spec — both come from the
single per-frame ``tex`` token in the stats file, partitioned by the
frame's ``type``. ``intra_ratio`` and ``skip_ratio`` are corpus-wide
averages of ``imb / (imb + pmb + smb)`` and ``smb / (imb + pmb + smb)``.

The parser intentionally covers the text pass-1 formats emitted by x264
and x265. libvpx's first-pass stats use a different binary layout (see
``vpx_codec_pkt_t`` / ``VPX_CODEC_STATS_PKT``) and remain opt-out until a
binary packet parser lands. Predictor integration (consuming the new
corpus columns) lands in a follow-up.
"""

from __future__ import annotations

import dataclasses
import math
from collections.abc import Iterable, Sequence
from pathlib import Path

# Public scalar column names — kept in lock-step with the corpus
# row-key tuple in ``vmaftune.__init__``. Keep the order stable so
# downstream consumers can rely on positional iteration.
ENCODER_STATS_COLUMNS: tuple[str, ...] = (
    "enc_internal_qp_mean",
    "enc_internal_qp_std",
    "enc_internal_bits_mean",
    "enc_internal_bits_std",
    "enc_internal_mv_mean",
    "enc_internal_mv_std",
    "enc_internal_itex_mean",
    "enc_internal_ptex_mean",
    "enc_internal_intra_ratio",
    "enc_internal_skip_ratio",
)


@dataclasses.dataclass(frozen=True)
class PerFrameStats:
    """One x264 stats-file frame record.

    Maps 1:1 to a non-comment line in the stats file. Numeric fields are
    coerced to ``float`` (``q``, ``aq``, coding-unit counts) or ``int``
    (bit costs and frame indexes); unknown / missing tokens default to
    zero so the aggregator never raises on a partial row.
    """

    in_idx: int
    out_idx: int
    frame_type: str  # one of {"I", "i", "P", "B", "b"}
    qp: float
    aq: float
    tex: int
    mv: int
    misc: int
    imb: float
    pmb: float
    smb: float

    @property
    def bits(self) -> int:
        """Total per-frame bits — tex + mv + misc."""
        return self.tex + self.mv + self.misc

    @property
    def mb_total(self) -> float:
        """Total macroblock / CTU count for the frame."""
        return self.imb + self.pmb + self.smb

    @property
    def is_intra(self) -> bool:
        """True for I and i (IDR / non-IDR intra)."""
        return self.frame_type in ("I", "i")


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------


def _coerce_int(token: str) -> int:
    """Tolerant int parse — empty / non-numeric tokens become zero.

    The stats file is encoder-emitted and well-formed in practice, but
    keeping the parser tolerant means a malformed line at run-end (e.g.
    the encoder dies mid-write) doesn't poison the rest of the corpus.
    """
    try:
        return int(token)
    except (TypeError, ValueError):
        return 0


def _coerce_float(token: str) -> float:
    try:
        return float(token)
    except (TypeError, ValueError):
        return 0.0


def _tokenize_stats_line(text: str) -> dict[str, str]:
    """Return case-insensitive key/value tokens for an encoder stats row."""
    tokens: dict[str, str] = {}
    for raw in text.replace(",", " ").split():
        tok = raw.strip().rstrip(";")
        if not tok:
            continue
        sep = ":" if ":" in tok else "=" if "=" in tok else ""
        if not sep:
            continue
        key, _, value = tok.partition(sep)
        if not key:
            continue
        tokens[key] = value
        tokens[key.lower()] = value
    return tokens


def _token(tokens: dict[str, str], *names: str, default: str = "0") -> str:
    for name in names:
        value = tokens.get(name)
        if value is None:
            value = tokens.get(name.lower())
        if value is not None:
            return value
    return default


def parse_stats_line(line: str) -> PerFrameStats | None:
    """Parse one stats-file line into :class:`PerFrameStats`.

    Returns ``None`` for the ``#options:`` header line, blank lines,
    and unrecognised lines. Per-frame lines are tokenised on whitespace
    and split on the first ``:`` / ``=`` per token; later ``:``
    characters (e.g. inside ``ref:0;1;2``) are preserved as part of the
    value.
    """
    text = line.strip()
    if not text or text.startswith("#"):
        return None
    tokens = _tokenize_stats_line(text)
    if "in" not in tokens or "type" not in tokens:
        return None
    return PerFrameStats(
        in_idx=_coerce_int(_token(tokens, "in")),
        out_idx=_coerce_int(_token(tokens, "out")),
        frame_type=_token(tokens, "type", default="P"),
        qp=_coerce_float(_token(tokens, "q")),
        aq=_coerce_float(_token(tokens, "aq", "q-aq")),
        tex=_coerce_int(_token(tokens, "tex")),
        mv=_coerce_int(_token(tokens, "mv")),
        misc=_coerce_int(_token(tokens, "misc")),
        imb=_coerce_float(_token(tokens, "imb", "icu")),
        pmb=_coerce_float(_token(tokens, "pmb", "pcu")),
        smb=_coerce_float(_token(tokens, "smb", "scu")),
    )


def parse_stats_file(path: Path) -> list[PerFrameStats]:
    """Load every per-frame record from an x264 / x265 stats file.

    Skips the ``#options:`` header and any blank lines. Returns an
    empty list if the file is missing or empty — callers decide how
    to react (the corpus row records zero-valued aggregates).
    """
    if not path.exists():
        return []
    frames: list[PerFrameStats] = []
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            rec = parse_stats_line(line)
            if rec is not None:
                frames.append(rec)
    return frames


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------


def _mean(values: Sequence[float]) -> float:
    if not values:
        return 0.0
    return sum(values) / float(len(values))


def _std(values: Sequence[float]) -> float:
    """Population standard deviation. Returns 0.0 for empty / singleton."""
    n = len(values)
    if n < 2:
        return 0.0
    mu = _mean(values)
    var = sum((v - mu) ** 2 for v in values) / float(n)
    return math.sqrt(var)


def aggregate_stats(frames: Iterable[PerFrameStats]) -> dict[str, float]:
    """Return the ten ``enc_internal_*`` scalar columns for one encode.

    Empty / missing input yields a fully-zero dict — corpus rows still
    populate the schema so downstream readers don't see ragged JSONL.
    """
    frame_list = list(frames)
    out: dict[str, float] = {col: 0.0 for col in ENCODER_STATS_COLUMNS}
    if not frame_list:
        return out

    qps = [f.qp for f in frame_list]
    bits = [float(f.bits) for f in frame_list]
    mvs = [float(f.mv) for f in frame_list]

    intra_frames = [f for f in frame_list if f.is_intra]
    inter_frames = [f for f in frame_list if not f.is_intra]
    itex_values = [float(f.tex) for f in intra_frames]
    ptex_values = [float(f.tex) for f in inter_frames]

    total_mb = sum(f.mb_total for f in frame_list)
    total_imb = sum(f.imb for f in frame_list)
    total_smb = sum(f.smb for f in frame_list)

    out["enc_internal_qp_mean"] = _mean(qps)
    out["enc_internal_qp_std"] = _std(qps)
    out["enc_internal_bits_mean"] = _mean(bits)
    out["enc_internal_bits_std"] = _std(bits)
    out["enc_internal_mv_mean"] = _mean(mvs)
    out["enc_internal_mv_std"] = _std(mvs)
    out["enc_internal_itex_mean"] = _mean(itex_values)
    out["enc_internal_ptex_mean"] = _mean(ptex_values)
    if total_mb > 0:
        out["enc_internal_intra_ratio"] = float(total_imb) / float(total_mb)
        out["enc_internal_skip_ratio"] = float(total_smb) / float(total_mb)
    return out


__all__ = [
    "ENCODER_STATS_COLUMNS",
    "PerFrameStats",
    "aggregate_stats",
    "parse_stats_file",
    "parse_stats_line",
]
