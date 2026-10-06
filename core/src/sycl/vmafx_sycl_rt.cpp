/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The SYCL runtime half of the VMAFx SYCL lane (RC4 WP3, ADR-2091); see
 * vmafx_sycl_rt.h. Every function here is extern "C" and catches what the
 * runtime throws: a sycl::exception never crosses into the C callers.
 *
 * Ordering (ADR-2091 items 1 to 3): a frame's conversions run on the
 * device's library queue (in-order) behind a join on the producer's event
 * (join(): an empty kernel that depends on it); the frame's ready event is
 * the last of them. Every engine read of the frame's planes is a device copy
 * on the reader's queue that waits on the ready event
 * (vmaf_sycl_picture_read_plane()), and its event is recorded on the frame.
 * The release is one join on the library queue over the ready event and
 * every recorded read: work behind it (the HOST release fence's
 * host task, the frees of the frame's memory, a producer waiting on the
 * SYCL_EVENT release fence) runs after the last reader of every context.
 */

#include "config.h"

#if HAVE_SYCL

#include <sycl/sycl.hpp>

#include <algorithm>
#include <cassert>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "common.h"
#include "detile.h"
#include "dmabuf_import.h"
#include "log.h"
#include "picture.h"
#include "vmafx_sycl_rt.h"

/* GPUs whose names the enumeration keeps for the process (HISS-02). */
#define VMAFX_SYCL_MAX_DEVICES 64
/* Bytes of a device name kept. */
#define VMAFX_SYCL_NAME_BYTES 256
/* Release events handed out and not yet both recorded and destroyed by every
 * holder (HISS-02 bound; a decoder pool holds a few dozen frames). */
#define VMAFX_SYCL_RELEASE_SLOTS 4096u

namespace
{
/* Memory a release frees once its join completed. */
struct VmafxSyclDeferred {
    sycl::event done;
    void *owned;
    std::array<void *, 3> imports;
};
} // namespace

/* An aggregate, initialised in its new-expression with the selected queue
 * (as VmafSyclState is since PR #1837): never default-construct the queue. */
struct VmafxSyclRt {
    sycl::queue lib;
    std::mutex lock; /* `deferred` */
    std::vector<VmafxSyclDeferred> deferred;
    uint64_t memory;
    int32_t index;
    bool immediate;
    char name[VMAFX_SYCL_NAME_BYTES];
};

struct VmafxSyclFrameRt {
    VmafxSyclRt *rt;
    std::mutex lock; /* `reads` */
    std::vector<sycl::event> reads;
    sycl::event ready; /* valid when `have_ready` */
    sycl::event last;  /* the frame's last library-queue command */
    bool have_ready;
    bool have_last;
};

