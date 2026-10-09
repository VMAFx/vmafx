/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The producer side of the VMAFx SYCL lane tests (RC4 WP3, ADR-2091); see
 * vmafx_sycl_producer.h. Test code: a frame's producer (uploads with row
 * padding at any offset, a held queue, events), a dma-buf exporter and a
 * device load, nothing the library uses.
 */

#include <sycl/sycl.hpp>
#include <sycl/backend.hpp>
#include <level_zero/ze_api.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "vmafx_sycl_producer.h"

/* `last` is the event of the queue's last command, kept from each submit:
 * sycl::queue::ext_oneapi_get_last_event() waited for that command to
 * complete (DPC++ 2026.0 and 2026.1.1, measured), which would hold the test's
 * thread. */
struct VsProducer {
    sycl::queue q;
    std::mutex lock;
    std::set<void *> exported; /* Level Zero allocations (zeMemFree) */
    uint32_t *sink;            /* the device hold's result */
    std::optional<sycl::event> last;
};

struct VsLoad {
    sycl::queue q;
    std::atomic<bool> stop;
    std::thread worker;
    uint32_t *sink;
};

namespace
{

std::optional<sycl::device> first_level_zero_gpu()
{
    for (const sycl::platform &p : sycl::platform::get_platforms()) {
        if (p.get_backend() != sycl::backend::ext_oneapi_level_zero) {
            continue;
        }
        for (const sycl::device &d : p.get_devices(sycl::info::device_type::gpu)) {
            return d;
        }
    }
    return std::nullopt;
}

int failed(const char *what, const std::exception &e)
{
    (void)std::fprintf(stderr, "vs_producer: %s: %s\n", what, e.what());
    return -1;
}

/* One busy kernel: a long integer recurrence per work-item, no private
 * array. */
void busy_kernel(sycl::queue &q, uint32_t *sink)
{
    q.parallel_for(sycl::range<1>(1u << 16), [=](sycl::id<1> id) {
        uint32_t x = (uint32_t)id[0];
        for (unsigned k = 0; k < 4096u; k++) {
            x = x * 1664525u + 1013904223u;
        }
        sink[id[0]] = x;
    });
}

/* The load thread: busy kernels, `in_flight` at most queued, until stopped. */
void load_worker(VsLoad *load, unsigned in_flight)
{
    unsigned queued = 0;
    while (!load->stop.load()) {
        busy_kernel(load->q, load->sink);
        if (++queued >= in_flight) {
            load->q.wait();
            queued = 0;
        }
    }
}

/* The sycl::event behind a handle of this file or of the library: an event
 * crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
sycl::event *event_of(uintptr_t handle)
{
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): a sycl::event crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
    return reinterpret_cast<sycl::event *>(handle);
}

} // namespace

extern "C" VsProducer *vs_producer_open(void)
{
    try {
        const std::optional<sycl::device> dev = first_level_zero_gpu();
        if (!dev) {
            return nullptr;
        }
        return new VsProducer{.q = sycl::queue(*dev, sycl::property::queue::in_order{})};
    } catch (const std::exception &e) {
        (void)failed("open", e);
        return nullptr;
    }
}

extern "C" void vs_producer_close(VsProducer *p)
{
    if (!p) {
        return;
    }
    try {
        p->q.wait_and_throw();
        if (p->sink) {
            sycl::free(p->sink, p->q);
        }

    } catch (const std::exception &e) {
        (void)failed("close", e);
    }
    delete p;
}

extern "C" uintptr_t vs_producer_queue(VsProducer *p)
{
    return reinterpret_cast<uintptr_t>(&p->q);
}

extern "C" void *vs_alloc(VsProducer *p, size_t bytes)
{
    try {
        return sycl::malloc_device(bytes, p->q);
    } catch (const std::exception &e) {
        (void)failed("alloc", e);
        return nullptr;
    }
}

extern "C" void *vs_alloc_shared(VsProducer *p, size_t bytes)
{
    try {
        return sycl::malloc_shared(bytes, p->q);
    } catch (const std::exception &e) {
        (void)failed("alloc shared", e);
        return nullptr;
    }
}

extern "C" void *vs_alloc_host(VsProducer *p, size_t bytes)
{
    try {
        return sycl::malloc_host(bytes, p->q);
    } catch (const std::exception &e) {
        (void)failed("alloc host", e);
        return nullptr;
    }
}

extern "C" void *vs_alloc_dmabuf(VsProducer *p, size_t bytes, int *fd, uint64_t *size)
{
    *fd = -1;
    *size = 0;
    try {
        auto ctx = sycl::get_native<sycl::backend::ext_oneapi_level_zero>(p->q.get_context());
        auto dev = sycl::get_native<sycl::backend::ext_oneapi_level_zero>(p->q.get_device());
        const ze_external_memory_export_desc_t export_desc = {
            .stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_DESC,
            .pNext = nullptr,
            .flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF,
        };
        const ze_device_mem_alloc_desc_t alloc = {
            .stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC,
            .pNext = &export_desc,
            .flags = 0,
            .ordinal = 0,
        };
        void *ptr = nullptr;
        if (zeMemAllocDevice(ctx, &alloc, bytes, 64, dev, &ptr) != ZE_RESULT_SUCCESS) {
            return nullptr;
        }
        ze_external_memory_export_fd_t out = {
            .stype = ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_FD,
            .pNext = nullptr,
            .flags = ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF,
            .fd = -1,
        };
        ze_memory_allocation_properties_t props = {
            .stype = ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES,
            .pNext = &out,
            .type = ZE_MEMORY_TYPE_UNKNOWN,
            .id = 0,
            .pageSize = 0,
        };
        if (zeMemGetAllocProperties(ctx, ptr, &props, nullptr) != ZE_RESULT_SUCCESS || out.fd < 0) {
            (void)zeMemFree(ctx, ptr);
            return nullptr;
        }
        const std::scoped_lock hold(p->lock);
        p->exported.insert(ptr);
        *fd = out.fd;
        *size = bytes;
        return ptr;
    } catch (const std::exception &e) {
        (void)failed("dma-buf alloc", e);
        return nullptr;
    }
}

extern "C" void vs_free(VsProducer *p, void *ptr)
{
    if (!ptr) {
        return;
    }
    try {
        p->q.wait_and_throw();
        bool l0 = false;
        {
            const std::scoped_lock hold(p->lock);
            l0 = p->exported.erase(ptr) != 0;
        }
        if (l0) {
            (void)zeMemFree(
                sycl::get_native<sycl::backend::ext_oneapi_level_zero>(p->q.get_context()), ptr);
        } else {
            sycl::free(ptr, p->q);
        }
    } catch (const std::exception &e) {
        (void)failed("free", e);
    }
}

extern "C" int vs_copy_2d(VsProducer *p, void *dst, size_t dst_pitch, const void *src,
                          size_t src_pitch, size_t row, size_t rows)
{
    /* One memcpy per row, or one for packed rows: ext_oneapi_memcpy2d() enqueued
     * behind a host task lost the device on the Arc A380 (xe, DPC++ 2026.0 and
     * 2026.1.1; ADR-2091), and the producer holds its queue with host tasks. */
    try {
        auto *out = static_cast<uint8_t *>(dst);
        const auto *in = static_cast<const uint8_t *>(src);
        if (dst_pitch == row && src_pitch == row) {
            p->last = p->q.memcpy(out, in, row * rows);
            return 0;
        }
        for (size_t y = 0; y < rows; y++) {
            p->last = p->q.memcpy(out + y * dst_pitch, in + y * src_pitch, row);
        }
        return 0;
    } catch (const std::exception &e) {
        return failed("copy", e);
    }
}

