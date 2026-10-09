/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VENDORED FROM VMAFx/pelorus@e2e4040311a443210927549c3a336f909c6473f3 — DO NOT EDIT.
 * Append-only ABI; single
 * source of truth is pelorus. Re-sync via scripts/sync-pelorus-interop.sh.
 * See docs/adr/1113-vendor-pelorus-interop-abi.md.
 *
 * Local edit vs the pelorus original: the intra-pelorus #include below is
 * rewritten from "pelorus/interop.h" to "libvmaf/pelorus/interop.h" so it resolves
 * under core/include/. Nothing else is changed.
 */

/*
 * interop.c — pack/parse for the Pelorus <-> vmafx side-data blob.
 *
 * Wire image (after the 16-byte UUID prefix):
 *   [PelorusSideData header (48)] [PelorusSectionDir dir[count] (16*count)]
 *   [section payloads, each 8-byte aligned]
 * All offsets in dir[] are relative to magic[0] (the header start). Section
 * payload starts are padded up to 8 bytes so a consumer can cast the returned
 * pointer to the section struct without an unaligned access (R5) -- note that
 * this guarantee is RELATIVE TO THE BLOB BASE: it holds for the caller only if
 * the caller's blob base is itself 8-byte aligned. A caller that cannot promise
 * that must memcpy the section bytes into a local, as vmafx's perceptual_weight.c
 * already does.
 *
 * The PARSER itself makes no such assumption. It reads the header and every
 * directory entry via memcpy into properly-aligned locals rather than casting
 * `base + offset`, so passing a misaligned blob is well-defined rather than UB
 * on strict-alignment targets and under -fsanitize=alignment (issue #44).
 */

/* NOLINTBEGIN(modernize-use-nullptr): this C translation unit is built as C23
 * by vmafx, where clang-tidy also proposes the `nullptr` keyword, but MSVC's C
 * mode has no `nullptr` (C2065); the Windows builds compile it with cl.exe.
 * The NULL macro stays. Same decision as vmafx ADR-1138
 * (docs/adr/1138-c-translation-units-keep-null.md in VMAFx/vmafx). */

#include "libvmaf/pelorus/interop.h"

#include <stdlib.h>
#include <string.h>

/* pelorus-sidedata-v1 = e1d7c4a2-6b93-4f08-9a55-0f3c2db17e64 */
const uint8_t pelorus_sidedata_uuid[PELORUS_SIDEDATA_UUID_LEN] = {
    0xe1, 0xd7, 0xc4, 0xa2, 0x6b, 0x93, 0x4f, 0x08, 0x9a, 0x55, 0x0f, 0x3c, 0x2d, 0xb1, 0x7e, 0x64};

#define PEL_ALIGN8(x) (((x) + 7u) & ~7u)

/* Section bits that are individually valid (R3); reject anything else. */
static int section_bit_valid(uint32_t id)
{
    switch (id) {
    case PEL_SEC_BANDING:
    case PEL_SEC_VARIANCE:
    case PEL_SEC_DENOISE:
    case PEL_SEC_FILMGRAIN:
    case PEL_SEC_MOTION:
    case PEL_SEC_QPREPORT:
    case PEL_SEC_MOTION_CONF:
    case PEL_SEC_COMPLEXITY:
        return 1;
    default:
        return 0;
    }
}

/* Validate sections, derive the present-section mask, reject duplicates. */
static pel_result validate_pack_sections(const PelorusPackSection *sections, int nb,
                                         uint32_t *out_mask)
{
    uint32_t mask = 0;
    int i;

    for (i = 0; i < nb; i++) {
        if (sections[i].data == NULL || sections[i].size == 0) {
            return PEL_ERR_INVALID;
        }
        if (!section_bit_valid((uint32_t)sections[i].id)) {
            return PEL_ERR_RANGE;
        }
        if (mask & (uint32_t)sections[i].id) {
            return PEL_ERR_INVALID; /* duplicate section */
        }
        mask |= (uint32_t)sections[i].id;
    }
    *out_mask = mask;
    return PEL_OK;
}