namespace
{

/* ---- Devices -------------------------------------------------------------------- */

/* The Level Zero GPUs: the lane imports through Level Zero (dma-buf, native
 * handles), so another backend's view of a GPU is not one of its devices. */
std::vector<sycl::device> level_zero_gpus()
{
    std::vector<sycl::device> gpus;
    for (const sycl::platform &p : sycl::platform::get_platforms()) {
        if (p.get_backend() != sycl::backend::ext_oneapi_level_zero) {
            continue;
        }
        for (const sycl::device &d : p.get_devices(sycl::info::device_type::gpu)) {
            gpus.push_back(d);
        }
    }
    return gpus;
}

std::mutex &names_lock()
{
    static std::mutex lock;
    return lock;
}

/* Names of the devices, kept for the process (VmafxDeviceInfo.name). */
char (&device_names()) [VMAFX_SYCL_MAX_DEVICES][VMAFX_SYCL_NAME_BYTES] {
    static char names[VMAFX_SYCL_MAX_DEVICES][VMAFX_SYCL_NAME_BYTES];
    return names;
}

const char *kept_name(int32_t index, const sycl::device &dev)
{
    if (index < 0 || index >= VMAFX_SYCL_MAX_DEVICES) {
        return "sycl";
    }
    const std::scoped_lock hold(names_lock());
    char *const name = device_names()[index];
    if (name[0] == '\0') {
        const std::string n = dev.get_info<sycl::info::device::name>();
        (void)std::snprintf(name, VMAFX_SYCL_NAME_BYTES, "%s", n.c_str());
    }
    return name;
}

/* The PCI bus id of `dev` ("dddd:bb:dd.f", the Intel device-info
 * extension) into `bus`; "" without it. The C side parses it
 * (vmafx_parse_pci_bus_id()). */
void device_bus_id(const sycl::device &dev, char bus[VMAFX_SYCL_BUS_ID_SIZE])
{
    bus[0] = '\0';
    if (!dev.has(sycl::aspect::ext_intel_pci_address)) {
        return;
    }
    const std::string id = dev.get_info<sycl::ext::intel::info::device::pci_address>();
    if (id.size() < VMAFX_SYCL_BUS_ID_SIZE) {
        std::memcpy(bus, id.c_str(), id.size() + 1u);
    }
}

/* Test switch (white-box, vmafx_sycl_rt_test_omit_immediate()): open
 * library queues without the immediate-command-list property. */
bool &omit_immediate()
{
    static bool omit = false;
    return omit;
}

sycl::property_list library_queue_props(bool *immediate)
{
#ifdef SYCL_EXT_INTEL_QUEUE_IMMEDIATE_COMMAND_LIST
    if (!omit_immediate()) {
        *immediate = true;
        return sycl::property_list{sycl::property::queue::in_order{},
                                   sycl::ext::intel::property::queue::immediate_command_list{}};
    }
#endif
    *immediate = false;
    return sycl::property_list{sycl::property::queue::in_order{}};
}

/* ---- Kernels ---------------------------------------------------------------------- */

/* A little-endian 16-bit sample at `p`, shifted right. */
inline uint16_t load16(const uint8_t *p, unsigned shift)
{
    return (uint16_t)((uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8)) >> shift);
}

inline void store16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

/* Cb / Cr pairs of an interleaved plane into two planes: the samples moved,
 * 16-bit ones shifted, nothing else (bit-exact with the host conversion). */
sycl::event launch_deinterleave(sycl::queue &q, const VmafxSyclPlaneOp &op)
{
    assert(op.bytes == 1u || op.bytes == 2u);
    const auto *src = static_cast<const uint8_t *>(op.src);
    auto *cb = static_cast<uint8_t *>(op.dst0);
    auto *cr = static_cast<uint8_t *>(op.dst1);
    const size_t sp = op.src_pitch;
    const size_t dp = op.dst_pitch;
    const unsigned bytes = op.bytes;
    const unsigned shift = op.shift;
    return q.parallel_for(sycl::range<2>(op.rows, op.w), [=](sycl::id<2> id) {
        const size_t y = id[0];
        const size_t x = id[1];
        const uint8_t *const in = src + y * sp + x * 2u * bytes;
        uint8_t *const out_b = cb + y * dp + x * bytes;
        uint8_t *const out_r = cr + y * dp + x * bytes;
        if (bytes == 1u) {
            out_b[0] = in[0];
            out_r[0] = in[1];
            return;
        }
        store16(out_b, load16(in, shift));
        store16(out_r, load16(in + 2, shift));
    });
}

/* A 16-bit plane shifted right into a plane of its own (the P010 luma). */
sycl::event launch_shift(sycl::queue &q, const VmafxSyclPlaneOp &op)
{
    const auto *src = static_cast<const uint8_t *>(op.src);
    auto *dst = static_cast<uint8_t *>(op.dst0);
    const size_t sp = op.src_pitch;
    const size_t dp = op.dst_pitch;
    const unsigned shift = op.shift;
    return q.parallel_for(sycl::range<2>(op.rows, op.w), [=](sycl::id<2> id) {
        const size_t y = id[0];
        const size_t x = id[1];
        store16(dst + y * dp + x * 2u, load16(src + y * sp + x * 2u, shift));
    });
}

/* One plane of a packed layout or of MSB planar words: the samples moved by
 * the plan of vmafx_import_plane_read() (core/src/vmafx/import_convert.h,
 * VmafxImportRead), shifted and masked, nothing else (bit-exact with the host
 * conversion). Scratch-free: every value lives in registers (ADR-1395). */
