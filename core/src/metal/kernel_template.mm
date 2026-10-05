/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Metal kernel-template helper bodies — runtime implementation
 *  (T8-1b / ADR-0420). Replaces the C scaffold's -ENOSYS stubs with
 *  real `[id<MTLDevice> newCommandQueue]` +
 *  `[id<MTLDevice> newSharedEvent]` +
 *  `[id<MTLBlitCommandEncoder> fillBuffer:range:value:]` +
 *  `[id<MTLCommandBuffer> waitUntilCompleted]` plumbing.
 *
 *  Handle ABI: the public-facing `kernel_template.h` carries
 *  `uintptr_t` slots so the header stays free of `<Metal/Metal.h>`.
 *  This TU bridges `uintptr_t` ↔ id<MTL...> via __bridge_retained /
 *  __bridge_transfer; the C struct owns +1 retains for the duration
 *  of init→close. Mirrors the MetalCpp pattern documented in
 *  ADR-0361 §"Header purity".
 */

#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "objc_handle.h"

extern "C" {
#include "../kernel_lifecycle_common.h"
#include "common.h"
#include "kernel_template.h"
}

/* The struct stores `uintptr_t` slots; objc_handle.h bridges them. */
using vmaf_metal::borrow;
using vmaf_metal::retain_to_slot;
using vmaf_metal::transfer;

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

