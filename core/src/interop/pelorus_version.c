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
 * VENDORED FROM VMAFx/pelorus@818d844 — DO NOT EDIT. Append-only ABI; single
 * source of truth is pelorus. Re-sync via scripts/sync-pelorus-interop.sh.
 * See docs/adr/1113-vendor-pelorus-interop-abi.md.
 *
 * Local edit vs the pelorus original: the intra-pelorus #include below is
 * rewritten from "pelorus/pelorus.h" to "libvmaf/pelorus/pelorus.h" so it resolves
 * under core/include/. Nothing else is changed.
 */

/* version.c — library version + result-string accessors. */

#include "libvmaf/pelorus/pelorus.h"

int pelorus_version(void)
{
    return PELORUS_VERSION_INT;
}

const char *pelorus_version_string(void)
{
    return PELORUS_VERSION_STR;
}

const char *pel_result_str(pel_result r)
{
    switch (r) {
    case PEL_OK:
        return "ok";
    case PEL_ERR_INVALID:
        return "invalid argument";
    case PEL_ERR_NOMEM:
        return "out of memory";
    case PEL_ERR_RANGE:
        return "value out of range";
    case PEL_ERR_ABI:
        return "ABI major mismatch / corrupt framing";
    case PEL_ERR_ABSENT:
        return "data not present";
    case PEL_ERR_TRUNCATED:
        return "buffer truncated";
    case PEL_ERR_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown error";
    }
}