sycl::event launch_gather(sycl::queue &q, const VmafxSyclPlaneOp &op)
{
    assert(op.bytes == 1u || op.bytes == 2u);
    assert(op.in_bytes == 1u || op.in_bytes == 2u || op.in_bytes == 4u);
    const auto *src = static_cast<const uint8_t *>(op.src);
    auto *dst = static_cast<uint8_t *>(op.dst0);
    const size_t sp = op.src_pitch;
    const size_t dp = op.dst_pitch;
    const unsigned bytes = op.bytes;
    const unsigned step = op.step;
    const unsigned offset = op.offset;
    const unsigned in_bytes = op.in_bytes;
    const unsigned shift = op.shift;
    const unsigned mask = op.mask;
    return q.parallel_for(sycl::range<2>(op.rows, op.w), [=](sycl::id<2> id) {
        const size_t y = id[0];
        const size_t x = id[1];
        const uint8_t *const at = src + y * sp + (x * step + offset) * in_bytes;
        unsigned v = at[0];
        if (in_bytes >= 2u) {
            v |= (unsigned)at[1] << 8;
        }
        if (in_bytes == 4u) {
            v |= ((unsigned)at[2] << 16) | ((unsigned)at[3] << 24);
        }
        v >>= shift;
        if (mask != 0u) {
            v &= mask;
        }
        uint8_t *const out = dst + y * dp + x * bytes;
        out[0] = (uint8_t)(v & 0xFFu);
        if (bytes == 2u) {
            out[1] = (uint8_t)((v >> 8) & 0xFFu);
        }
    });
}

/* `rows` rows of `row` bytes from `src` (`src_pitch` apart) to `dst`
 * (`dst_pitch` apart) after `deps`, as one kernel moving a byte per
 * work-item (any address, any pitch). Not ext_oneapi_memcpy2d(): one enqueued
 * behind a host task lost an Arc A380 under xe with DPC++ 2026.0 and 2026.1.1
 * (ADR-2091, Research-2160 finding 1), and producers hold their queues with
 * host tasks; and a plane with padded rows would take one memcpy() per row. */
sycl::event copy_rows(sycl::queue &q, void *dst, size_t dst_pitch, const void *src,
                      size_t src_pitch, size_t row, size_t rows,
                      const std::vector<sycl::event> &deps)
{
    const auto *in = static_cast<const uint8_t *>(src);
    auto *out = static_cast<uint8_t *>(dst);
    return q.submit([&](sycl::handler &h) {
        h.depends_on(deps);
        h.parallel_for(sycl::range<2>(rows, row), [=](sycl::id<2> id) {
            out[id[0] * dst_pitch + id[1]] = in[id[0] * src_pitch + id[1]];
        });
    });
}

int runtime_failed(const char *what, const std::exception &e)
{
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmafx: backend sycl: %s: %s\n", what, e.what());
    return -EIO;
}

/* ---- Release slots ---------------------------------------------------------------- */

struct ReleaseSlot {
    sycl::event *event; /* NULL: a free slot */
    uint32_t refs;
    bool recorded;
};

struct ReleaseTable {
    std::mutex lock;
    std::array<ReleaseSlot, VMAFX_SYCL_RELEASE_SLOTS> slots;
    uint32_t high; /* slots below it were used at least once */
};

ReleaseTable &release_table()
{
    static ReleaseTable table{};
    return table;
}

/* Record the release barrier's event into slot `slot` (1-based; 0: none). */
void slot_record(uint32_t slot, const sycl::event &done)
{
    if (slot == 0u || slot > VMAFX_SYCL_RELEASE_SLOTS) {
        return;
    }
    ReleaseTable &t = release_table();
    const std::scoped_lock hold(t.lock);
    ReleaseSlot &r = t.slots[slot - 1u];
    if (r.event) {
        *r.event = done;
        r.recorded = true;
    }
}

/* ---- Frames ----------------------------------------------------------------------- */

void note_last(VmafxSyclFrameRt *f, const sycl::event &ev)
{
    f->last = ev;
    f->have_last = true;
}