/* Fill the fixed framing fields of the blob header. */
static void write_pack_header(PelorusSideData *hdr, const PelorusSideData *meta,
                              uint32_t total_size, uint32_t section_mask, int nb)
{
    memcpy(hdr->magic, PELORUS_MAGIC_STR, PELORUS_MAGIC_LEN);
    hdr->abi_major = (uint16_t)PELORUS_ABI_MAJOR;
    hdr->abi_minor = (uint16_t)PELORUS_ABI_MINOR;
    hdr->total_size = total_size;
    hdr->section_mask = section_mask;
    hdr->section_count = (uint16_t)nb;
    hdr->header_size = (uint16_t)sizeof(PelorusSideData);
    hdr->frame_pts = meta->frame_pts;
    hdr->plane_layout = meta->plane_layout;
    hdr->bit_depth = meta->bit_depth;
    hdr->grid_cols = meta->grid_cols;
    hdr->grid_rows = meta->grid_rows;
    hdr->_pad0 = 0;
    hdr->producer_id = meta->producer_id;
    hdr->_pad1 = 0;
}

/* Total payload bytes of all (aligned) sections, starting at `cursor`. Accumulated in 64-bit so
 * the per-section 8-byte alignment and the aggregate sum cannot wrap the uint32 wire field
 * (PEL_ALIGN8 is 32-bit; a near-UINT32_MAX section would wrap to a tiny payload -> undersized
 * alloc + heap overflow on the memcpy of the payloads). */
static pel_result pack_total_size(const PelorusPackSection *sections, int nb, uint32_t cursor,
                                  uint32_t *out_total)
{
    uint64_t need = cursor;
    int i;

    for (i = 0; i < nb; i++) {
        uint64_t aligned = ((uint64_t)sections[i].size + 7u) & ~(uint64_t)7u;
        need += aligned;
    }
    if (need > UINT32_MAX) { /* total_size is a uint32_t wire field */
        return PEL_ERR_RANGE;
    }
    *out_total = (uint32_t)need;
    return PEL_OK;
}

/* Write the section directory and the 8-aligned payloads after the header. */
static void write_pack_sections(uint8_t *blob, const PelorusPackSection *sections, int nb,
                                uint32_t cursor)
{
    const uint32_t header_size = (uint32_t)sizeof(PelorusSideData);
    int i;

    for (i = 0; i < nb; i++) {
        PelorusSectionDir ent;

        ent.section_id = (uint32_t)sections[i].id;
        ent.offset = cursor; /* relative to magic[0] */
        ent.size = sections[i].size;
        ent.struct_minor = (uint32_t)PELORUS_ABI_MINOR;
        memcpy(blob + PELORUS_SIDEDATA_UUID_LEN + (size_t)header_size + (size_t)i * sizeof(ent),
               &ent, sizeof(ent));
        memcpy(blob + PELORUS_SIDEDATA_UUID_LEN + cursor, sections[i].data, sections[i].size);
        cursor += PEL_ALIGN8(sections[i].size);
    }
}

pel_result pel_blob_pack(const PelorusSideData *meta, const PelorusPackSection *sections, int nb,
                         uint8_t **out_blob, size_t *out_len)
{
    const uint32_t header_size = (uint32_t)sizeof(PelorusSideData);
    const uint32_t dir_size = (uint32_t)sizeof(PelorusSectionDir);
    uint32_t section_mask = 0;
    uint32_t cursor;
    uint32_t total_size = 0;
    size_t blob_len;
    uint8_t *blob;
    PelorusSideData hdr;
    pel_result rc;

    if (meta == NULL || out_blob == NULL || out_len == NULL) {
        return PEL_ERR_INVALID;
    }
    if (nb < 0 || (nb > 0 && sections == NULL)) {
        return PEL_ERR_INVALID;
    }
    if (nb > 32) { /* at most one entry per possible section bit */
        return PEL_ERR_RANGE;
    }

    rc = validate_pack_sections(sections, nb, &section_mask);
    if (rc != PEL_OK) {
        return rc;
    }

    /* Layout: header, dir[], then 8-aligned section payloads (header + dir is 48 + 16*nb
     * bytes, already a multiple of 8). */
    cursor = PEL_ALIGN8(header_size + (uint32_t)nb * dir_size);

    *out_blob = NULL;
    *out_len = 0;

    rc = pack_total_size(sections, nb, cursor, &total_size);
    if (rc != PEL_OK) {
        return rc;
    }

    blob_len = (size_t)PELORUS_SIDEDATA_UUID_LEN + (size_t)total_size;
    blob = calloc(1, blob_len); /* zero-fill so padding is deterministic */
    if (blob == NULL) {
        return PEL_ERR_NOMEM;
    }

    /* UUID prefix, then the header, built in an aligned local and memcpy'd out: both directions
     * stay cast-free, which keeps bugprone-casting-through-void a real guard (issue #44). */
    memcpy(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN);
    write_pack_header(&hdr, meta, total_size, section_mask, nb);
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN, &hdr, sizeof(hdr));

    /* Directory + payloads. */
    write_pack_sections(blob, sections, nb, cursor);

    *out_blob = blob;
    *out_len = blob_len;
    return PEL_OK;
}

