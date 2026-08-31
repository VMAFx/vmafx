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
 * C++23 implementation of luminance_tools.
 *
 * Migration note (ADR-0731 / cpp23 Wave 3 part B):
 *   - `git mv luminance_tools.c luminance_tools.cpp` preserves blame.
 *   - All public C symbols retain their original signatures and link
 *     under `extern "C"` via the updated luminance_tools.h guards,
 *     so callers (cambi.c, etc.) require no changes.
 *   - The `MAX()` preprocessor macro is replaced with `std::max` from
 *     <algorithm> — type-safe, no double-evaluation, no macro pollution.
 *   - The BT.1886 / PQ EOTF constants are `constexpr` variables rather
 *     than preprocessor literals — they participate in constant folding
 *     and are visible to the debugger.
 *   - `range_foot_head` and `normalize_range` become `[[nodiscard]]`
 *     inline helpers in an anonymous namespace — prevents accidental
 *     result discard at compile time.
 *   - `std::string_view` drives the EOTF dispatch in
 *     `vmaf_luminance_init_eotf`, eliminating the two `strcmp` calls
 *     and the implicit strlen traversals they entail.
 *   - `nullptr` replaces any legacy `NULL` usage in this TU.
 */

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <string_view>

extern "C" {
#include "log.h"
}
#include "luminance_tools.h"

namespace
{

constexpr double kBt1886Gamma = 2.4;
constexpr double kBt1886Lw = 300.0;
constexpr double kBt1886Lb = 0.01;

/* `static` removed: anonymous namespace already provides internal linkage
 * (Power of 10 #10 / -Wredundant-decls; adversarial review 2026-05-28
 * finding #12). */
/* `pix_range` is taken as `int`, not `enum VmafPixelRange`, on purpose.
 * VmafPixelRange is part of the public C API, so a caller can hand us any
 * integer; the `default:` arm below exists precisely to reject that. Reading
 * an out-of-range value *as the enum type* is undefined behaviour in C++,
 * and UBSan's -fsanitize=enum rightly flags it
 * ("load of value 127, which is not a valid value for type
 * 'enum VmafPixelRange'"). Widening the parameter makes the defensive check
 * well-defined instead of UB, without changing behaviour for valid input. */
[[nodiscard]] int range_foot_head(int bitdepth, int pix_range, int *foot, int *head) noexcept
{
    switch (pix_range) {
    case VMAF_PIXEL_RANGE_LIMITED:
        *foot = 16 * (1 << (bitdepth - 8));
        *head = 235 * (1 << (bitdepth - 8));
        break;
    case VMAF_PIXEL_RANGE_FULL:
        *foot = 0;
        *head = (1 << bitdepth) - 1;
        break;
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "unknown pixel range received");
        return -EINVAL;
    }
    return 0;
}

[[nodiscard]] double normalize_range(int sample, VmafLumaRange range) noexcept
{
    const int clipped = std::clamp(sample, range.foot, range.head);
    return static_cast<double>(clipped - range.foot) / static_cast<double>(range.head - range.foot);
}

} // namespace

extern "C" int vmaf_luminance_init_luma_range(VmafLumaRange *luma_range, int bitdepth,
                                              enum VmafPixelRange pix_range)
{
    return range_foot_head(bitdepth, static_cast<int>(pix_range), &luma_range->foot,
                           &luma_range->head);
}

extern "C" int vmaf_luminance_init_eotf(VmafEOTF *eotf, const char *eotf_str)
{
    if (!eotf_str)
        return -EINVAL;

    const std::string_view name{eotf_str};

    if (name == "bt1886") {
        *eotf = vmaf_luminance_bt1886_eotf;
    } else if (name == "pq") {
        *eotf = vmaf_luminance_pq_eotf;
    } else {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "unknown EOTF received");
        return -EINVAL;
    }
    return 0;
}

extern "C" double vmaf_luminance_bt1886_eotf(double V)
{
    const double a =
        std::pow(std::pow(kBt1886Lw, 1.0 / kBt1886Gamma) - std::pow(kBt1886Lb, 1.0 / kBt1886Gamma),
                 kBt1886Gamma);
    const double b =
        std::pow(kBt1886Lb, 1.0 / kBt1886Gamma) /
        (std::pow(kBt1886Lw, 1.0 / kBt1886Gamma) - std::pow(kBt1886Lb, 1.0 / kBt1886Gamma));
    return a * std::pow(std::max(V + b, 0.0), kBt1886Gamma);
}

extern "C" double vmaf_luminance_pq_eotf(double V)
{
    constexpr double m_1 = 0.1593017578125;
    constexpr double m_2 = 78.84375;
    constexpr double c_1 = 0.8359375;
    constexpr double c_2 = 18.8515625;
    constexpr double c_3 = 18.6875; // c_3 = c_1 + c_2 - 1

    const double num = std::pow(V, 1.0 / m_2) - c_1;
    const double num_clipped = std::max(num, 0.0);
    const double den = c_2 - c_3 * std::pow(V, 1.0 / m_2);
    return 10000.0 * std::pow(num_clipped / den, 1.0 / m_1);
}

extern "C" double vmaf_luminance_get_luminance(int sample, VmafLumaRange luma_range, VmafEOTF eotf)
{
    const double normalized = normalize_range(sample, luma_range);
    return eotf(normalized);
}