/* An empty kernel on `q` after every event of `deps`: a point on the device
 * all of them have passed. Not ext_oneapi_submit_barrier(): a barrier whose
 * wait list holds the event of a command still waiting on a host task (a
 * producer that holds its queue with one) returns only once that host task
 * ran, so the import or the release would wait on the host; a kernel with
 * the same dependencies is enqueued at once and the device waits (DPC++
 * 2026.0 and 2026.1.1, Research-2160 finding 2). */
sycl::event join(sycl::queue &q, const std::vector<sycl::event> &deps)
{
    return q.submit([&](sycl::handler &h) {
        h.depends_on(deps);
        h.single_task([] {});
    });
}

/* The release's barrier: after the ready point and every read. */
sycl::event release_barrier(VmafxSyclFrameRt *f)
{
    std::vector<sycl::event> wait;
    {
        const std::scoped_lock hold(f->lock);
        wait.swap(f->reads);
    }
    if (f->have_ready) {
        wait.push_back(f->ready);
    }
    return join(f->rt->lib, wait);
}

void defer_free(VmafxSyclRt *rt, const sycl::event &done, const VmafxSyclRelease *r)
{
    if (!r->owned && !r->imports[0] && !r->imports[1] && !r->imports[2]) {
        return;
    }
    const VmafxSyclDeferred d = {
        .done = done, .owned = r->owned, .imports = {r->imports[0], r->imports[1], r->imports[2]}};
    const std::scoped_lock hold(rt->lock);
    rt->deferred.push_back(d);
}

void free_now(VmafxSyclRt *rt, void *owned, const std::array<void *, 3> &imports)
{
    if (owned) {
        vmafx_sycl_rt_free(rt, owned);
    }
    for (void *const p : imports) {
        if (p) {
            vmaf_sycl_dmabuf_free_queue(&rt->lib, p);
        }
    }
}

bool completed(const sycl::event &e)
{
    return e.get_info<sycl::info::event::command_execution_status>() ==
           sycl::info::event_command_status::complete;
}

} // namespace

/* ---- Devices ------------------------------------------------------------------------ */

extern "C" int vmafx_sycl_rt_count(uint32_t *count)
{
    *count = 0u;
    try {
        const size_t n = level_zero_gpus().size();
        *count = (uint32_t)std::min<size_t>(n, VMAFX_SYCL_MAX_DEVICES);
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("device enumeration", e);
    }
}

extern "C" int vmafx_sycl_rt_info(int32_t index, const char **name, uint64_t *memory)
{
    try {
        const std::vector<sycl::device> gpus = level_zero_gpus();
        if (index < 0 ||
            std::cmp_greater_equal(index, std::min<size_t>(gpus.size(), VMAFX_SYCL_MAX_DEVICES))) {
            return -ENOENT;
        }
        const sycl::device &dev = gpus[(size_t)index];
        *name = kept_name(index, dev);
        *memory = dev.get_info<sycl::info::device::global_mem_size>();
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("device enumeration", e);
    }
}

extern "C" int vmafx_sycl_rt_bus_id(int32_t index, char bus[VMAFX_SYCL_BUS_ID_SIZE])
{
    try {
        const std::vector<sycl::device> gpus = level_zero_gpus();
        if (index < 0 ||
            std::cmp_greater_equal(index, std::min<size_t>(gpus.size(), VMAFX_SYCL_MAX_DEVICES))) {
            return -ENOENT;
        }
        device_bus_id(gpus[(size_t)index], bus);
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("device enumeration", e);
    }
}