extern "C" int vs_fill(VsProducer *p, void *dst, uint8_t value, size_t bytes)
{
    try {
        p->last = p->q.memset(dst, value, bytes);
        return 0;
    } catch (const std::exception &e) {
        return failed("fill", e);
    }
}

extern "C" int vs_hold(VsProducer *p, unsigned us)
{
    try {
        p->last = p->q.submit([&](sycl::handler &h) {
            h.host_task([us]() { std::this_thread::sleep_for(std::chrono::microseconds(us)); });
        });
        return 0;
    } catch (const std::exception &e) {
        return failed("hold", e);
    }
}

namespace
{

/* One work-item spinning `iterations` dependent integer steps. */
sycl::event spin(sycl::queue &q, uint32_t *sink, uint64_t iterations)
{
    return q.single_task([=]() {
        uint32_t x = 1u;
        for (uint64_t k = 0; k < iterations; k++) {
            x = x * 1664525u + 1013904223u;
        }
        *sink = x;
    });
}

/* Spin iterations per microsecond on this device (measured once). */
double spin_rate(sycl::queue &q, uint32_t *sink)
{
    static double rate = 0.0;
    if (rate > 0.0) {
        return rate;
    }
    const uint64_t probe = 1u << 20;
    spin(q, sink, probe).wait();
    const auto t0 = std::chrono::steady_clock::now();
    spin(q, sink, probe).wait();
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    rate = us > 0.0 ? (double)probe / us : 1.0;
    return rate;
}

} // namespace

