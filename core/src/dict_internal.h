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

#ifndef __VMAF_SRC_DICT_INTERNAL_H__
#define __VMAF_SRC_DICT_INTERNAL_H__

/*
 * dict_internal.h — internal helpers for dict.cpp, exposed only for
 * white-box unit tests.  Do NOT include this from any non-test file other
 * than dict.cpp itself.
 *
 * Motivation: the previous test approach used `#include "dict.cpp"` to
 * access the file-static `isnumeric` helper.  That is an ODR-violation
 * risk when dict.cpp is also compiled as a regular TU in the same link unit
 * (adding dict.cpp to test sources gives duplicate symbols; COMDAT folding
 * in MSVC silently produces the wrong body).  Moving isnumeric into this
 * header with `inline` linkage avoids the ODR risk entirely.
 * Adversarial review 2026-05-28 finding #10 (Power of 10 #8).
 */

#include <cstdlib>
#include <string_view>

/* isnumeric — returns true iff `str` can be parsed as a C floating-point
 * literal (leading/trailing whitespace is tolerated; otherwise the entire
 * string must be consumed).  Used by dict_normalize_numeric. */
[[nodiscard]] inline bool isnumeric(std::string_view str) noexcept
{
    char *end = nullptr;
    (void)std::strtof(str.data(), &end);
    if (end == str.data())
        return false;
    while (*end == ' ' || *end == '\t' || *end == '\n')
        ++end;
    return *end == '\0';
}

#endif /* __VMAF_SRC_DICT_INTERNAL_H__ */
