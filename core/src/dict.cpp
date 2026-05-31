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
 * dict.cpp — C++23 rewrite of dict.c (ADR-0727).
 *
 * Public C ABI is preserved exactly: every extern "C" symbol has the same
 * signature and semantics as the C original. C++23 idioms are used
 * internally:
 *   - std::expected<T,int>  replaces out-param + return-code patterns.
 *   - std::string_view      for read-only string parameters.
 *   - std::optional<T>      for nullable internal returns.
 *   - [[nodiscard]]         on every function whose return must be checked.
 *   - RAII (std::unique_ptr)replaces manual goto-cleanup patterns.
 *
 * Toolchain requirement: gcc >= 13 / clang >= 16 (std::expected, std::print
 * not used — std::format available but clarity gain is marginal here; kept
 * simple).  The CI matrix is already on gcc 14+ post-ADR-0692.
 */

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include "dict.h"
#include "libvmaf/feature.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/*
 * dict_ensure_allocated — lazily initialise *dict if null.
 * Returns std::expected<VmafDictionary*, int>: the ready dict ptr on success,
 * a negative errno on failure.
 */
[[nodiscard]] static std::expected<VmafDictionary *, int>
dict_ensure_allocated(VmafDictionary **dict)
{
    if (*dict)
        return *dict;

    auto *d = static_cast<VmafDictionary *>(std::malloc(sizeof(VmafDictionary)));
    if (!d)
        return std::unexpected(-ENOMEM);
    std::memset(d, 0, sizeof(*d));

    const std::size_t initial_sz = 8 * sizeof(*d->entry);
    d->entry = static_cast<VmafDictionaryEntry *>(std::malloc(initial_sz));
    if (!d->entry) {
        std::free(d);
        return std::unexpected(-ENOMEM);
    }
    std::memset(d->entry, 0, initial_sz);
    d->size = 8;
    *dict = d;
    return d;
}

/*
 * dict_normalize_numeric — if val is numeric, format it via %g and return the
 * normalised string as a unique_ptr<char[]>.  Returns nullopt (not numeric)
 * or an expected with the buffer.
 */
[[nodiscard]] static std::expected<std::unique_ptr<char[]>, int>
dict_normalize_numeric(std::string_view val)
{
    char *end = nullptr;
    // strtod operates on NUL-terminated C strings; val is NUL-terminated
    // because it comes from the public C API (const char *).
    // Use strtod (not strtof) to preserve double precision; strtof rounds
    // to ~6-7 significant digits and loses precision on widening to double.
    // Fix for CRITICAL finding in adversarial review PR #78.
    double dv = std::strtod(val.data(), &end);
    if (dv == 0.0 && end == val.data())
        return std::unexpected(0); // not numeric — sentinel 0 means "skip"

    const char *fmt = "%g";
    const int snp = std::snprintf(nullptr, 0, fmt, dv);
    if (snp < 0)
        return std::unexpected(-EINVAL);

    const std::size_t buf_sz = static_cast<std::size_t>(snp) + 1u;
    auto buf = std::make_unique<char[]>(buf_sz);
    (void)std::snprintf(buf.get(), buf_sz, fmt, dv);
    return buf;
}

/*
 * dict_grow_entries — double the entry array when full.
 */
[[nodiscard]] static std::expected<void, int> dict_grow_entries(VmafDictionary *d)
{
    if (d->cnt < d->size)
        return {};
    assert(d->size > 0);
    const std::size_t sz = d->size * sizeof(*d->entry) * 2u;
    auto *entry = static_cast<VmafDictionaryEntry *>(std::realloc(d->entry, sz));
    if (!entry)
        return std::unexpected(-ENOMEM);
    d->entry = entry;
    d->size *= 2;
    return {};
}

/*
 * dict_overwrite_existing — replace the value of an existing entry.
 * Idempotency guard: if the stored value already equals val, no-op.
 * (See the long comment in the original dict.c about the ASan
 * SAN-PREDICT-METADATA-LEAK pattern — that logic is preserved.)
 */
[[nodiscard]] static std::expected<void, int> dict_overwrite_existing(VmafDictionaryEntry *existing,
                                                                      std::string_view val)
{
    if (existing->val && std::strcmp(existing->val, val.data()) == 0)
        return {};

    const char *val_copy = ::strdup(val.data());
    if (!val_copy)
        return std::unexpected(-ENOMEM);
    std::free(const_cast<char *>(existing->val));
    existing->val = val_copy;
    return {};
}

/*
 * dict_append_new_entry — add a fresh key/value pair.
 * Zero-initialises the slot before writing (see original comment about
 * realloc not zeroing new bytes and partial-write windows).
 */
[[nodiscard]] static std::expected<void, int>
dict_append_new_entry(VmafDictionary *d, std::string_view key, std::string_view val)
{
    if (auto r = dict_grow_entries(d); !r)
        return r;

    auto *val_copy = ::strdup(val.data());
    if (!val_copy)
        return std::unexpected(-ENOMEM);
    auto *key_copy = ::strdup(key.data());
    if (!key_copy) {
        std::free(val_copy);
        return std::unexpected(-ENOMEM);
    }

    d->entry[d->cnt] = VmafDictionaryEntry{.key = nullptr, .val = nullptr};
    d->entry[d->cnt].key = key_copy;
    d->entry[d->cnt].val = val_copy;
    d->cnt++;
    return {};
}

