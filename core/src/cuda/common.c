/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
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

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "common.h"
#include "log.h"

static int is_cudastate_empty(VmafCudaState *cu_state)
{
    if (!cu_state)
        return 1;
    if (!cu_state->ctx)
        return 1;

    return 0;
}

static int init_with_primary_context(VmafCudaState *cu_state)
{
    if (!cu_state)
        return -EINVAL;

    CUdevice cu_device = 0;
    CUcontext cu_context = 0;
    CUresult res = CUDA_SUCCESS;

    const int device_id = 0;
    int n_gpu;
    res |= cu_state->f->cuDeviceGetCount(&n_gpu);
    if (device_id > n_gpu) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "Error: device_id %d is out of range\n", device_id);
        return -EINVAL;
    }

    res |= cu_state->f->cuDeviceGet(&cu_device, device_id);
    res |= cu_state->f->cuDevicePrimaryCtxRetain(&cu_context, cu_device);
    if (res != CUDA_SUCCESS) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "Error: failed to initialize CUDA\n");
        return -EINVAL;
    }

    cu_state->ctx = cu_context;
    cu_state->release_ctx = 1;
    cu_state->dev = cu_device;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent((cu_state->ctx)), fail);
    ctx_pushed = 1;

    int low, high;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxGetStreamPriorityRange(&low, &high), fail);
    // Use highest priority for VMAF compute to preempt lower-priority
    // work (e.g., NVENC/NVDEC) when sharing the GPU
    const int prio = high;
    const int prio2 = MAX(low, MIN(high, prio));
    CHECK_CUDA_GOTO(cu_state->f,
                    cuStreamCreateWithPriority(&cu_state->str, CU_STREAM_NON_BLOCKING, prio2),
                    fail);
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
fail_after_pop:
    /* Netflix#1300 — release the primary context we just retained so
     * vmaf_cuda_state_init's unwind only has to free c + c->f. Without
     * this the driver keeps the primary context alive for the lifetime
     * of the process. */
    (void)cu_state->f->cuDevicePrimaryCtxRelease(cu_state->dev);
    cu_state->ctx = 0;
    cu_state->release_ctx = 0;
    return _cuda_err;
}

static int init_with_provided_context(VmafCudaState *cu_state, CUcontext cu_context)
{
    if (!cu_state)
        return -EINVAL;
    if (!cu_context)
        return -EINVAL;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_context), fail);
    ctx_pushed = 1;

    CUdevice cu_device = 0;
    int err = cu_state->f->cuCtxGetDevice(&cu_device);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "failed to get CUDA device\n");
        _cuda_err = -EINVAL;
        goto fail;
    }

    cu_state->ctx = cu_context;
    cu_state->release_ctx = 0;
    cu_state->dev = cu_device;

    int low, high;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxGetStreamPriorityRange(&low, &high), fail);
    const int prio = 0;
    const int prio2 = MAX(low, MIN(high, prio));
    CHECK_CUDA_GOTO(cu_state->f,
                    cuStreamCreateWithPriority(&cu_state->str, CU_STREAM_NON_BLOCKING, prio2),
                    fail);
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_pop);

    return 0;

fail:
    if (ctx_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

int vmaf_cuda_state_init(VmafCudaState **cu_state, VmafCudaConfiguration cfg)
{
    if (!cu_state)
        return -EINVAL;

    VmafCudaState *const c = *cu_state = malloc(sizeof(*c));
    if (!c)
        return -ENOMEM;
    memset(c, 0, sizeof(*c));

    /* cuda_load_functions dlopens libcuda.so.1 via nv-codec-headers. A
     * failure here is almost always a runtime-env issue, not a bug in
     * libvmaf: the driver stub is either missing, not on the loader
     * path, or shadowed by a stale version. Every downstream kernel
     * launch dereferences c->f, so we must hard-fail with an
     * actionable message before any extractor touches it. */
    int err = cuda_load_functions(&c->f, NULL /* log_ctx */);
    if (err || !c->f) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "CUDA: failed to load the Nvidia driver library.\n"
                 "      libvmaf dlopens libcuda.so.1 at runtime via "
                 "nv-codec-headers; this step failed.\n"
                 "      Check that libcuda.so.1 exists and is on the "
                 "dynamic-loader path:\n"
                 "        ldconfig -p | grep -iE 'libcuda|libnvcuvid'\n"
                 "      The libvmaf_cuda backend cannot run without it. "
                 "See docs/backends/cuda/overview.md#runtime-requirements.\n");
        free(c);
        *cu_state = NULL;
        return -EINVAL;
    }

    err = c->f->cuInit(0);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "CUDA: cuInit(0) failed (err=%d). The driver was "
                 "loaded but initialization failed — typically a "
                 "driver/userspace version mismatch or no CUDA-capable "
                 "device visible to the process.\n",
                 err);
        cuda_free_functions(&c->f);
        free(c);
        *cu_state = NULL;
        return -EINVAL;
    }

    /* Netflix#1300 — if the inner init fails (no visible device,
     * primary-context retain refused, stream create failed, ...) the
     * pre-fix code returned the error code but left c + c->f on the
     * heap. The caller only has **cu_state populated with a half-
     * initialised struct and no way to free it because vmaf_cuda_release
     * refuses (is_cudastate_empty returns true for the no-context
     * case). Unwind here so the error path is allocation-neutral. */
    err = cfg.cu_ctx ? init_with_provided_context(c, cfg.cu_ctx) : init_with_primary_context(c);
    if (err) {
        cuda_free_functions(&c->f);
        free(c);
        *cu_state = NULL;
    }
    return err;
}

