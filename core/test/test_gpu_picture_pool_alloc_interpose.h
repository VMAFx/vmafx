/**
 *
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef VMAFX_TEST_GPU_PICTURE_POOL_ALLOC_INTERPOSE_H_
#define VMAFX_TEST_GPU_PICTURE_POOL_ALLOC_INTERPOSE_H_

#ifdef __cplusplus
#include <cstdlib>
#endif
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

void *vmafx_test_gpu_pool_malloc(size_t size);
void vmafx_test_gpu_pool_free(void *ptr);

#ifdef __cplusplus
}
#endif

#ifndef VMAFX_TEST_GPU_PICTURE_POOL_NO_INTERPOSE
#define malloc vmafx_test_gpu_pool_malloc
#define free vmafx_test_gpu_pool_free
#endif

#endif /* VMAFX_TEST_GPU_PICTURE_POOL_ALLOC_INTERPOSE_H_ */
