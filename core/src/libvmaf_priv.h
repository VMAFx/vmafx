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
 * Minimal internal accessor declarations for in-tree tests that need selected
 * libvmaf internals without including libvmaf.c and creating duplicate
 * external-linkage definitions.
 */

#ifndef LIBVMAF_PRIV_H
#define LIBVMAF_PRIV_H

#include "feature/feature_collector.h"

typedef struct VmafContext VmafContext;

/*
 * Return the feature collector owned by @vmaf, or NULL when @vmaf is NULL.
 * Test binaries use this instead of including libvmaf.c to reach the opaque
 * context layout.
 */
VmafFeatureCollector *vmaf_feature_collector_get(VmafContext *vmaf);

#endif /* LIBVMAF_PRIV_H */
