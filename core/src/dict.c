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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dict.h"
#include "libvmaf/feature.h"

VmafDictionaryEntry *vmaf_dictionary_get(VmafDictionary **dict, const char *key, uint64_t flags)
{
    if (!dict)
        return NULL;
    if (!(*dict))
        return NULL;
    if (!key)
        return NULL;

    (void)flags; // available for possible future use

    VmafDictionary *d = *dict;
    for (unsigned i = 0; i < d->cnt; i++) {
        if (!strcmp(key, d->entry[i].key))
            return &d->entry[i];
    }

    return NULL;
}

static int dict_ensure_allocated(VmafDictionary **dict)
{
    if (*dict)
        return 0;

    VmafDictionary *d = malloc(sizeof(*d));
    if (!d)
        return -ENOMEM;
    memset(d, 0, sizeof(*d));

    const size_t initial_sz = 8 * sizeof(*d->entry);
    d->entry = malloc(initial_sz);
    if (!d->entry) {
        free(d);
        return -ENOMEM;
    }
    memset(d->entry, 0, initial_sz);
    d->size = 8;
    *dict = d;
    return 0;
}

static int dict_normalize_numeric(const char *val, char **buf_out)
{
    *buf_out = NULL;
    char *end = NULL;
    /* strtod, not strtof: the result is stored in a `double` and reformatted
     * via `%g`.  strtof first truncated to single precision and silently
     * dropped the bottom half of the mantissa for any value with more than
     * ~7 decimal digits — observable when callers normalise option values
     * such as "1.234567890123" (model thresholds, score-format probes) and
     * subsequently parse the normalised string back through strtod.
     * Adversarial audit 2026-05-31. */
    double dv = strtod(val, &end);
    if (dv == 0 && val == end)
        return 0;

    const char *fmt = "%g";
    const int snp = snprintf(NULL, 0, fmt, dv);
    if (snp < 0)
        return -EINVAL;
    const size_t buf_sz = (size_t)snp + 1;
    char *buf = malloc(buf_sz);
    if (!buf)
        return -ENOMEM;
    (void)snprintf(buf, buf_sz, fmt, dv);
    *buf_out = buf;
    return 0;
}

static int dict_grow_entries(VmafDictionary *d)
{
    if (d->cnt < d->size)
        return 0;
    assert(d->size > 0);
    const size_t sz = d->size * sizeof(*d->entry) * 2;
    VmafDictionaryEntry *entry = (VmafDictionaryEntry *)realloc(d->entry, sz);
    if (!entry)
        return -ENOMEM;
    d->entry = entry;
    d->size *= 2;
    return 0;
}

static int dict_overwrite_existing(VmafDictionaryEntry *existing, const char *val)
{
    /*
     * Idempotency + leak guard. Prior to this fix, every call to this
     * helper unconditionally strdup'd `val` and freed `existing->val`,
     * which is correct for the disjoint-buffer case but tripped ASan
     * as `SAN-PREDICT-METADATA-LEAK` when the metadata-dispatch
     * re-entry path (`vmaf_dictionary_copy` / `vmaf_dictionary_merge`
     * walking entries from a source dict and re-`set`-ing them on a
     * destination dict that aliases the same backing storage) caused
     * `val` to point into the very buffer we were about to free, then
     * strdup'd from a freed region. Detect identical-value re-sets
     * up-front: the existing strdup'd buffer is already a valid
     * caller-owned copy, so we can leave it in place and skip both
     * strdup and free entirely.
     */
    if (existing->val && strcmp(existing->val, val) == 0)
        return 0;

    const char *val_copy = strdup(val);
    if (!val_copy)
        return -ENOMEM;
    free((char *)existing->val);
    existing->val = val_copy;
    return 0;
}

static int dict_append_new_entry(VmafDictionary *d, const char *key, const char *val)
{
    int err = dict_grow_entries(d);
    if (err)
        return err;

    const char *val_copy = strdup(val);
    if (!val_copy)
        return -ENOMEM;
    const char *key_copy = strdup(key);
    if (!key_copy) {
        free((char *)val_copy);
        return -ENOMEM;
    }

    /*
     * Zero-initialise the destination slot before writing so that a
     * later `vmaf_dictionary_free` traversal cannot see partially
     * stored garbage if a fault interleaves between the two field
     * writes (the realloc'd region beyond the previous `d->cnt` was
     * not memset by `dict_grow_entries` — `realloc` does not zero new
     * bytes). The struct-literal assignment compiles to two scalar
     * stores; the explicit zero closes the window without changing
     * observable behaviour on success paths.
     */
    d->entry[d->cnt] = (VmafDictionaryEntry){.key = NULL, .val = NULL};
    d->entry[d->cnt].key = key_copy;
    d->entry[d->cnt].val = val_copy;
    d->cnt++;
    return 0;
}

