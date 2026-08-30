# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""libvvenc codec adapter — VVC / H.266.

VVenC is Fraunhofer HHI's open-source VVC (H.266) encoder. VVC is the
ITU-T / ISO standard that succeeds HEVC and delivers ~30-50% better
compression at equal quality. For the fork's quality-aware encode
automation harness this matters because VVenC is the first
standardised codec on the adapter set whose rate-distortion curve
materially shifts the corpus distribution — every Phase B / C predictor
that conditions on ``encoder`` will have to learn a different curve
for VVC than for HEVC / AV1.

Phase A wires a single-pass QP encode plus a curated set of real
VVenC 1.14.0 tuning knobs. The knobs are forwarded via FFmpeg's
``libvvenc`` wrapper, which surfaces VVenC's config via opaque
``-vvenc-params <key>=<value>:<key>=<value>`` strings (verified from
``FFmpeg/n8.1`` ``libavcodec/libvvenc.c`` — the wrapper's
``vvenc_parse_vvenc_params`` walks the colon-separated KV list and
forwards each pair into ``vvenc_set_param``, the public
``include/vvenc/vvencCfg.h`` API). VVenC config keys come from
``source/Lib/apputils/VVEncAppCfg.h`` at tag ``v1.14.0``
(SHA ``9428ea8636ae7f443ecde89999d16b2dfc421524``, accessed
2026-05-09).

Two-pass / Pareto / per-shot dynamic QP land in later phases per
ADR-0237 and ADR-0285.

Subprocess boundary is the integration seam — tests mock
``subprocess.run`` rather than running ffmpeg / vvencapp.

Note on neural-network video coding (NN-VC). VVC the standard defines
NN-VC tool-points (intra prediction, loop filter, super-resolution),
but VVenC 1.14.0 does **not** ship implementations of any of them — the
public config surface (``VVEncAppCfg.h`` at ``v1.14.0``) contains zero
``IntraNN`` / ``NN`` / ``NNVC`` tokens. An earlier draft of this
adapter exposed an ``nnvc_intra`` toggle that emitted
``-vvenc-params IntraNN=1``; that key has never existed in any
released VVenC and was a fabrication. Per ADR-0285's 2026-05-09
status update the toggle has been removed; if NN-VC ever lands in
upstream VVenC the placeholder pattern from ADR-0294's
self-activating adapter set applies.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path

from . import _gop_common

# Compress the fork's canonical 7-name preset vocabulary onto VVenC's
# 5-level scale. The 7-name vocabulary is the union of x264's 10
# presets minus duplicates and is the one the search loop emits;
# every adapter decides locally how to project onto its native scale.
# Anything strictly slower than ``slow`` (placebo / slowest / slower)
# pins to VVenC's deepest preset; anything strictly faster than
# ``fast`` pins to ``faster``. ``medium`` is the default.
_PRESET_MAP: dict[str, str] = {
    "placebo": "slower",
    "slowest": "slower",
    "slower": "slower",
    "slow": "slow",
    "medium": "medium",
    "fast": "fast",
    "faster": "faster",
    "veryfast": "faster",
    "superfast": "faster",
    "ultrafast": "faster",
}

_NATIVE_PRESETS: tuple[str, ...] = (
    "faster",
    "fast",
    "medium",
    "slow",
    "slower",
)

# VVenC's documented ``Tier`` enum (VVEncAppCfg.h v1.14.0 line 183).
_TIERS: frozenset[str] = frozenset({"main", "high"})

# VVenC's supported internal bit-depths (VVEncAppCfg.h v1.14.0 line 911).
# 8 and 10 are the only practically useful values for VVC main / main10
# profiles.
_INTERNAL_BITDEPTHS: frozenset[int] = frozenset({8, 10})


