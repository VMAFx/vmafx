/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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
 * C++23 implementation of the feature-name string-building helpers.
 *
 * Migration note (ADR-0729 / Wave 3):
 *   - `git mv feature_name.c feature_name.cpp` preserves blame.
 *   - The public C API in feature_name.h is unchanged; all entry points
 *     retain their original C signatures and are exposed to C callers via the
 *     `extern "C"` guard added to feature_name.h.
 *   - `goto`-based cleanup in `vmaf_feature_name_from_options` replaced with
 *     `std::unique_ptr<VmafDictionary, DictDeleter>` RAII guard so the dict
 *     is always freed on every exit path.
 *   - `goto`-based failure cleanup in
 *     `vmaf_feature_name_dict_from_provided_features` replaced with the same
 *     RAII pattern.
 *   - `[[nodiscard]]` applied to the two public allocation functions so
 *     callers that discard the returned pointer get a compile-time warning.
 *   - `nullptr` replaces `NULL` within this translation unit.
 *   - C-style casts replaced with `static_cast<>`.
 *   - No behaviour change vs the original C implementation.
 */

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "alias.h"
#include "dict.h"
#include "feature_name.h"
#include "opt.h"

/* Custom deleter for VmafDictionary* owned in this TU. */
struct DictDeleter {
    void operator()(VmafDictionary *d) const noexcept
    {
        vmaf_dictionary_free(&d);
    }
};
using DictPtr = std::unique_ptr<VmafDictionary, DictDeleter>;

static size_t snprintfcat(char *buf, size_t buf_sz, char const *fmt, ...)
{
    va_list args;
    const size_t len = strnlen(buf, buf_sz);
    va_start(args, fmt);
    const size_t result = static_cast<size_t>(vsnprintf(buf + len, buf_sz - len, fmt, args));
    va_end(args);
    return result + len;
}

#define VMAF_FEATURE_NAME_DEFAULT_BUFFER_SIZE 256

[[nodiscard]] static char *vmaf_feature_name_from_opts_dict(const char *name,
                                                            const VmafOption *opts,
                                                            VmafDictionary *opts_dict)
{
    VmafDictionary *sorted_raw = nullptr;
    /* vmaf_dictionary_copy returns -EINVAL when *src is null, which is the
     * ordinary "this feature has no options" case rather than a failure. The C
     * original ignored the return value entirely and relied on the !sorted
     * guard below; bailing out on -EINVAL here broke every feature without an
     * options dict. Any OTHER non-zero return is a genuine failure (OOM inside
     * vmaf_dictionary_set), and that is still fatal (CERT MEM31-C; adversarial
     * review 2026-05-28 finding #15). */
    const int copy_err = vmaf_dictionary_copy(&opts_dict, &sorted_raw);
    if (copy_err != 0 && copy_err != -EINVAL)
        return nullptr;
    vmaf_dictionary_alphabetical_sort(sorted_raw);
    DictPtr sorted(sorted_raw); /* freed by DictPtr destructor on any exit */

    const size_t buf_sz = VMAF_FEATURE_NAME_DEFAULT_BUFFER_SIZE;
    char buf[VMAF_FEATURE_NAME_DEFAULT_BUFFER_SIZE + 1] = {0};

    if (!opts || !sorted) {
        snprintfcat(buf, buf_sz, "%s", name);
    } else {
        snprintfcat(buf, buf_sz, "%s", vmaf_feature_name_alias(name));

        for (unsigned i = 0; i < sorted->cnt; i++) {
            const VmafOption *opt = nullptr;
            for (unsigned j = 0; (opt = &opts[j]); j++) {
                if (!opt->name)
                    break;
                if (strcmp(opt->name, sorted->entry[i].key) != 0)
                    continue;
                if (!(opt->flags & VMAF_OPT_FLAG_FEATURE_PARAM))
                    continue;
                const char *key = opt->alias ? opt->alias : opt->name;
                const char *val = sorted->entry[i].val;

                if (opt->type == VMAF_OPT_TYPE_BOOL) {
                    snprintfcat(buf, buf_sz, "_%s", key);
                } else {
                    snprintfcat(buf, buf_sz, "_%s_%s", key, val);
                }
            }
        }
    }

    /* sorted freed automatically here by DictPtr */

    const size_t dst_sz = strnlen(buf, buf_sz) + 1U;
    char *dst =
        static_cast<char *>(malloc(dst_sz)); // NOLINT(cppcoreguidelines-no-malloc) — C ABI owner
    if (!dst)
        return nullptr;
    strncpy(dst, buf, dst_sz);
    return dst;
}

