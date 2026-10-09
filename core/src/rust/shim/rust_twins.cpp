/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * rust_twins.cpp - the C side of the Rust extractor framework (ADR-1713).
 *
 * Each Rust twin (vmafx_rs_twin_at()) becomes a VmafFeatureExtractor named
 * `<c name>_rust`: a copy of the C extractor's descriptor, so it shares the
 * option table, priv_size, provided features, flags and reads_prev_prev_ref,
 * with generic callbacks that
 *
 *   - read the parsed option values back from the C-layout priv blob and hand
 *     them to Rust by name,
 *   - build the feature-name dictionary exactly as the C extractors do,
 *     including the C descriptor's extend_name_dict() entries (ADR-2795),
 *   - pass the pictures and fex->prev_ref / prev_prev_ref as plane views,
 *   - route scores through the C collector (append_with_dict, append,
 *     get_score, set_aggregate).
 *
 * The shim instance lives in one pointer slot appended to the C priv blob.
 * Rust never sees a libvmaf struct.
 */

#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "opt.h"
#include "rust/include/vmafx_rs.h"
#include "rust/shim/rust_twins.h"

extern "C" {
/* ADR-0707: the TAD pilot's descriptor (feature/tad_rust.c). */
extern VmafFeatureExtractor vmaf_fex_tad;
}

namespace
{

constexpr unsigned kMaxRustExtractors = 32;
constexpr std::size_t kMaxOptions = 64;

struct TwinSlot {
    VmafFeatureExtractor fex;
    const VmafxRsTwin *twin;
};

/* Built once under g_once, read-only afterwards. */
std::array<TwinSlot, kMaxRustExtractors> g_slots{};
unsigned g_count = 0;
std::once_flag g_once;

struct Instance {
    const VmafxRsTwin *twin = nullptr;
    void *state = nullptr;
    VmafDictionary *name_dict = nullptr;
    VmafFeatureCollector *fc = nullptr;
    int host_err = 0;
};

std::size_t align_up(std::size_t n)
{
    constexpr std::size_t a = alignof(std::max_align_t);
    return (n + a - 1) / a * a;
}

/* The instance pointer sits in the last pointer-sized slot of priv
 * (priv_size = align_up(C priv_size) + sizeof(Instance *)). */
Instance *load_instance(const VmafFeatureExtractor *fex)
{
    Instance *inst = nullptr;
    if (!fex->priv || fex->priv_size < sizeof(inst))
        return nullptr;
    const char *tail = static_cast<const char *>(fex->priv) + fex->priv_size - sizeof(inst);
    std::memcpy(static_cast<void *>(&inst), tail, sizeof(inst));
    return inst;
}

void store_instance(VmafFeatureExtractor *fex, Instance *inst)
{
    char *tail = static_cast<char *>(fex->priv) + fex->priv_size - sizeof(inst);
    std::memcpy(tail, static_cast<const void *>(&inst), sizeof(inst));
}

const VmafxRsTwin *twin_for(const char *name)
{
    for (unsigned i = 0; i < g_count; i++) {
        if (g_slots[i].twin && std::strcmp(g_slots[i].fex.name, name) == 0)
            return g_slots[i].twin;
    }
    return nullptr;
}

int map_status(int32_t rc, const Instance *inst, const char *message)
{
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "%s: %s\n", inst->twin->rust_name,
             message ? message : "error without a message");
    switch (rc) {
    case VMAFX_RS_E_NOTSUP:
        return -ENOTSUP;
    case VMAFX_RS_E_NOMEM:
        return -ENOMEM;
    case VMAFX_RS_E_RANGE:
        return -ERANGE;
    case VMAFX_RS_E_HOST:
        return inst->host_err < 0 ? inst->host_err : -EINVAL;
    default:
        return -EINVAL;
    }
}

int32_t record(Instance *inst, int err)
{
    if (err < 0)
        inst->host_err = err;
    return err;
}

int32_t host_emit(void *ctx, const char *feature, uint32_t index, double value)
{
    auto *inst = static_cast<Instance *>(ctx);
    return record(inst, vmaf_feature_collector_append_with_dict(inst->fc, inst->name_dict, feature,
                                                                value, index));
}

