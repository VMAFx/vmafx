/* SPDX-License-Identifier: BSD-2-Clause-Patent */
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

#ifndef VMAF_SRC_LOG_H_
#define VMAF_SRC_LOG_H_

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

/**
 * @brief The level vmaf_set_log_level() last set (VMAF_LOG_LEVEL_INFO before
 *        the first call).
 */
enum VmafLogLevel vmaf_get_log_level(void);

/**
 * @brief A per-thread destination for vmaf_log() (ADR-1852, RC4 WP2).
 *
 * While a sink is installed on a thread, vmaf_log() on that thread formats
 * the message into one line (no trailing newline) and hands it to
 * `deliver` instead of writing to stderr, filtered by the sink's `level`
 * rather than the process level. The VMAFx API installs the sink of a
 * context that has a log callback around each engine call it makes on that
 * context, and the engine installs the submitting call's sink around every
 * job it runs on a worker thread (ADR-1906).
 */
/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no `using`. ADR-1138. */
typedef struct VmafLogSink {
    void (*deliver)(enum VmafLogLevel level, const char *message, void *user);
    void *user;
    enum VmafLogLevel level;
} VmafLogSink;
/* NOLINTEND(modernize-use-using) */

/**
 * @brief Install `sink` (NULL: none) on the calling thread.
 *
 * @return The sink installed before, so a caller can restore it.
 */
const VmafLogSink *vmaf_log_swap_thread_sink(const VmafLogSink *sink);

/**
 * @brief The sink installed on the calling thread, or NULL. A job the engine
 *        hands to a worker thread captures it so the worker logs where the
 *        submitting call logs (ADR-1906).
 */
const VmafLogSink *vmaf_log_thread_sink(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_LOG_H_ */
