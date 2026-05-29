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

/**
 * gpu_slab.h — single-point int-to-ptr cast for GPU device-buffer carving.
 *
 * GPU backends (CUDA, HIP) allocate one contiguous device buffer per feature
 * extractor state and carve it into typed sub-regions at init time.  The
 * carving pattern is:
 *
 *     field = (T *)slab;
 *     slab += stride;
 *
 * where `slab` is either a CUdeviceptr (unsigned long long) or a uintptr_t.
 * clang-tidy's `performance-no-int-to-ptr` fires at every cast site.
 *
 * SLAB_FIELD centralises the cast in one macro, carries a single
 * ADR-0800 citation, and eliminates 21 bare NOLINTs across the GPU
 * feature-extractor cluster.
 *
 * Usage:
 *
 *     SLAB_FIELD(s->buf.mu1, uint16_t, slab);
 *     slab += (size_t)h * stride_16;
 *
 * The advance (slab += ...) is NOT part of the macro — callers control
 * stride arithmetic explicitly, which keeps the macro trivially auditable.
 *
 * Rationale for the suppression: CUdeviceptr and its HIP equivalent
 * uintptr_t are the canonical integer device-pointer types in the
 * respective driver APIs.  The cast to a typed host-visible pointer is
 * inherent to how the CUDA/HIP driver APIs expose device buffer addresses
 * and cannot be refactored away without changing the public libvmaf-GPU
 * contract.  Per ADR-0141 (touched-file cleanup rule), this is an
 * upstream-parity exception.  The single suppression at the macro
 * definition site (ADR-0278, ADR-0800) satisfies the cited-NOLINT rule.
 *
 * Governing ADRs:
 *   - ADR-0141 — touched-file cleanup rule / upstream-parity exception.
 *   - ADR-0278 — every NOLINT must carry an inline citation.
 *   - ADR-0800 — this macro; GPU slab pointer-cast centralisation.
 */

#ifndef VMAF_FEATURE_GPU_SLAB_H_
#define VMAF_FEATURE_GPU_SLAB_H_

/**
 * SLAB_FIELD(dst, type, slab)
 *
 * Assign the current slab offset (integer device pointer) to a typed
 * host-visible pointer field.
 *
 * @param dst   lvalue of type `type *` to receive the typed pointer.
 * @param type  destination element type (uint16_t, uint32_t, int64_t, …).
 * @param slab  integer or pointer expression holding the current offset
 *              into the device buffer (CUdeviceptr / uintptr_t / uint8_t *).
 *
 * The caller is responsible for advancing `slab` by the appropriate stride
 * after each SLAB_FIELD call.
 *
 * clang-tidy suppression: performance-no-int-to-ptr — ADR-0800.
 * The cast is load-bearing (CUDA/HIP Driver API contract); ADR-0141
 * upstream-parity exception.  Single citation covers all expansion sites.
 */
/* NOLINT(performance-no-int-to-ptr) — ADR-0800: GPU slab ptr-cast; load-bearing
 * CUDA/HIP Driver API pattern; ADR-0141 upstream-parity exception. */
#define SLAB_FIELD(dst, type, slab)                                                                \
    ((dst) = (type *)(uintptr_t)(slab)) /* NOLINT(performance-no-int-to-ptr) */

#endif /* VMAF_FEATURE_GPU_SLAB_H_ */