@dataclasses.dataclass(frozen=True)
class VVenCAdapter:
    """libvvenc single-pass QP adapter (VVC / H.266)."""

    name: str = "libvvenc"
    encoder: str = "libvvenc"
    quality_knob: str = "qp"
    # VVenC accepts QP 0..63; we surface the perceptually informative
    # window, mirroring the AV1-style scale. The default lands in the
    # middle of the curve for typical 1080p/4K natural content.
    quality_range: tuple[int, int] = (17, 50)
    quality_default: int = 32
    invert_quality: bool = True  # higher QP = lower quality

    # Adapter-version bumps when the argv shape / preset list / quality
    # window / extra_params surface changes (ADR-0298). The 2026-05-09
    # rev removes the fabricated ``nnvc_intra`` toggle and adds the
    # real VVenC 1.14.0 tuning surface; cache keys must invalidate.
    adapter_version: str = "2"

    # Predictor probe-encode knobs. VVenC has no "ultrafast" — "faster"
    # is the fastest native preset. Even "faster" is meaningfully slower
    # than other codecs' ultrafast: the predictor's complexity barometer
    # for VVenC is therefore noisier than for x264 / x265 and validation
    # may legitimately fall back. ADR's `## Alternatives considered`
    # documents this.
    probe_preset: str = "faster"
    probe_quality: int = 32
    supports_qpfile: bool = False
    # ADR-0332: this encoder has no parseable first-pass stats file.
    supports_encoder_stats: bool = False
    # VVenC ROI-map file (ADR-0370): delivered via -vvenc-params ROIFile=.
    supports_saliency_roi: bool = True
    # ADR-0546 (Phase F real 2-pass): the FFmpeg ``libvvenc`` wrapper
    # accepts ``-pass N -passlogfile <prefix>`` since FFmpeg 6.1 — the
    # wrapper translates internally to VVenC's RcStatsFile config key
    # (``-vvenc-params RcStatsFile=...``). We rely on the generic
    # ffmpeg-side flags so the encode driver does not need a
    # VVenC-specific stats path.
    supports_two_pass: bool = True

    # Vocabulary the search loop sees — the canonical 7-name superset.
    # The adapter compresses to VVenC's 5-level native vocabulary at
    # ``encode()`` time via ``native_preset()``.
    presets: tuple[str, ...] = (
        "placebo",
        "slowest",
        "slower",
        "slow",
        "medium",
        "fast",
        "faster",
        "veryfast",
        "superfast",
        "ultrafast",
    )

    # ----- Real VVenC 1.14.0 tuning knobs -------------------------------
    # All defaults are the VVenC library defaults so a freshly-constructed
    # adapter emits an empty ``-vvenc-params`` string and reproduces the
    # bit-exact baseline of the previous (NNVC-stripped) adapter call. See
    # ADR-0285 §"Status update 2026-05-09" for the per-knob justification.
    #
    # Each docstring field cites the ``VVEncAppCfg.h`` line where the key
    # is defined at tag ``v1.14.0``
    # (SHA ``9428ea8636ae7f443ecde89999d16b2dfc421524``).

    #: Perceptual QP adaptation (XPSNR-driven). VVEncAppCfg.h:724.
    #: ``None`` → leave the library default (currently auto). ``True``
    #: emits ``PerceptQPA=1``, ``False`` emits ``PerceptQPA=0``.
    #: Materially shifts the rate-distortion curve and is therefore
    #: recorded per-row in ``encoder_extra_params`` for predictor
    #: conditioning rather than collapsed into the codec one-hot.
    perceptual_qpa: bool | None = None

    #: Internal coding bit-depth. VVEncAppCfg.h:911. ``None`` → default
    #: (matches MSBExtendedBitDepth, typically 10 for VVC). Allowed
    #: explicit values: 8 or 10.
    internal_bitdepth: int | None = None

    #: Profile / Level tier — ``main`` or ``high``. VVEncAppCfg.h:739.
    #: Caps the maximum bitrate and resolution that the bitstream
    #: signals; ``high`` is required above the main-tier ceiling.
    tier: str | None = None

    #: Tile partitioning ``(cols, rows)``. VVEncAppCfg.h:701 (``Tiles``).
    #: ``None`` → single tile (the default). Useful for parallel encode
    #: on high-resolution content.
    tiles: tuple[int, int] | None = None

    #: Max number of frames processed in parallel. VVEncAppCfg.h:1137.
    #: ``0`` disables, ``>=2`` enables parallel frames. ``None`` → leave
    #: the library default (auto).
    max_parallel_frames: int | None = None

    #: Reference-Picture-Resampling toggle. VVEncAppCfg.h:1162.
    #: ``0`` disabled, ``1`` enabled, ``2`` RPR-ready. VVC's
    #: resolution-adaptive feature; ``None`` → library default.
    rpr: int | None = None

    #: Sample Adaptive Offset loop filter. VVEncAppCfg.h:1023.
    #: ``True`` → ``SAO=1``, ``False`` → ``SAO=0``. Useful for
    #: ablation studies. ``None`` → library default (on).
    sao: bool | None = None

    #: Adaptive Loop Filter. VVEncAppCfg.h:1108. ``None`` → library
    #: default. Useful for ablation studies.
    alf: bool | None = None

    #: Cross-Component Adaptive Loop Filter. VVEncAppCfg.h:1110. Only
    #: meaningful when ``alf`` is enabled. ``None`` → library default.
    ccalf: bool | None = None

    def native_preset(self, preset: str) -> str:
        """Return the native VVenC preset for a 7-name canonical preset.

        Raises ``ValueError`` for unknown names. Pure function — no
        I/O — so the search loop can pre-compute the projection.
        """
        if preset not in _PRESET_MAP:
            raise ValueError(
                f"unknown libvvenc preset {preset!r}; expected one of " f"{tuple(_PRESET_MAP)}"
            )
        return _PRESET_MAP[preset]

    def validate(self, preset: str, qp: int) -> None:
        """Raise ``ValueError`` if ``(preset, qp)`` is unsupported.

        Also validates the optional tuning-knob fields. Range / enum
        checks are minimal but pin the user-facing surface so a typo
        (``Tier="medium"``) fails closed before ffmpeg launches.
        """
        if preset not in _PRESET_MAP:
            raise ValueError(
                f"unknown libvvenc preset {preset!r}; expected one of " f"{tuple(_PRESET_MAP)}"
            )
        lo, hi = self.quality_range
        if not lo <= qp <= hi:
            raise ValueError(f"qp {qp} outside libvvenc range [{lo}, {hi}]")
        if self.tier is not None and self.tier not in _TIERS:
            raise ValueError(f"libvvenc tier {self.tier!r} not in {sorted(_TIERS)}")
        if self.internal_bitdepth is not None and self.internal_bitdepth not in _INTERNAL_BITDEPTHS:
            raise ValueError(
                f"libvvenc internal_bitdepth {self.internal_bitdepth!r} "
                f"not in {sorted(_INTERNAL_BITDEPTHS)}"
            )
        if self.tiles is not None:
            cols, rows = self.tiles
            if cols < 1 or rows < 1:
                raise ValueError(f"libvvenc tiles must be >= 1 in both axes; got {self.tiles!r}")
        if self.max_parallel_frames is not None and self.max_parallel_frames < 0:
            raise ValueError(
                f"libvvenc max_parallel_frames must be >= 0; got " f"{self.max_parallel_frames!r}"
            )
        if self.rpr is not None and self.rpr not in (0, 1, 2):
            raise ValueError(f"libvvenc rpr must be 0/1/2; got {self.rpr!r}")

    def _build_kv_pairs(self) -> list[str]:
        """Build the ordered ``key=value`` pair list for ``-vvenc-params``.

        Ordering is deterministic (declaration order of the fields)
        which keeps argv byte-stable for cache-key hashing and snapshot
        tests. ``None`` valued fields are skipped — the library default
        is the documented baseline.
        """
        pairs: list[str] = []
        if self.perceptual_qpa is not None:
            pairs.append(f"PerceptQPA={1 if self.perceptual_qpa else 0}")
        if self.internal_bitdepth is not None:
            pairs.append(f"InternalBitDepth={self.internal_bitdepth}")
        if self.tier is not None:
            pairs.append(f"Tier={self.tier}")
        if self.tiles is not None:
            cols, rows = self.tiles
            # VVenC's ``IStreamToRefVec`` parser splits on 'x' for the
            # ``Tiles`` key (VVEncAppCfg.h:547).
            pairs.append(f"Tiles={cols}x{rows}")
        if self.max_parallel_frames is not None:
            pairs.append(f"MaxParallelFrames={self.max_parallel_frames}")
        if self.rpr is not None:
            pairs.append(f"RPR={self.rpr}")
        if self.sao is not None:
            pairs.append(f"SAO={1 if self.sao else 0}")
        if self.alf is not None:
            pairs.append(f"ALF={1 if self.alf else 0}")
        if self.ccalf is not None:
            pairs.append(f"CCALF={1 if self.ccalf else 0}")
        return pairs

    def ffmpeg_codec_args(self, preset: str, quality: int) -> list[str]:
        """FFmpeg argv slice for libvvenc single-pass constant-QP.

        VVenC's quality knob is ``-qp`` (not ``-crf``); the canonical
        preset name is compressed onto the 5-level native vocabulary
        via :meth:`native_preset`.
        """
        return [
            "-c:v",
            self.encoder,
            "-preset",
            self.native_preset(preset),
            "-qp",
            str(quality),
        ]

    def extra_params(self) -> tuple[str, ...]:
        """FFmpeg ``-c:v libvvenc`` arg suffix for VVenC tuning knobs.

        Returns an immutable tuple so callers can safely concatenate
        into ``EncodeRequest.extra_params``. Empty when no knob is set
        (preserves the bit-exact Phase A grid baseline).

        FFmpeg's ``libvvenc`` wrapper forwards opaque ``-vvenc-params
        key=value:key=value`` strings down to the underlying VVenC
        config object via ``vvenc_set_param`` (the public
        ``vvencCfg.h`` API). The keys emitted here are sourced verbatim
        from ``VVEncAppCfg.h`` at tag ``v1.14.0``.
        """
        pairs = self._build_kv_pairs()
        if not pairs:
            return ()
        return ("-vvenc-params", ":".join(pairs))

    def gop_args(self, keyint: int, min_keyint: int | None = None) -> tuple[str, ...]:
        """FFmpeg ``-g`` / ``-keyint_min``, honoured by libvvenc."""
        return _gop_common.default_gop_args(keyint, min_keyint)

    def force_keyframes_args(self, timestamps: tuple[float, ...]) -> tuple[str, ...]:
        """FFmpeg ``-force_key_frames`` with comma-separated seconds."""
        return _gop_common.default_force_keyframes_args(timestamps)

    def probe_args(self) -> list[str]:
        """Predictor probe-encode argv: native VVenC ``faster`` preset, fixed QP."""
        return [
            "-c:v",
            self.encoder,
            "-preset",
            self.native_preset(self.probe_preset),
            "-qp",
            str(self.probe_quality),
        ]

    def two_pass_args(self, pass_number: int, stats_path: Path) -> tuple[str, ...]:
        """FFmpeg argv slice for libvvenc 2-pass encoding (ADR-0546).

        FFmpeg's ``libvvenc`` wrapper honours the generic ``-pass`` /
        ``-passlogfile`` pair; internally the wrapper translates that
        into VVenC's ``RcStatsFile`` config key. We let FFmpeg do the
        translation so the encode driver does not need a VVenC-
        specific stats path or .json sidecar handling.

        ``stats_path`` is used as the passlog *prefix* — FFmpeg writes
        ``<prefix>-0.log`` as the stream-specific stats sidecar (same
        convention libx264 / libaom-av1 use).
        """
        if pass_number == 0:
            return ()
        if pass_number not in (1, 2):
            raise ValueError(
                f"libvvenc two_pass_args: pass_number must be 1 or 2, got {pass_number}"
            )
        return ("-pass", str(pass_number), "-passlogfile", str(stats_path))

    def roi_from_saliency(
        self,
        block_offsets: object,
        out_path: object,
        *,
        duration_frames: int = 1,
    ) -> object:
        """Write a VVenC ROI-map CSV file from a per-CTU-block offset array.

        Delegates to :func:`vmaftune.saliency.write_vvenc_roi_csv`.
        Returns the output ``Path`` the caller passes to
        :func:`vmaftune.saliency.augment_extra_params_with_vvenc_roi`.

        ``block_offsets`` must be at 64x64 CTU granularity — reduce via
        :func:`vmaftune.saliency.reduce_qp_map_to_blocks` with
        ``block=VVENC_CTU_SIDE`` (ADR-0370).
        """
        from pathlib import Path as _Path

        from vmaftune.saliency import write_vvenc_roi_csv  # local import

        return write_vvenc_roi_csv(block_offsets, _Path(out_path), duration_frames=duration_frames)


def native_presets() -> tuple[str, ...]:
    """Return VVenC's native 5-level preset vocabulary.

    Exposed for tests and for any consumer that needs to describe the
    encoder's actual scale rather than the harness's canonical
    superset.
    """
    return _NATIVE_PRESETS


def supported_tiers() -> tuple[str, ...]:
    """Return the documented VVenC ``Tier`` enum values.

    Sourced from ``VVEncAppCfg.h`` at tag ``v1.14.0`` line 183 — the
    ``TierToEnumMap`` C++ table.
    """
    return tuple(sorted(_TIERS))
