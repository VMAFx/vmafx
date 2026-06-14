/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * VENDORED FROM VMAFx/pelorus@835e097 — DO NOT EDIT. Append-only ABI; single
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
 *       PELORUS_ABI_MINOR bumps when a new section bit or appended field lands.
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
 * which versions the library, and of the vmafx control-plane API version). */
#define PELORUS_ABI_MAJOR 1u
#define PELORUS_ABI_MINOR 0u

/* The 16-byte UUID prefixing the AV_FRAME_DATA_SEI_UNREGISTERED payload (the
 * leading uuid_iso_iec_11578 mandated by the user-data-unregistered SEI
 * layout). Fixed, project-owned, version-4 random UUID. It is the routing key
 * that distinguishes a Pelorus blob from any other unregistered SEI on a frame.
 *
 *   pelorus-sidedata-v1 = e1d7c4a2-6b93-4f08-9a55-0f3c2db17e64
 */
#define PELORUS_SIDEDATA_UUID_LEN 16
extern const uint8_t pelorus_sidedata_uuid[PELORUS_SIDEDATA_UUID_LEN];

/* ---- Section catalogue (R1/R2: append-only; bits are NEVER reused) ------ */

enum pel_section {
    PEL_SEC_BANDING = 1u << 0,   /* (a) banding / flatness map summary       */
    PEL_SEC_VARIANCE = 1u << 1,  /* (b) local variance / edge summary        */
    PEL_SEC_DENOISE = 1u << 2,   /* (c) denoise residual statistics          */
    PEL_SEC_FILMGRAIN = 1u << 3, /* (d) film-grain params (AV1-shaped)       */
    PEL_SEC_MOTION = 1u << 4     /* (e) optical-flow MV hint summary         */
    /* bits 5..31 reserved — a retired bit is NEVER reused (R2). */
};

/* ---- Plane layout / producer identity ----------------------------------- */

enum pel_plane_layout {
    PEL_LAYOUT_420 = 0,
    PEL_LAYOUT_422 = 1,
    PEL_LAYOUT_444 = 2
};

/* Film-grain synthesis model the estimate targets. The deband/denoise/motion
 * filters are codec-agnostic (they help any HW encoder); only grain synthesis
 * is codec-specific — AV1 uses AOM params, HEVC/H.265 + VVC/H.266 use H.274
 * (SEI film-grain characteristics). The film-grain section carries both. */
enum pel_grain_model {
    PEL_GRAIN_NONE = 0,
    PEL_GRAIN_AOM = 1,  /* AV1 — maps to AV_FILM_GRAIN_PARAMS_AV1            */
    PEL_GRAIN_H274 = 2  /* HEVC/VVC — maps to AV_FILM_GRAIN_PARAMS_H274      */
};

