/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Dropped-dispatch probe for one HIP stream
 *  (T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01 in docs/state.md).
 *
 *  Each "frame" does what a vmafx HIP extractor does: it zeroes a small
 *  device array (hipMemsetAsync), launches PER kernels on the same stream,
 *  each of which writes a unique value into its own slot and adds one to a
 *  counter that is never reset, and copies the slots back into pinned host
 *  memory. The host then checks every slot. A stream that runs every command
 *  in order never reports a wrong slot and ends with the counter at
 *  FRAMES * PER. A command the device never ran shows up twice: its slot keeps
 *  an older value (0 if only the kernel was lost, the value of two frames
 *  earlier if the memset went too; the two frames alternate buffers) and the
 *  counter falls behind for good.
 *
 *  This is not part of the build. On an AMD host:
 *
 *    hipcc -O2 --offload-arch=gfx1036 scripts/dev/hip_dispatch_drop_probe.hip \
 *        -o /tmp/hip_dispatch_drop_probe
 *    /tmp/hip_dispatch_drop_probe 100000 12 0
 *
 *  Arguments: FRAMES (default 10000), PER kernels per frame (default 12),
 *  PIPELINE: 0 waits for each frame before submitting the next, which drains
 *  the stream between frames as libvmaf's collect()-then-submit() does;
 *  1 submits frame N before waiting for frame N - 1. Exit status 0 when no
 *  frame had a wrong slot, 1 otherwise, 3 on a HIP error. Replace gfx1036
 *  with the device's target (rocm_agent_enumerator prints it).
 */

#include <hip/hip_runtime.h>

#include <cstdio>
#include <cstdlib>

namespace
{

constexpr int k_max_per = 64;

struct Frame {
    unsigned long long *dev = nullptr;
    unsigned long long *host = nullptr;
    hipEvent_t done = nullptr;
    unsigned long long first = 0;
};

/* Everything one run owns; frames alternate between fr[0] and fr[1]. */
struct Probe {
    long frames = 10000;
    long per = 12;
    long pipeline = 0;
    size_t bytes = 0;
    hipStream_t str = nullptr;
    Frame fr[2];
    unsigned long long *dev_count = nullptr;
    unsigned long long next_val = 1;
    long bad_frames = 0;
    long bad_slots = 0;
};

void check_hip(hipError_t rc, const char *what)
{
    if (rc != hipSuccess) {
        (void)std::fprintf(stderr, "%s: %s\n", what, hipGetErrorString(rc));
        std::exit(3);
    }
}

} // namespace

extern "C" __global__ void probe_kernel(unsigned long long *out, int slot, unsigned long long val,
                                        unsigned long long *count)
{
    if (blockIdx.x == 0 && threadIdx.x == 0) {
        atomicAdd(count, 1ULL);
        out[slot] = val;
    }
}

namespace
{

/* Parse FRAMES, PER and PIPELINE; false on a value out of range. */
bool parse_args(Probe *p, int argc, char **argv)
{
    p->frames = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 10000;
    p->per = (argc > 2) ? std::strtol(argv[2], nullptr, 10) : 12;
    p->pipeline = (argc > 3) ? std::strtol(argv[3], nullptr, 10) : 0;
    p->bytes = sizeof(unsigned long long) * (size_t)p->per;
    return p->frames >= 1 && p->per >= 1 && p->per <= k_max_per;
}

void setup(Probe *p)
{
    check_hip(hipStreamCreate(&p->str), "hipStreamCreate");
    for (Frame &f : p->fr) {
        check_hip(hipMalloc(&f.dev, p->bytes), "hipMalloc");
        check_hip(hipHostMalloc(reinterpret_cast<void **>(&f.host), p->bytes, hipHostMallocDefault),
                  "hipHostMalloc");
        check_hip(hipEventCreateWithFlags(&f.done, hipEventDisableTiming), "hipEventCreate");
    }
    check_hip(hipMalloc(&p->dev_count, sizeof(unsigned long long)), "hipMalloc");
    check_hip(hipMemset(p->dev_count, 0, sizeof(unsigned long long)), "hipMemset");
}

/* One frame, as a vmafx HIP extractor issues it: memset, PER kernels on the
 * same stream, readback, event. */
void submit(Probe *p, long f)
{
    Frame &x = p->fr[f & 1];
    x.first = p->next_val;
    check_hip(hipMemsetAsync(x.dev, 0, p->bytes, p->str), "hipMemsetAsync");
    for (long k = 0; k < p->per; k++) {
        hipLaunchKernelGGL(probe_kernel, dim3((unsigned)(1 + (k % 5))), dim3(64), 0, p->str, x.dev,
                           (int)k, p->next_val, p->dev_count);
        p->next_val++;
    }
    check_hip(hipMemcpyAsync(x.host, x.dev, p->bytes, hipMemcpyDeviceToHost, p->str),
              "hipMemcpyAsync");
    check_hip(hipEventRecord(x.done, p->str), "hipEventRecord");
}

/* Wait for frame f and compare every slot with the value its kernel was given;
 * print the first ten bad frames slot by slot. */
void check(Probe *p, long f)
{
    Frame &x = p->fr[f & 1];
    check_hip(hipEventSynchronize(x.done), "hipEventSynchronize");
    long bad = 0;
    for (long k = 0; k < p->per; k++)
        bad += (x.host[k] != x.first + (unsigned long long)k) ? 1 : 0;
    if (bad == 0)
        return;
    p->bad_frames++;
    p->bad_slots += bad;
    if (p->bad_frames > 10)
        return;
    (void)std::printf("frame %ld: %ld wrong slots, slot value minus expected:", f, bad);
    for (long k = 0; k < p->per; k++)
        (void)std::printf(" %lld", (long long)(x.host[k] - (x.first + (unsigned long long)k)));
    (void)std::printf("\n");
}

} // namespace

int main(int argc, char **argv)
{
    Probe p;
    if (!parse_args(&p, argc, argv)) {
        (void)std::fprintf(stderr, "usage: %s [FRAMES>=1] [PER 1..%d] [PIPELINE 0|1]\n", argv[0],
                           k_max_per);
        return 2;
    }
    setup(&p);
    for (long f = 0; f < p.frames; f++) {
        submit(&p, f);
        if (p.pipeline == 0)
            check(&p, f);
        else if (f > 0)
            check(&p, f - 1);
    }
    if (p.pipeline != 0)
        check(&p, p.frames - 1);
    unsigned long long count = 0;
    check_hip(hipMemcpy(&count, p.dev_count, sizeof(count), hipMemcpyDeviceToHost), "hipMemcpy");
    const unsigned long long expect = (unsigned long long)p.frames * (unsigned long long)p.per;
    (void)std::printf("pipeline=%ld frames=%ld per=%ld bad_frames=%ld bad_slots=%ld "
                      "dispatches=%llu/%llu lost=%llu\n",
                      p.pipeline, p.frames, p.per, p.bad_frames, p.bad_slots, count, expect,
                      expect - count);
    return (p.bad_frames != 0 || count != expect) ? 1 : 0;
}