int32_t host_emit_raw(void *ctx, const char *feature, uint32_t index, double value)
{
    auto *inst = static_cast<Instance *>(ctx);
    return record(inst, vmaf_feature_collector_append(inst->fc, feature, value, index));
}

int32_t host_get(void *ctx, const char *feature, uint32_t index, double *value)
{
    auto *inst = static_cast<Instance *>(ctx);
    const VmafDictionaryEntry *entry = vmaf_dictionary_get(&inst->name_dict, feature, 0);
    return vmaf_feature_collector_get_score(inst->fc, entry ? entry->val : feature, value, index);
}

int32_t host_set_aggregate(void *ctx, const char *feature, double value)
{
    auto *inst = static_cast<Instance *>(ctx);
    return record(inst, vmaf_feature_collector_set_aggregate(inst->fc, feature, value));
}

void host_log(void *ctx, int32_t level, const char *message)
{
    (void)ctx;
    const bool known = level >= VMAF_LOG_LEVEL_ERROR && level <= VMAF_LOG_LEVEL_DEBUG;
    vmaf_log(known ? static_cast<enum VmafLogLevel>(level) : VMAF_LOG_LEVEL_ERROR, "%s\n",
             message ? message : "");
}

VmafxRsHost make_host(Instance *inst)
{
    return VmafxRsHost{
        .ctx = inst,
        .emit = host_emit,
        .emit_raw = host_emit_raw,
        .get = host_get,
        .set_aggregate = host_set_aggregate,
        .log = host_log,
    };
}

/* close(): the collector may already be gone, so every collector callback
 * fails and only the log works. */
int32_t closed_emit(void *ctx, const char *feature, uint32_t index, double value)
{
    (void)ctx;
    (void)feature;
    (void)index;
    (void)value;
    return -EINVAL;
}

int32_t closed_get(void *ctx, const char *feature, uint32_t index, double *value)
{
    (void)ctx;
    (void)feature;
    (void)index;
    (void)value;
    return -EINVAL;
}

int32_t closed_set_aggregate(void *ctx, const char *feature, double value)
{
    (void)ctx;
    (void)feature;
    (void)value;
    return -EINVAL;
}

VmafxRsHost make_close_host(Instance *inst)
{
    return VmafxRsHost{
        .ctx = inst,
        .emit = closed_emit,
        .emit_raw = closed_emit,
        .get = closed_get,
        .set_aggregate = closed_set_aggregate,
        .log = host_log,
    };
}

VmafxRsPicture to_rs_picture(const VmafPicture &p)
{
    VmafxRsPicture r{};
    r.pix_fmt = static_cast<uint32_t>(p.pix_fmt);
    r.bpc = p.bpc;
    r.n_planes = p.pix_fmt == VMAF_PIX_FMT_YUV400P ? 1U : 3U;
    for (unsigned i = 0; i < 3; i++)
        r.plane[i] =
            VmafxRsPlane{.data = p.data[i], .stride = p.stride[i], .w = p.w[i], .h = p.h[i]};
    return r;
}

/* A framework-held previous picture, or NULL while the window is empty. */
const VmafxRsPicture *picture_or_null(const VmafPicture &p, VmafxRsPicture *storage)
{
    if (!p.data[0])
        return nullptr;
    *storage = to_rs_picture(p);
    return storage;
}

/* One parsed option, read back from the priv blob by its table entry. */
int read_option(const VmafFeatureExtractor *fex, const VmafOption *opt, VmafxRsOption *out)
{
    assert(fex && fex->priv && opt && opt->name && out);
    const char *field = static_cast<const char *>(fex->priv) + opt->offset;
    *out = VmafxRsOption{.name = opt->name,
                         .kind = static_cast<uint32_t>(opt->type),
                         .b = 0,
                         .i = 0,
                         .d = 0.0,
                         .s = nullptr};
    switch (opt->type) {
    case VMAF_OPT_TYPE_BOOL: {
        bool b = false;
        std::memcpy(&b, field, sizeof(b));
        out->b = b ? 1 : 0;
        return 0;
    }
    case VMAF_OPT_TYPE_INT:
        std::memcpy(&out->i, field, sizeof(out->i));
        return 0;
    case VMAF_OPT_TYPE_DOUBLE:
        std::memcpy(&out->d, field, sizeof(out->d));
        return 0;
    case VMAF_OPT_TYPE_STRING:
        std::memcpy(static_cast<void *>(&out->s), field, sizeof(out->s));
        return 0;
    default:
        return -EINVAL;
    }
}

