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
 * C++23 implementation of aligned_malloc / aligned_free.
 *
 * Migration note (ADR-0720 / Research-0732 Wave 1):
 *   - `git mv mem.c mem.cpp` preserves blame.
 *   - The public C ABI (both functions declared `extern "C"` in mem.h) is
 *     unchanged — every C caller and ffmpeg-patches consumer links without
 *     modification.
 *   - _POSIX_C_SOURCE is no longer needed: C++ compilers expose
 *     posix_memalign via <cstdlib> without the feature-test macro.
 *   - `[[nodiscard]]` on aligned_malloc surfaces ignored-return warnings at
 *     every call site that forgets to check for NULL — improving safety
 *     without any ABI change.
 *   - Zero-on-allocate and poison-on-free behaviour: posix_memalign /
 *     _aligned_malloc return uninitialised memory; free() / _aligned_free do
 *     NOT zero memory.  These semantics are preserved exactly — this
 *     translation adds no zeroing or poisoning of its own.  Callers that
 *     require zero initialisation must call memset after allocation (see
 *     metadata_handler.cpp for an example using memset on the returned node).
 *   - Alignment guarantee: the caller-supplied `alignment` is passed through
 *     unchanged to posix_memalign / _aligned_malloc.  No rounding occurs here.
 */

#include <cstddef>
#include <cstdlib>

#include "mem.h"

[[nodiscard]] void *aligned_malloc(size_t size, size_t alignment)
{
    void *ptr = nullptr;

#if defined(_MSC_VER) || defined(__MINGW32__)
    ptr = _aligned_malloc(size, alignment);
    if (ptr == nullptr) {
        return nullptr;
    }
#else
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
#endif
    return ptr;
}

void aligned_free(void *ptr)
{
#if defined(_MSC_VER) || defined(__MINGW32__)
    _aligned_free(ptr);
#else
    free(ptr); // NOLINT(cppcoreguidelines-no-malloc) — C ABI allocator pair (ADR-0141 / ADR-0278)
#endif
}