extern "C" int vmafx_sycl_rt_open(int32_t index, uintptr_t external_queue, VmafxSyclRt **out)
{
    *out = nullptr;
    try {
        bool immediate = false;
        const sycl::property_list props = library_queue_props(&immediate);
        std::optional<sycl::queue> lib;
        int32_t at = -1;
        if (external_queue != 0u) {
            /* NOLINTNEXTLINE(performance-no-int-to-ptr): the caller's sycl::queue crosses the ABI as uintptr_t (VmafxDeviceDesc.external, ADR-1929). */
            const auto *caller = reinterpret_cast<const sycl::queue *>(external_queue);
            lib.emplace(caller->get_context(), caller->get_device(), props);
        } else {
            const std::vector<sycl::device> gpus = level_zero_gpus();
            at = index == -1 ? 0 : index;
            if (at < 0 ||
                std::cmp_greater_equal(at, std::min<size_t>(gpus.size(), VMAFX_SYCL_MAX_DEVICES))) {
                return -ENOENT;
            }
            lib.emplace(gpus[(size_t)at], props);
        }
        auto *rt = new (std::nothrow) VmafxSyclRt{.lib = std::move(*lib)};
        if (!rt) {
            return -ENOMEM;
        }
        const sycl::device dev = rt->lib.get_device();
        rt->index = at;
        rt->immediate = immediate;
        rt->memory = dev.get_info<sycl::info::device::global_mem_size>();
        (void)std::snprintf(rt->name, sizeof(rt->name), "%s",
                            dev.get_info<sycl::info::device::name>().c_str());
        *out = rt;
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("cannot open the device", e);
    }
}

extern "C" void vmafx_sycl_rt_close(VmafxSyclRt *rt)
{
    if (!rt) {
        return;
    }
    try {
        rt->lib.wait_and_throw();
    } catch (const std::exception &e) {
        (void)runtime_failed("device close", e);
    }
    std::vector<VmafxSyclDeferred> left;
    {
        const std::scoped_lock hold(rt->lock);
        left.swap(rt->deferred);
    }
    for (const VmafxSyclDeferred &d : left) {
        free_now(rt, d.owned, d.imports);
    }
    delete rt;
}

extern "C" int32_t vmafx_sycl_rt_index(const VmafxSyclRt *rt)
{
    return rt->index;
}

extern "C" const char *vmafx_sycl_rt_name(const VmafxSyclRt *rt)
{
    return rt->name;
}

extern "C" uint64_t vmafx_sycl_rt_memory(const VmafxSyclRt *rt)
{
    return rt->memory;
}

extern "C" void vmafx_sycl_rt_device_bus_id(const VmafxSyclRt *rt, char bus[VMAFX_SYCL_BUS_ID_SIZE])
{
    try {
        device_bus_id(rt->lib.get_device(), bus);
    } catch (const std::exception &) {
        bus[0] = '\0'; /* the runtime threw: unknown */
    }
}

extern "C" bool vmafx_sycl_rt_immediate(const VmafxSyclRt *rt)
{
    return rt->immediate;
}

extern "C" void *vmafx_sycl_rt_queue(VmafxSyclRt *rt)
{
    return &rt->lib;
}

extern "C" int vmafx_sycl_rt_engine_state(VmafxSyclRt *rt, VmafSyclState **out)
{
    return vmaf_sycl_state_init_queue(out, &rt->lib);
}

extern "C" void vmafx_sycl_rt_test_omit_immediate(bool omit)
{
    omit_immediate() = omit;
}

/* ---- Memory ------------------------------------------------------------------------- */

extern "C" enum VmafxSyclPointerKind vmafx_sycl_rt_pointer_kind(VmafxSyclRt *rt, const void *ptr)
{
    try {
        switch (sycl::get_pointer_type(ptr, rt->lib.get_context())) {
        case sycl::usm::alloc::host:
            return VMAFX_SYCL_POINTER_HOST;
        case sycl::usm::alloc::device:
            return VMAFX_SYCL_POINTER_DEVICE;
        case sycl::usm::alloc::shared:
            return VMAFX_SYCL_POINTER_SHARED;
        default:
            return VMAFX_SYCL_POINTER_UNKNOWN;
        }
    } catch (const std::exception &e) {
        (void)runtime_failed("pointer query", e);
        return VMAFX_SYCL_POINTER_UNKNOWN;
    }
}

extern "C" void *vmafx_sycl_rt_alloc(VmafxSyclRt *rt, size_t bytes)
{
    try {
        return sycl::malloc_device(bytes, rt->lib);
    } catch (const std::exception &e) {
        (void)runtime_failed("device allocation", e);
        return nullptr;
    }
}

