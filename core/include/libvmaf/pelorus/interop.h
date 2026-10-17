/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-FileCopyrightText: 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VENDORED FROM VMAFx/pelorus@33b4a625cad64a7e9ca14f6db06eb91ba8f56663 — DO NOT EDIT.
 * Append-only ABI; single
 * source of truth is pelorus. Re-sync via scripts/sync-pelorus-interop.sh.
 * See docs/adr/1113-vendor-pelorus-interop-abi.md.
 *
 * Local edit vs the pelorus original: the intra-pelorus #include below is
 * rewritten from "pelorus/pelorus.h" to "libvmaf/pelorus/pelorus.h" so it resolves
 * under core/include/. Nothing else is changed.
 */

/*
 * interop.h — Pelorus <-> vmafx data-plane interop contract.
 *
 * A versioned, self-describing per-frame metadata blob. In an FFmpeg
 * filtergraph it rides each AVFrame as AV_FRAME_DATA_SEI_UNREGISTERED
 * (libavutil/frame.h), prefixed by PELORUS_SIDEDATA_UUID so it round-trips the
 * graph (every well-behaved filter calls av_frame_copy_props) and never
 * collides with codec-meaningful side data. The same blob is the IPC payload
 * when the data plane crosses a process boundary.
 *
 * vf_pelorus_* filters WRITE sections; vmafx vf_libvmaf* filters READ them for
 * perceptually-weighted scoring. Pelorus is the SOLE writer (single-writer
 * invariant); vmafx never mutates the blob.
 *
 * ABI STABILITY CONTRACT (normative — both repos depend on this; see
 * docs/api/interop-abi.md and docs/adr/0103-interop-sidedata-abi.md):
 *
 *   R1. APPEND-ONLY. New fields are added at the END of a section struct (above
 *       its "APPEND-ONLY" marker), or as a NEW section with a new bit. Never
 *       reorder, never resize an existing field, never repurpose a field.
 *   R2. NEVER REMOVE a section bit or a field. Deprecate by documentation;
 *       producers may stop populating it, but the slot stays reserved forever.
 *   R3. Every section is independently OPTIONAL, gated by section_mask. A
 *       consumer MUST ignore bits it does not understand (forward-compat) and
 *       MUST tolerate absence of bits it does understand (back-compat).
 *   R4. Offsets/sizes are explicit (PelorusSectionDir) so a newer producer's
 *       larger section is parseable by an older consumer: read
 *       min(known_size, dir.size) bytes and ignore the tail.
 *   R5. The blob is a flat, pointer-free byte image (no embedded pointers),
 *       little-endian on the wire. v1 producers/consumers run on
 *       little-endian hosts (x86_64, aarch64); a byte-swap path is a future,
 *       additive change gated on PELORUS_ABI_MINOR.
 *   R6. PELORUS_ABI_MAJOR bumps ONLY on a breaking change (which R1/R2 forbid
 *       for additive evolution) — in practice it never bumps.
 *       PELORUS_ABI_MINOR bumps when a new section bit, an appended field or a
 *       new carrier form (ABI 1.5: the zero-free form below) lands.
 */
#ifndef PELORUS_INTEROP_H
#define PELORUS_INTEROP_H

#include <stddef.h>
#include <stdint.h>

#include "libvmaf/pelorus/pelorus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Identity ----------------------------------------------------------- */

/* Canonical 8-byte magic at the head of every blob: ASCII "PELOR1\0\0".
 * Compare as bytes (memcmp); do NOT reinterpret as an integer — endianness of
 * the magic itself is fixed by these literal bytes (R5). */
#define PELORUS_MAGIC_STR "PELOR1\0\0"
#define PELORUS_MAGIC_LEN 8

/* Semantic ABI version of the blob LAYOUT (independent of PELORUS_VERSION_*,
 * which versions the library, and of the vmafx control-plane API version).
 *
 * MINOR history (R6 — bumps on every additive change):
 *   1.0  initial sections (a..e: banding/variance/denoise/filmgrain/motion).
 *   1.1  + PEL_SEC_QPREPORT (f): encoder-honored QP / bit readback (ADR-0119).
 *   1.2  + PEL_SEC_MOTION_CONF (g): per-block MV confidence map (ADR-0131).
 *   1.3  + PEL_SEC_COMPLEXITY (h): per-frame complexity scalar (ADR-0132).
 *   1.4  + PEL_SEC_ENC_TELEMETRY (i): per-frame encoder telemetry (ADR-0174);
 *        + PEL_SEC_ENCODE_RECORD (j): encode-record digest (ADR-0175);
 *        + PelorusMotionSection.block_size_log2 at the tail (32 -> 36 bytes).
 *   1.5  + the zero-free carrier form of a blob (pelorus_carrier_uuid, COBS),
 *          pel_blob_carrier_encode() and pel_blob_unwrap() (ADR-0183). No
 *          section or field changes. */
#define PELORUS_ABI_MAJOR 1u
#define PELORUS_ABI_MINOR 5u

/* The 16-byte UUID prefixing the AV_FRAME_DATA_SEI_UNREGISTERED payload (the
 * leading uuid_iso_iec_11578 mandated by the user-data-unregistered SEI
 * layout). Fixed, project-owned, version-4 random UUID. It is the routing key
 * that distinguishes a Pelorus blob from any other unregistered SEI on a frame.
 *
 *   pelorus-sidedata-v1 = e1d7c4a2-6b93-4f08-9a55-0f3c2db17e64
 */
#define PELORUS_SIDEDATA_UUID_LEN 16
extern const uint8_t pelorus_sidedata_uuid[PELORUS_SIDEDATA_UUID_LEN];

/* The 16-byte UUID of the zero-free carrier form (ABI 1.5, ADR-0183): the same
 * blob, its image (everything after pelorus_sidedata_uuid) COBS-encoded so the
 * SEI payload holds no 0x00 byte. NVENC truncates SEI payloads with long runs
 * of zero bytes (issue #284); a payload without zero bytes needs no emulation
 * prevention at all. A reader that predates ABI 1.5 sees a foreign SEI and
 * ignores it (R3). The UUID itself holds no 0x00 byte.
 *
 *   pelorus-sidedata-zero-free-v1 = 3f9b37b8-fd9a-4621-920e-9b78b55cf9b5
 */
extern const uint8_t pelorus_carrier_uuid[PELORUS_SIDEDATA_UUID_LEN];

/* Upper bound of the carrier form of an n-byte blob (UUID included): COBS
 * adds one code byte, plus one per 254 bytes without a zero. Part of the ABI 1.5
 * contract: the bound never shrinks. Domain: n <= SIZE_MAX / 2 (the sum would
 * wrap above it); n is evaluated twice, so pass a plain variable. For the exact
 * length, ask pel_blob_carrier_encode(blob, len, NULL, 0, &need) instead. */
#define PEL_CARRIER_MAX_LEN(n) ((size_t)(n) + 1u + (size_t)(n) / 254u)

/* ---- Section catalogue (R1/R2: append-only; bits are NEVER reused) ------ */

