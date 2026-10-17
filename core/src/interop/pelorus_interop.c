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

/* pelorus-sidedata-zero-free-v1 = 3f9b37b8-fd9a-4621-920e-9b78b55cf9b5 (ABI 1.5) */
const uint8_t pelorus_carrier_uuid[PELORUS_SIDEDATA_UUID_LEN] = {
    0x3f, 0x9b, 0x37, 0xb8, 0xfd, 0x9a, 0x46, 0x21, 0x92, 0x0e, 0x9b, 0x78, 0xb5, 0x5c, 0xf9, 0xb5};

/* COBS code byte of a full block: 254 data bytes and no implied zero. */
#define PEL_COBS_FULL 0xFFu

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
    case PEL_SEC_ENC_TELEMETRY:
    case PEL_SEC_ENCODE_RECORD:
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

/* Check the pack arguments and compute the image layout: section mask, first
 * payload offset (`cursor`) and total_size. Shared by both pack entry points. */
static pel_result pack_layout(const PelorusSideData *meta, const PelorusPackSection *sections,
                              int nb, uint32_t *out_mask, uint32_t *out_cursor, uint32_t *out_total)
{
    const uint32_t header_size = (uint32_t)sizeof(PelorusSideData);
    const uint32_t dir_size = (uint32_t)sizeof(PelorusSectionDir);
    pel_result rc;

    if (meta == NULL) {
        return PEL_ERR_INVALID;
    }
    if (nb < 0 || (nb > 0 && sections == NULL)) {
        return PEL_ERR_INVALID;
    }
    if (nb > 32) { /* at most one entry per possible section bit */
        return PEL_ERR_RANGE;
    }

    rc = validate_pack_sections(sections, nb, out_mask);
    if (rc != PEL_OK) {
        return rc;
    }

    /* Layout: header, dir[], then 8-aligned section payloads (header + dir is 48 + 16*nb
     * bytes, already a multiple of 8). */
    *out_cursor = PEL_ALIGN8(header_size + (uint32_t)nb * dir_size);
    return pack_total_size(sections, nb, *out_cursor, out_total);
}

/* Write the whole image into `blob` (16 + total bytes, already zeroed so padding is
 * deterministic). The header is built in an aligned local and memcpy'd out: both
 * directions stay cast-free, which keeps bugprone-casting-through-void a real guard
 * (issue #44). */
static void pack_image(uint8_t *blob, const PelorusSideData *meta,
                       const PelorusPackSection *sections, int nb, uint32_t mask, uint32_t cursor,
                       uint32_t total)
{
    PelorusSideData hdr;

    memcpy(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN);
    write_pack_header(&hdr, meta, total, mask, nb);
    memcpy(blob + PELORUS_SIDEDATA_UUID_LEN, &hdr, sizeof(hdr));
    write_pack_sections(blob, sections, nb, cursor);
}

pel_result pel_blob_pack(const PelorusSideData *meta, const PelorusPackSection *sections, int nb,
                         uint8_t **out_blob, size_t *out_len)
{
    uint32_t section_mask = 0;
    uint32_t cursor = 0;
    uint32_t total_size = 0;
    size_t blob_len;
    uint8_t *blob;
    pel_result rc;

    if (out_blob == NULL || out_len == NULL) {
        return PEL_ERR_INVALID;
    }
    *out_blob = NULL;
    *out_len = 0;

    rc = pack_layout(meta, sections, nb, &section_mask, &cursor, &total_size);
    if (rc != PEL_OK) {
        return rc;
    }

    blob_len = (size_t)PELORUS_SIDEDATA_UUID_LEN + (size_t)total_size;
    blob = calloc(1, blob_len); /* zero-fill so padding is deterministic */
    if (blob == NULL) {
        return PEL_ERR_NOMEM;
    }
    pack_image(blob, meta, sections, nb, section_mask, cursor, total_size);

    *out_blob = blob;
    *out_len = blob_len;
    return PEL_OK;
}

pel_result pel_blob_pack_into(const PelorusSideData *meta, const PelorusPackSection *sections,
                              int nb, uint8_t *buf, size_t cap, size_t *out_len)
{
    uint32_t section_mask = 0;
    uint32_t cursor = 0;
    uint32_t total_size = 0;
    size_t need;
    pel_result rc;

    if (out_len == NULL) {
        return PEL_ERR_INVALID;
    }
    *out_len = 0;
    if (buf == NULL && cap > 0) {
        return PEL_ERR_INVALID;
    }

    rc = pack_layout(meta, sections, nb, &section_mask, &cursor, &total_size);
    if (rc != PEL_OK) {
        return rc;
    }

    need = (size_t)PELORUS_SIDEDATA_UUID_LEN + (size_t)total_size;
    *out_len = need; /* also on a short buffer: the caller sizes from it */
    if (buf == NULL || cap < need) {
        return PEL_ERR_RANGE;
    }
    memset(buf, 0, need); /* deterministic padding, as calloc gives pel_blob_pack */
    pack_image(buf, meta, sections, nb, section_mask, cursor, total_size);
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

/* Validate the blob framing (uuid, magic, major, total_size); on PEL_OK `*hdr` holds the
 * header and `*image` / `*image_len` the byte range that starts at the header magic. */
static pel_result blob_framing(const uint8_t *blob, size_t len, PelorusSideData *hdr,
                               const uint8_t **image, size_t *image_len)
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
    /* Framing sanity: the declared size must fit the received bytes. */
    if (hdr->total_size > *image_len || hdr->header_size < sizeof(*hdr)) {
        return PEL_ERR_TRUNCATED;
    }
    return PEL_OK;
}

