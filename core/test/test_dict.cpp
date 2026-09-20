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

// test.h declares run_tests() without extern "C"; wrap it so the C test harness
// (test.c, compiled as C) can link the symbol from this C++ TU.
extern "C" {
#include <cstring>

#include "test.h"
}
#include "dict.h"
#include "libvmaf/feature.h"

/* Use dict_internal.h to access isnumeric() without the ODR risk of
 * `#include "dict.cpp"`.  dict_internal.h defines isnumeric as `inline`
 * so this TU and dict.cpp can both see the definition with no ODR violation
 * (adversarial review 2026-05-28 finding #10; Power of 10 #8). */
#include "../src/dict_internal.h"

namespace
{

struct DictionaryItem {
    const char *key;
    const char *value;
};

mu_message_t test_vmaf_dictionary_growth()
{
    constexpr DictionaryItem items[] = {
        {.key = "key_1", .value = "val_1"}, {.key = "key_2", .value = "val_2"},
        {.key = "key_3", .value = "val_3"}, {.key = "key_4", .value = "val_4"},
        {.key = "key_5", .value = "val_5"}, {.key = "key_6", .value = "val_6"},
        {.key = "key_7", .value = "val_7"}, {.key = "key_8", .value = "val_8"},
        {.key = "key_9", .value = "val_9"},
    };
    VmafDictionary *dict = nullptr;
    int err = 0;
    for (unsigned i = 0; i < 8; ++i)
        err |= vmaf_dictionary_set(&dict, items[i].key, items[i].value, 0);
    mu_assert("first eight dictionary inserts should succeed", !err && dict);
    const unsigned initial_size = dict->size;
    mu_assert("this test asserts that initial_size is 8", initial_size == 8);
    mu_assert("dictionary should be completely full", dict->cnt == initial_size);

    err = vmaf_dictionary_set(&dict, items[8].key, items[8].value, 0);
    mu_assert("ninth dictionary insert should succeed", !err);
    mu_assert("dictionary capacity should have doubled",
              dict->cnt == initial_size + 1 && dict->size == initial_size * 2);
    vmaf_dictionary_free(&dict);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_lookup()
{
    VmafDictionary *dict = nullptr;
    const int err = vmaf_dictionary_set(&dict, "key_5", "val_5", 0);
    mu_assert("dictionary insert should succeed", !err && dict);
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&dict, "key_5", 0);
    mu_assert("dictionary should return an entry with valid key",
              entry && !strcmp(entry->val, "val_5"));
    entry = vmaf_dictionary_get(&dict, "invalid_key", 0);
    mu_assert("dictionary should return nullptr with invalid key", !entry);
    vmaf_dictionary_free(&dict);
    mu_assert("dictionary should be nullptr after free", !dict);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_do_not_overwrite()
{
    VmafDictionary *dict = nullptr;
    int err = vmaf_dictionary_set(&dict, "key", "original", 0);
    mu_assert("initial dictionary insert should succeed", !err && dict);
    err = vmaf_dictionary_set(&dict, "key", "replacement", VMAF_DICT_DO_NOT_OVERWRITE);
    mu_assert("different value must not overwrite an existing key", err);
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&dict, "key", 0);
    mu_assert("failed overwrite must preserve the original value",
              entry && !strcmp(entry->val, "original"));
    err = vmaf_dictionary_set(&dict, "key", "original", VMAF_DICT_DO_NOT_OVERWRITE);
    mu_assert("matching existing value should be accepted", !err);
    vmaf_dictionary_free(&dict);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_overwrite()
{
    VmafDictionary *dict = nullptr;
    int err = vmaf_dictionary_set(&dict, "key", "original", 0);
    err |= vmaf_dictionary_set(&dict, "key", "replacement", 0);
    mu_assert("dictionary overwrite should succeed", !err && dict);
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&dict, "key", 0);
    mu_assert("dictionary should return the replacement value",
              entry && !strcmp(entry->val, "replacement"));
    vmaf_dictionary_free(&dict);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_copy()
{
    VmafDictionary *dict = nullptr;
    VmafDictionary *copy = nullptr;
    int err = vmaf_dictionary_set(&dict, "key", "value", 0);
    mu_assert("dictionary insert should succeed", !err && dict);
    err = vmaf_dictionary_copy(&dict, &copy);
    mu_assert("dictionary copy should succeed", !err && copy);
    mu_assert("dictionary copy should preserve entry count", dict->cnt == copy->cnt);
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&copy, "key", 0);
    mu_assert("dictionary copy should preserve entries", entry && !strcmp(entry->val, "value"));
    vmaf_dictionary_free(&dict);
    vmaf_dictionary_free(&copy);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_merge_nulls()
{
    VmafDictionary *a = nullptr;
    VmafDictionary *b = nullptr;
    const VmafDictionary *merged = vmaf_dictionary_merge(&a, &b, 0);
    mu_assert("merging two nullptr dictionaries should return nullptr", !merged);
    return nullptr;
}

mu_message_t check_merge_one_null(bool populated_first)
{
    VmafDictionary *populated = nullptr;
    VmafDictionary *empty = nullptr;
    const int err = vmaf_dictionary_set(&populated, "key", "value", 0);
    mu_assert("dictionary insert should succeed", !err && populated);
    VmafDictionary *merged = populated_first ? vmaf_dictionary_merge(&populated, &empty, 0) :
                                               vmaf_dictionary_merge(&empty, &populated, 0);
    mu_assert("merging one nullptr dictionary should succeed", merged);
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&merged, "key", 0);
    mu_assert("merged dictionary should preserve the populated side",
              entry && !strcmp(entry->val, "value"));
    vmaf_dictionary_free(&merged);
    vmaf_dictionary_free(&populated);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_merge_null_right()
{
    return check_merge_one_null(true);
}

mu_message_t test_vmaf_dictionary_merge_null_left()
{
    return check_merge_one_null(false);
}

mu_message_t test_vmaf_dictionary_merge_two_populated()
{
    VmafDictionary *a = nullptr;
    VmafDictionary *b = nullptr;
    int err = vmaf_dictionary_set(&a, "key_a", "val_a", 0);
    err |= vmaf_dictionary_set(&b, "key_b", "val_b", 0);
    mu_assert("dictionary inserts should succeed", !err && a && b);
    VmafDictionary *merged = vmaf_dictionary_merge(&b, &a, 0);
    mu_assert("merging two populated dictionaries should succeed", merged);
    const VmafDictionaryEntry *entry_a = vmaf_dictionary_get(&merged, "key_a", 0);
    const VmafDictionaryEntry *entry_b = vmaf_dictionary_get(&merged, "key_b", 0);
    mu_assert("merged dictionary should preserve both values", entry_a && entry_b &&
                                                                   !strcmp(entry_a->val, "val_a") &&
                                                                   !strcmp(entry_b->val, "val_b"));
    vmaf_dictionary_free(&merged);
    vmaf_dictionary_free(&a);
    vmaf_dictionary_free(&b);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_merge_duplicate_last_wins()
{
    VmafDictionary *a = nullptr;
    VmafDictionary *b = nullptr;
    int err = vmaf_dictionary_set(&a, "duplicate", "first", 0);
    err |= vmaf_dictionary_set(&a, "duplicate", "second", 0);
    err |= vmaf_dictionary_set(&b, "other", "value", 0);
    mu_assert("dictionary inserts should succeed", !err && a && b);
    VmafDictionary *merged = vmaf_dictionary_merge(&b, &a, 0);
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&merged, "duplicate", 0);
    mu_assert("last duplicate value should win", entry && !strcmp(entry->val, "second"));
    vmaf_dictionary_free(&merged);
    vmaf_dictionary_free(&a);
    vmaf_dictionary_free(&b);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_merge_duplicate_rejected()
{
    VmafDictionary *a = nullptr;
    VmafDictionary *b = nullptr;
    int err = vmaf_dictionary_set(&a, "duplicate", "value_a", 0);
    err |= vmaf_dictionary_set(&b, "duplicate", "value_b", 0);
    mu_assert("dictionary inserts should succeed", !err && a && b);
    const VmafDictionary *merged = vmaf_dictionary_merge(&b, &a, VMAF_DICT_DO_NOT_OVERWRITE);
    mu_assert("conflicting duplicate should reject the merge", !merged);
    vmaf_dictionary_free(&a);
    vmaf_dictionary_free(&b);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_compare_disjoint()
{
    VmafDictionary *a = nullptr;
    VmafDictionary *b = nullptr;
    int err = vmaf_dictionary_set(&a, "key_1", "val_1", 0);
    err |= vmaf_dictionary_set(&b, "key_2", "val_2", 0);
    mu_assert("dictionary inserts should succeed", !err && a && b);
    mu_assert("disjoint dictionaries should not compare equal", vmaf_dictionary_compare(a, b));
    vmaf_dictionary_free(&a);
    vmaf_dictionary_free(&b);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_compare_order_independent()
{
    VmafDictionary *a = nullptr;
    VmafDictionary *b = nullptr;
    int err = vmaf_dictionary_set(&a, "key_1", "val_1", 0);
    err |= vmaf_dictionary_set(&a, "key_2", "val_2", 0);
    err |= vmaf_dictionary_set(&b, "key_2", "val_2", 0);
    err |= vmaf_dictionary_set(&b, "key_1", "val_1", 0);
    mu_assert("dictionary inserts should succeed", !err && a && b);
    mu_assert("entry order should not affect comparison", !vmaf_dictionary_compare(a, b));
    vmaf_dictionary_free(&a);
    vmaf_dictionary_free(&b);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_compare_count()
{
    VmafDictionary *a = nullptr;
    VmafDictionary *b = nullptr;
    int err = vmaf_dictionary_set(&a, "key_1", "val_1", 0);
    err |= vmaf_dictionary_set(&a, "key_2", "val_2", 0);
    err |= vmaf_dictionary_set(&b, "key_1", "val_1", 0);
    mu_assert("dictionary inserts should succeed", !err && a && b);
    mu_assert("different entry counts should not compare equal", vmaf_dictionary_compare(a, b));
    vmaf_dictionary_free(&a);
    vmaf_dictionary_free(&b);
    return nullptr;
}

mu_message_t test_vmaf_dictionary_compare_value()
{
    VmafDictionary *a = nullptr;
    VmafDictionary *b = nullptr;
    int err = vmaf_dictionary_set(&a, "key", "value_a", 0);
    err |= vmaf_dictionary_set(&b, "key", "value_b", 0);
    mu_assert("dictionary inserts should succeed", !err && a && b);
    mu_assert("different values should not compare equal", vmaf_dictionary_compare(a, b));
    vmaf_dictionary_free(&a);
    vmaf_dictionary_free(&b);
    return nullptr;
}

mu_message_t check_normalized_insert(const char *input, const char *expected)
{
    VmafDictionary *dict = nullptr;
    const int err = vmaf_dictionary_set(&dict, "key", input, VMAF_DICT_NORMALIZE_NUMERICAL_VALUES);
    mu_assert("normalized dictionary insert should succeed", !err && dict);
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&dict, "key", 0);
    mu_assert("dictionary should store the normalized value",
              entry && !strcmp(entry->val, expected));
    mu_assert("dictionary should contain one entry", dict->cnt == 1);
    vmaf_dictionary_free(&dict);
    return nullptr;
}

mu_message_t check_normalized_duplicate(const char *input)
{
    VmafDictionary *dict = nullptr;
    int err = vmaf_dictionary_set(&dict, "key", "1.0", VMAF_DICT_NORMALIZE_NUMERICAL_VALUES);
    err |= vmaf_dictionary_set(&dict, "key", input,
                               VMAF_DICT_NORMALIZE_NUMERICAL_VALUES | VMAF_DICT_DO_NOT_OVERWRITE);
    mu_assert("equivalent normalized value should not conflict", !err && dict);
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&dict, "key", 0);
    mu_assert("equivalent normalized value should remain canonical",
              entry && !strcmp(entry->val, "1") && dict->cnt == 1);
    vmaf_dictionary_free(&dict);
    return nullptr;
}

mu_message_t test_normalize_decimal()
{
    return check_normalized_insert("1.0000", "1");
}

mu_message_t test_normalize_integer()
{
    return check_normalized_insert("1", "1");
}

mu_message_t test_normalize_duplicate_decimal()
{
    return check_normalized_duplicate("1.00");
}

mu_message_t test_normalize_duplicate_whitespace()
{
    return check_normalized_duplicate(" 1.00 ");
}

mu_message_t test_normalize_numeric_prefix()
{
    return check_normalized_duplicate("1abc");
}

mu_message_t test_normalize_non_numeric()
{
    return check_normalized_insert("true", "true");
}

mu_message_t test_vmaf_feature_dictionary()
{
    int err = 0;

    VmafFeatureDictionary *dict = nullptr;
    err = vmaf_feature_dictionary_set(&dict, "option", "value");
    mu_assert("problem during vmaf_feature_dictionary_set", !err);
    mu_assert("dictionary should not be NULL after setting first option", dict);
    err = vmaf_feature_dictionary_free(&dict);
    mu_assert("problem during vmaf_feature_dictionary_free", !err);
    mu_assert("dictionary should be NULL after free", !dict);

    return nullptr;
}

mu_message_t test_vmaf_dictionary_alphabetical_sort()
{
    int err = 0;

    VmafDictionary *dict = nullptr;
    err |= vmaf_dictionary_set(&dict, "z", "z", 0);
    err |= vmaf_dictionary_set(&dict, "y", "y", 0);
    err |= vmaf_dictionary_set(&dict, "x", "x", 0);
    err |= vmaf_dictionary_set(&dict, "a", "a", 0);
    err |= vmaf_dictionary_set(&dict, "b", "b", 0);
    err |= vmaf_dictionary_set(&dict, "c", "c", 0);
    err |= vmaf_dictionary_set(&dict, "2", "2", 0);
    err |= vmaf_dictionary_set(&dict, "1", "1", 0);
    err |= vmaf_dictionary_set(&dict, "0", "0", 0);
    mu_assert("problem during vmaf_feature_dictionary_set", !err);
    mu_assert("dict should have 9 entries", dict->cnt == 9);

    vmaf_dictionary_alphabetical_sort(dict);
    mu_assert("dict should have 9 entries", dict->cnt == 9);
    const char *const expected_order[9] = {"0", "1", "2", "a", "b", "c", "x", "y", "z"};

    for (unsigned i = 0; i < 9; i++) {
        mu_assert("dict is not alphabetically sorted",
                  !strcmp(dict->entry[i].key, expected_order[i]));
    }

    err = vmaf_dictionary_free(&dict);
    mu_assert("problem during vmaf_feature_dictionary_free", !err);

    return nullptr;
}

mu_message_t test_isnumeric_rejects_non_numbers()
{
    mu_assert("problem during isnumeric", !isnumeric("abc"));
    mu_assert("problem during isnumeric", !isnumeric("/a/b/c"));
    mu_assert("problem during isnumeric", !isnumeric("abc123"));
    mu_assert("problem during isnumeric", !isnumeric("123abc"));
    return nullptr;
}

mu_message_t test_isnumeric_accepts_numbers()
{
    mu_assert("problem during isnumeric", isnumeric("123"));
    mu_assert("problem during isnumeric", isnumeric("123.456"));
    mu_assert("problem during isnumeric", isnumeric("    123.456    "));
    mu_assert("problem during isnumeric", isnumeric("NaN"));
    mu_assert("problem during isnumeric", isnumeric("inf"));
    mu_assert("problem during isnumeric", isnumeric("-inf"));

    return nullptr;
}

mu_message_t test_dictionary_core_group()
{
    mu_run_test(test_vmaf_dictionary_growth);
    mu_run_test(test_vmaf_dictionary_lookup);
    mu_run_test(test_vmaf_dictionary_do_not_overwrite);
    mu_run_test(test_vmaf_dictionary_overwrite);
    mu_run_test(test_vmaf_dictionary_copy);
    return nullptr;
}

mu_message_t test_dictionary_merge_group()
{
    mu_run_test(test_vmaf_dictionary_merge_nulls);
    mu_run_test(test_vmaf_dictionary_merge_null_right);
    mu_run_test(test_vmaf_dictionary_merge_null_left);
    mu_run_test(test_vmaf_dictionary_merge_two_populated);
    mu_run_test(test_vmaf_dictionary_merge_duplicate_last_wins);
    mu_run_test(test_vmaf_dictionary_merge_duplicate_rejected);
    return nullptr;
}

mu_message_t test_dictionary_compare_group()
{
    mu_run_test(test_vmaf_dictionary_compare_disjoint);
    mu_run_test(test_vmaf_dictionary_compare_order_independent);
    mu_run_test(test_vmaf_dictionary_compare_count);
    mu_run_test(test_vmaf_dictionary_compare_value);
    return nullptr;
}

mu_message_t test_dictionary_normalize_group()
{
    mu_run_test(test_normalize_decimal);
    mu_run_test(test_normalize_integer);
    mu_run_test(test_normalize_duplicate_decimal);
    mu_run_test(test_normalize_duplicate_whitespace);
    mu_run_test(test_normalize_numeric_prefix);
    mu_run_test(test_normalize_non_numeric);
    return nullptr;
}

mu_message_t test_dictionary_misc_group()
{
    mu_run_test(test_vmaf_feature_dictionary);
    mu_run_test(test_vmaf_dictionary_alphabetical_sort);
    mu_run_test(test_isnumeric_rejects_non_numbers);
    mu_run_test(test_isnumeric_accepts_numbers);
    return nullptr;
}

} // namespace

mu_message_t run_tests()
{
    mu_run_test(test_dictionary_core_group);
    mu_run_test(test_dictionary_merge_group);
    mu_run_test(test_dictionary_compare_group);
    mu_run_test(test_dictionary_normalize_group);
    mu_run_test(test_dictionary_misc_group);
    return nullptr;
}
