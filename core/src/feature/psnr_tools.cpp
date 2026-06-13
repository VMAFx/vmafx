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
 * C++23 implementation of psnr_tools.
 *
 * Migration note (ADR-0731 / cpp23 Wave 3 part B):
 *   - `git mv psnr_tools.c psnr_tools.cpp` preserves blame.
 *   - The public C API `psnr_constants()` is unchanged; psnr_tools.h
 *     declares it under `extern "C"` so every C caller links without
 *     modification.
 *   - Format dispatch replaces the strcmp chain with a lookup table
 *     of `std::string_view` keys, eliminating repeated strlen traversals
 *     and making it straightforward to add new formats without touching
 *     a long if-else ladder.
 *   - `[[nodiscard]]` on the internal helper makes accidental result
 *     discard a compile-time error rather than a silent bug.
 *   - `nullptr` replaces `NULL` throughout this TU.
 */

#include <array>
#include <cerrno>
#include <optional>
#include <string_view>
#include <utility>

#include "psnr_tools.h"

namespace
{

struct PsnrParams {
    double peak;
    double psnr_max;
};

/*
 * Lookup table mapping pixel-format string → (peak, psnr_max).
 * Sorted by bit-depth bucket so a linear scan finds the right row quickly;
 * the total number of supported formats is small, so no hash map is needed.
 */
struct FormatEntry {
    std::string_view fmt;
    PsnrParams params;
};

// NOLINTNEXTLINE(cert-err58-cpp) — constexpr aggregate; no dynamic init
constexpr std::array<FormatEntry, 12> kFormatTable{{
    {"yuv420p", {255.0, 60.0}},
    {"yuv422p", {255.0, 60.0}},
    {"yuv444p", {255.0, 60.0}},
    {"yuv420p10le", {255.75, 72.0}},
    {"yuv422p10le", {255.75, 72.0}},
    {"yuv444p10le", {255.75, 72.0}},
    {"yuv420p12le", {255.9375, 84.0}},
    {"yuv422p12le", {255.9375, 84.0}},
    {"yuv444p12le", {255.9375, 84.0}},
    {"yuv420p16le", {255.99609375, 108.0}},
    {"yuv422p16le", {255.99609375, 108.0}},
    {"yuv444p16le", {255.99609375, 108.0}},
}};

[[nodiscard]] static std::optional<PsnrParams> lookup_format(std::string_view fmt) noexcept
{
    for (const auto &entry : kFormatTable) {
        if (entry.fmt == fmt)
            return entry.params;
    }
    return std::nullopt;
}

} // namespace

extern "C" int psnr_constants(const char *fmt, double *peak, double *psnr_max)
{
    if (!fmt || !peak || !psnr_max)
        return 1;

    auto result = lookup_format(fmt);
    if (!result)
        return 1;

    *peak = result->peak;
    *psnr_max = result->psnr_max;
    return 0;
}
