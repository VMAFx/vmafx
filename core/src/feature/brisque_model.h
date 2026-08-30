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

#ifndef __VMAF_FEATURE_BRISQUE_MODEL_H__
#define __VMAF_FEATURE_BRISQUE_MODEL_H__

/*
 * brisque_model.h — declarations for the build-time-embedded libsvm model.
 *
 * The BRISQUE no-reference extractor (feature/brisque.c) ships the canonical
 * LIVE-lab-trained EPSILON_SVR "allmodel" (Mittal/Moorthy/Bovik, IEEE TIP
 * 2012). The model is NOT committed as a giant C byte array: the source of
 * truth is model/other_models/brisque_live.model (native libsvm text format,
 * ~343 KB), and the build embeds it via an `xxd -i` meson custom_target —
 * identical to how libvmaf's own SVR JSON models are baked in (see
 * core/src/meson.build `json_model_c_sources`). The generated C lives in the
 * build directory and never enters the tree, keeping the repository under the
 * 1 MB check-added-large-files gate (the previous 2.1 MB committed header
 * blew that gate — ADR-1115).
 *
 * The two symbols below are defined by the generated translation unit
 * (brisque_live.model.c, symbol name derived from the file name by xxd's -n
 * sanitiser). They are present only when the embed actually ran — i.e.
 * built_in_models is enabled AND the host xxd supports -n. That exact
 * condition is published as VMAF_BRISQUE_BUILT_IN_MODEL in config.h; brisque.c
 * guards the references with it and falls back to the `model_path` option
 * (svm_load_model) otherwise.
 *
 * model file sha256: see model/brisque_live_card.md / model/other_models/NOTICE-brisque.
 * Redistributed under a documented research-use attribution exception — see
 * ADR-1115 and docs/metrics/brisque.md.
 */

#include "config.h"

#if VMAF_BRISQUE_BUILT_IN_MODEL
/* Native libsvm text model bytes (NOT NUL-terminated; the length symbol is the
 * exact byte count). svm_parse_model_from_buffer() takes (buf, len). */
extern const unsigned char src_brisque_live_model[];
extern const unsigned int src_brisque_live_model_len;
#endif /* VMAF_BRISQUE_BUILT_IN_MODEL */

#endif /* __VMAF_FEATURE_BRISQUE_MODEL_H__ */
