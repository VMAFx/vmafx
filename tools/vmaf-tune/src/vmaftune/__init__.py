# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""vmaf-tune — quality-aware encode automation harness (Phase A).

Phase A ships a grid-sweep corpus generator over libx264. The JSONL
schema emitted by `corpus.py` is the API contract Phase B (target-VMAF
bisect) and Phase C (per-title CRF predictor) will consume; bump
``SCHEMA_VERSION`` if a row's keys / value semantics change.

See ``docs/adr/0237-quality-aware-encode-automation.md`` for the
roadmap and ``docs/research/0044-quality-aware-encode-automation.md``
for the option-space digest.
"""

from __future__ import annotations

__version__ = "0.0.2"

# Bump on any backward-incompatible row-schema change.
#
# - v2 added the ``clip_mode`` key (additive, default ``"full"``) for
#   the sample-clip mode introduced under ADR-0297.
# - v3 adds the HDR provenance triple (``hdr_transfer`` / ``hdr_primaries``
#   / ``hdr_forced``) wired up by the ``corpus.iter_rows`` HDR integration
#   (ADR-0300 status update 2026-05-08), plus the canonical-6 per-frame
#   libvmaf features as mean and std aggregates (12 new columns:
#   ``adm2_mean``, ``vif_scale[0..3]_mean``, ``motion2_mean`` + matching
#   ``_std`` counterparts — see ADR-0366). HDR keys default to SDR-equivalent
#   values (``""`` / ``""`` / ``False``); canonical-6 columns carry ``NaN``
#   when libvmaf does not expose a pooled feature. The TransNet-V2 shot
#   metadata trio (``shot_count`` / ``shot_avg_duration_sec`` /
#   ``shot_duration_std_sec``) is also additive in v3 — keys default to
#   ``0`` / ``0.0`` / ``0.0`` when shot detection is unavailable (ADR-0223
#   / research-0086). Also additive in v3: ten ``enc_internal_*`` scalar
#   aggregates (per ADR-0332) capturing x264's pass-1 stats-file signal
#   — predicted bitrate, QP, motion-vector cost, texture cost, intra /
#   skip macroblock ratios. Hardware encoders (NVENC / AMF / QSV /
#   VideoToolbox) opt out and emit ``0.0``.
SCHEMA_VERSION = 3

# Canonical-6 libvmaf feature names. Mirrors the trainer-side
# ``CANONICAL6`` tuple in ``ai/scripts/train_fr_regressor_v2.py`` and
# ``CANONICAL_6`` in ``ai/scripts/train_fr_regressor_v3.py``. Order is
# load-bearing — downstream code indexes into the derived ``_mean`` /
# ``_std`` column tuples positionally.
CANONICAL6_FEATURES: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)

# v3 row-keys derived from CANONICAL6_FEATURES — emitted in addition to
# the v2 keys when ``SCHEMA_VERSION >= 3``. Mean and std are computed
# from libvmaf's ``pooled_metrics.<feature>`` block (``mean``, ``stddev``).
CANONICAL6_MEAN_KEYS: tuple[str, ...] = tuple(f"{f}_mean" for f in CANONICAL6_FEATURES)
CANONICAL6_STD_KEYS: tuple[str, ...] = tuple(f"{f}_std" for f in CANONICAL6_FEATURES)
CANONICAL6_AGGREGATE_KEYS: tuple[str, ...] = CANONICAL6_MEAN_KEYS + CANONICAL6_STD_KEYS

# Canonical row-key tuple — exposed so tests, downstream loaders, and
# the Phase B bisect can verify the contract programmatically.
CORPUS_ROW_KEYS: tuple[str, ...] = (
    "schema_version",
    "run_id",
    "timestamp",
    "src",
    "src_sha256",
    "width",
    "height",
    "pix_fmt",
    "framerate",
    "duration_s",
    "encoder",
    "encoder_version",
    "preset",
    "crf",
    "extra_params",
    "encode_path",
    "encode_size_bytes",
    "bitrate_kbps",
    "encode_time_ms",
    "vmaf_score",
    "vmaf_model",
    "score_time_ms",
    "ffmpeg_version",
    "vmaf_binary_version",
    "exit_status",
    "clip_mode",
    "hdr_transfer",
    "hdr_primaries",
    "hdr_forced",
    # TransNet-V2 shot-metadata trio (ADR-0223 / research-0086). All
    # additive; ``shot_count == 0`` flags "shot detection unavailable
    # for this source" so downstream consumers can opt out without
    # a schema-version bump.
    "shot_count",
    "shot_avg_duration_sec",
    "shot_duration_std_sec",
    *CANONICAL6_AGGREGATE_KEYS,
    # ADR-0332: per-frame encoder-internal stats aggregates.
    # Populated for codecs whose adapter declares
    # ``supports_encoder_stats = True`` (libx264 in v1; libx265
    # capture wired with parser support deferred). libvpx-vp9 writes
    # binary first-pass stats, and hardware encoders (NVENC / AMF /
    # QSV / VideoToolbox) opt out and emit ``0.0``.
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

__all__ = [
    "CANONICAL6_AGGREGATE_KEYS",
    "CANONICAL6_FEATURES",
    "CANONICAL6_MEAN_KEYS",
    "CANONICAL6_STD_KEYS",
    "CORPUS_ROW_KEYS",
    "SCHEMA_VERSION",
    "__version__",
]
