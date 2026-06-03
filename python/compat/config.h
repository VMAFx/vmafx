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
 * Stub config.h for building the Python/Cython extension.
 *
 * The Cython extension directly includes libvmaf .c source files which
 * pull in cpu.h -> config.h.  The meson-generated config.h enables
 * architecture-specific SIMD paths (ARCH_X86, ARCH_AARCH64) whose
 * implementation files are NOT compiled into the extension, causing
 * undefined-symbol errors at import time.
 *
 * This header disables all architecture dispatching so the generic C
 * fallback code is used instead.
 */

#pragma once

#define ARCH_X86 0
#define ARCH_X86_32 0
#define ARCH_X86_64 0
#define ARCH_AARCH64 0
#define HAVE_ASM 0
#define HAVE_AVX512 0
#define HAVE_CUDA 0
#define HAVE_SYCL 0