id<MTLLibrary> vmaf_metal_library_load(id<MTLDevice> device, int *rc)
{
    const uintptr_t start = std::bit_cast<uintptr_t>(&libvmaf_metallib_start[0]);
    const uintptr_t blob_size = std::bit_cast<uintptr_t>(&libvmaf_metallib_end[0]) - start;
    if (blob_size == 0) {
        *rc = -ENODEV;
        return nil;
    }
    dispatch_data_t const data = dispatch_data_create(
        libvmaf_metallib_start, blob_size, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (data == nullptr) {
        *rc = -ENOMEM;
        return nil;
    }
    NSError *err = nil;
    id<MTLLibrary> const lib = [device newLibraryWithData:data error:&err];
    if (lib == nil) {
        *rc = -ENODEV;
    }
    return lib;
}

int vmaf_metal_kernel_lifecycle_init(VmafMetalKernelLifecycle *lc, VmafMetalContext *ctx)
{
    if (lc == nullptr) {
        return -EINVAL;
    }
    VMAF_LIFECYCLE_ZERO(lc);

    if (ctx == nullptr) {
        return -EINVAL;
    }

    void *const device_handle = vmaf_metal_context_device_handle(ctx);
    if (device_handle == nullptr) {
        return -ENODEV;
    }
    id<MTLDevice> const device = (__bridge id<MTLDevice>)device_handle;

    id<MTLCommandQueue> const queue = [device newCommandQueue];
    if (queue == nil) {
        return -ENOMEM;
    }
    id<MTLSharedEvent> const submit_ev = [device newSharedEvent];
    if (submit_ev == nil) {
        return -ENOMEM;
    }
    id<MTLSharedEvent> const finished_ev = [device newSharedEvent];
    if (finished_ev == nil) {
        return -ENOMEM;
    }

    lc->cmd_queue = retain_to_slot(queue);
    lc->submit    = retain_to_slot(submit_ev);
    lc->finished  = retain_to_slot(finished_ev);

    return 0;
}

int vmaf_metal_kernel_buffer_alloc(VmafMetalKernelBuffer *buf, VmafMetalContext *ctx, size_t bytes)
{
    if (buf == nullptr) {
        return -EINVAL;
    }
    VMAF_LIFECYCLE_ZERO(buf);
    buf->bytes = bytes;

    if (ctx == nullptr) {
        return -EINVAL;
    }
    if (bytes == 0) {
        return -EINVAL;
    }

    void *const device_handle = vmaf_metal_context_device_handle(ctx);
    if (device_handle == nullptr) {
        return -ENODEV;
    }
    id<MTLDevice> const device = (__bridge id<MTLDevice>)device_handle;

    id<MTLBuffer> const b = [device newBufferWithLength:bytes
                                          options:MTLResourceStorageModeShared];
    if (b == nil) {
        return -ENOMEM;
    }
    buf->buffer    = retain_to_slot(b);
    buf->host_view = [b contents];
    return 0;
}

int vmaf_metal_kernel_submit_pre_launch(VmafMetalKernelLifecycle *lc, VmafMetalContext *ctx,
                                        VmafMetalKernelBuffer *buf,
                                        uintptr_t picture_command_buffer,
                                        uintptr_t dist_ready_event)
{
    (void)ctx;
    if (lc == nullptr || buf == nullptr) {
        return -EINVAL;
    }
    if (lc->cmd_queue == 0 || buf->buffer == 0) {
        return -EINVAL;
    }

    id<MTLCommandQueue> const queue = borrow<id<MTLCommandQueue>>(lc->cmd_queue);
    id<MTLBuffer> const accum       = borrow<id<MTLBuffer>>(buf->buffer);

    id<MTLCommandBuffer> const cmd = [queue commandBuffer];
    if (cmd == nil) {
        return -ENOMEM;
    }

    /* Step 1: zero the accumulator on our private command queue. */
    id<MTLBlitCommandEncoder> const blit = [cmd blitCommandEncoder];
    if (blit == nil) {
        return -ENOMEM;
    }
    [blit fillBuffer:accum range:NSMakeRange(0, buf->bytes) value:0];
    [blit endEncoding];

    /* Step 2: wait for the dist-side ready event on the picture's
     * command buffer (caller-provided, so the picture work and our
     * pre-launch are causally ordered). When either handle is 0 we
     * skip the cross-queue wait — this is the path during single-
     * buffer testing where there's no producer queue to sync with. */
    if (picture_command_buffer != 0 && dist_ready_event != 0) {
        id<MTLCommandBuffer> const pic_cmd =
            borrow<id<MTLCommandBuffer>>(picture_command_buffer);
        id<MTLEvent> const evt = borrow<id<MTLEvent>>(dist_ready_event);
        [pic_cmd encodeWaitForEvent:evt value:1];
    }

    [cmd commit];
    return 0;
}

int vmaf_metal_kernel_collect_wait(VmafMetalKernelLifecycle *lc, VmafMetalContext *ctx)
{
    (void)ctx;
    if (lc == nullptr) {
        return -EINVAL;
    }
    if (lc->cmd_queue == 0) {
        return -EINVAL;
    }

    /* Drain the private command queue. The simplest correct sequence
     * commits a no-op blit + waits on its command buffer; this fences
     * everything previously enqueued, which is what consumers expect
     * after submit() before reading [contents] in collect(). */
    id<MTLCommandQueue> const queue = borrow<id<MTLCommandQueue>>(lc->cmd_queue);
    id<MTLCommandBuffer> const fence = [queue commandBuffer];
    if (fence == nil) {
        return -ENOMEM;
    }
    [fence commit];
    [fence waitUntilCompleted];
    return 0;
}

int vmaf_metal_kernel_lifecycle_close(VmafMetalKernelLifecycle *lc, VmafMetalContext *ctx)
{
    (void)ctx;
    if (lc == nullptr) {
        return 0;
    }

    /* Best-effort drain before release so any in-flight work finishes. */
    if (lc->cmd_queue != 0) {
        id<MTLCommandQueue> const queue = borrow<id<MTLCommandQueue>>(lc->cmd_queue);
        id<MTLCommandBuffer> const fence = [queue commandBuffer];
        if (fence != nil) {
            [fence commit];
            [fence waitUntilCompleted];
        }
    }

    /* Bridge-transfer each slot back to ARC ownership so the +1
     * retains we took in init() are released by scope exit. */
    if (lc->finished != 0) {
        id const ev __attribute__((unused)) = transfer(lc->finished);
        lc->finished = 0;
    }
    if (lc->submit != 0) {
        id const ev __attribute__((unused)) = transfer(lc->submit);
        lc->submit = 0;
    }
    if (lc->cmd_queue != 0) {
        id const q __attribute__((unused)) = transfer(lc->cmd_queue);
        lc->cmd_queue = 0;
    }
    return 0;
}

int vmaf_metal_kernel_buffer_free(VmafMetalKernelBuffer *buf, VmafMetalContext *ctx)
{
    (void)ctx;
    if (buf == nullptr) {
        return 0;
    }
    if (buf->buffer != 0) {
        id const b __attribute__((unused)) = transfer(buf->buffer);
        buf->buffer = 0;
    }
    buf->host_view = nullptr;
    buf->bytes     = 0;
    return 0;
}
