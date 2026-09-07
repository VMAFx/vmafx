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

namespace
{

[[nodiscard]] std::optional<bool> parse_bool(std::string_view sv) noexcept
{
    if (sv == "true")
        return true;
    if (sv == "false")
        return false;
    return std::nullopt;
}

/* Takes `const char *`, not string_view: strtol needs NUL termination, and
 * every caller is the C ABI entry point below, which already holds one. A
 * string_view would only be NUL-terminated by convention -- exactly what
 * bugprone-suspicious-stringview-data-usage flags. */
[[nodiscard]] std::optional<int> parse_int(const char *s, int min_val, int max_val) noexcept
{
    if (!s || *s == '\0')
        return std::nullopt;

    char *end = nullptr;
    errno = 0;
    const long n = std::strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return std::nullopt;
    if (errno == ERANGE)
        return std::nullopt;
    if (n < static_cast<long>(min_val) || n > static_cast<long>(max_val))
        return std::nullopt;
    return static_cast<int>(n);
}

/* `const char *` for the same reason as parse_int above. */
[[nodiscard]] std::optional<double> parse_double(const char *s, double min_val,
                                                 double max_val) noexcept
{
    if (!s || *s == '\0')
        return std::nullopt;

    char *end = nullptr;
    errno = 0;
    const double n = std::strtod(s, &end);
    if (end == s || *end != '\0')
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

/* One helper per option type. Each keeps the exact three-step contract the
 * switch had: write the default, treat a null value as "default only", then
 * parse and reject on failure with -EINVAL. Split out so vmaf_option_set stays
 * inside the readability-function-size budget (ADR-1142); no return code,
 * errno interaction or write order changes. */
[[nodiscard]] int set_bool(const VmafOption *opt, uint8_t *base, const char *val) noexcept
{
    bool *dst = reinterpret_cast<bool *>(base);
    *dst = opt->default_val.b;
    if (!val)
        return 0;
    const auto result = parse_bool(val);
    if (!result)
        return -EINVAL;
    *dst = *result;
    return 0;
}

[[nodiscard]] int set_int(const VmafOption *opt, uint8_t *base, const char *val) noexcept
{
    int *dst = reinterpret_cast<int *>(base);
    *dst = opt->default_val.i;
    if (!val)
        return 0;
    const auto result = parse_int(val, static_cast<int>(opt->min), static_cast<int>(opt->max));
    if (!result)
        return -EINVAL;
    *dst = *result;
    return 0;
}

[[nodiscard]] int set_double(const VmafOption *opt, uint8_t *base, const char *val) noexcept
{
    double *dst = reinterpret_cast<double *>(base);
    *dst = opt->default_val.d;
    if (!val)
        return 0;
    const auto result = parse_double(val, opt->min, opt->max);
    if (!result)
        return -EINVAL;
    *dst = *result;
    return 0;
}

[[nodiscard]] int set_string(const VmafOption *opt, uint8_t *base, const char *val) noexcept
{
    char **dst = reinterpret_cast<char **>(base);
    /* opt.h changed default_val.s to const char* (prevents write-to-rodata).
     * The public char** ABI is preserved per ADR-0721; const_cast is the
     * approved bridge. The pointer is stored but never written through. */
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — ADR-0721 / ADR-0278
    *dst = const_cast<char *>(opt->default_val.s);
    if (!val)
        return 0;
    /* String options store a borrowed pointer — lifetime owned by the caller;
     * no allocation here, matching the original C behaviour. ADR-0721: the
     * public VmafOption API exposes `char *`, so removing this const_cast would
     * be a public ABI change; the original opt.c performed the identical
     * implicit cast. */
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — ADR-0721 / ADR-0278
    *dst = const_cast<char *>(val);
    return 0;
}

} // namespace

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
    case VMAF_OPT_TYPE_BOOL:
        return set_bool(opt, base, val);
    case VMAF_OPT_TYPE_INT:
        return set_int(opt, base, val);
    case VMAF_OPT_TYPE_DOUBLE:
        return set_double(opt, base, val);
    case VMAF_OPT_TYPE_STRING:
        return set_string(opt, base, val);
    default:
        return -EINVAL;
    }
}
