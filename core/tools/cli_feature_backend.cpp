/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * `--feature` backend routing and the backend receipt for the vmaf CLI
 * (ADR-1359).
 */

#include "cli_feature_backend.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace
{

void format_twin_warning(char *warn, size_t warn_sz, const CliFeatureChoice &choice,
                         const char *backend, const char *feature_name, const char *option,
                         const VmafPictureConfiguration *pic_cfg)
{
    int written = 0;
    if (choice.status == -ENOENT) {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: the %s backend has no twin of this "
                           "extractor; computing it on the CPU\n",
                           feature_name, backend);
    } else if (choice.status == -ENOTSUP && option) {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: %s cannot honour option '%s'; "
                           "computing it on the CPU\n",
                           feature_name, choice.twin_name, option);
    } else if (choice.status == -ENOTSUP && pic_cfg) {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: %s cannot run %ux%u %u-bit pictures "
                           "with these options; computing it on the CPU\n",
                           feature_name, choice.twin_name, pic_cfg->pic_params.w,
                           pic_cfg->pic_params.h, pic_cfg->pic_params.bpc);
    } else if (choice.status == -ENODEV) {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: %s feature extraction is disabled "
                           "(non-zero --gpumask); computing it on the CPU\n",
                           feature_name, backend);
    } else {
        written = snprintf(warn, warn_sz,
                           "vmaf: warning: --feature %s: could not check for a %s twin "
                           "(error %d); computing it on the CPU\n",
                           feature_name, backend, choice.status);
    }
    if (written < 0)
        warn[0] = '\0';
}

} // namespace

bool cli_backend_is_device(const char *backend)
{
    return backend && strcmp(backend, "auto") != 0 && strcmp(backend, "cpu") != 0;
}

CliFeatureChoice cli_choose_feature_extractor(VmafContext *vmaf, const char *backend,
                                              const char *feature_name,
                                              const VmafFeatureDictionary *opts_dict,
                                              const VmafPictureConfiguration *pic_cfg, char *warn,
                                              size_t warn_sz)
{
    if (warn && warn_sz > 0)
        warn[0] = '\0';
    if (!cli_backend_is_device(backend))
        return CliFeatureChoice{.extractor = feature_name, .twin_name = nullptr, .status = 0};
    const char *twin = nullptr;
    const char *option = nullptr;
    const int status =
        vmaf_feature_backend_twin(vmaf, feature_name, opts_dict, pic_cfg, &twin, &option);
    const CliFeatureChoice choice = {
        .extractor = status == 0 && twin ? twin : feature_name,
        .twin_name = twin,
        .status = status,
    };
    /* -EINVAL: the name is not a CPU extractor (a twin name or an unknown
     * name) or an option value does not parse. Registering the name as given
     * either works or reports that error, so a warning would only add noise. */
    if (choice.extractor == feature_name && status != -EINVAL && warn && warn_sz > 0)
        format_twin_warning(warn, warn_sz, choice, backend, feature_name, option, pic_cfg);
    return choice;
}

int cli_collect_extractor_report(VmafContext *vmaf, CliExtractorReport *report)
{
    if (!report)
        return -EINVAL;
    report->cnt = 0;
    for (unsigned i = 0; i < CLI_FEATURE_REPORT_MAX; i++) {
        const char *name = nullptr;
        enum VmafBackend backend = VMAF_BACKEND_UNKNOWN;
        const int err = vmaf_registered_feature_extractor(vmaf, i, &name, &backend);
        if (err == -ENOENT)
            return 0;
        if (err)
            return err;
        report->name[i] = name;
        report->backend[i] = backend;
        report->cnt = i + 1;
    }
    return 0;
}

const char *cli_backend_label(enum VmafBackend backend)
{
    switch (backend) {
    case VMAF_BACKEND_CUDA:
        return "cuda";
    case VMAF_BACKEND_SYCL:
        return "sycl";
    case VMAF_BACKEND_HIP:
        return "hip";
    case VMAF_BACKEND_METAL:
        return "metal";
    case VMAF_BACKEND_VULKAN:
        return "vulkan";
    case VMAF_BACKEND_UNKNOWN:
    default:
        return "cpu";
    }
}

const char *cli_report_backend_used(const CliExtractorReport *report)
{
    if (!report)
        return "cpu";
    for (unsigned i = 0; i < report->cnt && i < CLI_FEATURE_REPORT_MAX; i++) {
        if (report->backend[i] != VMAF_BACKEND_UNKNOWN)
            return cli_backend_label(report->backend[i]);
    }
    return "cpu";
}

namespace
{

/* snprintf-style appender: counts every byte, stores what fits. */
struct TextSink {
    char *buf;
    size_t sz;
    size_t len;
};

void sink_put(TextSink *sink, char ch)
{
    if (sink->buf && sink->len + 1 < sink->sz)
        sink->buf[sink->len] = ch;
    sink->len++;
}

void sink_puts(TextSink *sink, const char *text)
{
    constexpr size_t max_text = 4096;
    const size_t text_len = strnlen(text, max_text);
    for (size_t i = 0; i < text_len; i++)
        sink_put(sink, text[i]);
}

/* Registry names are C identifiers. Anything else is replaced so the receipt
 * stays valid JSON without an escaper. */
void sink_put_name(TextSink *sink, const char *name)
{
    sink_put(sink, '"');
    constexpr size_t max_name = 256;
    for (size_t i = 0; name && i < max_name && name[i]; i++) {
        const char ch = name[i];
        const bool plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
        sink_put(sink, plain ? ch : '_');
    }
    sink_put(sink, '"');
}

} // namespace

size_t cli_format_backend_members(const CliExtractorReport *report, char *buf, size_t sz)
{
    TextSink sink = {.buf = buf, .sz = sz, .len = 0};
    sink_puts(&sink, "\"backend_used\": ");
    sink_put_name(&sink, cli_report_backend_used(report));
    sink_puts(&sink, ", \"feature_backends\": [");
    const unsigned cnt = report ? report->cnt : 0;
    for (unsigned i = 0; i < cnt && i < CLI_FEATURE_REPORT_MAX; i++) {
        sink_puts(&sink, i ? ", {\"extractor\": " : "{\"extractor\": ");
        sink_put_name(&sink, report->name[i]);
        sink_puts(&sink, ", \"backend\": ");
        sink_put_name(&sink, cli_backend_label(report->backend[i]));
        sink_put(&sink, '}');
    }
    sink_put(&sink, ']');
    if (buf && sz > 0)
        buf[sink.len < sz ? sink.len : sz - 1] = '\0';
    return sink.len;
}
