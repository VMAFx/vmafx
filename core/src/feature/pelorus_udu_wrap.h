/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * pelorus_udu_wrap.h — classification of a user data unregistered SEI payload
 * that may carry Pelorus side data (Pelorus ADR-0210, VMAFx ADR-3340).
 *
 * libx265 writes its own UUID in front of every caller payload, so a Pelorus
 * blob or carrier that went through x265 arrives as
 *   x265 UUID || Pelorus UUID || image.
 * The classifier is an allow-list: the payload is DIRECT when it starts with
 * the blob or carrier UUID, X265_WRAPPED when it starts with the x265 UUID and
 * the next 16 bytes are the blob or carrier UUID with at least one byte after
 * them, and NONE otherwise. An unknown outer UUID is never skipped, even when a
 * Pelorus UUID follows it. Pelorus' own copy is
 * ffmpeg-patches/files/pelorus_udu_wrap.h; both replay
 * testdata/pelorus/udu-wrap-vectors.txt.
 */

#ifndef VMAF_FEATURE_PELORUS_UDU_WRAP_H_
#define VMAF_FEATURE_PELORUS_UDU_WRAP_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libvmaf/pelorus/interop.h"

enum VmafPelorusUduClass {
    VMAF_PELORUS_UDU_NONE = 0,
    VMAF_PELORUS_UDU_DIRECT,
    VMAF_PELORUS_UDU_X265_WRAPPED,
};

/* uuid_iso_iec_11578 libx265 writes in front of every user data unregistered
 * payload since x265 2.1: 2ca2de09-b517-47db-bb55-a4fe7fc2fc4e. */
static const uint8_t vmaf_pelorus_udu_x265_uuid[PELORUS_SIDEDATA_UUID_LEN] = {
    0x2c, 0xa2, 0xde, 0x09, 0xb5, 0x17, 0x47, 0xdb, 0xbb, 0x55, 0xa4, 0xfe, 0x7f, 0xc2, 0xfc, 0x4e,
};

static inline int vmaf_pelorus_udu_is_pelorus_uuid(const uint8_t *p)
{
    return memcmp(p, pelorus_sidedata_uuid, PELORUS_SIDEDATA_UUID_LEN) == 0 ||
           memcmp(p, pelorus_carrier_uuid, PELORUS_SIDEDATA_UUID_LEN) == 0;
}

/* payload = uuid_iso_iec_11578 followed by the SEI's data bytes. */
static inline enum VmafPelorusUduClass vmaf_pelorus_udu_classify(const uint8_t *payload, size_t len)
{
    const size_t uuid_len = (size_t)PELORUS_SIDEDATA_UUID_LEN;

    if (payload == NULL || len < uuid_len) {
        return VMAF_PELORUS_UDU_NONE;
    }
    if (vmaf_pelorus_udu_is_pelorus_uuid(payload)) {
        return VMAF_PELORUS_UDU_DIRECT;
    }
    if (len > 2u * uuid_len && memcmp(payload, vmaf_pelorus_udu_x265_uuid, uuid_len) == 0 &&
        vmaf_pelorus_udu_is_pelorus_uuid(payload + uuid_len)) {
        return VMAF_PELORUS_UDU_X265_WRAPPED;
    }
    return VMAF_PELORUS_UDU_NONE;
}

#endif /* VMAF_FEATURE_PELORUS_UDU_WRAP_H_ */