extern "C" int vs_hold_device(VsProducer *p, unsigned us)
{
    try {
        if (!p->sink) {
            p->sink = sycl::malloc_device<uint32_t>(1, p->q);
        }
        const double rate = spin_rate(p->q, p->sink);
        p->last = spin(p->q, p->sink, (uint64_t)(rate * us));
        return 0;
    } catch (const std::exception &e) {
        return failed("device hold", e);
    }
}

extern "C" int vs_after(VsProducer *p, uintptr_t event)
{
    try {
        p->last = p->q.ext_oneapi_submit_barrier({*event_of(event)});
        return 0;
    } catch (const std::exception &e) {
        return failed("after", e);
    }
}

extern "C" int vs_finish(VsProducer *p)
{
    try {
        p->q.wait_and_throw();
        return 0;
    } catch (const std::exception &e) {
        return failed("finish", e);
    }
}

extern "C" uintptr_t vs_last_event(VsProducer *p)
{
    try {
        return p->last ? reinterpret_cast<uintptr_t>(new sycl::event(*p->last)) : 0u;
    } catch (const std::exception &e) {
        (void)failed("last event", e);
        return 0u;
    }
}

extern "C" int vs_store_last_event(VsProducer *p, uintptr_t handle)
{
    try {
        if (!p->last || handle == 0u) {
            return -1;
        }
        *event_of(handle) = *p->last;
        return 0;
    } catch (const std::exception &e) {
        return failed("store event", e);
    }
}

extern "C" void vs_event_free(uintptr_t event)
{
    delete event_of(event);
}

extern "C" int vs_event_done(uintptr_t event)
{
    try {
        return event_of(event)->get_info<sycl::info::event::command_execution_status>() ==
                       sycl::info::event_command_status::complete ?
                   1 :
                   0;
    } catch (const std::exception &e) {
        return failed("event", e);
    }
}

extern "C" VsLoad *vs_load_start(unsigned in_flight)
{
    try {
        const std::optional<sycl::device> dev = first_level_zero_gpu();
        if (!dev) {
            return nullptr;
        }
        auto *load =
            new VsLoad{.q = sycl::queue(*dev, sycl::property::queue::in_order{}), .stop = false};
        load->sink = sycl::malloc_device<uint32_t>(1u << 16, load->q);
        load->worker = std::thread(load_worker, load, in_flight);
        return load;
    } catch (const std::exception &e) {
        (void)failed("load", e);
        return nullptr;
    }
}

extern "C" void vs_load_stop(VsLoad *load)
{
    if (!load) {
        return;
    }
    load->stop.store(true);
    load->worker.join();
    try {
        load->q.wait_and_throw();
        sycl::free(load->sink, load->q);
    } catch (const std::exception &e) {
        (void)failed("load stop", e);
    }
    delete load;
}