enum pel_section {
    PEL_SEC_BANDING = 1u << 0,       /* (a) banding / flatness map summary       */
    PEL_SEC_VARIANCE = 1u << 1,      /* (b) local variance / edge summary        */
    PEL_SEC_DENOISE = 1u << 2,       /* (c) denoise residual statistics          */
    PEL_SEC_FILMGRAIN = 1u << 3,     /* (d) film-grain params (AV1-shaped)       */
    PEL_SEC_MOTION = 1u << 4,        /* (e) optical-flow MV hint summary         */
    PEL_SEC_QPREPORT = 1u << 5,      /* (f) encoder-honored QP / bit readback    */
    PEL_SEC_MOTION_CONF = 1u << 6,   /* (g) per-block MV confidence map (ADR-0113)*/
    PEL_SEC_COMPLEXITY = 1u << 7,    /* (h) per-frame complexity scalar (ADR-0132) */
    PEL_SEC_ENC_TELEMETRY = 1u << 8, /* (i) per-frame encoder telemetry (ADR-0174) */
    PEL_SEC_ENCODE_RECORD = 1u << 9  /* (j) encode-record digest (ADR-0175)        */
    /* bits 10..31 reserved — a retired bit is NEVER reused (R2). */
};

/* ---- Plane layout / producer identity ----------------------------------- */

enum pel_plane_layout { PEL_LAYOUT_420 = 0, PEL_LAYOUT_422 = 1, PEL_LAYOUT_444 = 2 };

/* Film-grain synthesis model the estimate targets. The deband/denoise/motion
 * filters are codec-agnostic (they help any HW encoder); only grain synthesis
 * is codec-specific — AV1 uses AOM params, HEVC/H.265 + VVC/H.266 use H.274
 * (SEI film-grain characteristics). The film-grain section carries both. */
enum pel_grain_model {
    PEL_GRAIN_NONE = 0,
    PEL_GRAIN_AOM = 1, /* AV1 — maps to AV_FILM_GRAIN_PARAMS_AV1            */
    PEL_GRAIN_H274 = 2 /* HEVC/VVC — maps to AV_FILM_GRAIN_PARAMS_H274      */
};

/* Which encoder-feedback surface produced a PEL_SEC_QPREPORT section. The
 * closed loop (ADR-0114 step 6 / ADR-0119) reads an encoder's ACTUAL per-block
 * QP + bit decisions back, so a later pass (or vmafx) can verify the ROI /
 * delta-QP map it requested was honored and refine it. The source tags the
 * vendor API the numbers came from, for diagnostics + cross-vendor comparison. */
enum pel_qp_report_source {
    PEL_QPSRC_NONE = 0,  /* unset / synthetic                                */
    PEL_QPSRC_QSV = 1,   /* Intel oneVPL mfxEncodeBlkStats / mfxEncodeFrameStats */
    PEL_QPSRC_NVENC = 2, /* NVENC per-block QP feedback (reserved)           */
    PEL_QPSRC_VULKAN = 3 /* Vulkan-Video encode feedback (reserved)         */
};

/* fourcc of the writing stage, for diagnostics (e.g. 'PLRS'). */
#define PEL_FOURCC(a, b, c, d)                                                                     \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* ---- Blob framing ------------------------------------------------------- */

/* One entry per present section; lets an older consumer locate and tail-skip a
 * newer producer's section (R4). The dir[] array immediately follows the
 * PelorusSideData header; section payloads follow dir[] at their offsets. */
typedef struct PelorusSectionDir {
    uint32_t section_id;   /* one enum pel_section bit                        */
    uint32_t offset;       /* byte offset from the start of the blob          */
    uint32_t size;         /* byte size of that section's struct in THIS blob */
    uint32_t struct_minor; /* producer's PELORUS_ABI_MINOR for this section   */
} PelorusSectionDir;

/* Blob header. Fixed layout; only ever appended to at the tail (R1). All
 * offsets are blob-relative (start of magic[0]). header_size lets a consumer
 * find dir[] regardless of future header growth. */
typedef struct PelorusSideData {
    uint8_t magic[8];       /* PELORUS_MAGIC_STR                               */
    uint16_t abi_major;     /* PELORUS_ABI_MAJOR                               */
    uint16_t abi_minor;     /* producer's PELORUS_ABI_MINOR                    */
    uint32_t total_size;    /* total blob bytes (header + dir[] + sections)    */
    uint32_t section_mask;  /* OR of present enum pel_section bits             */
    uint16_t section_count; /* number of PelorusSectionDir entries following   */
    uint16_t header_size;   /* sizeof(PelorusSideData); dir[] starts here      */
    uint64_t frame_pts;     /* echo of AVFrame.pts for desync detection        */
    uint8_t plane_layout;   /* enum pel_plane_layout                           */
    uint8_t bit_depth;      /* 8 / 10 / 12                                     */
    uint16_t grid_cols;     /* cell grid shared by all map summaries           */
    uint16_t grid_rows;
    uint16_t _pad0;       /* reserved, zero                                  */
    uint32_t producer_id; /* PEL_FOURCC of the writer                        */
    uint32_t _pad1;       /* reserved, zero (keeps sizeof a multiple of 8)   */
    /* PelorusSectionDir dir[section_count] follows immediately. */
} PelorusSideData;

/* ===================================================================== *
 *  Section payloads. Flat POD; APPEND fields only at the END of each.
 *  All per-cell map summaries reference (grid_cols x grid_rows) cells via
 *  blob-relative offsets so the maps live contiguously after the structs.
 * ===================================================================== */

/* (a) Banding / flatness — written by vf_pelorus_deband / _analyze. */
typedef struct PelorusBandingSection {
    float global_banding_risk;   /* 0..1 frame-level                         */
    float flat_area_fraction;    /* fraction of pixels in flat regions       */
    uint32_t cell_data_offset;   /* blob-relative; uint8 risk per cell        */
    uint32_t cell_data_size;     /* grid_cols*grid_rows bytes                 */
    float contour_strength_mean; /* mean false-contour gradient magnitude     */
    float dominant_band_luma;    /* normalized luma where banding peaks       */
    /* --- APPEND-ONLY below this line --- */
} PelorusBandingSection;

/* (b) Local variance / edge — written by vf_pelorus_analyze. */
typedef struct PelorusVarianceSection {
    float global_variance;    /* spatial activity, normalized                  */
    float edge_density;       /* fraction of edge pixels                       */
    float texture_energy;     /* high-frequency energy proxy                   */
    uint32_t var_cell_offset; /* blob-relative; float variance per cell     */
    uint32_t var_cell_size;
    uint32_t edge_cell_offset; /* blob-relative; uint8 edge per cell         */
    uint32_t edge_cell_size;
    /* --- APPEND-ONLY below this line --- */
} PelorusVarianceSection;

/* (c) Denoise residual statistics — written by vf_pelorus_denoise. */
typedef struct PelorusDenoiseSection {
    float residual_energy_y; /* mean |in-out| on luma                        */
    float residual_energy_u;
    float residual_energy_v;
    float applied_strength;     /* actual denoise strength used, 0..1         */
    float noise_sigma_estimate; /* pre-denoise sigma estimate                 */
    float psnr_vs_input;        /* denoised-vs-input PSNR (dB)                */
    uint8_t denoiser_id;        /* which Pelorus denoiser ran                 */
    uint8_t _pad[3];            /* reserved, zero                             */
    /* --- APPEND-ONLY below this line --- */
} PelorusDenoiseSection;