[[nodiscard]] static int option_is_default(const VmafOption *opt, const void *data) noexcept
{
    if (!opt)
        return -EINVAL;
    if (!data)
        return -EINVAL;

    switch (opt->type) {
    case VMAF_OPT_TYPE_BOOL:
        return static_cast<int>(opt->default_val.b == *(static_cast<const bool *>(data)));
    case VMAF_OPT_TYPE_INT:
        return static_cast<int>(opt->default_val.i == *(static_cast<const int *>(data)));
    case VMAF_OPT_TYPE_DOUBLE:
        return static_cast<int>(opt->default_val.d == *(static_cast<const double *>(data)));
    case VMAF_OPT_TYPE_STRING:
        return static_cast<int>(
            !strcmp(opt->default_val.s, *(static_cast<const char *const *>(data))));
    default:
        return -EINVAL;
    }
}

[[nodiscard]] char *vmaf_feature_name_from_options(const char *name, const VmafOption *opts,
                                                   void *obj)
{
    if (!name)
        return nullptr;

    if (!opts || !obj)
        return vmaf_feature_name_from_opts_dict(name, opts, nullptr);

    /* opts_raw is managed manually during the loop because vmaf_dictionary_set
     * may reallocate the dict (changing the pointer value) — assigning to a
     * unique_ptr and calling reset() inside the loop would free the previous
     * dict pointer that vmaf_dictionary_set has already reused.  We transfer
     * ownership into a RAII guard only AFTER the loop completes, so the guard
     * handles cleanup on any subsequent early return. */
    VmafDictionary *opts_raw = nullptr;

    const VmafOption *opt = nullptr;
    for (unsigned i = 0; (opt = &opts[i]); i++) {
        if (!opt->name)
            break;
        if (!(opt->flags & VMAF_OPT_FLAG_FEATURE_PARAM))
            continue;

        const void *data = static_cast<const uint8_t *>(obj) + opt->offset;
        if (option_is_default(opt, data))
            continue;

        const size_t buf_sz = VMAF_FEATURE_NAME_DEFAULT_BUFFER_SIZE;
        char buf[VMAF_FEATURE_NAME_DEFAULT_BUFFER_SIZE + 1] = {0};

        switch (opt->type) {
        case VMAF_OPT_TYPE_BOOL:
            (void)snprintf(buf, buf_sz, "%s",
                           *(static_cast<const bool *>(data)) ? "true" : "false");
            break;
        case VMAF_OPT_TYPE_INT:
            (void)snprintf(buf, buf_sz, "%d", *(static_cast<const int *>(data)));
            break;
        case VMAF_OPT_TYPE_DOUBLE:
            (void)snprintf(buf, buf_sz, "%g", *(static_cast<const double *>(data)));
            break;
        case VMAF_OPT_TYPE_STRING:
            (void)snprintf(buf, buf_sz, "%s", *(static_cast<const char *const *>(data)));
            break;
        default:
            break;
        }

        const int err = vmaf_dictionary_set(&opts_raw, opt->name, buf, 0);
        if (err) {
            vmaf_dictionary_free(&opts_raw);
            return nullptr;
        }
    }

    /* Now safe to adopt into RAII guard: opts_raw is stable after the loop. */
    DictPtr opts_guard(opts_raw);
    char *output = vmaf_feature_name_from_opts_dict(name, opts, opts_guard.get());
    /* opts_guard destructor frees opts_raw on scope exit (success or failure). */
    return output;
}

[[nodiscard]] VmafDictionary *
vmaf_feature_name_dict_from_provided_features(const char **provided_features,
                                              const VmafOption *opts, void *obj)
{
    /* Same ownership discipline as vmaf_feature_name_from_options above:
     * vmaf_dictionary_set may reallocate, so we adopt into the RAII guard
     * only after the loop completes cleanly. */
    VmafDictionary *dict_raw = nullptr;

    const char *feature_name;
    for (unsigned i = 0; (feature_name = provided_features[i]); i++) {
        char *fn = vmaf_feature_name_from_options(feature_name, opts, obj);
        if (!fn) {
            vmaf_dictionary_free(&dict_raw);
            return nullptr;
        }

        const int err = vmaf_dictionary_set(&dict_raw, feature_name, fn, 0);
        free(fn); // NOLINT(cppcoreguidelines-no-malloc) — C ABI string owner
        if (err) {
            vmaf_dictionary_free(&dict_raw);
            return nullptr;
        }
    }

    return dict_raw; /* caller owns the returned dict */
}
