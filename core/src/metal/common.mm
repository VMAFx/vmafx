/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal backend common surface — runtime implementation (T8-1b /
 *  ADR-0420). Replaces the C scaffold from T8-1 (ADR-0361) with real
 *  Objective-C++ message sends against `<Metal/Metal.h>`.
 *
 *  Memory model: this TU compiles with `-fobjc-arc` so id<MTLDevice> /
 *  id<MTLCommandQueue> handles are auto-retained on assignment and
 *  released on struct teardown. We bridge between Objective-C
 *  references and C `void *` via `(__bridge_retained void *)` (id →
 *  void *, +1 retain) and `(__bridge_transfer id<...>)` (void * → id,
 *  -1 retain). The C struct stores `void *` slots so the .h header
 *  stays free of `<Metal/Metal.h>` — same header-purity contract the
 *  scaffold pins in `common.h` and the public `libvmaf_metal.h`
 *  (ADR-0361 §"Header purity").
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" {
#include "common.h"
#include "state_priv.h"
#include "import.h"
#include "libvmaf/libvmaf_metal.h"
}

/* Struct layouts for VmafMetalContext + VmafMetalState live in
 * state_priv.h so the IOSurface import TU (picture_import.mm) can
 * construct + tear down states without going through a constructor
 * thunk. Header purity is preserved: state_priv.h carries no
 * Objective-C types, only `void *` slots that bridge-retain the
 * id<MTL...> handles. */

/*
 * Pick an MTLDevice by index. -1 ⇒ system default
 * (`MTLCreateSystemDefaultDevice`, picks the integrated Apple-Family-7+
 * GPU on Apple Silicon). 0..N-1 ⇒ the index into the *filtered Apple7+
 * subset* of `MTLCopyAllDevices` (macOS only; iOS/tvOS only have the
 * system default). This is the same numbering space that
 * `vmaf_metal_device_count()` counts and `vmaf_metal_list_devices()`
 * prints, so `--metal-device N` selects exactly the device shown as
 * `[N]` in the device list.
 *
 * Apple-Family-7 gate per ADR-0361: M1 and later support compute
 * shaders + nontrivial threadgroup memory. Pre-M1 devices (Intel
 * Macs with discrete or integrated GPUs, A-series < A14) fail the
 * `supportsFamily:` check and surface as -ENODEV. The fork's NEON
 * path remains the production fallback for those hosts.
 */
static id<MTLDevice> select_device_or_nil(int device_index)
{
    if (device_index == -1) {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev == nil) {
            return nil;
        }
        if (![dev supportsFamily:MTLGPUFamilyApple7]) {
            return nil;
        }
        return dev;
    }

#if defined(MAC_OS_VERSION_11_0) || (defined(MAC_OS_X_VERSION_10_11) && !TARGET_OS_IPHONE)
    if (device_index < 0) {
        return nil;
    }
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    if (all == nil) {
        return nil;
    }
    /* Index into the *filtered Apple7+ subset*, not the raw
     * MTLCopyAllDevices() array. vmaf_metal_device_count() returns the
     * count of Apple7+ devices and vmaf_metal_list_devices() prints them
     * with a re-based [0..N-1] index, so a raw-array index here would
     * select the wrong device (or reject a valid one) whenever a non-
     * Apple7 device precedes an Apple7+ one in enumeration order. */
    NSUInteger family7_index = 0;
    for (NSUInteger i = 0; i < all.count; i++) {
        id<MTLDevice> dev = all[i];
        if (![dev supportsFamily:MTLGPUFamilyApple7]) {
            continue;
        }
        if (family7_index == (NSUInteger)device_index) {
            return dev;
        }
        family7_index++;
    }
    return nil;
#else
    (void)device_index;
    return nil;
#endif
}

/* ---- Internal C++ entry points (common.h) ---- */

int vmaf_metal_context_new(VmafMetalContext **out, int device_index)
{
    if (out == NULL) {
        return -EINVAL;
    }

    id<MTLDevice> device = select_device_or_nil(device_index);
    if (device == nil) {
        return -ENODEV;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) {
        return -ENOMEM;
    }

    VmafMetalContext *ctx = (VmafMetalContext *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -ENOMEM;
    }
    ctx->device_index = device_index;
    ctx->device        = (__bridge_retained void *)device;
    ctx->command_queue = (__bridge_retained void *)queue;

    *out = ctx;
    return 0;
}