/* (d) Film-grain params — written by vf_pelorus_grain_estimate. Carries the AV1
 * (AOM) parameters field-for-field (mirrors AVFilmGrainAOMParams, convertible
 * to AV_FILM_GRAIN_PARAMS_AV1) AND a codec tag + the H.274 scalar knobs for
 * HEVC/H.265 + VVC/H.266 (AV_FILM_GRAIN_PARAMS_H274 / SEI FGC). The full H.274
 * component-model tables are large and codec-meaningful, so the grain-estimate
 * filter attaches them as a native AV_FRAME_DATA_FILM_GRAIN_PARAMS for the
 * encoder; this section carries the codec-neutral intent + the AV1 params +
 * the H.274 mode scalars for downstream tooling. `grain_model` says which set
 * is authoritative. */
typedef struct PelorusFilmGrainSection {
    uint64_t seed;
    int32_t num_y_points;             /* AV1: <= 14                            */
    int32_t num_uv_points[2];         /* AV1: {cb, cr}, each <= 10             */
    int32_t scaling_shift;            /* AV1 [8,11]                            */
    int32_t ar_coeff_lag;             /* AV1: coeff count = 2*lag*(lag+1)      */
    int32_t ar_coeff_shift;           /* AV1 [6,9]                            */
    int32_t grain_scale_shift;        /* AV1                                   */
    int32_t uv_mult[2];               /* AV1 [-128,127] (coded value - 128)    */
    int32_t uv_mult_luma[2];          /* AV1 [-128,127] (coded value - 128)    */
    int32_t uv_offset[2];             /* AV1 [-256,255] (coded value - 256)    */
    uint8_t apply;                    /* 1 => producer recommends grain synth   */
    uint8_t chroma_scaling_from_luma; /* AV1                                  */
    uint8_t overlap_flag;             /* AV1                                   */
    uint8_t limit_output_range;       /* AV1                                   */
    uint8_t y_points[14][2];          /* AV1: {value, scaling} (== AOM)        */
    uint8_t uv_points[2][10][2];      /* AV1 (== AOM)                          */
    int8_t ar_coeffs_y[24];           /* AV1 (== AOM)                          */
    int8_t ar_coeffs_uv[2][25];       /* AV1 (== AOM)                          */
    uint8_t grain_model;              /* enum pel_grain_model (none/aom/h274)  */
    uint8_t h274_model_id;            /* H.274 FGC model_id (0=freq,1=AR)      */
    uint8_t h274_blending_mode;       /* H.274 blending_mode_id                */
    uint8_t h274_log2_scale;          /* H.274 log2_scale_factor               */
    uint8_t _pad[6];                  /* reserved, zero                        */
    /* --- APPEND-ONLY below this line --- */
} PelorusFilmGrainSection;

/* (e) Optical-flow motion-vector hint summary — written by vf_pelorus_mc. */
typedef struct PelorusMotionSection {
    float global_motion_x;       /* mean MV x, luma pixels (not Q2)           */
    float global_motion_y;       /* mean MV y, luma pixels (not Q2)           */
    float motion_magnitude_mean; /* mean |MV|, luma pixels                    */
    float motion_magnitude_p95;  /* 95th pct |MV|, luma pixels — pan/cut cue  */
    float motion_entropy;        /* MV-field complexity, unitless             */
    uint32_t mv_field_offset;    /* blob-relative; int16 (dx,dy) per cell,
                                  * QUARTER-PEL (Q2 = round(pel*4)) luma units */
    uint32_t mv_field_size;      /* grid_cols*grid_rows*2*sizeof(int16)        */
    uint8_t has_scene_cut;       /* producer's scene-cut flag                 */
    uint8_t _pad[3];             /* reserved, zero                            */
    /* ABI 1.4 (ADR-0174, #218). A 1.3 producer's section ends before this field:
     * read it only when PEL_SD_FIELD_OK-style size checks show `got` covers it. */
    uint8_t block_size_log2; /* log2 of the mv-field cell edge, luma pixels (3 => 8x8);
                              * 0 = not reported: infer the edge from the grid */
    uint8_t _pad1[3];        /* reserved, zero                            */
    /* --- APPEND-ONLY below this line --- */
} PelorusMotionSection;

/* How the per-block motion confidence (PEL_SEC_MOTION_CONF) is derived. */
enum pel_motion_conf_metric {
    PEL_MOTION_CONF_SAD = 0, /* 255*(1 - clamp(winning per-pixel SAD / scale)) */
};

/* (g) Per-block motion-vector confidence map — written by vf_pelorus_mc
 * alongside PEL_SEC_MOTION. One uint8 per cell on the shared grid_cols*grid_rows
 * grid (0 = untrustworthy / noise-matched, 255 = sharp low-residual match); a
 * motion-compensated consumer (the ADR-0113 denoise warp) gates its warped fetch
 * by it. The grid is appended after the packed blob; conf_field_offset is
 * blob-relative, mirroring PelorusMotionSection.mv_field_offset. */
typedef struct PelorusMotionConfSection {
    uint32_t conf_field_offset; /* blob-relative; uint8 confidence per cell   */
    uint32_t conf_field_size;   /* grid_cols*grid_rows*sizeof(uint8)          */
    uint8_t conf_metric;        /* enum pel_motion_conf_metric                */
    uint8_t _pad[7];            /* reserved, zero                             */
    /* --- APPEND-ONLY below this line --- */
} PelorusMotionConfSection;

/* (h) Per-frame complexity scalar — written by vf_pelorus_analyze. An aggregate
 * of the per-tile texture/edge/banding it already measures (plus the motion
 * component when PEL_SEC_MOTION is present upstream), EMA-smoothed within a shot
 * and reset on a scene cut. The per-shot CRF steering (ADR-0132) maps this to a
 * per-frame qoffset; the autotune loop learns the mapping. `complexity` and its
 * components are in [0,1]. */
typedef struct PelorusComplexitySection {
    float complexity;       /* aggregate frame complexity [0,1]                 */
    float texture_energy;   /* variance/edge component [0,1]                    */
    float motion_component; /* from PEL_SEC_MOTION if present, else 0           */
    uint8_t has_scene_cut;  /* mirrored from motion (drives the consumer's EMA) */
    uint8_t _pad[3];        /* reserved, zero                                  */
    /* --- APPEND-ONLY below this line --- */
} PelorusComplexitySection;

/* (f) Encoder-honored QP / bit readback — written by a Pelorus-side closed-loop
 * encoder-stat reader AFTER the encoder ran (ADR-0114 step 6 / ADR-0119). The
 * single-writer invariant is unchanged: Pelorus writes this section (a future
 * libavcodec QSV stat reader within Pelorus), vmafx only reads it. Unlike
 * sections (a)..(e),
 * which carry PRE-encode GPU measurements, this section carries the encoder's
 * ACTUAL post-encode per-block decisions so a later pass — or vmafx — can verify
 * the requested ROI / delta-QP map was honored and refine it. v1 source is QSV
 * (oneVPL mfxEncodeBlkStats per-block QP + mfxEncodeFrameStats frame summary +
 * mfxExtEncodedUnitsInfo bit sizes); NVENC / Vulkan-Video sources are reserved.
 *
 * Per-cell maps follow the existing (grid_cols x grid_rows) cell convention via
 * blob-relative offsets: qp_cell is int8 actual QP per cell (encoder QP scale —
 * 0..51 for AVC/HEVC, the raw signed value the API reports), bits_cell is uint32
 * encoded bits per cell. A producer that does not populate a given map MUST
 * leave its *_offset AND *_size zero (the bits_cell map is a documented
 * follow-up — ADR-0119 — so v1 producers leave bits_cell_offset/size at 0).
 * honored_fraction is the consumer's computed agreement between the requested
 * delta-QP sign and the observed per-cell QP movement (0..1); 0 when no request
 * map was available to compare against. */
