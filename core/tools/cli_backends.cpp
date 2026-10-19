/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * `vmaf --list-backends` (ADR-1874): which scoring backends this binary was
 * built with, and which of them initialise on this host. "Usable" is decided
 * by the backend's own state initialiser on its default device, the call a
 * scoring run makes, not by vendor tools or the help text.
 */

#include "cli_backends.h"

#include <array>
#include <cerrno>
#include <cstdio>

#ifdef HAVE_CUDA
#include "libvmaf/libvmaf_cuda.h"
#endif
#ifdef HAVE_SYCL
#include "libvmaf/libvmaf_sycl.h"
#endif
#ifdef HAVE_HIP
#include "libvmaf/libvmaf_hip.h"
#endif
#ifdef HAVE_METAL
#include "libvmaf/libvmaf_metal.h"
#endif

namespace
{

/* Initialise and release one backend's state; 0 when it initialised. */
using InitStatusFn = int (*)();

int cpu_init_status()
{
    return 0;
}

#ifdef HAVE_CUDA
int cuda_init_status()
{
    VmafCudaState *state = nullptr;
    const VmafCudaConfiguration cfg = {nullptr};
    const int err = vmaf_cuda_state_init(&state, cfg);
    if (err)
        return err;
    return vmaf_cuda_state_free(state);
}
constexpr InitStatusFn kCudaInit = cuda_init_status;
#else
constexpr InitStatusFn kCudaInit = nullptr;
#endif

#ifdef HAVE_SYCL
int sycl_init_status()
{
    VmafSyclState *state = nullptr;
    const VmafSyclConfiguration cfg = {.device_index = 0, .enable_profiling = 0};
    const int err = vmaf_sycl_state_init(&state, cfg);
    if (err)
        return err;
    vmaf_sycl_state_free(&state);
    return 0;
}
constexpr InitStatusFn kSyclInit = sycl_init_status;
#else
constexpr InitStatusFn kSyclInit = nullptr;
#endif

#ifdef HAVE_HIP
int hip_init_status()
{
    VmafHipState *state = nullptr;
    const VmafHipConfiguration cfg = {.device_index = 0, .flags = 0};
    const int err = vmaf_hip_state_init(&state, cfg);
    if (err)
        return err;
    vmaf_hip_state_free(&state);
    return 0;
}
constexpr InitStatusFn kHipInit = hip_init_status;
#else
constexpr InitStatusFn kHipInit = nullptr;
#endif

#ifdef HAVE_METAL
int metal_init_status()
{
    VmafMetalState *state = nullptr;
    const VmafMetalConfiguration cfg = {.device_index = 0, .flags = 0};
    const int err = vmaf_metal_state_init(&state, cfg);
    if (err)
        return err;
    vmaf_metal_state_free(&state);
    return 0;
}
constexpr InitStatusFn kMetalInit = metal_init_status;
#else
constexpr InitStatusFn kMetalInit = nullptr;
#endif

/* A backend the CLI knows; `init` is nullptr when it is not compiled in. */
struct CliBackend {
    const char *name;
    InitStatusFn init;
};

/* The order of the report; the names `--backend` accepts. */
constexpr std::array<CliBackend, 5> kBackends = {{
    {.name = "cpu", .init = cpu_init_status},
    {.name = "cuda", .init = kCudaInit},
    {.name = "sycl", .init = kSyclInit},
    {.name = "hip", .init = kHipInit},
    {.name = "metal", .init = kMetalInit},
}};

/* One backend's line of the report: 0, or -EIO when a write fails. */
int write_backend(FILE *out, const CliBackend &backend, bool last)
{
    const bool compiled = backend.init != nullptr;
    const int status = compiled ? backend.init() : -ENOSYS;
    if (fprintf(out, R"(    {"name": "%s", "compiled": %s, "usable": %s)", backend.name,
                compiled ? "true" : "false", status == 0 ? "true" : "false") < 0)
        return -EIO;
    if (compiled && status != 0 && fprintf(out, R"(, "init_status": %d)", status) < 0)
        return -EIO;
    if (fprintf(out, "}%s\n", last ? "" : ",") < 0)
        return -EIO;
    return 0;
}

} // namespace

extern "C" int cli_list_backends(FILE *out)
{
    if (!out)
        return -EINVAL;
    if (fputs("{\n  \"backends\": [\n", out) < 0)
        return -EIO;
    for (size_t i = 0; i < kBackends.size(); i++) {
        const int err = write_backend(out, kBackends[i], i + 1 == kBackends.size());
        if (err)
            return err;
    }
    if (fputs("  ]\n}\n", out) < 0 || fflush(out) != 0)
        return -EIO;
    return 0;
}
