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
 * pointer to the section struct without an unaligned access (R5).
 */

#include "libvmaf/pelorus/interop.h"

#include <stdlib.h>
#include <string.h>

/* pelorus-sidedata-v1 = e1d7c4a2-6b93-4f08-9a55-0f3c2db17e64 */
const uint8_t pelorus_sidedata_uuid[PELORUS_SIDEDATA_UUID_LEN] = {
    0xe1, 0xd7, 0xc4, 0xa2, 0x6b, 0x93, 0x4f, 0x08,
    0x9a, 0x55, 0x0f, 0x3c, 0x2d, 0xb1, 0x7e, 0x64
};

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
        return 1;
    default:
        return 0;
    }
}

pel_result pel_blob_pack(const PelorusSideData *meta,
                         const PelorusPackSection *sections, int nb,
                         uint8_t **out_blob, size_t *out_len)
{
    const uint32_t header_size = (uint32_t)sizeof(PelorusSideData);
    const uint32_t dir_size = (uint32_t)sizeof(PelorusSectionDir);
    uint32_t section_mask = 0;
    uint32_t cursor;
    uint32_t total_size;
    size_t blob_len;
    uint8_t *blob;
    PelorusSideData *hdr;
    PelorusSectionDir *dir;
    int i;

    if (meta == NULL || out_blob == NULL || out_len == NULL) {
        return PEL_ERR_INVALID;
    }
    if (nb < 0 || (nb > 0 && sections == NULL)) {
        return PEL_ERR_INVALID;
    }
    if (nb > 32) { /* at most one entry per possible section bit */
        return PEL_ERR_RANGE;
    }

    /* Validate sections, derive the mask, reject duplicates. */
    for (i = 0; i < nb; i++) {
        if (sections[i].data == NULL || sections[i].size == 0) {
            return PEL_ERR_INVALID;
        }
        if (!section_bit_valid((uint32_t)sections[i].id)) {
            return PEL_ERR_RANGE;
        }
        if (section_mask & (uint32_t)sections[i].id) {
            return PEL_ERR_INVALID; /* duplicate section */
        }
        section_mask |= (uint32_t)sections[i].id;
    }

    /* Layout: header, dir[], then 8-aligned section payloads. */
    cursor = PEL_ALIGN8(header_size + (uint32_t)nb * dir_size);
    /* (header_size+dir is 48 + 16*nb -> already a multiple of 8.) */

    *out_blob = NULL;
    *out_len = 0;

    /* total bytes of all (aligned) section payloads */
    {
        uint32_t payload = 0;
        for (i = 0; i < nb; i++) {
            payload += PEL_ALIGN8(sections[i].size);
        }
        total_size = cursor + payload;
    }

    blob_len = (size_t)PELORUS_SIDEDATA_UUID_LEN + (size_t)total_size;
    blob = calloc(1, blob_len); /* zero-fill so padding is deterministic */
    if (blob == NULL) {
        return PEL_ERR_NOMEM;
    }

    /* UUID prefix. */
    memcpy(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN);

    /* Header. */
    hdr = (PelorusSideData *)(void *)(blob + PELORUS_SIDEDATA_UUID_LEN);
    memcpy(hdr->magic, PELORUS_MAGIC_STR, PELORUS_MAGIC_LEN);
    hdr->abi_major = (uint16_t)PELORUS_ABI_MAJOR;
    hdr->abi_minor = (uint16_t)PELORUS_ABI_MINOR;
    hdr->total_size = total_size;
    hdr->section_mask = section_mask;
    hdr->section_count = (uint16_t)nb;
    hdr->header_size = (uint16_t)header_size;
    hdr->frame_pts = meta->frame_pts;
    hdr->plane_layout = meta->plane_layout;
    hdr->bit_depth = meta->bit_depth;
    hdr->grid_cols = meta->grid_cols;
    hdr->grid_rows = meta->grid_rows;
    hdr->_pad0 = 0;
    hdr->producer_id = meta->producer_id;
    hdr->_pad1 = 0;

    /* Directory + payloads. */
    dir = (PelorusSectionDir *)(void *)(blob + PELORUS_SIDEDATA_UUID_LEN +
                                        header_size);
    for (i = 0; i < nb; i++) {
        dir[i].section_id = (uint32_t)sections[i].id;
        dir[i].offset = cursor; /* relative to magic[0] */
        dir[i].size = sections[i].size;
        dir[i].struct_minor = (uint32_t)PELORUS_ABI_MINOR;
        memcpy(blob + PELORUS_SIDEDATA_UUID_LEN + cursor, sections[i].data,
               sections[i].size);
        cursor += PEL_ALIGN8(sections[i].size);
    }

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
    const PelorusSideData *hdr;

    if (blob == NULL ||
        len < (size_t)PELORUS_SIDEDATA_UUID_LEN + sizeof(PelorusSideData)) {
        return 0;
    }
    if (memcmp(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN) != 0) {
        return 0;
    }
    hdr = (const PelorusSideData *)(const void *)(blob +
                                                  PELORUS_SIDEDATA_UUID_LEN);
    if (memcmp(hdr->magic, PELORUS_MAGIC_STR, PELORUS_MAGIC_LEN) != 0) {
        return 0;
    }
    return hdr->abi_major == (uint16_t)PELORUS_ABI_MAJOR;
}

pel_result pel_blob_find_section(const uint8_t *blob, size_t len,
                                 enum pel_section sec,
                                 size_t consumer_known_size,
                                 const void **out_ptr, size_t *out_size)
{
    const PelorusSideData *hdr;
    const PelorusSectionDir *dir;
    const uint8_t *image;
    size_t image_len;
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
    if (len < (size_t)PELORUS_SIDEDATA_UUID_LEN + sizeof(PelorusSideData)) {
        return PEL_ERR_ABSENT;
    }
    if (memcmp(blob, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN) != 0) {
        return PEL_ERR_ABSENT;
    }

    image = blob + PELORUS_SIDEDATA_UUID_LEN;
    image_len = len - (size_t)PELORUS_SIDEDATA_UUID_LEN;
    hdr = (const PelorusSideData *)(const void *)image;

    if (memcmp(hdr->magic, PELORUS_MAGIC_STR, PELORUS_MAGIC_LEN) != 0) {
        return PEL_ERR_ABSENT;
    }
    if (hdr->abi_major != (uint16_t)PELORUS_ABI_MAJOR) {
        return PEL_ERR_ABI; /* consumer cannot trust the layout (R6) */
    }
    /* Framing sanity: declared size must fit, dir[] must fit. */
    if (hdr->total_size > image_len || hdr->header_size < sizeof(*hdr)) {
        return PEL_ERR_TRUNCATED;
    }
    if ((size_t)hdr->header_size +
            (size_t)hdr->section_count * sizeof(PelorusSectionDir) >
        image_len) {
        return PEL_ERR_TRUNCATED;
    }
    if ((hdr->section_mask & (uint32_t)sec) == 0) {
        return PEL_ERR_ABSENT;
    }

    dir = (const PelorusSectionDir *)(const void *)(image + hdr->header_size);
    for (i = 0; i < hdr->section_count; i++) {
        size_t off;
        size_t sz;

        if (dir[i].section_id != (uint32_t)sec) {
            continue;
        }
        off = dir[i].offset;
        sz = dir[i].size;
        if (off > image_len || sz > image_len - off) {
            return PEL_ERR_TRUNCATED;
        }
        *out_ptr = image + off;
        *out_size = (sz < consumer_known_size) ? sz : consumer_known_size; /* R4 */
        return PEL_OK;
    }
    return PEL_ERR_ABSENT;
}
