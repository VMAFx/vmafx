/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The build description of the provenance record (#2142, ADR-2073, RC4 WP5):
 * what core/src/meson.build wrote into vmafx_build_info.h and
 * vmafx_build_commit.h, the digest of it (`build_id`), and the SIMD level the
 * CPU extractors dispatch to under the cpumask in effect.
 *
 * build_id covers what decides the arithmetic of a build: compilers, build
 * options, the strict floating-point policy (ADR-1461), backends and Rust
 * extractors. The commit and the host architecture are recorded but not
 * covered: they have fields of their own.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "cpu.h"
#include "internal.h"
#include "provenance_json.h"
#include "sha256.h"
#include "vmafx_build_commit.h"
#include "vmafx_build_info.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static const VmafxBuildInfo build_info = {
    .commit = VMAFX_BUILD_COMMIT,
    .compiler = VMAFX_BUILD_COMPILER,
    .flags = VMAFX_BUILD_FLAGS,
    .fp_policy = VMAFX_BUILD_FP_POLICY,
    .backends = VMAFX_BUILD_BACKENDS,
    .arch = VMAFX_BUILD_ARCH,
    .rust_twins = VMAFX_BUILD_RUST,
};

static pthread_once_t build_id_once = PTHREAD_ONCE_INIT;
static char build_id_text[VMAFX_DIGEST_TEXT_SIZE];

const VmafxBuildInfo *vmafx_build_info(void)
{
    return &build_info;
}

int vmafx_build_id_of(const VmafxBuildInfo *info, char out[VMAFX_DIGEST_TEXT_SIZE])
{
    assert(info && out);
    VmafxJsonObject object;
    vmafx_json_object_init(&object);
    vmafx_json_object_string(&object, "backends", info->backends);
    vmafx_json_object_string(&object, "build_flags", info->flags);
    vmafx_json_object_string(&object, "compiler", info->compiler);
    vmafx_json_object_string(&object, "fp_policy", info->fp_policy);
    vmafx_json_object_u32(&object, "rust_twins", info->rust_twins);
    char *const canonical = vmafx_json_object_finish(&object, NULL);
    vmafx_json_object_release(&object);
    if (!canonical) {
        return -ENOMEM;
    }
    vmafx_digest_text(canonical, strlen(canonical), out);
    free(canonical);
    return 0;
}

static void compute_build_id(void)
{
    if (vmafx_build_id_of(&build_info, build_id_text) != 0) {
        (void)snprintf(build_id_text, sizeof(build_id_text), "%s", "unknown");
    }
}

const char *vmafx_build_id(void)
{
    (void)pthread_once(&build_id_once, compute_build_id);
    return build_id_text;
}

#if ARCH_X86
static const char *simd_level_of(unsigned flags)
{
    static const struct {
        unsigned flag;
        const char *name;
    } levels[] = {
        {VMAF_X86_CPU_FLAG_AVX512ICL, "avx512icl"}, {VMAF_X86_CPU_FLAG_AVX512, "avx512"},
        {VMAF_X86_CPU_FLAG_AVX2, "avx2"},           {VMAF_X86_CPU_FLAG_SSE41, "sse4.1"},
        {VMAF_X86_CPU_FLAG_SSSE3, "ssse3"},         {VMAF_X86_CPU_FLAG_SSE2, "sse2"},
    };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (flags & levels[i].flag) {
            return levels[i].name;
        }
    }
    return "scalar";
}
#elif ARCH_AARCH64
static const char *simd_level_of(unsigned flags)
{
    if (flags & VMAF_ARM_CPU_FLAG_SVE2) {
        return "sve2";
    }
    return (flags & VMAF_ARM_CPU_FLAG_NEON) ? "neon" : "scalar";
}
#else
static const char *simd_level_of(unsigned flags)
{
    (void)flags;
    return "scalar";
}
#endif

const char *vmafx_simd_level(void)
{
    return simd_level_of(vmaf_get_cpu_flags());
}

/* NOLINTEND(modernize-use-nullptr) */
