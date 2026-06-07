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

#ifndef __VMAF_SRC_OPT_H__
#define __VMAF_SRC_OPT_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Scalar type of a VmafOption value. */
enum VmafOptionType {
    VMAF_OPT_TYPE_BOOL,   /**< Boolean (parsed from "true"/"false"/"1"/"0"). */
    VMAF_OPT_TYPE_INT,    /**< Integer. */
    VMAF_OPT_TYPE_DOUBLE, /**< Double-precision float. */
    VMAF_OPT_TYPE_STRING, /**< NUL-terminated string (heap-allocated). */
};

/** @brief Modifier flags for a VmafOption. */
enum VmafOptionFlag {
    /** Option controls a feature-extractor parameter (exposed via the dict API). */
    VMAF_OPT_FLAG_FEATURE_PARAM = 1 << 0,
};

/**
 * @brief Descriptor for a single configurable option on a feature-extractor context.
 *
 * A feature extractor exposes an array of VmafOption terminated by a
 * zero-initialised sentinel.  vmaf_option_set() walks the array and writes the
 * parsed value at the byte offset @p offset into the context struct.
 *
 * @var VmafOption::name         Primary option name (used as the dict key).
 * @var VmafOption::help         Human-readable description.
 * @var VmafOption::alias        Optional secondary name (may be NULL).
 * @var VmafOption::offset       Byte offset of the field within the context struct.
 * @var VmafOption::type         Scalar type of the field.
 * @var VmafOption::default_val  Value written when the option is not supplied.
 * @var VmafOption::min          Minimum accepted value (numeric types only).
 * @var VmafOption::max          Maximum accepted value (numeric types only).
 * @var VmafOption::flags        Combination of VmafOptionFlag.
 */
typedef struct VmafOption {
    const char *name;
    const char *help;
    const char *alias;
    int offset;
    enum VmafOptionType type;
    union {
        bool b;
        int i;
        double d;
        char *s;
    } default_val;
    double min, max;
    uint64_t flags;
} VmafOption;

/**
 * @brief Parse @p val and write it into the field described by @p opt.
 *
 * The write target is @p obj + opt->offset.  Range checks are applied for
 * numeric types.
 *
 * @param opt  Option descriptor.
 * @param obj  Pointer to the context struct that owns the field.
 * @param val  String representation of the value to set.
 * @return 0 on success, negative errno on failure (e.g. -ERANGE, -EINVAL).
 */
int vmaf_option_set(const VmafOption *opt, void *obj, const char *val);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __VMAF_SRC_OPT_H__ */