int read_options(const VmafFeatureExtractor *fex, std::array<VmafxRsOption, kMaxOptions> *opts,
                 std::size_t *n)
{
    *n = 0;
    if (!fex->options)
        return 0;
    for (std::size_t i = 0; fex->options[i].name; i++) {
        if (i >= kMaxOptions)
            return -EINVAL;
        const int err = read_option(fex, &fex->options[i], &(*opts)[i]);
        if (err)
            return err;
        *n = i + 1;
    }
    return 0;
}

int free_instance(Instance *inst)
{
    if (inst->state) {
        const VmafxRsHost host = make_close_host(inst);
        inst->twin->close(inst->state, &host);
    }
    const int err = inst->name_dict ? vmaf_dictionary_free(&inst->name_dict) : 0;
    delete inst;
    return err;
}

int call_rust_init(VmafFeatureExtractor *fex, Instance *inst, const VmafxRsGeometry &geom)
{
    std::array<VmafxRsOption, kMaxOptions> opts{};
    std::size_t n = 0;
    const int err = read_options(fex, &opts, &n);
    if (err)
        return err;
    const char *message = nullptr;
    const int32_t rc = inst->twin->init(&inst->state, opts.data(), n, &geom, &message);
    return rc == VMAFX_RS_OK ? 0 : map_status(rc, inst, message);
}

int twin_init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
              unsigned h)
{
    const VmafxRsTwin *twin = twin_for(fex->name);
    if (!twin || !fex->priv)
        return -EINVAL;
    auto *inst = new (std::nothrow) Instance{};
    if (!inst)
        return -ENOMEM;
    inst->twin = twin;
    inst->name_dict = vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                                    fex->options, fex->priv);
    /* The C extractor's own additions (ADR-2795), on the same option layout. */
    int dict_err = inst->name_dict ? 0 : -ENOMEM;
    if (!dict_err && fex->extend_name_dict)
        dict_err = fex->extend_name_dict(fex, &inst->name_dict);
    if (dict_err) {
        const int free_err = free_instance(inst);
        return free_err ? free_err : dict_err;
    }
    const VmafxRsGeometry geom{
        .pix_fmt = static_cast<uint32_t>(pix_fmt), .bpc = bpc, .w = w, .h = h};
    const int err = call_rust_init(fex, inst, geom);
    if (err) {
        const int free_err = free_instance(inst);
        return free_err ? free_err : err;
    }
    store_instance(fex, inst);
    return 0;
}

int twin_extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                 VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                 VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    Instance *inst = load_instance(fex);
    if (!inst || !ref_pic || !dist_pic || !feature_collector)
        return -EINVAL;
    inst->fc = feature_collector;
    const VmafxRsPicture ref = to_rs_picture(*ref_pic);
    const VmafxRsPicture dist = to_rs_picture(*dist_pic);
    VmafxRsPicture prev{};
    VmafxRsPicture prev_prev{};
    const VmafxRsFrame frame{
        .index = index,
        .ref_pic = &ref,
        .dist_pic = &dist,
        .prev_ref = picture_or_null(fex->prev_ref, &prev),
        .prev_prev_ref = picture_or_null(fex->prev_prev_ref, &prev_prev),
    };
    const VmafxRsHost host = make_host(inst);
    const char *message = nullptr;
    const int32_t rc = inst->twin->extract(inst->state, &frame, &host, &message);
    return rc == VMAFX_RS_OK ? 0 : map_status(rc, inst, message);
}

