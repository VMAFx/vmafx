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
 */

/*
 * pelorus.h — umbrella public header for libpelorus.
 *
 * libpelorus is the shared core of the Pelorus GPU pre-encode pipeline: the
 * versioned Pelorus<->vmafx side-data interop ABI (interop.h) plus the
 * filter-parameter contracts (deband.h, ...) that the FFmpeg vf_pelorus_*
 * filters and the vmafx autotune loop both depend on.
 *
 * ABI surface: everything under include/pelorus/ is public and stability-tagged.
 * Internal helpers never appear here.
 */
#ifndef PELORUS_PELORUS_H
#define PELORUS_PELORUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Library version (SemVer; tracks git tags v<MAJOR>.<MINOR>.<PATCH>) --- */
#define PELORUS_VERSION_MAJOR 0
#define PELORUS_VERSION_MINOR 1
#define PELORUS_VERSION_PATCH 0
#define PELORUS_VERSION_STR "0.1.0"

/* Packed integer version for runtime comparisons: (major<<16)|(minor<<8)|patch. */
#define PELORUS_VERSION_INT                                                                        \
    ((PELORUS_VERSION_MAJOR << 16) | (PELORUS_VERSION_MINOR << 8) | (PELORUS_VERSION_PATCH))

/* Returns the compiled-in PELORUS_VERSION_INT of the linked library. */
int pelorus_version(void);

/* Returns the compiled-in PELORUS_VERSION_STR of the linked library. */
const char *pelorus_version_string(void);

/* ---- Result codes ------------------------------------------------------- *
 * Every libpelorus entry point that can fail returns a pel_result. Negative
 * values are errors; PEL_OK is success. No bare -1 crosses an API boundary.
 */
typedef enum pel_result {
    PEL_OK = 0,              /* success                                        */
    PEL_ERR_INVALID = -1,    /* invalid argument / NULL where non-NULL needed  */
    PEL_ERR_NOMEM = -2,      /* allocation failed                              */
    PEL_ERR_RANGE = -3,      /* value outside the documented valid range       */
    PEL_ERR_ABI = -4,        /* blob ABI major mismatch / corrupt framing      */
    PEL_ERR_ABSENT = -5,     /* requested section / data not present           */
    PEL_ERR_TRUNCATED = -6,  /* buffer shorter than its self-described size    */
    PEL_ERR_UNSUPPORTED = -7 /* feature not compiled in / not available       */
} pel_result;

/* Human-readable, static string for a pel_result. Never NULL. */
const char *pel_result_str(pel_result r);

#ifdef __cplusplus
}
#endif
#endif /* PELORUS_PELORUS_H */