int vmaf_dictionary_set(VmafDictionary **dict, const char *key, const char *val, uint64_t flags)
{
    if (!dict || !key || !val)
        return -EINVAL;

    int err = dict_ensure_allocated(dict);
    if (err)
        return err;

    char *buf = NULL;
    if (flags & VMAF_DICT_NORMALIZE_NUMERICAL_VALUES) {
        err = dict_normalize_numeric(val, &buf);
        if (err)
            return err;
    }
    val = buf ? buf : val;

    VmafDictionary *d = *dict;
    VmafDictionaryEntry *existing = vmaf_dictionary_get(&d, key, 0);
    if (existing && (flags & VMAF_DICT_DO_NOT_OVERWRITE)) {
        err = !strcmp(existing->val, val) ? 0 : -EINVAL;
    } else if (existing) {
        err = dict_overwrite_existing(existing, val);
    } else {
        err = dict_append_new_entry(d, key, val);
    }

    free(buf);
    return err;
}

int vmaf_dictionary_copy(VmafDictionary **src, VmafDictionary **dst)
{
    if (!src)
        return -EINVAL;
    if (!(*src))
        return -EINVAL;
    if (!dst)
        return -EINVAL;

    int err = 0;

    VmafDictionary *d = *src;
    for (unsigned i = 0; i < d->cnt; i++)
        err |= vmaf_dictionary_set(dst, d->entry[i].key, d->entry[i].val, 0);

    return err;
}

int vmaf_dictionary_free(VmafDictionary **dict)
{
    if (!dict)
        return -EINVAL;
    if (!(*dict))
        return 0;

    VmafDictionary *d = *dict;
    for (unsigned i = 0; i < d->cnt; i++) {
        /* free(NULL) is well-defined per C99 §7.20.3.2 / POSIX free(3); the
         * NULL guard is redundant. CodeQL cpp/guarded-free. */
        free((char *)d->entry[i].key);
        free((char *)d->entry[i].val);
    }
    free(d->entry);
    free(d);
    *dict = NULL;

    return 0;
}

VmafDictionary *vmaf_dictionary_merge(VmafDictionary **dict_a, VmafDictionary **dict_b,
                                      uint64_t flags)
{
    int err = 0;
    VmafDictionary *a = *dict_a;
    VmafDictionary *b = *dict_b;
    VmafDictionary *d = NULL;

    if (a) {
        err = vmaf_dictionary_copy(&a, &d);
        if (err)
            goto fail;
    }

    if (b) {
        for (unsigned i = 0; i < b->cnt; i++)
            err |= vmaf_dictionary_set(&d, b->entry[i].key, b->entry[i].val, flags);
        if (err)
            goto fail;
    }

    return d;

fail:
    (void)vmaf_dictionary_free(&d);
    return NULL;
}

int vmaf_dictionary_compare(VmafDictionary *a, VmafDictionary *b)
{
    if (!a && !b)
        return 0;
    if (!a != !b)
        return -EINVAL;
    if (a->cnt != b->cnt)
        return -EINVAL;

    for (unsigned i = 0; i < a->cnt; i++) {
        const VmafDictionaryEntry *e = vmaf_dictionary_get(&b, a->entry[i].key, 0);
        if (!e)
            return -EINVAL;
        if (strcmp(e->val, a->entry[i].val) != 0)
            return -EINVAL;
    }

    return 0;
}

static int alphabetical_compare(const void *a, const void *b)
{
    const VmafDictionaryEntry *entry_a = a;
    const VmafDictionaryEntry *entry_b = b;
    return strcmp(entry_a->key, entry_b->key);
}

void vmaf_dictionary_alphabetical_sort(VmafDictionary *dict)
{
    if (!dict)
        return;
    qsort(dict->entry, dict->cnt, sizeof(*dict->entry), alphabetical_compare);
}

static int isnumeric(const char *str)
{
    char *end = NULL;
    (void)strtof(str, &end);
    if (end == str)
        return 0;
    while (*end == ' ' || *end == '\t' || *end == '\n')
        end++;
    return *end == '\0';
}

int vmaf_feature_dictionary_set(VmafFeatureDictionary **dict, const char *key, const char *val)
{
    uint64_t flags = 0;
    if (isnumeric(val))
        flags |= VMAF_DICT_NORMALIZE_NUMERICAL_VALUES;
    return vmaf_dictionary_set((VmafDictionary **)dict, key, val, flags);
}

int vmaf_feature_dictionary_free(VmafFeatureDictionary **dict)
{
    return vmaf_dictionary_free((VmafDictionary **)dict);
}
