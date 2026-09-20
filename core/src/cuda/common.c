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
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "log.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

static int is_cudastate_empty(const VmafCudaState *cu_state)
{
    if (!cu_state)
        return 1;
    if (!cu_state->ctx)
        return 1;

    return 0;
}

bool vmaf_cuda_arch_supported(int major, int minor)
{
    if (major != VMAF_CUDA_MIN_COMPUTE_MAJOR)
        return major > VMAF_CUDA_MIN_COMPUTE_MAJOR;
    return minor >= VMAF_CUDA_MIN_COMPUTE_MINOR;
}

/* Reject a device below the ADR-1223 compute-capability floor with an
 * actionable message. Returns 0 when the device is supported, -ENOTSUP when it
 * is not, and -EINVAL when the capability could not be queried at all. */
static int check_device_arch(VmafCudaState *cu_state, CUdevice dev)
{
    int major = 0;
    int minor = 0;
    CUresult res = cu_state->f->cuDeviceGetAttribute(
        &major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
    res |= cu_state->f->cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
                                             dev);
    if (res != CUDA_SUCCESS) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "CUDA: could not query the device compute capability.\n");
        return -EINVAL;
    }

    if (vmaf_cuda_arch_supported(major, minor))
        return 0;

    char name[128] = {0};
    if (cu_state->f->cuDeviceGetName(name, (int)sizeof(name) - 1, dev) != CUDA_SUCCESS)
        (void)snprintf(name, sizeof(name), "<unknown>");

    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "CUDA: device \"%s\" has compute capability %d.%d, below the "
             "minimum %d.%d (Ampere).\n"
             "      libvmaf ships no cubin or PTX below sm_%d%d, so no kernel "
             "can be loaded on this GPU.\n"
             "      Turing (sm_75) and older were dropped in ADR-1223. Use an "
             "Ampere or newer GPU, or build with -Denable_cuda=false and run "
             "on the CPU backend.\n",
             name, major, minor, VMAF_CUDA_MIN_COMPUTE_MAJOR, VMAF_CUDA_MIN_COMPUTE_MINOR,
             VMAF_CUDA_MIN_COMPUTE_MAJOR, VMAF_CUDA_MIN_COMPUTE_MINOR);
    return -ENOTSUP;
}

static int cleanup_context_init(VmafCudaState *cu_state, int err, bool context_pushed,
                                bool release_primary)
{
    if (context_pushed)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
    if (cu_state->str)
        (void)cu_state->f->cuStreamDestroy(cu_state->str);
    cu_state->str = 0;
    if (release_primary)
        (void)cu_state->f->cuDevicePrimaryCtxRelease(cu_state->dev);
    cu_state->ctx = 0;
    cu_state->release_ctx = 0;
    return err;
}

static int get_context_device(VmafCudaState *cu_state, CUdevice *cu_device)
{
    const CUresult res = cu_state->f->cuCtxGetDevice(cu_device);
    if (res != CUDA_SUCCESS) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "failed to get CUDA device\n");
        return -EINVAL;
    }
    return check_device_arch(cu_state, *cu_device);
}

static int init_context_stream(VmafCudaState *cu_state, CUcontext cu_context, CUdevice cu_device,
                               bool device_known, bool release_primary, bool high_priority)
{
    int _cuda_err;
    int ctx_pushed = 0;
    cu_state->ctx = cu_context;
    cu_state->release_ctx = release_primary;
    if (device_known)
        cu_state->dev = cu_device;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_context), fail);
    ctx_pushed = 1;

    if (!device_known) {
        _cuda_err = get_context_device(cu_state, &cu_device);
        if (_cuda_err)
            return cleanup_context_init(cu_state, _cuda_err, true, release_primary);
        cu_state->dev = cu_device;
    }

    int low;
    int high;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxGetStreamPriorityRange(&low, &high), fail);
    /* Primary-context work gets the highest available priority so it can
     * preempt lower-priority NVENC/NVDEC work on a shared GPU. */
    const int priority = high_priority ? high : 0;
    const int bounded_priority = VMAF_CUDA_MAX(low, VMAF_CUDA_MIN(high, priority));
    CHECK_CUDA_GOTO(
        cu_state->f,
        cuStreamCreateWithPriority(&cu_state->str, CU_STREAM_NON_BLOCKING, bounded_priority), fail);
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPopCurrent(NULL), fail_after_stream);
    return 0;

