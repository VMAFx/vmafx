/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Shared tokenizer for GPU dispatch-strategy env-variable parsing.
 *
 *  Each GPU backend (CUDA, SYCL, Vulkan, …) exposes an environment
 *  variable of the form:
 *
 *    VMAF_<BACKEND>_DISPATCH=<feature>:<strategy>[,<feature>:<strategy>…]
 *
 *  The token-parsing loop that scans this string for a given feature
 *  name was previously duplicated verbatim across every backend's
 *  dispatch_strategy TU.  This header consolidates it into a single
 *  static inline so the duplication is eliminated without introducing a
 *  new link-time dependency.  See ADR-0483.
 *
 *  Design constraints:
 *  - Pure C89-compatible (no VLAs, no C99 bool, no __typeof__).
 *  - No heap allocation, no global mutable state.
 *  - The header is #include-able from both .c (CUDA/Vulkan/HIP) and
 *    .cpp (SYCL) translation units.
 *  - The caller maps the returned *strategy_idx back to its own enum so
 *    this header remains free of any backend-specific types.
 */
#ifndef LIBVMAF_GPU_DISPATCH_PARSE_H_
#define LIBVMAF_GPU_DISPATCH_PARSE_H_

/* NOLINTBEGIN(modernize-deprecated-headers): shared C header, also included
 * from C++ (SYCL) translation units, which is how clang-tidy analyses it.
 * The <cstring>/<cstddef> spellings do not exist in C. Same reasoning as
 * feature/feature_extractor.h. */
#include <stddef.h>
#include <string.h>
/* NOLINTEND(modernize-deprecated-headers) */

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#ifdef __cplusplus
extern "C" {
#endif

/* Match the strategy name a token names, or -1 when it names none.
 *
 * Split out of `vmaf_gpu_dispatch_parse_env` so that function stays inside the
 * lint profile's branch budget: the terminator check below is four alternatives
 * on its own. */
static inline int vmaf_gpu_dispatch_match_strategy(const char *value,
                                                   const char *const *strategy_names)
{
    for (int idx = 0; strategy_names[idx] != NULL; ++idx) {
        const size_t slen = strlen(strategy_names[idx]);
        /* strncmp alone matches "directx" against "direct" because it only
         * compares slen bytes — it does not check that the byte AFTER the match
         * is a token boundary.  Without this check,
         * VMAF_<BACKEND>_DISPATCH=feature:directx would silently route to the
         * "direct" strategy instead of being treated as unknown.  The accepted
         * terminators are: '\0' (end of env value), ',' (next token separator),
         * '\n' (line end — useful when env values are written to a file and read
         * back line-by-line in tests), and ' '/'\t' (trailing whitespace inside
         * the current token, per the grammar in this header's doc comment). */
        if (strncmp(value, strategy_names[idx], slen) != 0)
            continue;
        const char after = value[slen];
        if (after == '\0' || after == ',' || after == '\n' || after == ' ' || after == '\t')
            return idx;
    }
    return -1;
}

/**
 * vmaf_gpu_dispatch_parse_env - scan an env-variable value for a
 * per-feature strategy override.
 *
 * @param env_value      The raw value of the backend's dispatch env var
 *                       (e.g. the result of getenv("VMAF_CUDA_DISPATCH")).
 *                       May be NULL — returns 0 immediately.
 * @param feature_name   The feature name to look up (e.g. "float_vif").
 *                       May be NULL — returns 0 immediately.
 * @param strategy_names NULL-terminated array of strategy name strings,
 *                       ordered to match the caller's enum values
 *                       (index 0 → enum value 0, etc.).
 *                       Example: {"direct", "graph", NULL}
 * @param out_strategy_idx  Written with the matched strategy index (>= 0)
 *                          when a match is found.  Unchanged on no-match.
 *
 * @return 1 if a matching token was found and *out_strategy_idx was
 *         written; 0 otherwise.
 *
 * Token grammar:
 *   env_value := token (',' token)*
 *   token     := WS* feature_name ':' strategy_name WS*
 *   WS        := ' ' | '\t'
 *
 * Matching is case-sensitive on both feature_name and strategy_name.
 * Only the first matching token for a given feature_name is returned.
 */
static inline int vmaf_gpu_dispatch_parse_env(const char *env_value, const char *feature_name,
                                              const char *const *strategy_names,
                                              int *out_strategy_idx)
{
    size_t name_len;
    const char *p;

    if (!env_value || !feature_name || !strategy_names || !out_strategy_idx)
        return 0;

    name_len = strlen(feature_name);
    p = env_value;

    while (*p) {
        const char *colon;
        const char *v;
        const char *next;
        size_t tok_name_len;
        int idx;

        /* Skip leading whitespace and separators. */
        while (*p == ' ' || *p == '\t' || *p == ',')
            ++p;
        if (!*p)
            break;

        colon = strchr(p, ':');
        if (!colon)
            break;

        tok_name_len = (size_t)(colon - p);
        if (tok_name_len == name_len && memcmp(p, feature_name, name_len) == 0) {
            v = colon + 1;
            idx = vmaf_gpu_dispatch_match_strategy(v, strategy_names);
            if (idx >= 0) {
                *out_strategy_idx = idx;
                return 1;
            }
            /* Feature matched but strategy string unknown — skip token. */
        }

        next = strchr(colon, ',');
        if (!next)
            break;
        p = next + 1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_GPU_DISPATCH_PARSE_H_ */

/* NOLINTEND(modernize-use-nullptr) */
