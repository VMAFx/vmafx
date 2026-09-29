/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Motion SAD kernel shared by motion_sycl and motion_v2_sycl; see
 *  integer_motion_pipeline_sycl.h for the arithmetic contract.
 *
 *  One work-item per output pixel. A work-group stages the (prev - cur)
 *  difference of its 8x32 outputs plus the two-sample filter halo in local
 *  memory, filters it vertically at the five columns the horizontal tap
 *  needs, filters those horizontally, and sums |h| through a sub-group
 *  reduction and one device atomic per work-group. Integer arithmetic
 *  throughout, so the order of the sums cannot change the result. The copy
 *  of `cur` the caller may ask for is a device memcpy after the kernel.
 */

#include "integer_motion_pipeline_sycl.h"

#include <sycl/sycl.hpp>

#include "sycl_compat.h"
#include "sycl_tile_index.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace motion_sycl_pipeline
{
namespace
{

constexpr int32_t kFilter[5] = {3571, 16004, 26386, 16004, 3571}; /* sum 65536 */
constexpr int kWgX = 32;
constexpr int kWgY = 8;
constexpr int kHalf = 2;
constexpr int kTileW = kWgX + 2 * kHalf; /* 36 */
constexpr int kTileH = kWgY + 2 * kHalf; /* 12 */
constexpr unsigned kGroupSize = kWgX * kWgY;
constexpr int kMaxSubgroups = 32;

/* The vertical pass sums |filter[k] * diff| <= 65536 * (2^bpc - 1), plus a
 * rounding term of 2^(bpc - 1). That fits int32 up to 15 bits per sample;
 * 16-bit input takes the int64 path, as the CPU's 16-bit pipeline does. */
constexpr unsigned kInt32VerticalMaxBpc = 15;

/* Reflect-101, the CPU's mirror(): -1 -> 1, extent -> extent - 2. */
inline int reflect_101(int idx, int extent)
{
    if (idx < 0) {
        return -idx;
    }
    if (idx >= extent) {
        return (2 * extent) - idx - 2;
    }
    return idx;
}

inline int32_t read_sample(const void *plane, size_t offset, unsigned bpc)
{
    if (bpc <= 8) {
        return static_cast<const uint8_t *>(plane)[offset];
    }
    return static_cast<const uint16_t *>(plane)[offset];
}

/* Stage prev - cur for the work-group's outputs and halo. The CPU differences
 * in that order, and the order matters: the arithmetic shift rounds a
 * negative sum towards minus infinity. */
inline void load_diff(sycl::nd_item<2> item, const sycl::local_accessor<int32_t, 2> &diff,
                      const SadArgs &args)
{
    const int tile_y = (int)(item.get_group(0) * kWgY) - kHalf;
    const int tile_x = (int)(item.get_group(1) * kWgX) - kHalf;
    const bool interior = (tile_y >= 0) && (tile_y + kTileH <= (int)args.height) && (tile_x >= 0) &&
                          (tile_x + kTileW <= (int)args.width);
    constexpr unsigned tile_elems = kTileH * kTileW;
    for (unsigned i = item.get_local_linear_id(); i < tile_elems; i += kGroupSize) {
        const unsigned row = i / kTileW;
        const unsigned col = i % kTileW;
        int y = tile_y + (int)row;
        int x = tile_x + (int)col;
        if (!interior) {
            y = vmaf_sycl_tile_index(reflect_101(y, (int)args.height), (int)args.height);
            x = vmaf_sycl_tile_index(reflect_101(x, (int)args.width), (int)args.width);
        }
        const size_t offset = ((size_t)y * args.width) + (size_t)x;
        diff[row][col] =
            read_sample(args.prev, offset, args.bpc) - read_sample(args.cur, offset, args.bpc);
    }
}

/* Vertical 5-tap filter of tile column `col` around local row `ly`. */
template <typename Acc>
inline int32_t vertical_tap(const sycl::local_accessor<int32_t, 2> &diff, unsigned ly, unsigned col,
                            unsigned bpc)
{
    const Acc sum = ((Acc)kFilter[0] * (diff[ly][col] + diff[ly + 4][col])) +
                    ((Acc)kFilter[1] * (diff[ly + 1][col] + diff[ly + 3][col])) +
                    ((Acc)kFilter[2] * diff[ly + 2][col]);
    const Acc round = (Acc)1 << (bpc - 1);
    return (int32_t)((sum + round) >> bpc);
}

/* |h| at this work-item's pixel, 0 for the padding work-items. `Acc` is the
 * vertical accumulator; the host picks it per launch, so the kernel carries
 * no per-pixel width test. */
template <typename Acc>
inline int64_t filtered_abs(sycl::nd_item<2> item, const sycl::local_accessor<int32_t, 2> &diff,
                            const SadArgs &args)
{
    const int x = (int)item.get_global_id(1);
    const int y = (int)item.get_global_id(0);
    if (!std::cmp_less(x, args.width) || !std::cmp_less(y, args.height)) {
        return 0;
    }
    const unsigned lx = item.get_local_id(1);
    const unsigned ly = item.get_local_id(0);
    int32_t v[5];
#pragma unroll
    for (unsigned hx = 0; hx < 5; hx++) {
        v[hx] = vertical_tap<Acc>(diff, ly, lx + hx, args.bpc);
    }
    const int64_t sum = ((int64_t)kFilter[0] * (v[0] + v[4])) +
                        ((int64_t)kFilter[1] * (v[1] + v[3])) + ((int64_t)kFilter[2] * v[2]);
    const int64_t h = (sum + 32768) >> 16;
    return (h < 0) ? -h : h;
}

inline void reduce_sad(sycl::nd_item<2> item, const sycl::local_accessor<int64_t, 1> &scratch,
                       int64_t value, int64_t *sad)
{
    const sycl::sub_group subgroup = item.get_sub_group();
    const int64_t subgroup_sum = sycl::reduce_over_group(subgroup, value, sycl::plus<int64_t>{});
    if (subgroup.get_local_linear_id() == 0) {
        scratch[subgroup.get_group_linear_id()] = subgroup_sum;
    }
    item.barrier(sycl::access::fence_space::local_space);
    if (item.get_local_linear_id() == 0) {
        int64_t total = 0;
        const uint32_t subgroups = subgroup.get_group_linear_range();
        for (uint32_t i = 0; i < subgroups; i++) {
            total += scratch[i];
        }
        const sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                               sycl::access::address_space::global_space>
            output(*sad);
        output.fetch_add(total);
    }
}

size_t plane_bytes(const SadArgs &args)
{
    return (size_t)args.width * args.height * ((args.bpc <= 8) ? 1U : 2U);
}

template <typename Acc> void submit_sad(sycl::queue &queue, const SadArgs &args)
{
    const size_t global_h = (((size_t)args.height + kWgY - 1) / kWgY) * kWgY;
    const size_t global_w = (((size_t)args.width + kWgX - 1) / kWgX) * kWgX;
    const sycl::nd_range<2> range({global_h, global_w}, {(size_t)kWgY, (size_t)kWgX});

    queue.submit([&](sycl::handler &cgh) {
        const sycl::local_accessor<int32_t, 2> diff(sycl::range<2>(kTileH, kTileW), cgh);
        const sycl::local_accessor<int64_t, 1> scratch(sycl::range<1>(kMaxSubgroups), cgh);
        const SadArgs kernel_args = args;
        cgh.parallel_for(range, [=](sycl::nd_item<2> item) VMAF_SYCL_REQD_SG_SIZE(32) {
            load_diff(item, diff, kernel_args);
            item.barrier(sycl::access::fence_space::local_space);
            const int64_t value = filtered_abs<Acc>(item, diff, kernel_args);
            reduce_sad(item, scratch, value, kernel_args.sad);
        });
    });
}

} // namespace

void enqueue_sad(sycl::queue &queue, const SadArgs &args)
{
    if (args.bpc <= kInt32VerticalMaxBpc) {
        submit_sad<int32_t>(queue, args);
    } else {
        submit_sad<int64_t>(queue, args);
    }
    /* A plain copy, not a store in the kernel: one more message per pixel
     * there measured no faster on a UHD 770 or an Arc B580. */
    enqueue_copy(queue, args);
}

void enqueue_copy(sycl::queue &queue, const SadArgs &args)
{
    if (args.cur_copy != nullptr) {
        queue.memcpy(args.cur_copy, args.cur, plane_bytes(args));
    }
}

} // namespace motion_sycl_pipeline