fail:
    return cleanup_context_init(cu_state, _cuda_err, ctx_pushed != 0, release_primary);
fail_after_stream:
    return cleanup_context_init(cu_state, _cuda_err, false, release_primary);
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
    if (res != CUDA_SUCCESS) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "Error: failed to initialize CUDA\n");
        return -EINVAL;
    }

    /* ADR-1223 — check before retaining a primary context, so an unsupported
     * device costs nothing to unwind. */
    const int arch_err = check_device_arch(cu_state, cu_device);
    if (arch_err)
        return arch_err;

    res |= cu_state->f->cuDevicePrimaryCtxRetain(&cu_context, cu_device);
    if (res != CUDA_SUCCESS) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "Error: failed to initialize CUDA\n");
        return -EINVAL;
    }

    return init_context_stream(cu_state, cu_context, cu_device, true, true, true);
}

static int init_with_provided_context(VmafCudaState *cu_state, CUcontext cu_context)
{
    if (!cu_state)
        return -EINVAL;
    if (!cu_context)
        return -EINVAL;

    return init_context_stream(cu_state, cu_context, 0, false, false, false);
}

static int init_cuda_driver(VmafCudaState *cu_state)
{
    /* cuda_load_functions dlopens libcuda.so.1 via nv-codec-headers. A
     * failure here is almost always a runtime-env issue, not a bug in
     * libvmaf: the driver stub is either missing, not on the loader
     * path, or shadowed by a stale version. Every downstream kernel
     * launch dereferences c->f, so we must hard-fail with an
     * actionable message before any extractor touches it. */
    int err = cuda_load_functions(&cu_state->f, NULL /* log_ctx */);
    if (err || !cu_state->f) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "CUDA: failed to load the Nvidia driver library.\n"
                 "      libvmaf dlopens libcuda.so.1 at runtime via "
                 "nv-codec-headers; this step failed.\n"
                 "      Check that libcuda.so.1 exists and is on the "
                 "dynamic-loader path:\n"
                 "        ldconfig -p | grep -iE 'libcuda|libnvcuvid'\n"
                 "      The libvmaf_cuda backend cannot run without it. "
                 "See docs/backends/cuda/overview.md#runtime-requirements.\n");
        return -ENOSYS;
    }

    err = cu_state->f->cuInit(0);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "CUDA: cuInit(0) failed (err=%d). The driver was "
                 "loaded but initialization failed — typically a "
                 "driver/userspace version mismatch or no CUDA-capable "
                 "device visible to the process.\n",
                 err);
        cuda_free_functions(&cu_state->f);
        return -ENODEV;
    }
    return 0;
}

int vmaf_cuda_state_init(VmafCudaState **cu_state, VmafCudaConfiguration cfg)
{
    if (!cu_state)
        return -EINVAL;

    VmafCudaState *const c = *cu_state = malloc(sizeof(*c));
    if (!c)
        return -ENOMEM;
    memset(c, 0, sizeof(*c));

    int err = init_cuda_driver(c);
    if (err) {
        free(c);
        *cu_state = NULL;
        return err;
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

int vmaf_cuda_release(VmafCudaState *cu_state)
{
    if (is_cudastate_empty(cu_state))
        return 0;

    int _cuda_err;
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

    int _cuda_err;
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

    int _cuda_err;
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

    int _cuda_err;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_state->f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_state->f, cuMemHostAlloc(p_buf, size, 0x01), fail);
    if (!(*p_buf)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "failed to allocate host memory\n");
        (void)cu_state->f->cuCtxPopCurrent(NULL);
        return -ENOMEM;
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

    int _cuda_err;
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

int vmaf_cuda_buffer_get_dptr(const VmafCudaBuffer *buf, CUdeviceptr *ptr)
{
    if (!buf)
        return -EINVAL;
    if (!ptr)
        return -EINVAL;

    *ptr = buf->data;
    return 0;
}

/* NOLINTEND(modernize-use-nullptr) */