// ---------------------------------------------------------------------------
// Internal isnumeric helper — defined in dict_internal.h (shared with tests)
// ---------------------------------------------------------------------------

/* isnumeric is defined as `inline bool` in dict_internal.h so that the
 * white-box test (test_dict.cpp) can include it without an ODR violation.
 * The previous `static bool isnumeric` local definition is removed here
 * (adversarial review 2026-05-28 finding #10). */
#include "dict_internal.h"

// ---------------------------------------------------------------------------
// Public C API — all functions in extern "C" to preserve ABI
// ---------------------------------------------------------------------------

extern "C" {

VmafDictionaryEntry *vmaf_dictionary_get(VmafDictionary **dict, const char *key, uint64_t flags)
{
    if (!dict || !(*dict) || !key)
        return nullptr;
    (void)flags; // reserved for future use

    VmafDictionary *d = *dict;
    const std::string_view k{key};
    for (unsigned i = 0; i < d->cnt; i++) {
        if (k == d->entry[i].key)
            return &d->entry[i];
    }
    return nullptr;
}

int vmaf_dictionary_set(VmafDictionary **dict, const char *key, const char *val, uint64_t flags)
{
    if (!dict || !key || !val)
        return -EINVAL;

    auto dict_or_err = dict_ensure_allocated(dict);
    if (!dict_or_err)
        return dict_or_err.error();

    // Numeric normalisation: build a temporary normalised string if requested.
    std::unique_ptr<char[]> norm_buf;
    const char *effective_val = val;
    if (flags & VMAF_DICT_NORMALIZE_NUMERICAL_VALUES) {
        auto r = dict_normalize_numeric(val);
        if (!r && r.error() != 0)
            return r.error(); // genuine failure
        if (r) {
            norm_buf = std::move(*r);
            effective_val = norm_buf.get();
        }
        // r.error() == 0 means "not numeric" — keep effective_val = val.
    }

    VmafDictionary *d = *dict;
    VmafDictionaryEntry *existing = vmaf_dictionary_get(&d, key, 0);

    std::expected<void, int> result{};
    if (existing && (flags & VMAF_DICT_DO_NOT_OVERWRITE)) {
        result = (std::strcmp(existing->val, effective_val) == 0) ? std::expected<void, int>{} :
                                                                    std::unexpected(-EINVAL);
    } else if (existing) {
        result = dict_overwrite_existing(existing, effective_val);
    } else {
        result = dict_append_new_entry(d, key, effective_val);
    }

    return result ? 0 : result.error();
}

int vmaf_dictionary_copy(VmafDictionary **src, VmafDictionary **dst)
{
    if (!src || !(*src) || !dst)
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
        /* free(nullptr) is well-defined per C99 §7.20.3.2; guards are
         * redundant but harmless. */
        std::free(const_cast<char *>(d->entry[i].key));
        std::free(const_cast<char *>(d->entry[i].val));
    }
    std::free(d->entry);
    std::free(d);
    *dict = nullptr;
    return 0;
}

VmafDictionary *vmaf_dictionary_merge(VmafDictionary **dict_a, VmafDictionary **dict_b,
                                      uint64_t flags)
{
    VmafDictionary *merged = nullptr;

    if (*dict_a) {
        if (vmaf_dictionary_copy(dict_a, &merged) != 0) {
            (void)vmaf_dictionary_free(&merged);
            return nullptr;
        }
    }

    if (*dict_b) {
        VmafDictionary *b = *dict_b;
        for (unsigned i = 0; i < b->cnt; i++) {
            if (vmaf_dictionary_set(&merged, b->entry[i].key, b->entry[i].val, flags) != 0) {
                (void)vmaf_dictionary_free(&merged);
                return nullptr;
            }
        }
    }

    return merged;
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
        if (std::strcmp(e->val, a->entry[i].val) != 0)
            return -EINVAL;
    }
    return 0;
}

void vmaf_dictionary_alphabetical_sort(VmafDictionary *dict)
{
    if (!dict)
        return;
    std::qsort(dict->entry, dict->cnt, sizeof(*dict->entry),
               [](const void *a, const void *b) noexcept -> int {
                   const auto *ea = static_cast<const VmafDictionaryEntry *>(a);
                   const auto *eb = static_cast<const VmafDictionaryEntry *>(b);
                   return std::strcmp(ea->key, eb->key);
               });
}

int vmaf_feature_dictionary_set(VmafFeatureDictionary **dict, const char *key, const char *val)
{
    uint64_t flags = 0;
    if (isnumeric(val))
        flags |= VMAF_DICT_NORMALIZE_NUMERICAL_VALUES;
    return vmaf_dictionary_set(reinterpret_cast<VmafDictionary **>(dict), key, val, flags);
}

int vmaf_feature_dictionary_free(VmafFeatureDictionary **dict)
{
    return vmaf_dictionary_free(reinterpret_cast<VmafDictionary **>(dict));
}

} // extern "C"