/* Validate the blob framing and the header for `sec` (dir[] must fit and be aligned, the
 * section bit must be set); on PEL_OK the outputs are those of blob_framing. */
static pel_result find_section_framing(const uint8_t *blob, size_t len, enum pel_section sec,
                                       PelorusSideData *hdr, const uint8_t **image,
                                       size_t *image_len)
{
    pel_result rc = blob_framing(blob, len, hdr, image, image_len);

    if (rc != PEL_OK) {
        return rc;
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

pel_result pel_blob_map(const uint8_t *blob, size_t len, uint32_t offset, uint32_t size,
                        uint32_t elem_count, uint32_t elem_size, const void **out_ptr)
{
    PelorusSideData hdr;
    const uint8_t *image = NULL;
    size_t image_len = 0;
    pel_result rc;

    if (out_ptr != NULL) {
        *out_ptr = NULL;
    }
    if (blob == NULL || out_ptr == NULL || elem_count == 0u || elem_size == 0u) {
        return PEL_ERR_INVALID;
    }
    rc = blob_framing(blob, len, &hdr, &image, &image_len);
    if (rc != PEL_OK) {
        return rc;
    }
    /* Check 3: the size is exactly the element count times the element size (64-bit, so the
     * product of two untrusted uint32 values cannot wrap). */
    if ((uint64_t)elem_count * (uint64_t)elem_size != (uint64_t)size) {
        return PEL_ERR_ABI;
    }
    /* Check 4: maps are 8-aligned like section payloads (R5). */
    if ((offset & 7u) != 0u) {
        return PEL_ERR_ABI;
    }
    /* A map never aliases the header or dir[]: they end before the first payload. Both
     * terms are untrusted, so the sum is formed in 64 bits. */
    if ((uint64_t)offset <
        (uint64_t)hdr.header_size + (uint64_t)hdr.section_count * sizeof(PelorusSectionDir)) {
        return PEL_ERR_ABI;
    }
    /* Check 5: inside total_size, which blob_framing bounded by the received length. The
     * subtraction cannot wrap: offset <= total_size is tested first. */
    if (offset > hdr.total_size || size > hdr.total_size - offset) {
        return PEL_ERR_TRUNCATED;
    }
    *out_ptr = image + offset;
    return PEL_OK;
}

/* COBS-encode in[0..n) into out (Cheshire and Baker, 1999; no frame delimiter):
 * each block is a code byte c in 1..0xFF followed by c - 1 non-zero bytes, and
 * every block except a full (0xFF) one and the last stands for a trailing zero.
 * A full block that ends the input is not followed by an empty block. With out
 * NULL only the length is computed. Returns the encoded length; the loop is
 * bounded by n (HISS-02). */
static size_t cobs_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t code_at = 0; /* position of the open block's code byte */
    size_t w = 1;       /* next output position                    */
    size_t code = 1;    /* 1 + data bytes of the open block        */
    size_t i;

    for (i = 0; i < n; i++) {
        const int full = in[i] != 0u && code + 1u == PEL_COBS_FULL && i + 1u < n;

        if (in[i] != 0u) {
            if (out != NULL) {
                out[w] = in[i];
            }
            w++;
            code++;
        }
        if (in[i] == 0u || full) {
            if (out != NULL) {
                out[code_at] = (uint8_t)code;
            }
            code_at = w++;
            code = 1;
        }
    }
    if (out != NULL) {
        out[code_at] = (uint8_t)code;
    }
    return w;
}

/* Copy the data bytes of one COBS block; PEL_ERR_ABI when one of them is zero. */
static pel_result cobs_copy_block(const uint8_t *in, size_t count, uint8_t *out)
{
    size_t k;

    for (k = 0; k < count; k++) {
        if (in[k] == 0u) {
            return PEL_ERR_ABI;
        }
        out[k] = in[k];
    }
    return PEL_OK;
}

/* Strictly decode the COBS bytes in[0..n) into out, which holds at least n
 * bytes (a decoded image is never longer than its encoding). On PEL_OK *out_n
 * is the decoded length. PEL_ERR_ABI for a zero code byte, a block past the
 * end, a zero data byte or an empty final block after a full one (non-canonical:
 * cobs_encode never writes it, so encode(decode(x)) == x for every accepted x). */
static pel_result cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t *out_n)
{
    size_t r = 0;
    size_t w = 0;
    size_t prev = 0;

    while (r < n) { /* bounded: r grows by code >= 1 each pass (HISS-02) */
        const size_t code = in[r];

        if (code == 0u || code - 1u > n - r - 1u) {
            return PEL_ERR_ABI;
        }
        if (r + code == n && code == 1u && prev == PEL_COBS_FULL) {
            return PEL_ERR_ABI;
        }
        if (cobs_copy_block(in + r + 1u, code - 1u, out + w) != PEL_OK) {
            return PEL_ERR_ABI;
        }
        w += code - 1u;
        r += code;
        if (code != PEL_COBS_FULL && r < n) {
            out[w++] = 0u;
        }
        prev = code;
    }
    *out_n = w;
    return PEL_OK;
}

