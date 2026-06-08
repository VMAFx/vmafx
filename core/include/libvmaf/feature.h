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

#ifndef LIBVMAF_FEATURE_H
#define LIBVMAF_FEATURE_H

#include <libvmaf/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @typedef VmafFeatureDictionary
 * @brief Opaque key/value dictionary used to override feature-extractor options.
 *
 * Built incrementally via @ref vmaf_feature_dictionary_set and consumed by
 * @ref vmaf_use_feature and @ref vmaf_model_feature_overload. Numeric values
 * are detected at set-time and stored in a normalised form so that
 * `"1"` / `"1.0"` / `" 1 "` compare equal downstream.
 *
 * Ownership transfer rules:
 *   - On success of @ref vmaf_use_feature / @ref vmaf_model_feature_overload,
 *     ownership of the dictionary passes to the VmafContext / VmafModel and
 *     the caller MUST NOT free it.
 *   - On failure of those calls (non-zero return), the caller still owns the
 *     dictionary and is responsible for releasing it with
 *     @ref vmaf_feature_dictionary_free.
 */
typedef struct VmafFeatureDictionary VmafFeatureDictionary;

/**
 * @brief Set (or replace) a key/value pair in a feature-extractor options dictionary.
 *
 * On the first call against an empty dictionary, pass `*dict == NULL` and the
 * function allocates a fresh dictionary in place. Subsequent calls add or
 * overwrite entries. Values that parse as a decimal integer or float are
 * normalised internally (whitespace stripped, canonical numeric form stored)
 * so `"1"` and `" 1.0 "` compare equal across feature-extractor option
 * resolution.
 *
 * Both @p key and @p val are copied; the caller retains ownership of the
 * input strings and may free them after this call returns.
 *
 * @param dict In/out: address of a `VmafFeatureDictionary *`. The first call
 *             must point at a NULL handle; on success @p *dict is updated to
 *             the allocated dictionary. The caller owns @p *dict on failure
 *             and must release it via @ref vmaf_feature_dictionary_free.
 *             Must not be NULL.
 * @param key  NUL-terminated option name (e.g. `"adm_enhn_gain_limit"`).
 *             Must not be NULL or empty.
 * @param val  NUL-terminated option value. May be a number, boolean, or
 *             arbitrary string; the feature extractor decides interpretation.
 *
 * @return 0 on success, or a negative errno code on error:
 *         `-EINVAL` for NULL arguments, `-ENOMEM` if allocation fails.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_feature_dictionary_set(VmafFeatureDictionary **dict, const char *key,
                                            const char *val);

/**
 * @brief Release a feature-extractor options dictionary and clear the handle.
 *
 * Safe to call on a NULL @p dict or with `*dict == NULL`; both are no-ops.
 * On success @p *dict is set to NULL so the handle cannot be reused.
 *
 * Only call this on dictionaries the caller still owns - once ownership has
 * passed to a VmafContext via @ref vmaf_use_feature, or to a VmafModel via
 * @ref vmaf_model_feature_overload, calling this is a double-free. See the
 * `VmafFeatureDictionary` ownership-transfer rules above.
 *
 * @param dict In/out: address of the dictionary handle to free. NULL is a
 *             no-op; on a non-NULL @p dict, @p *dict is reset to NULL.
 *
 * @return 0 on success, or a negative errno code on internal failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_feature_dictionary_free(VmafFeatureDictionary **dict);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_FEATURE_H */
