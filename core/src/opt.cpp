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

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <string_view>

#include "opt.h"

// ---------------------------------------------------------------------------
// Internal helpers returning std::optional<T> — parse failure → nullopt.
// These never touch the C errno state beyond what strtol/strtod set themselves.
// The C ABI boundary (vmaf_option_set) converts nullopt → -EINVAL.
// ---------------------------------------------------------------------------

[[nodiscard]] static std::optional<bool> parse_bool(std::string_view sv) noexcept
{
    if (sv == "true")
        return true;
    if (sv == "false")
        return false;
    return std::nullopt;
}

[[nodiscard]] static std::optional<int> parse_int(std::string_view sv, int min_val,
                                                  int max_val) noexcept
{
    if (sv.empty())
        return std::nullopt;

    /* strtol requires a NUL-terminated string; sv may not be NUL-terminated in
     * general, but in practice vmaf_option_set is always called with a C string
     * literal or argv element, so sv.data() is NUL-terminated. We still
     * validate *end == '\0' which covers the sv-is-a-slice case. */
    char *end = nullptr;
    errno = 0;
    const long n = std::strtol(sv.data(), &end, 10);
    if (end == sv.data() || *end != '\0')
        return std::nullopt;
    if (errno == ERANGE)
        return std::nullopt;
    if (n < static_cast<long>(min_val) || n > static_cast<long>(max_val))
        return std::nullopt;
    return static_cast<int>(n);
}

[[nodiscard]] static std::optional<double> parse_double(std::string_view sv, double min_val,
                                                        double max_val) noexcept
{
    if (sv.empty())
        return std::nullopt;

    char *end = nullptr;
    errno = 0;
    const double n = std::strtod(sv.data(), &end);
    if (end == sv.data() || *end != '\0')
        return std::nullopt;
    if (errno == ERANGE)
        return std::nullopt;
    /* NaN bypasses ordered comparisons (NaN < x and NaN > x are both false),
     * so reject it explicitly before the bounds check. Infinity is already
     * rejected when the bound is finite (Inf > max is true), but NaN is not.
     * T-ROUND8-OPT-NAN-BYPASS / CWE-704. */
    if (std::isnan(n))
        return std::nullopt;
    if (n < min_val || n > max_val)
        return std::nullopt;
    return n;
}

// ---------------------------------------------------------------------------
// C ABI entry point — identity preserved exactly (same signature, same errno
// / return-code contract).  The [[nodiscard]] annotation is advisory for C++
// callers; the C callers in feature_extractor.cpp are unaffected.
// ---------------------------------------------------------------------------

// extern "C" matches the declaration in opt.h and ensures C linkage when
// opt.cpp is compiled instead of opt.c (ADR-0772).
extern "C" [[nodiscard]] int vmaf_option_set(const VmafOption *opt, void *obj, const char *val)
{
    if (!obj || !opt)
        return -EINVAL;

    uint8_t *base = static_cast<uint8_t *>(obj) + opt->offset;

    /* memcpy bypasses the enum lvalue: reading opt->type through the
     * enum VmafOptionType type when the stored value (e.g. 9999) is not a
     * named enumerator triggers UBSan enum-invalid-value on the load itself,
     * before any cast can help.  static_cast<int> was insufficient because
     * UBSan fires at the lvalue-to-rvalue conversion (the load), not at the
     * cast expression.  memcpy reads raw bytes into a plain int, eliminating
     * the typed load entirely and making the dispatch UBSan-clean.
     * See ADR-1080 (UBSan enum-invalid-value in vmaf_log / vmaf_option_set). */
    int type_raw;
    memcpy(&type_raw, &opt->type, sizeof(type_raw));
    switch (type_raw) {
    case VMAF_OPT_TYPE_BOOL: {
        bool *dst = reinterpret_cast<bool *>(base);
        *dst = opt->default_val.b;
        if (!val)
            return 0;
        auto result = parse_bool(val);
        if (!result)
            return -EINVAL;
        *dst = *result;
        return 0;
    }
    case VMAF_OPT_TYPE_INT: {
        int *dst = reinterpret_cast<int *>(base);
        *dst = opt->default_val.i;
        if (!val)
            return 0;
        auto result = parse_int(val, static_cast<int>(opt->min), static_cast<int>(opt->max));
        if (!result)
            return -EINVAL;
        *dst = *result;
        return 0;
    }
    case VMAF_OPT_TYPE_DOUBLE: {
        double *dst = reinterpret_cast<double *>(base);
        *dst = opt->default_val.d;
        if (!val)
            return 0;
        auto result = parse_double(val, opt->min, opt->max);
        if (!result)
            return -EINVAL;
        *dst = *result;
        return 0;
    }
    case VMAF_OPT_TYPE_STRING: {
        char **dst = reinterpret_cast<char **>(base);
        /* opt.h changed default_val.s to const char* (prevents write-to-rodata).
         * The public char** ABI is preserved per ADR-0721; const_cast is the
         * approved bridge. The pointer is stored but never written through. */
        *dst =
            const_cast<char *>(opt->default_val.s); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        if (!val)
            return 0;
        /* String options store a borrowed pointer — lifetime owned by the
         * caller; no allocation here, matching the original C behaviour. */
        *dst = const_cast<char *>(val); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        // ADR-0721: the public VmafOption API exposes `char *` (not `const char *`)
        // for string values. Removing the const_cast would require a public ABI
        // change (VmafOption.default_val.s type). Preserved verbatim for ABI
        // stability; the original opt.c performed the identical implicit cast.
        return 0;
    }
    default:
        return -EINVAL;
    }
}