typedef struct PelorusQpReportSection {
    float avg_qp;                /* frame mean QP (fractional; mfxEncodeFrameStats.Qp) */
    float psnr_y;                /* encoder-reported luma PSNR (dB), NaN/0 if absent   */
    float psnr_u;                /* chroma Cb PSNR (dB)                                */
    float psnr_v;                /* chroma Cr PSNR (dB)                                */
    uint64_t total_bits;         /* encoded frame size in bits (sum of unit sizes)     */
    uint32_t num_intra_blocks;   /* mfxEncodeFrameStats.NumIntraBlock                  */
    uint32_t num_inter_blocks;   /* mfxEncodeFrameStats.NumInterBlock                  */
    uint32_t num_skipped_blocks; /* mfxEncodeFrameStats.NumSkippedBlock                */
    uint32_t qp_cell_offset;     /* blob-relative; int8 actual QP per cell             */
    uint32_t qp_cell_size;       /* grid_cols*grid_rows bytes (0 if qp_valid == 0)     */
    uint32_t bits_cell_offset;   /* blob-relative; uint32 encoded bits per cell        */
    uint32_t bits_cell_size;     /* grid_cols*grid_rows*sizeof(uint32) bytes           */
    float honored_fraction;      /* 0..1 cells whose QP moved as the ROI map requested */
    uint8_t report_source;       /* enum pel_qp_report_source                          */
    uint8_t block_size_log2;     /* log2 of the encoder block edge (4 => 16x16)        */
    uint8_t qp_valid;            /* 1 => qp_cell map populated; 0 => frame stats only   */
    uint8_t _pad[5];             /* reserved, zero (keeps sizeof a multiple of 8)      */
    /* --- APPEND-ONLY below this line --- */
} PelorusQpReportSection;

/* ---- (i) Encoder telemetry (ABI 1.4, ADR-0174, docs/api/encoder-telemetry.md) ---- *
 *
 * One record per coded frame, normalised across encoders. Each optional field
 * has a PEL_TLM_F_* bit in present_mask; a clear bit means "not reported": the
 * writer stores zero and the reader ignores the value. Bits are append-only and
 * a retired bit is never reused (R2). The JSON key of each bit is the lower-case
 * suffix (registry: libpelorus/schema/telemetry-fields.json). */
#define PEL_TLM_F_DISPLAY_INDEX (UINT64_C(1) << 0u)
#define PEL_TLM_F_DECODE_INDEX (UINT64_C(1) << 1u)
#define PEL_TLM_F_FRAME_BYTES (UINT64_C(1) << 2u)
#define PEL_TLM_F_HEADER_BITS (UINT64_C(1) << 3u)
#define PEL_TLM_F_RESIDUAL_BITS (UINT64_C(1) << 4u)
#define PEL_TLM_F_AVG_QP (UINT64_C(1) << 5u)
#define PEL_TLM_F_AVG_QP_NORM (UINT64_C(1) << 6u)
#define PEL_TLM_F_PSNR_Y (UINT64_C(1) << 7u)
#define PEL_TLM_F_PSNR_U (UINT64_C(1) << 8u)
#define PEL_TLM_F_PSNR_V (UINT64_C(1) << 9u)
#define PEL_TLM_F_SSIM_Y (UINT64_C(1) << 10u)
#define PEL_TLM_F_INTRA_FRACTION (UINT64_C(1) << 11u)
#define PEL_TLM_F_INTER_FRACTION (UINT64_C(1) << 12u)
#define PEL_TLM_F_SKIP_FRACTION (UINT64_C(1) << 13u)
#define PEL_TLM_F_PICTURE_TYPE (UINT64_C(1) << 14u)
#define PEL_TLM_F_KEY_FRAME (UINT64_C(1) << 15u)
#define PEL_TLM_F_REFERENCE (UINT64_C(1) << 16u)
#define PEL_TLM_F_SHOWN (UINT64_C(1) << 17u)
#define PEL_TLM_F_SCENE_CUT (UINT64_C(1) << 18u)
#define PEL_TLM_F_QP_MAP (UINT64_C(1) << 19u)
#define PEL_TLM_F_BITS_MAP (UINT64_C(1) << 20u)
#define PEL_TLM_F_MODE_MAP (UINT64_C(1) << 21u)
#define PEL_TLM_F_CODED_BIT_DEPTH (UINT64_C(1) << 22u)
/* Every bit this ABI minor defines (bits 0..22); bits 23..63 are free. A reader
 * ignores bits it does not know (R3); the validator rejects them. */
#define PEL_TLM_KNOWN_MASK ((UINT64_C(1) << 23u) - 1u)

/* frame_flags values; each flag has its own presence bit (15..18). */
#define PEL_TLM_FRAME_KEY 0x01u       /* random-access point: IDR, IRAP, AV1 key frame */
#define PEL_TLM_FRAME_REFERENCE 0x02u /* used for prediction                          */
#define PEL_TLM_FRAME_SHOWN 0x04u     /* displayed; an AV1 hidden alt-ref frame is 0  */
#define PEL_TLM_FRAME_SCENE_CUT 0x08u /* the encoder detected a cut                   */

/* Per-frame map bound (HISS-02): map_cols * map_rows <= this (8K at 8x8 is
 * 518 400 elements). With all three maps a frame carries at most 7 340 032
 * map bytes. */
#define PEL_TLM_MAP_MAX_ELEMS (1u << 20u)

/* Value 0 is invalid in every telemetry enumeration except pel_block_mode. The
 * JSON form writes the lower-case name given in each comment. */
/* pel_tlm_codec: FFmpeg codec names. */
enum pel_tlm_codec {
    PEL_TLM_CODEC_H264 = 1, /* h264 */
    PEL_TLM_CODEC_HEVC = 2, /* hevc */
    PEL_TLM_CODEC_VVC = 3,  /* vvc  */
    PEL_TLM_CODEC_AV1 = 4,  /* av1  */
    PEL_TLM_CODEC_VP9 = 5   /* vp9  */
};

enum pel_qp_scale {
    PEL_QP_SCALE_SLICE_QP = 1,      /* slice_qp: H.26x SliceQpY, -QpBdOffsetY..51 (VVC ..63) */
    PEL_QP_SCALE_AV1_QINDEX = 2,    /* av1_qindex: base_q_idx 0..255                         */
    PEL_QP_SCALE_VP9_QINDEX = 3,    /* vp9_qindex: 0..255                                    */
    PEL_QP_SCALE_AV1_ENCODER_QP = 4 /* av1_encoder_qp: libaom / SVT-AV1 --qp 0..63           */
};

/* pel_picture_type: the numeric values of FFmpeg AVPictureType. */
enum pel_picture_type {
    PEL_PICTURE_I = 1, /* i: also AV1 key and intra-only frames */
    PEL_PICTURE_P = 2, /* p: also every other AV1 frame         */
    PEL_PICTURE_B = 3  /* b                                     */
};

enum pel_tlm_granularity {
    PEL_TLM_GRAN_FRAME = 1, /* frame: no maps                          */
    PEL_TLM_GRAN_ROW = 2,   /* row: map_cols == 1, one element per row */
    PEL_TLM_GRAN_BLOCK = 3  /* block                                   */
};