extern "C" void vmafx_sycl_rt_free(VmafxSyclRt *rt, void *ptr)
{
    try {
        sycl::free(ptr, rt->lib);
    } catch (const std::exception &e) {
        (void)runtime_failed("device free", e);
    }
}

extern "C" int vmafx_sycl_rt_dmabuf_import(VmafxSyclRt *rt, int fd, size_t size, void **ptr)
{
    return vmaf_sycl_dmabuf_import_queue(&rt->lib, fd, size, ptr);
}

extern "C" void vmafx_sycl_rt_collect(VmafxSyclRt *rt)
{
    std::vector<VmafxSyclDeferred> done;
    try {
        const std::scoped_lock hold(rt->lock);
        const auto tail = std::ranges::stable_partition(
            rt->deferred, [](const VmafxSyclDeferred &d) { return !completed(d.done); });
        done.assign(tail.begin(), tail.end());
        rt->deferred.erase(tail.begin(), tail.end());
    } catch (const std::exception &e) {
        (void)runtime_failed("deferred free", e);
    }
    for (const VmafxSyclDeferred &d : done) {
        free_now(rt, d.owned, d.imports);
    }
}

/* ---- Frames ------------------------------------------------------------------------- */

extern "C" VmafxSyclFrameRt *vmafx_sycl_rt_frame_new(VmafxSyclRt *rt)
{
    try {
        return new VmafxSyclFrameRt{.rt = rt};
    } catch (const std::exception &e) {
        (void)runtime_failed("frame state", e);
        return nullptr;
    }
}