void pel_blob_free(uint8_t *blob)
{
    free(blob);
}

int pel_blob_is_present(const uint8_t *blob, size_t len)
{
    PelorusSideData hdr;

    if (blob == NULL || len < (size_t)PELORUS_SIDEDATA_UUID_LEN + sizeof(PelorusSideData)) {
        return 0;
    }
    if (memcmp(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN) != 0) {
        return 0;
    }
    /* memcpy, not a cast: the blob base is caller-supplied and may be misaligned. */
    memcpy(&hdr, blob + PELORUS_SIDEDATA_UUID_LEN, sizeof(hdr));
    if (memcmp(hdr.magic, PELORUS_MAGIC_STR, PELORUS_MAGIC_LEN) != 0) {
        return 0;
    }
    return hdr.abi_major == (uint16_t)PELORUS_ABI_MAJOR;
}

/* Validate the blob framing and the header for `sec`; on PEL_OK `*hdr` holds the header and
 * `*image` / `*image_len` the byte range that starts at the header magic. */
static pel_result find_section_framing(const uint8_t *blob, size_t len, enum pel_section sec,
                                       PelorusSideData *hdr, const uint8_t **image,
                                       size_t *image_len)
{
    if (len < (size_t)PELORUS_SIDEDATA_UUID_LEN + sizeof(PelorusSideData)) {
        return PEL_ERR_ABSENT;
    }
    if (memcmp(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN) != 0) {
        return PEL_ERR_ABSENT;
    }

    *image = blob + PELORUS_SIDEDATA_UUID_LEN;
    *image_len = len - (size_t)PELORUS_SIDEDATA_UUID_LEN;
    /* memcpy, not a cast: the blob base is caller-supplied and may be misaligned. */
    memcpy(hdr, *image, sizeof(*hdr));

    if (memcmp(hdr->magic, PELORUS_MAGIC_STR, PELORUS_MAGIC_LEN) != 0) {
        return PEL_ERR_ABSENT;
    }
    if (hdr->abi_major != (uint16_t)PELORUS_ABI_MAJOR) {
        return PEL_ERR_ABI; /* consumer cannot trust the layout (R6) */
    }
    /* Framing sanity: declared size must fit, dir[] must fit. */
    if (hdr->total_size > *image_len || hdr->header_size < sizeof(*hdr)) {
        return PEL_ERR_TRUNCATED;
    }
    /* The packer always 8-aligns the directory. A header_size that is not a
     * multiple of 8 is corrupt framing from an untrusted producer; reject it
     * rather than walking a misaligned dir[]. */
    if ((hdr->header_size & 7u) != 0u) {
        return PEL_ERR_ABI;
    }
    if ((size_t)hdr->header_size + (size_t)hdr->section_count * sizeof(PelorusSectionDir) >
        *image_len) {
        return PEL_ERR_TRUNCATED;
    }
    if ((hdr->section_mask & (uint32_t)sec) == 0) {
        return PEL_ERR_ABSENT;
    }
    return PEL_OK;
}

/* Check one directory entry against the image and hand out its payload range. */
static pel_result find_section_payload(const PelorusSectionDir *ent, const uint8_t *image,
                                       size_t image_len, size_t consumer_known_size,
                                       const void **out_ptr, size_t *out_size)
{
    size_t off = ent->offset;
    size_t sz = ent->size;

    /* R5: the packer 8-aligns every section payload so a consumer can cast the
     * returned pointer to the section struct (which may hold a u64) without an
     * unaligned access. A misaligned offset is corrupt framing, not a short
     * buffer — reject it before handing out a castable pointer. */
    if ((off & 7u) != 0u) {
        return PEL_ERR_ABI;
    }
    if (off > image_len || sz > image_len - off) {
        return PEL_ERR_TRUNCATED;
    }
    *out_ptr = image + off;
    *out_size = (sz < consumer_known_size) ? sz : consumer_known_size; /* R4 */
    return PEL_OK;
}