void vmaf_metal_context_destroy(VmafMetalContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    /* Bridge-transfer back to ARC ownership so the autorelease pool
     * + ARC release on scope exit drop the +1 retain we took in _new. */
    if (ctx->command_queue != NULL) {
        id<MTLCommandQueue> q __attribute__((unused)) =
            (__bridge_transfer id<MTLCommandQueue>)ctx->command_queue;
        ctx->command_queue = NULL;
    }
    if (ctx->device != NULL) {
        id<MTLDevice> d __attribute__((unused)) =
            (__bridge_transfer id<MTLDevice>)ctx->device;
        ctx->device = NULL;
    }
    free(ctx);
}

void *vmaf_metal_context_device_handle(VmafMetalContext *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }
    return ctx->device;
}

void *vmaf_metal_context_queue_handle(VmafMetalContext *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }
    return ctx->command_queue;
}

int vmaf_metal_device_count(void)
{
#if !TARGET_OS_IPHONE
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    NSUInteger n = (all == nil) ? 0 : all.count;
    NSUInteger family7 = 0;
    for (NSUInteger i = 0; i < n; i++) {
        if ([all[i] supportsFamily:MTLGPUFamilyApple7]) {
            family7++;
        }
    }
    return (int)family7;
#else
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev != nil && [dev supportsFamily:MTLGPUFamilyApple7]) {
        return 1;
    }
    return 0;
#endif
}

/* ---- Public C-API entry points (libvmaf_metal.h) ---- */

int vmaf_metal_available(void)
{
    return 1;
}

int vmaf_metal_state_init(VmafMetalState **out, VmafMetalConfiguration cfg)
{
    if (out == NULL) {
        return -EINVAL;
    }
    if (cfg.flags != 0) {
        return -EINVAL;
    }

    VmafMetalState *state = (VmafMetalState *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return -ENOMEM;
    }

    id<MTLDevice> device = select_device_or_nil(cfg.device_index);
    if (device == nil) {
        free(state);
        return -ENODEV;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) {
        free(state);
        return -ENOMEM;
    }

    state->ctx.device_index  = cfg.device_index;
    state->ctx.device        = (__bridge_retained void *)device;
    state->ctx.command_queue = (__bridge_retained void *)queue;

    *out = state;
    return 0;
}

/* vmaf_metal_import_state() lives in libvmaf.c under HAVE_METAL —
 * the dispatcher hand-off needs VmafContext layout (mirrors
 * vmaf_vulkan_import_state). T8-IOS (ADR-0423) flipped this from a
 * common.mm no-op to a real VmafContext->metal.state setter. */

void vmaf_metal_state_free(VmafMetalState **state)
{
    if (state == NULL || *state == NULL) {
        return;
    }
    VmafMetalState *s = *state;
    /* T8-IOS (ADR-0423): release the IOSurface import ring before
     * the device/queue handles — the ring may hold VmafPicture
     * buffers we own. No-op if no import was ever started. */
    vmaf_metal_state_import_ring_free(s);
    if (s->ctx.command_queue != NULL) {
        id<MTLCommandQueue> q __attribute__((unused)) =
            (__bridge_transfer id<MTLCommandQueue>)s->ctx.command_queue;
        s->ctx.command_queue = NULL;
    }
    if (s->ctx.device != NULL) {
        id<MTLDevice> d __attribute__((unused)) =
            (__bridge_transfer id<MTLDevice>)s->ctx.device;
        s->ctx.device = NULL;
    }
    free(s);
    *state = NULL;
}

int vmaf_metal_list_devices(void)
{
#if !TARGET_OS_IPHONE
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    NSUInteger n = (all == nil) ? 0 : all.count;
    int printed = 0;
    for (NSUInteger i = 0; i < n; i++) {
        id<MTLDevice> dev = all[i];
        if (![dev supportsFamily:MTLGPUFamilyApple7]) {
            continue;
        }
        const char *name = dev.name.UTF8String ? dev.name.UTF8String : "(unknown)";
        const char *family = "Apple7";
        if ([dev supportsFamily:MTLGPUFamilyApple9]) {
            family = "Apple9";
        } else if ([dev supportsFamily:MTLGPUFamilyApple8]) {
            family = "Apple8";
        }
        fprintf(stdout, "  [%d] %s (GPU family: %s)\n", printed, name, family);
        printed++;
    }
    return printed;
#else
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev != nil && [dev supportsFamily:MTLGPUFamilyApple7]) {
        const char *name = dev.name.UTF8String ? dev.name.UTF8String : "(unknown)";
        fprintf(stdout, "  [0] %s (GPU family: Apple7+)\n", name);
        return 1;
    }
    return 0;
#endif
}