/* fourcc of the writing stage, for diagnostics (e.g. 'PLRS'). */
#define PEL_FOURCC(a, b, c, d)                                                 \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |           \
     ((uint32_t)(d) << 24))

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
    uint8_t magic[8];      /* PELORUS_MAGIC_STR                               */
    uint16_t abi_major;    /* PELORUS_ABI_MAJOR                               */
    uint16_t abi_minor;    /* producer's PELORUS_ABI_MINOR                    */
    uint32_t total_size;   /* total blob bytes (header + dir[] + sections)    */
    uint32_t section_mask; /* OR of present enum pel_section bits             */
    uint16_t section_count;/* number of PelorusSectionDir entries following   */
    uint16_t header_size;  /* sizeof(PelorusSideData); dir[] starts here      */
    uint64_t frame_pts;    /* echo of AVFrame.pts for desync detection        */
    uint8_t plane_layout;  /* enum pel_plane_layout                           */
    uint8_t bit_depth;     /* 8 / 10 / 12                                     */
    uint16_t grid_cols;    /* cell grid shared by all map summaries           */
    uint16_t grid_rows;
    uint16_t _pad0;        /* reserved, zero                                  */
    uint32_t producer_id;  /* PEL_FOURCC of the writer                        */
    uint32_t _pad1;        /* reserved, zero (keeps sizeof a multiple of 8)   */
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
    float global_variance;  /* spatial activity, normalized                  */
    float edge_density;     /* fraction of edge pixels                       */
    float texture_energy;   /* high-frequency energy proxy                   */
    uint32_t var_cell_offset;  /* blob-relative; float variance per cell     */
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
    int32_t num_y_points;           /* AV1: <= 14                            */
    int32_t num_uv_points[2];       /* AV1: {cb, cr}, each <= 10             */
    int32_t scaling_shift;          /* AV1 [8,11]                            */
    int32_t ar_coeff_lag;           /* AV1: coeff count = 2*lag*(lag+1)      */
    int32_t ar_coeff_shift;         /* AV1 [6,9]                            */
    int32_t grain_scale_shift;      /* AV1                                   */
    int32_t uv_mult[2];             /* AV1                                   */
    int32_t uv_mult_luma[2];        /* AV1                                   */
    int32_t uv_offset[2];           /* AV1 9-bit [-256,255]                  */
    uint8_t apply;                  /* 1 => producer recommends grain synth   */
    uint8_t chroma_scaling_from_luma; /* AV1                                  */
    uint8_t overlap_flag;           /* AV1                                   */
    uint8_t limit_output_range;     /* AV1                                   */
    uint8_t y_points[14][2];        /* AV1: {value, scaling} (== AOM)        */
    uint8_t uv_points[2][10][2];    /* AV1 (== AOM)                          */
    int8_t ar_coeffs_y[24];         /* AV1 (== AOM)                          */
    int8_t ar_coeffs_uv[2][25];     /* AV1 (== AOM)                          */
    uint8_t grain_model;            /* enum pel_grain_model (none/aom/h274)  */
    uint8_t h274_model_id;          /* H.274 FGC model_id (0=freq,1=AR)      */
    uint8_t h274_blending_mode;     /* H.274 blending_mode_id                */
    uint8_t h274_log2_scale;        /* H.274 log2_scale_factor               */
    uint8_t _pad[6];                /* reserved, zero                        */
    /* --- APPEND-ONLY below this line --- */
} PelorusFilmGrainSection;

/* (e) Optical-flow motion-vector hint summary — written by vf_pelorus_mc. */
typedef struct PelorusMotionSection {
    float global_motion_x;      /* mean MV, pixels                           */
    float global_motion_y;
    float motion_magnitude_mean;
    float motion_magnitude_p95; /* 95th pct — scene-cut / pan detector        */
    float motion_entropy;       /* MV-field complexity                       */
    uint32_t mv_field_offset;   /* blob-relative; int16 (dx,dy) per cell      */
    uint32_t mv_field_size;     /* grid_cols*grid_rows*2*sizeof(int16)        */
    uint8_t has_scene_cut;      /* producer's scene-cut flag                 */
    uint8_t _pad[3];            /* reserved, zero                            */
    /* --- APPEND-ONLY below this line --- */
} PelorusMotionSection;

/* ---- Layout locks (ABI is frozen byte-for-byte; see R1/R2) -------------- */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(PelorusSectionDir) == 16, "PelorusSectionDir ABI");
_Static_assert(sizeof(PelorusSideData) == 48, "PelorusSideData header ABI");
_Static_assert(sizeof(PelorusBandingSection) == 24, "banding section ABI");
_Static_assert(sizeof(PelorusVarianceSection) == 28, "variance section ABI");
_Static_assert(sizeof(PelorusDenoiseSection) == 28, "denoise section ABI");
_Static_assert(sizeof(PelorusFilmGrainSection) == 216, "filmgrain section ABI");
_Static_assert(sizeof(PelorusMotionSection) == 32, "motion section ABI");
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
pel_result pel_blob_pack(const PelorusSideData *meta,
                         const PelorusPackSection *sections, int nb,
                         uint8_t **out_blob, size_t *out_len);

/* Free a buffer returned by pel_blob_pack. */
void pel_blob_free(uint8_t *blob);

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
pel_result pel_blob_find_section(const uint8_t *blob, size_t len,
                                 enum pel_section sec,
                                 size_t consumer_known_size,
                                 const void **out_ptr, size_t *out_size);

/* True if blob/len carries a valid Pelorus blob (uuid + magic + abi_major).
 * Cheap pre-check before iterating sections. */
int pel_blob_is_present(const uint8_t *blob, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* PELORUS_INTEROP_H */
