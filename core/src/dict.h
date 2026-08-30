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

#ifndef __VMAF_SRC_DICT_H__
#define __VMAF_SRC_DICT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A single key/value pair stored in a VmafDictionary. */
typedef struct VmafDictionaryEntry {
    const char *key, *val;
} VmafDictionaryEntry;

/**
 * @brief Simple string-keyed dictionary used to pass feature options.
 *
 * Internally a flat, heap-allocated array of VmafDictionaryEntry.  Callers
 * pass a pointer-to-pointer so functions can allocate the dict on first use.
 */
typedef struct VmafDictionary {
    VmafDictionaryEntry *entry;
    unsigned size, cnt;
} VmafDictionary;

/**
 * @brief Flags that control dictionary mutation behaviour.
 *
 * @var VMAF_DICT_DO_NOT_OVERWRITE
 *   Return an error instead of overwriting an existing key.
 * @var VMAF_DICT_NORMALIZE_NUMERICAL_VALUES
 *   Canonicalise numeric string values (strip trailing zeros, etc.).
 */
enum VmafDictionaryFlags {
    VMAF_DICT_DO_NOT_OVERWRITE = 1 << 0,
    VMAF_DICT_NORMALIZE_NUMERICAL_VALUES = 1 << 1,
};

/**
 * @brief Insert or update a key/value pair.
 *
 * Allocates *dict on first use.  Both @p key and @p val are copied.
 *
 * @param dict   Pointer to the dictionary pointer (may point to NULL).
 * @param key    Key string (must not be NULL).
 * @param val    Value string (must not be NULL).
 * @param flags  Combination of VmafDictionaryFlags.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_dictionary_set(VmafDictionary **dict, const char *key, const char *val, uint64_t flags);

/**
 * @brief Look up a key and return a pointer to its entry.
 *
 * @param dict   Pointer to the dictionary pointer.
 * @param key    Key to search for.
 * @param flags  Reserved; pass 0.
 * @return Pointer to the matching VmafDictionaryEntry, or NULL if not found.
 */
VmafDictionaryEntry *vmaf_dictionary_get(VmafDictionary **dict, const char *key, uint64_t flags);

/**
 * @brief Deep-copy @p src into @p dst.
 *
 * @param src  Source dictionary.
 * @param dst  Destination dictionary pointer (may point to NULL; allocated if so).
 * @return 0 on success, negative errno on failure.
 */
int vmaf_dictionary_copy(VmafDictionary **src, VmafDictionary **dst);

/**
 * @brief Merge @p dict_a and @p dict_b into a new dictionary.
 *
 * @param dict_a  First source dictionary.
 * @param dict_b  Second source dictionary.
 * @param flags   Combination of VmafDictionaryFlags controlling collision handling.
 * @return Newly-allocated merged dictionary, or NULL on failure.
 */
VmafDictionary *vmaf_dictionary_merge(VmafDictionary **dict_a, VmafDictionary **dict_b,
                                      uint64_t flags);

/**
 * @brief Compare two dictionaries for equality.
 *
 * @param dict_a  First dictionary.
 * @param dict_b  Second dictionary.
 * @return 0 if equal, non-zero otherwise.
 */
int vmaf_dictionary_compare(VmafDictionary *dict_a, VmafDictionary *dict_b);

/**
 * @brief Sort @p dict entries in ascending alphabetical key order (in-place).
 *
 * @param dict  Dictionary to sort.
 */
void vmaf_dictionary_alphabetical_sort(VmafDictionary *dict);

/**
 * @brief Free all memory owned by @p dict and set *dict to NULL.
 *
 * @param dict  Pointer to the dictionary pointer.  Safe to call with *dict == NULL.
 * @return 0 on success, negative errno on failure.
 */
int vmaf_dictionary_free(VmafDictionary **dict);

#ifdef __cplusplus
}
#endif

#endif /* __VMAF_SRC_DICT_H__ */