/* pel_block_mode: mode_map element. */
enum pel_block_mode {
    PEL_BLOCK_MODE_NONE = 0,  /* not reported for this element */
    PEL_BLOCK_MODE_INTRA = 1, /* intra */
    PEL_BLOCK_MODE_INTER = 2, /* inter */
    PEL_BLOCK_MODE_SKIP = 3   /* skip  */
};

enum pel_tlm_metric_source {
    PEL_TLM_METRIC_ENCODER = 1,    /* encoder: reconstruction vs input             */
    PEL_TLM_METRIC_ADAPTER_SSE = 2 /* adapter_sse: PSNR from encoder-reported SSE */
};

enum pel_tlm_adapter {
    PEL_TLM_ADAPTER_FFMPEG_QUALITY_STATS = 1, /* ffmpeg_quality_stats */
    PEL_TLM_ADAPTER_X265_CSV = 2,             /* x265_csv             */
    PEL_TLM_ADAPTER_SVTAV1_STAT_FILE = 3,     /* svtav1_stat_file     */
    PEL_TLM_ADAPTER_LIBAOM_STATS = 4,         /* libaom_stats         */
    PEL_TLM_ADAPTER_VVENC_LOG = 5,            /* vvenc_log            */
    PEL_TLM_ADAPTER_QSV = 6,                  /* qsv                  */
    PEL_TLM_ADAPTER_NVENC = 7,                /* nvenc                */
    PEL_TLM_ADAPTER_AMF = 8,                  /* amf                  */
    PEL_TLM_ADAPTER_VULKAN_FEEDBACK = 9,      /* vulkan_feedback      */
    PEL_TLM_ADAPTER_EXTERNAL = 10             /* external (caller-filled) */
};

/* (i) Encoder telemetry record. 8-byte aligned, no implicit padding. The blob
 * header's bit_depth / grid_cols / grid_rows describe the Pelorus analysis grid
 * and do not apply here: coded_bit_depth and map_cols / map_rows do. Maps sit
 * after the packed sections at 8-aligned blob-relative offsets (the mv_field
 * convention); read them through pel_blob_map(). */
typedef struct PelorusEncTelemetrySection {
    uint64_t present_mask;    /*   0: OR of PEL_TLM_F_*                         */
    uint32_t display_index;   /*   8: 0-based display order            (bit 0)  */
    uint32_t decode_index;    /*  12: 0-based coding order             (bit 1)  */
    uint32_t frame_bytes;     /*  16: coded frame, headers included    (bit 2)  */
    uint32_t header_bits;     /*  20: non-residual bits                (bit 3)  */
    uint32_t residual_bits;   /*  24: transform-coefficient bits       (bit 4)  */
    float avg_qp;             /*  28: frame-mean QP in the qp_scale    (bit 5)  */
    float avg_qp_norm;        /*  32: H.264-equivalent QP              (bit 6)  */
    float psnr_y;             /*  36: dB, finite, >= 0                 (bit 7)  */
    float psnr_u;             /*  40: dB                               (bit 8)  */
    float psnr_v;             /*  44: dB                               (bit 9)  */
    float ssim_y;             /*  48: linear 0..1, not dB              (bit 10) */
    float intra_fraction;     /*  52: luma area share coded intra      (bit 11) */
    float inter_fraction;     /*  56: share coded inter, not skip      (bit 12) */
    float skip_fraction;      /*  60: share coded skip                 (bit 13) */
    uint32_t qp_map_offset;   /*  64: blob-relative, 8-aligned; int16 Q2 (bit 19) */
    uint32_t qp_map_size;     /*  68: map_cols * map_rows * 2          (bit 19) */
    uint32_t bits_map_offset; /*  72: blob-relative, 8-aligned; uint32 (bit 20) */
    uint32_t bits_map_size;   /*  76: map_cols * map_rows * 4          (bit 20) */
    uint32_t mode_map_offset; /*  80: blob-relative, 8-aligned; uint8  (bit 21) */
    uint32_t mode_map_size;   /*  84: map_cols * map_rows              (bit 21) */
    uint16_t map_cols;        /*  88: elements per row (1 for row granularity)  */
    uint16_t map_rows;        /*  90: element rows                              */
    uint8_t codec;            /*  92: enum pel_tlm_codec, never 0               */
    uint8_t qp_scale;         /*  93: enum pel_qp_scale; with bits 5, 6 or 19   */
    uint8_t picture_type;     /*  94: enum pel_picture_type            (bit 14) */
    uint8_t frame_flags;      /*  95: PEL_TLM_FRAME_*              (bits 15-18) */
    uint8_t granularity;      /*  96: enum pel_tlm_granularity, never 0         */
    uint8_t block_size_log2;  /*  97: element edge 1 << n luma px; 2..7, 0 at frame */
    uint8_t coded_bit_depth;  /*  98: 8, 10 or 12                      (bit 22) */
    uint8_t metric_source;    /*  99: enum pel_tlm_metric_source; with bits 7-10 */
    uint8_t adapter;          /* 100: enum pel_tlm_adapter, never 0             */
    uint8_t _pad[3];          /* 101: reserved, zero                            */
    /* --- APPEND-ONLY below this line --- */
} PelorusEncTelemetrySection;

/* ---- (j) Encode-record digest (ABI 1.4, ADR-0175, docs/api/encode-record.md) ---- */

enum pel_digest_alg { PEL_DIGEST_ALG_SHA256 = 1 }; /* 0 is invalid */

enum pel_locator_kind {
    PEL_LOCATOR_NONE = 0,       /* no locator                        */
    PEL_LOCATOR_MEDIA_PATH = 1, /* path relative to the media file   */
    PEL_LOCATOR_URI = 2         /* URI                               */
};

#define PEL_ENCODE_RECORD_MAJOR 1u          /* record_major of pelorus/encode-record/1 */
#define PEL_ENCODE_RECORD_LOCATOR_MAX 4096u /* locator bytes, UTF-8, no NUL            */
#define PEL_DIGEST_TEXT_SIZE 72u            /* "sha256:" + 64 hex digits + NUL         */

/* The SHA-256 digest of a canonical encode record plus a locator of the record
 * file. The record itself never rides in side data. A producer attaches the
 * section at least to the first frame; a consumer takes the first valid one. The
 * locator follows the packed sections at an 8-aligned blob-relative offset, so
 * pel_blob_map(..., locator_size, 1, ...) validates it. */
typedef struct PelorusEncodeRecordSection {
    uint8_t digest[32];      /*  0: raw SHA-256 of the canonical text           */
    uint32_t locator_offset; /* 32: blob-relative offset of the locator         */
    uint32_t locator_size;   /* 36: 0..PEL_ENCODE_RECORD_LOCATOR_MAX; 0 = none  */
    uint8_t digest_alg;      /* 40: enum pel_digest_alg; 0 is invalid           */
    uint8_t locator_kind;    /* 41: enum pel_locator_kind                       */
    uint8_t record_major;    /* 42: PEL_ENCODE_RECORD_MAJOR                     */
    uint8_t _pad[5];         /* 43: reserved, zero                              */
    /* --- APPEND-ONLY below this line --- */
} PelorusEncodeRecordSection;