int twin_flush(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    Instance *inst = load_instance(fex);
    if (!inst || !feature_collector)
        return -EINVAL;
    inst->fc = feature_collector;
    const VmafxRsHost host = make_host(inst);
    const char *message = nullptr;
    const int32_t rc = inst->twin->flush(inst->state, &host, &message);
    if (rc == VMAFX_RS_OK)
        return 0;
    if (rc == VMAFX_RS_DONE)
        return 1;
    return map_status(rc, inst, message);
}

/* ADR-2090 / lane request MI-1: the Rust twin's own advance, never the C
 * extractor's, which would derive from the Rust scores into the C-layout priv
 * and leave the Rust flush to append every frame a second time. With worker
 * threads the engine initialises the registered context before its first
 * advance (libvmaf.c, init_shared_rust_twin()), so the instance exists here. */
int twin_advance(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    Instance *inst = load_instance(fex);
    if (!inst || !feature_collector)
        return -EINVAL;
    inst->fc = feature_collector;
    const VmafxRsHost host = make_host(inst);
    const char *message = nullptr;
    const int32_t rc = inst->twin->advance(inst->state, &host, &message);
    return rc == VMAFX_RS_OK ? 0 : map_status(rc, inst, message);
}

int twin_close(VmafFeatureExtractor *fex)
{
    Instance *inst = load_instance(fex);
    if (!inst)
        return 0;
    store_instance(fex, nullptr);
    return free_instance(inst);
}

bool twin_is_usable(const VmafxRsTwin *twin)
{
    if (twin->abi_version != VMAFX_RS_ABI_VERSION) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "Rust twin %s: ABI version %u, libvmaf expects %u; skipped\n", twin->rust_name,
                 twin->abi_version, VMAFX_RS_ABI_VERSION);
        return false;
    }
    return twin->c_name && twin->rust_name && twin->init && twin->extract && twin->flush &&
           twin->close && twin->advance;
}

/* The descriptor of a twin: the C extractor's, with the shim's callbacks. */
void add_twin(const VmafxRsTwin *twin, const VmafFeatureExtractor *c_fex)
{
    TwinSlot &slot = g_slots[g_count];
    slot.fex = *c_fex;
    slot.fex.name = twin->rust_name;
    slot.fex.init = twin_init;
    slot.fex.extract = twin_extract;
    slot.fex.flush = c_fex->flush ? twin_flush : nullptr;
    slot.fex.advance = c_fex->advance ? twin_advance : nullptr;
    slot.fex.close = twin_close;
    slot.fex.submit = nullptr;
    slot.fex.collect = nullptr;
    slot.fex.priv = nullptr;
    slot.fex.priv_size = align_up(c_fex->priv_size) + sizeof(Instance *);
    slot.fex.flags = c_fex->flags | VMAF_FEATURE_EXTRACTOR_RUST;
    slot.twin = twin;
    g_count++;
}

void build_registry()
{
    for (std::size_t i = 0; g_count + 1 < kMaxRustExtractors; i++) {
        const VmafxRsTwin *twin = vmafx_rs_twin_at(i);
        if (!twin)
            break;
        if (!twin_is_usable(twin))
            continue;
        const VmafFeatureExtractor *c_fex = vmaf_get_feature_extractor_by_name(twin->c_name);
        if (!c_fex) {
            vmaf_log(VMAF_LOG_LEVEL_DEBUG,
                     "Rust twin %s: C extractor %s is not built in; not registered\n",
                     twin->rust_name, twin->c_name);
            continue;
        }
        add_twin(twin, c_fex);
    }
    /* ADR-0707: the TAD pilot is a Rust extractor without a C counterpart. */
    assert(g_count < kMaxRustExtractors);
    g_slots[g_count].fex = vmaf_fex_tad;
    g_slots[g_count].twin = nullptr;
    g_count++;
}

VmafFeatureExtractor *rust_extractor_at(unsigned i)
{
    return i < g_count ? &g_slots[i].fex : nullptr;
}

} /* anonymous namespace */

extern "C" void vmaf_rust_twins_install(void)
{
    std::call_once(g_once, build_registry);
    vmaf_feature_extractor_install_rust_registry(rust_extractor_at);
}
