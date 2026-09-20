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

#ifndef __VMAF_SRC_METADATA_H__
#define __VMAF_SRC_METADATA_H__

typedef struct VmafContext VmafContext;

/**
 * Metadata structure.
 *
 * @param feature_name   Name of the feature to fetch.
 *
 * @param picture_index  Picture index.
 *
 * @param score          Score.
 *
 * @note This structure is used to pass metadata to a callback function.
 */
typedef struct VmafMetadata {
    char *feature_name;
    unsigned picture_index;
    double score;
} VmafMetadata;

/**
 * Metadata configuration.
 *
 * @param feature_name Name of the feature to fetch.
 *
 * @param callback     Callback to receive metadata.
 *
 * @param data         User data to pass to the callback.
 */
typedef struct VmafMetadataConfiguration {
    char *feature_name;
    void (*callback)(void *data, const VmafMetadata *metadata);
    void *data;
} VmafMetadataConfiguration;

#endif /* __VMAF_SRC_METADATA_H__ */
