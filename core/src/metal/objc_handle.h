/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Objective-C++ helpers every Metal host translation unit shares: bridging the
 *  `uintptr_t` handle slots of the C structs (kernel_template.h, state_priv.h) to
 *  Metal objects, and loading the one `metallib` the build embeds. Objective-C++
 *  only: the C structs keep their `uintptr_t` slots so C translation units never
 *  see `<Metal/Metal.h>` (ADR-0361 "Header purity").
 *
 *  The slot to pointer conversion is `std::bit_cast`, not an integer to pointer
 *  cast, so clang-tidy's `performance-no-int-to-ptr` and
 *  `bugprone-casting-through-void` have nothing to flag (ADR-1142).
 */

#ifndef LIBVMAF_METAL_OBJC_HANDLE_H_
#define LIBVMAF_METAL_OBJC_HANDLE_H_

#ifndef __cplusplus
#error "objc_handle.h is Objective-C++ only"
#endif

#include <bit>
#include <cstdint>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace vmaf_metal
{

/** The object a slot holds, without taking ownership (ARC keeps the slot's +1). */
template <class T> inline T borrow(uintptr_t slot)
{
    return (__bridge T)std::bit_cast<void *>(slot);
}

/** Moves a +1 reference into a slot; pair it with transfer(). */
inline uintptr_t retain_to_slot(id obj)
{
    if (obj == nil) {
        return 0;
    }
    return std::bit_cast<uintptr_t>((__bridge_retained void *)obj);
}

/** Takes the +1 reference back out of a slot so ARC releases it. */
inline id transfer(uintptr_t slot)
{
    if (slot == 0) {
        return nil;
    }
    return (__bridge_transfer id)std::bit_cast<void *>(slot);
}

} /* namespace vmaf_metal */

/** The embedded `metallib` as an `MTLLibrary`; nil with *rc = -ENODEV / -ENOMEM on failure. */
id<MTLLibrary> vmaf_metal_library_load(id<MTLDevice> device, int *rc);

#endif /* LIBVMAF_METAL_OBJC_HANDLE_H_ */