pel_result pel_blob_carrier_encode(const uint8_t *blob, size_t len, uint8_t *out, size_t cap,
                                   size_t *out_len)
{
    const size_t uuid_len = (size_t)PELORUS_SIDEDATA_UUID_LEN;
    size_t need;

    if (out_len == NULL) {
        return PEL_ERR_INVALID;
    }
    *out_len = 0;
    if (blob == NULL || (out == NULL && cap > 0u)) {
        return PEL_ERR_INVALID;
    }
    if (!pel_blob_is_present(blob, len)) {
        return PEL_ERR_ABSENT;
    }
    need = uuid_len + cobs_encode(blob + uuid_len, len - uuid_len, NULL);
    *out_len = need; /* also on a short buffer: the caller sizes from it */
    if (out == NULL || cap < need) {
        return PEL_ERR_RANGE;
    }
    memcpy(out, pelorus_carrier_uuid, uuid_len);
    (void)cobs_encode(blob + uuid_len, len - uuid_len, out + uuid_len);
    return PEL_OK;
}

/* Decode a carrier payload (pelorus_carrier_uuid first, len >= 16) into scratch
 * as pelorus_sidedata_uuid + image. */
static pel_result unwrap_carrier(const uint8_t *data, size_t len, uint8_t *scratch, size_t cap,
                                 size_t *out_len)
{
    const size_t uuid_len = (size_t)PELORUS_SIDEDATA_UUID_LEN;
    size_t image_len = 0;
    pel_result rc;

    if (scratch == NULL || cap < len) {
        *out_len = len; /* enough for any carrier of this length */
        return PEL_ERR_RANGE;
    }
    rc = cobs_decode(data + uuid_len, len - uuid_len, scratch + uuid_len, &image_len);
    if (rc != PEL_OK) {
        return rc;
    }
    if (image_len < sizeof(PelorusSideData)) {
        return PEL_ERR_TRUNCATED;
    }
    memcpy(scratch, pelorus_sidedata_uuid, uuid_len);
    if (!pel_blob_is_present(scratch, uuid_len + image_len)) {
        return PEL_ERR_ABI; /* well-formed stuffing, but not a Pelorus image */
    }
    *out_len = uuid_len + image_len;
    return PEL_OK;
}

pel_result pel_blob_unwrap(const uint8_t *data, size_t len, uint8_t *scratch, size_t cap,
                           const uint8_t **out_blob, size_t *out_len)
{
    const size_t uuid_len = (size_t)PELORUS_SIDEDATA_UUID_LEN;
    pel_result rc;

    if (out_blob != NULL) {
        *out_blob = NULL;
    }
    if (data == NULL || out_blob == NULL || out_len == NULL || (scratch == NULL && cap > 0u)) {
        return PEL_ERR_INVALID;
    }
    *out_len = 0;
    if (len < uuid_len) {
        return PEL_ERR_ABSENT;
    }
    if (memcmp(data, pelorus_sidedata_uuid, uuid_len) == 0) {
        *out_blob = data;
        *out_len = len;
        return PEL_OK;
    }
    if (memcmp(data, pelorus_carrier_uuid, uuid_len) != 0) {
        return PEL_ERR_ABSENT;
    }
    rc = unwrap_carrier(data, len, scratch, cap, out_len);
    if (rc == PEL_OK) {
        *out_blob = scratch;
    }
    return rc;
}

pel_result pel_encode_record_digest_text(const PelorusEncodeRecordSection *s, size_t got, char *out,
                                         size_t cap)
{
    static const char hex_digits[] = "0123456789abcdef";
    static const char prefix[] = "sha256:";
    const size_t prefix_len = sizeof(prefix) - 1u;
    size_t i;

    if (s == NULL || out == NULL || got < sizeof(*s)) {
        return PEL_ERR_INVALID;
    }
    if (s->digest_alg != (uint8_t)PEL_DIGEST_ALG_SHA256) {
        return PEL_ERR_INVALID;
    }
    if (cap < (size_t)PEL_DIGEST_TEXT_SIZE) {
        return PEL_ERR_RANGE;
    }
    memcpy(out, prefix, prefix_len);
    for (i = 0; i < sizeof(s->digest); i++) {
        const unsigned byte = s->digest[i];
        out[prefix_len + 2u * i] = hex_digits[byte >> 4u];
        out[prefix_len + 2u * i + 1u] = hex_digits[byte & 0x0fu];
    }
    out[prefix_len + 2u * sizeof(s->digest)] = '\0';
    return PEL_OK;
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