/* ---- Layout locks (ABI is frozen byte-for-byte; see R1/R2) -------------- */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(PelorusSectionDir) == 16, "PelorusSectionDir ABI");
_Static_assert(sizeof(PelorusSideData) == 48, "PelorusSideData header ABI");
_Static_assert(sizeof(PelorusBandingSection) == 24, "banding section ABI");
_Static_assert(sizeof(PelorusVarianceSection) == 28, "variance section ABI");
_Static_assert(sizeof(PelorusDenoiseSection) == 28, "denoise section ABI");
_Static_assert(sizeof(PelorusFilmGrainSection) == 216, "filmgrain section ABI");
_Static_assert(sizeof(PelorusMotionSection) == 36, "motion section ABI");
_Static_assert(offsetof(PelorusMotionSection, block_size_log2) == 32, "motion section ABI");
_Static_assert(sizeof(PelorusMotionConfSection) == 16, "motion-conf section ABI");
_Static_assert(sizeof(PelorusComplexitySection) == 16, "complexity section ABI");
_Static_assert(sizeof(PelorusQpReportSection) == 64, "qp-report section ABI");
_Static_assert(sizeof(PelorusEncTelemetrySection) == 104, "enc-telemetry section ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, display_index) == 8, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, decode_index) == 12, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, frame_bytes) == 16, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, header_bits) == 20, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, residual_bits) == 24, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, avg_qp) == 28, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, avg_qp_norm) == 32, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, psnr_y) == 36, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, psnr_u) == 40, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, psnr_v) == 44, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, ssim_y) == 48, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, intra_fraction) == 52, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, inter_fraction) == 56, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, skip_fraction) == 60, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, qp_map_offset) == 64, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, qp_map_size) == 68, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, bits_map_offset) == 72, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, bits_map_size) == 76, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, mode_map_offset) == 80, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, mode_map_size) == 84, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, map_cols) == 88, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, map_rows) == 90, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, codec) == 92, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, qp_scale) == 93, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, picture_type) == 94, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, frame_flags) == 95, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, granularity) == 96, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, block_size_log2) == 97, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, coded_bit_depth) == 98, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, metric_source) == 99, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, adapter) == 100, "enc-telemetry ABI");
_Static_assert(offsetof(PelorusEncTelemetrySection, _pad) == 101, "enc-telemetry ABI");
_Static_assert(sizeof(PelorusEncodeRecordSection) == 48, "encode-record section ABI");
_Static_assert(offsetof(PelorusEncodeRecordSection, locator_offset) == 32, "encode-record ABI");
_Static_assert(offsetof(PelorusEncodeRecordSection, locator_size) == 36, "encode-record ABI");
_Static_assert(offsetof(PelorusEncodeRecordSection, digest_alg) == 40, "encode-record ABI");
_Static_assert(offsetof(PelorusEncodeRecordSection, locator_kind) == 41, "encode-record ABI");
_Static_assert(offsetof(PelorusEncodeRecordSection, record_major) == 42, "encode-record ABI");
_Static_assert(offsetof(PelorusEncodeRecordSection, _pad) == 43, "encode-record ABI");
#endif

/* ---- Pack / parse API (implemented in interop.c; vendored by both repos) - */

/* One section to be packed: its bit, a pointer to the POD struct, and the
 * struct's size as known by THIS producer (sizeof(PelorusXxxSection)). */
typedef struct PelorusPackSection {
    enum pel_section id;
    const void *data;
    uint32_t size;
} PelorusPackSection;

/*
 * Pack a UUID-prefixed Pelorus blob ready for av_frame_new_side_data_from_buf.
 *
 * On success allocates *out_blob (caller frees with pel_blob_free / free) and
 * sets *out_len to (16 + total_size). The first 16 bytes are
 * pelorus_sidedata_uuid; the remainder is the flat PelorusSideData image.
 *
 *   meta      header fields the caller controls (frame_pts, plane_layout,
 *             bit_depth, grid_cols, grid_rows, producer_id). The framing
 *             fields (magic, abi versions, total_size, section_mask,
 *             section_count, header_size) are filled in by pack.
 *   sections  the sections to include; section_mask is derived from these.
 *   nb        number of sections (0 is valid -- a header-only blob).
 *
 * Per-cell map data referenced by a section's *_offset fields must already be
 * placed by the caller; v1 sections carry only summary scalars + offsets, and
 * map payloads (when used) are appended by the caller after pack — see
 * docs/api/interop-abi.md. Returns PEL_OK or a negative pel_result.
 */
pel_result pel_blob_pack(const PelorusSideData *meta, const PelorusPackSection *sections, int nb,
                         uint8_t **out_blob, size_t *out_len);

/* Free a buffer returned by pel_blob_pack. */
void pel_blob_free(uint8_t *blob);

/*
 * pel_blob_pack into a caller buffer: the same byte image, no allocation (ABI
 * 1.4; the telemetry packer builds on it so a producer sizes one buffer at init
 * and allocates nothing per frame, HISS-03).
 *
 *   buf, cap  caller buffer and its capacity in bytes.
 *   out_len   receives 16 + total_size on PEL_OK, and the length the image
 *             NEEDS on PEL_ERR_RANGE from a short buffer (query with cap 0).
 *
 * The first *out_len bytes of buf are written (padding zeroed); nothing past
 * them is touched. Returns PEL_OK, PEL_ERR_INVALID (NULL meta/out_len, NULL buf
 * with cap > 0, bad section list), or PEL_ERR_RANGE (unknown section bit, more
 * than 32 sections, size overflow, or cap shorter than the image).
 */
pel_result pel_blob_pack_into(const PelorusSideData *meta, const PelorusPackSection *sections,
                              int nb, uint8_t *buf, size_t cap, size_t *out_len);

/*
 * Validate a UUID-prefixed blob (uuid + magic + abi_major) and locate a
 * section. Returns a pointer into the blob (no copy) plus the number of bytes
 * the consumer may safely read: min(producer_size, consumer_known_size) (R4).
 *
 *   blob, len           the full AV_FRAME_DATA_SEI_UNREGISTERED payload.
 *   sec                 the section to find.
 *   consumer_known_size sizeof(PelorusXxxSection) in the CONSUMER's headers.
 *   out_ptr, out_size   receive the section pointer and readable size.
 *
 * Returns PEL_OK, PEL_ERR_ABSENT (no Pelorus blob / section not present),
 * PEL_ERR_ABI (uuid/magic/major mismatch), or PEL_ERR_TRUNCATED.
 */
pel_result pel_blob_find_section(const uint8_t *blob, size_t len, enum pel_section sec,
                                 size_t consumer_known_size, const void **out_ptr,
                                 size_t *out_size);

/* True if blob/len carries a valid Pelorus blob (uuid + magic + abi_major).
 * Cheap pre-check before iterating sections. */
int pel_blob_is_present(const uint8_t *blob, size_t len);

/*
 * Locate a map (or any other payload) a section references by blob-relative
 * offset and size, after checking it against the blob (ABI 1.4; reader checks 3
 * to 5 of docs/api/encoder-telemetry.md, "Maps"). In order:
 *   - the blob framing (uuid, magic, major, total_size <= len - 16);
 *   - size == elem_count * elem_size, else PEL_ERR_ABI;
 *   - offset % 8 == 0, else PEL_ERR_ABI;
 *   - offset >= header_size + section_count * sizeof(PelorusSectionDir): a map
 *     that overlaps the header or the directory is PEL_ERR_ABI;
 *   - offset <= total_size and size <= total_size - offset, else
 *     PEL_ERR_TRUNCATED.
 * The caller first checks the section's own geometry (presence bit, element
 * counts, their bound). On PEL_OK *out_ptr points into the blob (no copy); it is
 * castable to the element type when the blob base is 8-byte aligned (R5).
 *
 * Returns PEL_OK, PEL_ERR_INVALID (NULL blob/out_ptr, elem_count or elem_size
 * 0), PEL_ERR_ABSENT (not a Pelorus blob), PEL_ERR_ABI or PEL_ERR_TRUNCATED.
 */
pel_result pel_blob_map(const uint8_t *blob, size_t len, uint32_t offset, uint32_t size,
                        uint32_t elem_count, uint32_t elem_size, const void **out_ptr);

/*
 * Write the zero-free carrier form of a blob (ABI 1.5, ADR-0183,
 * docs/api/interop-abi.md "Zero-free carrier form"): pelorus_carrier_uuid,
 * then the COBS encoding (Cheshire and Baker, 1999; no frame delimiter) of the
 * len - 16 bytes after pelorus_sidedata_uuid. The output holds no 0x00 byte.
 * Patch 0022 writes this form on NVENC; the Pelorus filters keep attaching the
 * blob itself.
 *
 *   blob, len  a blob that pel_blob_is_present() accepts; written whole.
 *   out, cap   caller-owned buffer; must not overlap blob.
 *              PEL_CARRIER_MAX_LEN(len) always suffices.
 *   out_len    receives the carrier length on PEL_OK, and the length it NEEDS
 *              on PEL_ERR_RANGE from a short buffer (query with out NULL, cap 0).
 *
 * Reentrant: no global or static state, nothing allocated; concurrent calls on
 * buffers that do not alias are safe. On an error out is not written.
 *   PEL_OK            out[0..*out_len) holds the carrier
 *   PEL_ERR_INVALID   blob or out_len NULL, or out NULL with cap > 0
 *   PEL_ERR_ABSENT    blob is not a Pelorus blob (write it unchanged); *out_len 0
 *   PEL_ERR_RANGE     out NULL or cap shorter than the carrier; *out_len = need
 */
pel_result pel_blob_carrier_encode(const uint8_t *blob, size_t len, uint8_t *out, size_t cap,
                                   size_t *out_len);

/*
 * Accept a user data unregistered payload in either form and hand out the
 * blob (ABI 1.5, ADR-0183). Call it before pel_blob_find_section() on every
 * AV_FRAME_DATA_SEI_UNREGISTERED entry a decoder exports.
 *
 *   data, len    the payload, UUID first.
 *   scratch, cap caller-owned buffer for a decoded carrier; cap >= len always
 *                suffices (decoding never grows the payload). Must not overlap
 *                data (no in-place unwrap); 8-byte aligned keeps the R5 cast
 *                guarantee for section pointers.
 *   out_blob     receives data itself for a blob (pelorus_sidedata_uuid
 *                first; scratch untouched), or scratch for a carrier, decoded
 *                to pelorus_sidedata_uuid + image. It aliases data or scratch:
 *                keep that buffer alive and unchanged while using the blob.
 *   out_len      receives the blob length.
 *
 * The decoder is strict: a 0x00 byte, a code byte whose block runs past the
 * end, an empty final block after a 0xFF block (an encoding the encoder never
 * writes), a decoded image shorter than the header or one without the Pelorus
 * magic and ABI major fail. Sections and maps are checked afterwards by
 * pel_blob_find_section() and pel_blob_map(), as for any blob. Reentrant: no
 * global or static state, nothing allocated. On any error *out_blob is NULL,
 * and for a carrier scratch may be partly written.
 *   PEL_OK            *out_blob, *out_len: the blob
 *   PEL_ERR_INVALID   data, out_blob or out_len NULL, or scratch NULL with cap > 0
 *   PEL_ERR_ABSENT    len < 16 or neither UUID: a foreign payload, ignore it
 *   PEL_ERR_RANGE     carrier with scratch NULL or cap < len; *out_len = len
 *   PEL_ERR_ABI       malformed COBS, or the decoded image is not a Pelorus blob
 *   PEL_ERR_TRUNCATED decoded image shorter than the 48-byte header
 */
pel_result pel_blob_unwrap(const uint8_t *data, size_t len, uint8_t *scratch, size_t cap,
                           const uint8_t **out_blob, size_t *out_len);

/*
 * Format an encode-record section's digest as the 71-character text
 * "sha256:" + 64 lower-case hex digits plus a NUL: the exact string
 * vmafx_context_set_encode_record() accepts (ADR-0175).
 *
 *   s, got  the section and its readable size from pel_blob_find_section.
 *   out     receives the text; cap must be >= PEL_DIGEST_TEXT_SIZE.
 *
 * Returns PEL_OK, PEL_ERR_INVALID (NULL s/out, got shorter than the section, or
 * digest_alg != PEL_DIGEST_ALG_SHA256) or PEL_ERR_RANGE (cap too small). On
 * error out is left unchanged.
 */
pel_result pel_encode_record_digest_text(const PelorusEncodeRecordSection *s, size_t got, char *out,
                                         size_t cap);

/* ---- QP-report reader stub (closed loop; ADR-0114 step 6 / ADR-0119) ----- *
 *
 * Abstract per-block QP/bit readback, decoupled from any vendor SDK type so
 * libpelorus stays dependency-free (it is vendored verbatim by vmafx, which
 * never links oneVPL/NVENC). The encoder-side reader — e.g. a future
 * libavcodec QSV consumer — extracts the per-block actual QP from
 * mfxEncodeBlkStats (mfxMBInfo.Qp for AVC / mfxCTUInfo.QP for HEVC) into a
 * row-major int8 array, reads the frame summary from mfxEncodeFrameStats, then
 * calls this helper to fold both into a PEL_SEC_QPREPORT section ready for
 * pel_blob_pack. v0.1.0 implements the FOLD (block-grid -> cell-grid average
 * QP); the SDK extraction + honored-fraction comparison against the requested
 * ROI map are the documented follow-up (see docs/metrics/qp-feedback.md). */
typedef struct PelorusQpReportInput {
    const int8_t *block_qp;  /* row-major per-block actual QP, blk_cols*blk_rows */
    uint16_t blk_cols;       /* encoder block grid width (frame_w / block_edge)   */
    uint16_t blk_rows;       /* encoder block grid height                         */
    uint8_t block_size_log2; /* log2 block edge (4 => 16x16)                     */
    uint8_t report_source;   /* enum pel_qp_report_source                         */
    uint8_t _pad[2];         /* reserved, zero                                    */
    float avg_qp;            /* frame mean QP (mfxEncodeFrameStats.Qp)            */
    float psnr_y;            /* encoder-reported PSNR (dB); pass 0 if absent      */
    float psnr_u;
    float psnr_v;
    uint64_t total_bits;         /* encoded frame size in bits                   */
    uint32_t num_intra_blocks;   /* mfxEncodeFrameStats.NumIntraBlock            */
    uint32_t num_inter_blocks;   /* mfxEncodeFrameStats.NumInterBlock            */
    uint32_t num_skipped_blocks; /* mfxEncodeFrameStats.NumSkippedBlock          */
} PelorusQpReportInput;

/*
 * Build a PEL_SEC_QPREPORT section + its per-cell int8 QP map from a readback.
 *
 * Folds the encoder's per-block QP grid (in->block_qp, blk_cols x blk_rows)
 * onto the blob's (grid_cols x grid_rows) cell grid by averaging the blocks
 * that fall in each cell, writing the result into qp_cell_out and setting the
 * section's qp_cell_* fields. The frame-summary scalars are copied through.
 *
 *   in              the abstract readback (see PelorusQpReportInput).
 *   grid_cols/rows  the blob cell grid (must match the meta passed to pack).
 *   out_section     receives the populated PelorusQpReportSection. The caller
 *                   sets qp_cell_offset to the blob-relative offset where it
 *                   will append qp_cell_out AFTER pack (mirrors the other
 *                   per-cell map sections — see docs/api/interop-abi.md).
 *   qp_cell_out     caller buffer, >= grid_cols*grid_rows bytes, receives the
 *                   folded int8 per-cell QP map (NULL => frame-stats only,
 *                   qp_valid = 0).
 *   qp_cell_cap     capacity of qp_cell_out in bytes.
 *
 * honored_fraction is left 0 here (no requested map is available to this fold);
 * a consumer that holds the requested ROI delta-QP map sets it afterwards.
 * Returns PEL_OK, PEL_ERR_INVALID (NULL/empty grids), or PEL_ERR_RANGE
 * (qp_cell_out too small for the cell grid).
 */
pel_result pel_qp_report_from_blocks(const PelorusQpReportInput *in, uint16_t grid_cols,
                                     uint16_t grid_rows, PelorusQpReportSection *out_section,
                                     int8_t *qp_cell_out, size_t qp_cell_cap);

/* ---- x265 CSV stat reader (the runnable closed-loop surface) ------------- *
 *
 * The QSV `mfxEncodeBlkStats` path (the v1 source named in ADR-0119) needs
 * Intel HW that is low-power-bugged on the dev box, so per-block readback there
 * is code-complete-but-unvalidated. To get ONE end-to-end-runnable surface that
 * actually populates PEL_SEC_QPREPORT with non-synthetic numbers, libpelorus
 * also reads the per-frame statistics x265 emits with `--csv --csv-log-level 2`
 * (HEVC software encoder). x265 is a real post-encode honored-QP surface: the
 * CSV's Type/POC/QP/Bits/PSNR columns are the encoder's ACTUAL per-frame
 * decisions, and under fixed-QP (`--qp N --aq-mode 0`) the per-slice-type QP the
 * encoder honors differs from the single requested QP, which is exactly the
 * "requested vs honored" signal the loop verifies.
 *
 * libpelorus stays SDK-free: this reader is pure stdio CSV parsing (no oneVPL,
 * no libx265 link), so vmafx still vendors interop.c verbatim. The granularity
 * is FRAME-level (x265 CSV does not expose per-CTU QP); the per-cell qp_cell map
 * is therefore left unpopulated (qp_valid = 0) by this reader, matching the
 * frame-stats-only branch of pel_qp_report_from_blocks. ADR-0122. */

/* One parsed x265 CSV frame row (the columns this reader consumes; x265 emits
 * far more, all ignored). Display-order is irrelevant — rows are matched to the
 * requested QP array by encode order of appearance, NOT POC, because the
 * requested per-frame QP a downstream pass holds is in encode order. */
typedef struct PelorusX265Frame {
    int32_t poc;     /* picture order count (display order), for diagnostics    */
    float qp;        /* the QP x265 honored for this frame (CSV "QP" column)    */
    uint64_t bits;   /* encoded bits for this frame (CSV "Bits" column)         */
    float psnr_y;    /* CSV "Y PSNR" (dB); 0 if the CSV had no PSNR columns      */
    float psnr_u;    /* CSV "U PSNR"                                            */
    float psnr_v;    /* CSV "V PSNR"                                            */
    char slice_type; /* 'I' / 'P' / 'B' from the CSV "Type" column              */
} PelorusX265Frame;
/* Not a wire type, but a public transfer struct written into a caller buffer by
 * pel_x265_csv_parse — pin the layout so a stray field insert is a build error. */
_Static_assert(sizeof(PelorusX265Frame) == 32, "PelorusX265Frame helper layout");

/*
 * Parse an x265 `--csv --csv-log-level 2` file into per-frame rows.
 *
 * Reads the column header to locate Type/POC/QP/Bits and (optionally) the
 * Y/U/V PSNR columns by name, so it is robust to x265's column-set changing
 * with build flags. Rows are returned in the file's row order (x265 writes one
 * row per encoded frame in encode order).
 *
 *   path       the CSV file written by x265, as a NUL-terminated UTF-8 string on
 *              every platform (ADR-0149). POSIX passes the bytes to fopen
 *              unchanged; Windows converts them to UTF-16 and uses _wfopen, so
 *              the process ANSI code page never applies. No \\?\ prefix is
 *              added: pass one yourself for a Windows path over MAX_PATH.
 *   out_frames caller buffer of >= cap PelorusX265Frame; receives the rows.
 *   cap        capacity of out_frames in entries.
 *   out_count  receives the number of frame rows parsed (<= cap).
 *
 * Returns PEL_OK, PEL_ERR_INVALID (NULL args, cap == 0, or — Windows only — a
 * path that is not well-formed UTF-8), PEL_ERR_ABSENT (the file cannot be
 * opened — missing, unreadable, or on Windows longer than 32767 UTF-16 code
 * units — / no header / no recognizable QP+Bits columns), PEL_ERR_NOMEM
 * (Windows only: the temporary UTF-16 path copy could not be allocated),
 * PEL_ERR_TRUNCATED (read error mid-file), or PEL_ERR_RANGE (more rows in the
 * file than cap — out_count is set to cap and the tail is dropped).
 */
pel_result pel_x265_csv_parse(const char *path, PelorusX265Frame *out_frames, size_t cap,
                              size_t *out_count);

/*
 * Fold parsed x265 per-frame rows into a single PEL_SEC_QPREPORT section.
 *
 * Aggregates the GOP the CSV describes into one frame-stats-only report: avg_qp
 * is the bit-weighted mean honored QP, total_bits the sum, psnr_* the
 * bit-weighted mean PSNR (0 if the CSV had none). qp_valid stays 0 (no per-cell
 * map — x265 CSV is frame-granular). report_source is tagged PEL_QPSRC_NONE
 * (x265 is a software reference surface, not one of the HW vendor enums).
 *
 * honored_fraction is computed when requested_qp is non-NULL: it is the fraction
 * of frames whose honored QP moved in the SAME direction, relative to the GOP
 * mean, as the requested QP did — i.e. the sign-agreement between the requested
 * per-frame delta-QP and the achieved per-frame delta-QP. A frame where both
 * deltas are ~0 (within eps) counts as agreement. This is the frame-granular
 * analogue of the per-cell ROI honored-fraction the QSV path will compute. With
 * requested_qp NULL it is left 0 (no request map to compare against).
 *
 *   frames        the parsed rows (from pel_x265_csv_parse).
 *   nb            number of rows.
 *   requested_qp  optional: the per-frame QP a downstream pass REQUESTED, same
 *                 order/length as frames (NULL => honored_fraction stays 0).
 *   out_section   receives the populated PelorusQpReportSection (qp_valid = 0).
 *
 * Returns PEL_OK or PEL_ERR_INVALID (NULL frames/out_section, or nb == 0).
 */
pel_result pel_qp_report_from_x265_frames(const PelorusX265Frame *frames, size_t nb,
                                          const float *requested_qp,
                                          PelorusQpReportSection *out_section);

#ifdef __cplusplus
}
#endif
#endif /* PELORUS_INTEROP_H */