pel_result pel_blob_find_section(const uint8_t *blob, size_t len, enum pel_section sec,
                                 size_t consumer_known_size, const void **out_ptr, size_t *out_size)
{
    PelorusSideData hdr;
    const uint8_t *image = NULL;
    size_t image_len = 0;
    pel_result rc;
    uint16_t i;

    if (out_ptr != NULL) {
        *out_ptr = NULL;
    }
    if (out_size != NULL) {
        *out_size = 0;
    }
    if (blob == NULL || out_ptr == NULL || out_size == NULL) {
        return PEL_ERR_INVALID;
    }
    rc = find_section_framing(blob, len, sec, &hdr, &image, &image_len);
    if (rc != PEL_OK) {
        return rc;
    }

    for (i = 0; i < hdr.section_count; i++) {
        PelorusSectionDir ent;

        memcpy(&ent, image + (size_t)hdr.header_size + (size_t)i * sizeof(ent), sizeof(ent));
        if (ent.section_id != (uint32_t)sec) {
            continue;
        }
        return find_section_payload(&ent, image, image_len, consumer_known_size, out_ptr, out_size);
    }
    return PEL_ERR_ABSENT;
}

/* Copy the frame statistics into the section; the per-cell QP grid is not valid yet. */
static void qp_report_fill_stats(const PelorusQpReportInput *in, PelorusQpReportSection *out)
{
    memset(out, 0, sizeof(*out));
    out->avg_qp = in->avg_qp;
    out->psnr_y = in->psnr_y;
    out->psnr_u = in->psnr_u;
    out->psnr_v = in->psnr_v;
    out->total_bits = in->total_bits;
    out->num_intra_blocks = in->num_intra_blocks;
    out->num_inter_blocks = in->num_inter_blocks;
    out->num_skipped_blocks = in->num_skipped_blocks;
    out->block_size_log2 = in->block_size_log2;
    out->report_source = in->report_source;
    out->honored_fraction = 0.0f; /* no requested map at this layer */
    out->qp_valid = 0;
}

/* Average the blocks whose centre lands in cell (cx, cy): nearest-cell box. */
static int8_t qp_cell_average(const PelorusQpReportInput *in, uint16_t grid_cols,
                              uint16_t grid_rows, uint16_t cx, uint16_t cy)
{
    uint32_t bx0 = (uint32_t)cx * in->blk_cols / grid_cols;
    uint32_t bx1 = (uint32_t)(cx + 1) * in->blk_cols / grid_cols;
    uint32_t by0 = (uint32_t)cy * in->blk_rows / grid_rows;
    uint32_t by1 = (uint32_t)(cy + 1) * in->blk_rows / grid_rows;
    int64_t sum = 0; /* int64 so the accumulate + divide stay exact and */
    uint32_t n = 0;  /* sign-correct for any type-legal block count       */
    uint32_t by;

    if (bx1 <= bx0) {
        bx1 = bx0 + 1; /* guarantee >=1 sampled block when cells > blocks */
    }
    if (by1 <= by0) {
        by1 = by0 + 1;
    }
    for (by = by0; by < by1 && by < in->blk_rows; by++) {
        uint32_t bx;
        for (bx = bx0; bx < bx1 && bx < in->blk_cols; bx++) {
            sum += in->block_qp[by * (uint32_t)in->blk_cols + bx];
            n++;
        }
    }
    return (n > 0U) ? (int8_t)(sum / (int64_t)n) : 0;
}

pel_result pel_qp_report_from_blocks(const PelorusQpReportInput *in, uint16_t grid_cols,
                                     uint16_t grid_rows, PelorusQpReportSection *out_section,
                                     int8_t *qp_cell_out, size_t qp_cell_cap)
{
    uint32_t cells;
    uint16_t cy;

    if (in == NULL || out_section == NULL || grid_cols == 0 || grid_rows == 0) {
        return PEL_ERR_INVALID;
    }

    qp_report_fill_stats(in, out_section);

    /* Frame-stats-only path: caller passed no per-block grid. */
    if (in->block_qp == NULL || qp_cell_out == NULL || in->blk_cols == 0 || in->blk_rows == 0) {
        return PEL_OK;
    }

    cells = (uint32_t)grid_cols * (uint32_t)grid_rows;
    if (qp_cell_cap < (size_t)cells) {
        return PEL_ERR_RANGE;
    }

    /* Fold the block grid onto the cell grid: each cell averages the blocks
     * whose centre lands in it (nearest-cell box). The block and cell grids are
     * independent rasters over the same frame, so map by proportional index. */
    for (cy = 0; cy < grid_rows; cy++) {
        uint16_t cx;
        for (cx = 0; cx < grid_cols; cx++) {
            qp_cell_out[(uint32_t)cy * grid_cols + cx] =
                qp_cell_average(in, grid_cols, grid_rows, cx, cy);
        }
    }

    out_section->qp_valid = 1;
    out_section->qp_cell_size = cells; /* int8 per cell */
    /* qp_cell_offset is filled by the caller after it picks the blob layout. */
    return PEL_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
