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

#ifndef FEATURE_COMPAT_BUILTIN_H_
#define FEATURE_COMPAT_BUILTIN_H_

/*
 * MSVC (but not clang-cl) lacks GCC's __builtin_clz / __builtin_clzll.
 * Provide drop-in replacements via the Win32 intrinsic __lzcnt / __lzcnt64,
 * which the CPU exposes on every Haswell-era x86_64 target supported by the
 * MSVC Windows GPU build-only CI legs.
 */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>

static inline int __builtin_clz(unsigned x)
{
    return (int)__lzcnt(x);
}

static inline int __builtin_clzll(unsigned long long x)
{
    return (int)__lzcnt64(x);
}
#endif

#endif /* FEATURE_COMPAT_BUILTIN_H_ */
