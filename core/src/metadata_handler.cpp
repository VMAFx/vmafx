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
 * C++20 implementation of the callback-list metadata handler.
 *
 * Migration note (ADR-0708 / Research-0732):
 *   - `git mv metadata_handler.c metadata_handler.cpp` preserves blame.
 *   - The public C API in metadata_handler.h is unchanged; all three
 *     functions retain their original C signatures and are declared
 *     `extern "C"` in the header, so every C caller (feature_collector.c)
 *     links without modification.
 *   - `std::unique_ptr<VmafCallbackList>` RAII-manages the list head,
 *     replacing the manual `free(metadata)` tail-call in
 *     `vmaf_metadata_destroy` with guaranteed cleanup on any exit path.
 *   - The per-node teardown loop is encapsulated in `NodeDeleter`, which
 *     means a future early-return inside `vmaf_metadata_destroy` cannot
 *     leak nodes — the deleter always runs when the unique_ptr goes out
 *     of scope.
 *   - `nullptr` replaces `NULL` throughout this file (no change to the
 *     struct fields, which are C-linkage and keep `NULL` in callers).
 */

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "metadata_handler.h"

/* RAII deleter for the VmafCallbackItem linked list.
 * Walks and frees every node before the list head itself is released by
 * the unique_ptr machinery.  This eliminates the manual traversal loop
 * that was previously in vmaf_metadata_destroy and ensures correctness
 * even if additional early-return paths are added in the future. */
struct CallbackListDeleter {
    void operator()(VmafCallbackList *list) const noexcept
    {
        if (!list)
            return;
        VmafCallbackItem *node = list->head;
        while (node) {
            VmafCallbackItem *next = node->next;
            /* free() on a VmafCallbackItem that carries no heap-owned
             * sub-fields (metadata_cfg, callback, data are all value or
             * borrowed-pointer types — callers own those lifetimes). */
            free(node); // NOLINT(cppcoreguidelines-no-malloc) — C ABI node
            node = next;
        }
        free(list); // NOLINT(cppcoreguidelines-no-malloc) — C ABI struct
    }
};

/* Convenience alias used only within this translation unit. */
using CallbackListPtr = std::unique_ptr<VmafCallbackList, CallbackListDeleter>;

int vmaf_metadata_init(VmafCallbackList **const metadata)
{
    if (!metadata)
        return -EINVAL;

    auto *list = static_cast<VmafCallbackList *>(malloc(sizeof(VmafCallbackList)));
    if (!list)
        return -ENOMEM;

    list->head = nullptr;
    *metadata = list;
    return 0;
}

int vmaf_metadata_append(VmafCallbackList *metadata, const VmafMetadataConfiguration metadata_cfg)
{
    if (!metadata)
        return -EINVAL;

    auto *node = static_cast<VmafCallbackItem *>(malloc(sizeof(VmafCallbackItem)));
    if (!node)
        return -ENOMEM;

    /* Zero-initialise before field assignment so that the `callback` and
     * `data` fields (not set by this function) start at NULL/nullptr rather
     * than carrying stack garbage.  memset is safe here: VmafCallbackItem
     * is a C struct with no virtual functions or non-trivial members. */
    memset(node, 0, sizeof(*node));
    node->metadata_cfg = metadata_cfg;
    node->next = nullptr;

    /* Append at tail so callbacks fire in registration order. */
    if (!metadata->head) {
        metadata->head = node;
    } else {
        VmafCallbackItem *iter = metadata->head;
        while (iter->next)
            iter = iter->next;
        iter->next = node;
    }

    return 0;
}

int vmaf_metadata_destroy(VmafCallbackList *metadata)
{
    if (!metadata)
        return -EINVAL;

    /* Adopt the raw C pointer into a unique_ptr with our custom deleter.
     * When `guard` goes out of scope at the end of this function (including
     * on any future early-return path), CallbackListDeleter::operator()
     * walks and frees every node, then frees the list struct itself.
     *
     * No behaviour change vs the original C implementation on the happy
     * path; the improvement is the guaranteed teardown on any new
     * early-return that might be added during future maintenance. */
    CallbackListPtr guard(metadata);
    (void)guard; /* explicit acknowledgement that we want the side-effect */
    return 0;
}