extern "C" int vmafx_sycl_rt_frame_after_event(VmafxSyclFrameRt *f, uintptr_t event)
{
    try {
        /* NOLINTNEXTLINE(performance-no-int-to-ptr): the producer's sycl::event crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
        const sycl::event producer = *reinterpret_cast<const sycl::event *>(event);
        note_last(f, join(f->rt->lib, {producer}));
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("acquire join", e);
    }
}

extern "C" int vmafx_sycl_rt_frame_deinterleave(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op)
{
    try {
        note_last(f, launch_deinterleave(f->rt->lib, *op));
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("de-interleave", e);
    }
}

extern "C" int vmafx_sycl_rt_frame_shift(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op)
{
    try {
        note_last(f, launch_shift(f->rt->lib, *op));
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("shift", e);
    }
}

extern "C" int vmafx_sycl_rt_frame_gather(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op)
{
    try {
        note_last(f, launch_gather(f->rt->lib, *op));
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("gather", e);
    }
}

extern "C" int vmafx_sycl_rt_frame_detile(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op,
                                          enum VmafxSyclTiling tiling)
{
    const vmaf_sycl_detile::Plane plane = {
        .src = static_cast<const uint8_t *>(op->src),
        .dst = static_cast<uint8_t *>(op->dst0),
        .dst_pitch = op->dst_pitch,
        .row_bytes = (size_t)op->w * op->bytes,
        .tiles_per_row = (unsigned)(op->src_pitch / vmaf_sycl_detile::kTileWidth),
        .rows = op->rows,
        .shift = op->bytes == 2u ? op->shift : 0u,
        .tile4 = tiling == VMAFX_SYCL_TILING_4,
    };
    try {
        note_last(f, vmaf_sycl_detile::launch(f->rt->lib, plane));
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("de-tile", e);
    }
}

extern "C" int vmafx_sycl_rt_frame_stage_host(VmafxSyclFrameRt *f, const VmafxSyclPlaneOp *op)
{
    sycl::queue &q = f->rt->lib;
    const size_t row = (size_t)op->w * op->bytes;
    void *host = nullptr;
    try {
        host = sycl::malloc_host(row * op->rows, q);
        if (!host) {
            return -ENOMEM;
        }
        copy_rows(q, host, row, op->src, op->src_pitch, row, op->rows, {}).wait_and_throw();
        copy_rows(q, op->dst0, op->dst_pitch, host, row, row, op->rows, {}).wait_and_throw();
        sycl::free(host, q);
        return 0;
    } catch (const std::exception &e) {
        if (host) {
            vmafx_sycl_rt_free(f->rt, host);
        }
        return runtime_failed("planted host copy", e);
    }
}

extern "C" int vmafx_sycl_rt_frame_ready(VmafxSyclFrameRt *f)
{
    if (f->have_last) {
        f->ready = f->last;
        f->have_ready = true;
    }
    return 0;
}

namespace
{
/* The HOST release fence's host task, behind the release join on the in-order
 * library queue. */
void submit_signal(VmafxSyclRt *rt, const VmafxSyclRelease *r)
{
    void (*const signal)(void *) = r->signal;
    void *const arg = r->arg;
    rt->lib.submit([&](sycl::handler &h) { h.host_task([=]() { signal(arg); }); });
}
} // namespace

extern "C" int vmafx_sycl_rt_frame_release(VmafxSyclFrameRt *f, const VmafxSyclRelease *r)
{
    VmafxSyclRt *const rt = f->rt;
    try {
        const sycl::event done = release_barrier(f);
        if (r->signal) {
            submit_signal(rt, r);
        }
        slot_record(r->slot, done);
        slot_record(r->idle_slot, done);
        defer_free(rt, done, r);
        delete f;
        vmafx_sycl_rt_collect(rt);
        return 0;
    } catch (const std::exception &e) {
        /* Nothing more can be ordered behind the readers: drain the queue
         * (discard), then open the release fences, so no waiter hangs. */
        const int err = runtime_failed("frame release", e);
        vmafx_sycl_rt_frame_discard(f, r);
        slot_record(r->slot, sycl::event());
        slot_record(r->idle_slot, sycl::event());
        return err;
    }
}

extern "C" void vmafx_sycl_rt_frame_discard(VmafxSyclFrameRt *f, const VmafxSyclRelease *r)
{
    if (!f) {
        return;
    }
    VmafxSyclRt *const rt = f->rt;
    try {
        rt->lib.wait_and_throw();
    } catch (const std::exception &e) {
        (void)runtime_failed("discard drain", e);
    }
    if (r) {
        free_now(rt, r->owned, {r->imports[0], r->imports[1], r->imports[2]});
        if (r->signal) {
            r->signal(r->arg);
        }
    }
    delete f;
}

extern "C" uint32_t vmafx_sycl_rt_frame_reads(VmafxSyclFrameRt *f)
{
    const std::scoped_lock hold(f->lock);
    return (uint32_t)f->reads.size();
}

/* ---- Engine readers (common.h) ----------------------------------------------------- */

extern "C" bool vmaf_sycl_picture_on_device(const VmafPicture *pic)
{
    const auto *priv = pic ? static_cast<const VmafPicturePrivate *>(pic->priv) : nullptr;
    return priv && priv->buf_type == VMAF_PICTURE_BUFFER_TYPE_SYCL_DEVICE && priv->sycl.frame;
}

extern "C" int vmaf_sycl_picture_read_plane(const VmafPicture *pic, unsigned plane, void *queue_ptr,
                                            void *dst, size_t dst_pitch, size_t row_bytes,
                                            unsigned rows, void *done)
{
    if (!vmaf_sycl_picture_on_device(pic) || plane > 2u || !queue_ptr || !dst ||
        !pic->data[plane] || pic->stride[plane] <= 0 || dst_pitch < row_bytes) {
        return -EINVAL;
    }
    const auto *priv = static_cast<const VmafPicturePrivate *>(pic->priv);
    auto *f = static_cast<VmafxSyclFrameRt *>(priv->sycl.frame);
    auto &q = *static_cast<sycl::queue *>(queue_ptr);
    try {
        const std::scoped_lock hold(f->lock);
        const std::vector<sycl::event> deps =
            f->have_ready ? std::vector<sycl::event>{f->ready} : std::vector<sycl::event>{};
        const sycl::event ev = copy_rows(q, dst, dst_pitch, pic->data[plane],
                                         (size_t)pic->stride[plane], row_bytes, rows, deps);
        f->reads.push_back(ev);
        if (done) {
            *static_cast<sycl::event *>(done) = ev;
        }
        return 0;
    } catch (const std::exception &e) {
        return runtime_failed("device picture read", e);
    }
}

/* ---- Release slots -------------------------------------------------------------------- */

extern "C" int vmafx_sycl_rt_slot_new(bool recorded, uint32_t *slot)
{
    ReleaseTable &t = release_table();
    sycl::event *const event = new (std::nothrow) sycl::event();
    if (!event) {
        return -ENOMEM;
    }
    const std::scoped_lock hold(t.lock);
    uint32_t at = VMAFX_SYCL_RELEASE_SLOTS;
    for (uint32_t i = 0; i < t.high && at == VMAFX_SYCL_RELEASE_SLOTS; i++) {
        at = t.slots[i].event ? at : i;
    }
    if (at == VMAFX_SYCL_RELEASE_SLOTS && t.high < VMAFX_SYCL_RELEASE_SLOTS) {
        at = t.high++;
    }
    if (at == VMAFX_SYCL_RELEASE_SLOTS) {
        delete event;
        return -ENOMEM;
    }
    t.slots[at] = {.event = event, .refs = 1u, .recorded = recorded};
    *slot = at + 1u;
    return 0;
}

extern "C" uintptr_t vmafx_sycl_rt_slot_handle(uint32_t slot)
{
    ReleaseTable &t = release_table();
    const std::scoped_lock hold(t.lock);
    return slot >= 1u && slot <= VMAFX_SYCL_RELEASE_SLOTS ?
               reinterpret_cast<uintptr_t>(t.slots[slot - 1u].event) :
               0u;
}

extern "C" int vmafx_sycl_rt_slot_of(uintptr_t handle, uint32_t *slot)
{
    ReleaseTable &t = release_table();
    const std::scoped_lock hold(t.lock);
    for (uint32_t i = 0; i < t.high; i++) {
        if (t.slots[i].event && reinterpret_cast<uintptr_t>(t.slots[i].event) == handle) {
            *slot = i + 1u;
            return 0;
        }
    }
    return -ENOENT;
}

extern "C" void vmafx_sycl_rt_slot_ref(uint32_t slot)
{
    ReleaseTable &t = release_table();
    const std::scoped_lock hold(t.lock);
    if (slot >= 1u && slot <= VMAFX_SYCL_RELEASE_SLOTS && t.slots[slot - 1u].event) {
        t.slots[slot - 1u].refs++;
    }
}

extern "C" void vmafx_sycl_rt_slot_unref(uint32_t slot)
{
    ReleaseTable &t = release_table();
    const sycl::event *gone = nullptr;
    {
        const std::scoped_lock hold(t.lock);
        if (slot < 1u || slot > VMAFX_SYCL_RELEASE_SLOTS || !t.slots[slot - 1u].event) {
            return;
        }
        ReleaseSlot &r = t.slots[slot - 1u];
        if (--r.refs == 0u) {
            gone = r.event;
            r = {.event = nullptr, .refs = 0u, .recorded = false};
        }
    }
    delete gone;
}

extern "C" void vmafx_sycl_rt_slot_open(uint32_t slot)
{
    slot_record(slot, sycl::event());
}

extern "C" int vmafx_sycl_rt_slot_poll(uint32_t slot)
{
    ReleaseTable &t = release_table();
    std::optional<sycl::event> ev;
    {
        const std::scoped_lock hold(t.lock);
        if (slot < 1u || slot > VMAFX_SYCL_RELEASE_SLOTS || !t.slots[slot - 1u].event) {
            return -EINVAL;
        }
        if (!t.slots[slot - 1u].recorded) {
            return 0;
        }
        ev = *t.slots[slot - 1u].event;
    }
    try {
        return completed(*ev) ? 1 : 0;
    } catch (const std::exception &e) {
        return runtime_failed("release event", e);
    }
}

extern "C" int vmafx_sycl_rt_event_poll(uintptr_t event)
{
    try {
        /* NOLINTNEXTLINE(performance-no-int-to-ptr): a sycl::event crosses the ABI as uintptr_t (VmafxFence.handle, ADR-1929). */
        return completed(*reinterpret_cast<const sycl::event *>(event)) ? 1 : 0;
    } catch (const std::exception &e) {
        return runtime_failed("event", e);
    }
}

#endif /* HAVE_SYCL */