int vmaf_cuda_sync(VmafCudaState *cu_state)
{
    if (is_cudastate_empty(cu_state))
        return -EINVAL;

    int _cuda_err = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent((cu_state->ctx)), fail);
    int err = cu_state->f->cuCtxSynchronize();
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail);

    return err;

fail:
    return _cuda_err;
}

int vmaf_cuda_release(VmafCudaState *cu_state)
{
    if (is_cudastate_empty(cu_state))
        return 0;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_state->f, cuStreamDestroy(cu_state->str), fail);
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_pop);

    if (cu_state->release_ctx)
        CHECK_CUDA_GOTO(cu_state->f, cuDevicePrimaryCtxRelease(cu_state->dev), fail_after_pop);

    /* Save the dlopen'd driver function table before the memset so we
     * can release it afterwards. Order matters: zeroing cu_state first
     * guarantees that any caller that reinspects the struct after
     * vmaf_close() sees a NULL f field rather than a pointer to freed
     * memory. Netflix#1300 — the original code leaked the CudaFunctions
     * table (~hundreds of function pointers) on every init/close
     * cycle. */
    CudaFunctions *f = cu_state->f;
    memset((void *)cu_state, 0, sizeof(*cu_state));
    if (f)
        cuda_free_functions(&f);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

int vmaf_cuda_state_free(VmafCudaState *cu_state)
{
    /* NULL-safe like libc free(). Netflix#1300 — vmaf_cuda_state_init()
     * heap-allocates a VmafCudaState that vmaf_cuda_import_state()
     * copies by value into the VmafContext; vmaf_close() only tears
     * down the copy, never the original allocation. Callers must
     * invoke vmaf_cuda_state_free() after vmaf_close() to release the
     * original struct. By this point vmaf_close() has already run
     * vmaf_cuda_release(), which destroys stream + context and memsets
     * the struct to zero, so the only work left here is the free()
     * itself. */
    if (!cu_state)
        return 0;
    free(cu_state);
    return 0;
}

int vmaf_cuda_buffer_alloc(VmafCudaState *cu_state, VmafCudaBuffer **p_buf, size_t size)
{
    if (is_cudastate_empty(cu_state))
        return -EINVAL;
    if (!p_buf)
        return -EINVAL;

    VmafCudaBuffer *buf = (VmafCudaBuffer *)calloc(1, sizeof(*buf));
    if (!buf)
        return -ENOMEM;
    buf->size = size;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_state->f, cuMemAlloc(&buf->data, buf->size), fail);
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_pop);

    *p_buf = buf;
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
fail_after_pop:
    free(buf);
    *p_buf = NULL;
    return _cuda_err;
}

int vmaf_cuda_buffer_free(VmafCudaState *cu_state, VmafCudaBuffer *buf)
{
    if (is_cudastate_empty(cu_state))
        return -EINVAL;
    if (!buf)
        return -EINVAL;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_state->f, cuMemFree(buf->data), fail);
    memset(buf, 0, sizeof(*buf));

    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

int vmaf_cuda_buffer_host_alloc(VmafCudaState *cu_state, void **p_buf, size_t size)
{
    if (is_cudastate_empty(cu_state))
        return -EINVAL;
    if (!p_buf)
        return -EINVAL;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_state->f, cuMemHostAlloc(p_buf, size, 0x01), fail);
    if (!(*p_buf)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "failed to allocate host memory\n");
        _cuda_err = -ENOMEM;
        goto fail;
    }
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

int vmaf_cuda_buffer_host_free(VmafCudaState *cu_state, void *buf)
{
    if (is_cudastate_empty(cu_state))
        return -EINVAL;
    if (!buf)
        return -EINVAL;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_state->f, cuMemFreeHost(buf), fail);
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

int vmaf_cuda_buffer_upload_async(VmafCudaState *cu_state, VmafCudaBuffer *buf, const void *src,
                                  CUstream c_stream)
{
    if (is_cudastate_empty(cu_state))
        return -EINVAL;
    if (!buf)
        return -EINVAL;
    if (!src)
        return -EINVAL;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(
        cu_state->f,
        cuMemcpyHtoDAsync(buf->data, src, buf->size, c_stream != 0 ? c_stream : cu_state->str),
        fail);
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

int vmaf_cuda_buffer_download_async(VmafCudaState *cu_state, VmafCudaBuffer *buf, void *dst,
                                    CUstream c_stream)
{
    if (is_cudastate_empty(cu_state))
        return -EINVAL;
    if (!buf)
        return -EINVAL;
    if (!dst)
        return -EINVAL;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(
        cu_state->f,
        cuMemcpyDtoHAsync(dst, buf->data, buf->size, c_stream != 0 ? c_stream : cu_state->str),
        fail);
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

int vmaf_cuda_buffer_get_dptr(VmafCudaBuffer *buf, CUdeviceptr *ptr)
{
    if (!buf)
        return -EINVAL;
    if (!ptr)
        return -EINVAL;

    *ptr = buf->data;
    return 0;
}
