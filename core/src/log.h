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

#ifndef __VMAF_SRC_LOG_H__
#define __VMAF_SRC_LOG_H__

#include "libvmaf/libvmaf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the minimum log level for vmaf_log() output.
 *
 * Messages below @p log_level are silently discarded.
 *
 * @param log_level  Minimum level to emit (e.g. VMAF_LOG_LEVEL_INFO).
 */
void vmaf_set_log_level(enum VmafLogLevel log_level);

/**
 * @brief Emit a formatted log message at the given level.
 *
 * The message is suppressed if @p log_level is below the level set by
 * vmaf_set_log_level().
 *
 * @param log_level  Severity level of this message.
 * @param fmt        printf-style format string.
 * @param ...        Format arguments.
 */
void vmaf_log(enum VmafLogLevel log_level, const char *fmt, ...);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __VMAF_SRC_LOG_H__ */
