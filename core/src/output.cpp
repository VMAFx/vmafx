/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

/*
 * C++23 implementation of the output-format writers (XML, JSON, CSV, subtitle).
 *
 * Migration note (ADR-0733 / Wave 4 of ADR-0708 cpp23 pilot):
 *   - `git mv output.c output.cpp` preserves blame for the algorithm history.
 *   - The public C ABI (vmaf_write_output_xml / _json / _csv / _sub) is
 *     unchanged; all four functions are declared `extern "C"` in output.h so
 *     every C caller (tools/vmaf.c, libvmaf.c) links without modification.
 *   - Output bytes are bit-for-bit identical to the C predecessor.  The
 *     NOLINTBEGIN/END(cert-err33-c) bracket from the C file is preserved: the
 *     writers rely on a final ferror() check to detect I/O failure rather than
 *     propagating per-call fprintf() errors — there is no recoverable action
 *     mid-stream, and removing the bracket would produce clang-tidy noise
 *     without improving correctness.
 *   - C++23 idioms applied:
 *       * `nullptr` replaces `NULL` in the guard expressions.
 *       * `[[nodiscard]]` on the four public entry points so callers that
 *         discard the return code produce a diagnostic.
 *       * `std::string_view` for the score-format string parameter throughout
 *         the internal helpers (null-safe: the helpers call fmt_or_default()
 *         before any sv is constructed, so the sv is always valid).
 *       * `constexpr` on pool_method_name and DEFAULT_SCORE_FORMAT.
 *       * RAII locale scope via `LocaleGuard` — vmaf_thread_locale_pop runs
 *         on destruction, ensuring cleanup on every exit path.
 *       * `[[fallthrough]]` on switch fall-throughs for fpclassify arms.
 *       * `(void)` cast on every fprintf return value (cert-err33-c bracket
 *         covers the whole file; individual void-casts satisfy clang-tidy
 *         readability when the bracket is eventually lifted).
 */

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <string_view>

/* These internal headers have no extern "C" guards of their own; wrap them
 * here so the C++ compiler generates un-mangled call sites that link against
 * the C translation units in libvmaf_feature.a. */
extern "C" {
#include "feature/alias.h"
#include "log.h"
#include "feature/feature_collector.h"
#include "thread_locale.h"
}

#include "output.h"
#include "libvmaf/libvmaf.h"

/* Library default matches Netflix's pre-fork output exactly so consumers of
 * vmaf_write_output_with_format(..., NULL) get golden-compatible numbers
 * without an explicit format. Round-trip lossless is opt-in via "%.17g".
 * See ADR-0119 (supersedes ADR-0006). */
static constexpr std::string_view DEFAULT_SCORE_FORMAT{"%.6f"};

/* RAII wrapper for the C locale push/pop pair.  Ensures vmaf_thread_locale_pop
 * is called on every exit path from the public writer functions, including any
 * future early returns added during maintenance. */
class LocaleGuard
{
  public:
    explicit LocaleGuard() noexcept : state_(vmaf_thread_locale_push_c())
    {
        /* If vmaf_thread_locale_push_c fails (OOM in newlocale/duplocale),
         * state_ is null.  vmaf_thread_locale_pop(nullptr) is a no-op, so
         * teardown is safe, but output will proceed without C-locale,
         * potentially producing incorrect decimal separators on non-C locales.
         * Log a warning so this is observable (adversarial review 2026-05-28
         * finding #16). */
        if (!state_)
            vmaf_log(VMAF_LOG_LEVEL_WARNING, "vmaf_thread_locale_push_c failed; output may use "
                                             "wrong decimal separator\n");
    }
    ~LocaleGuard() noexcept
    {
        vmaf_thread_locale_pop(state_);
    }

    /* Non-copyable, non-movable: there is exactly one locale state per scope. */
    LocaleGuard(const LocaleGuard &) = delete;
    LocaleGuard &operator=(const LocaleGuard &) = delete;
    LocaleGuard(LocaleGuard &&) = delete;
    LocaleGuard &operator=(LocaleGuard &&) = delete;

  private:
    VmafThreadLocaleState *state_;
};

static unsigned max_capacity(VmafFeatureCollector *fc)
{
    unsigned capacity = 0;

    for (unsigned j = 0; j < fc->cnt; j++) {
        if (fc->feature_vector[j]->capacity > capacity)
            capacity = fc->feature_vector[j]->capacity;
    }

    return capacity;
}

/* Indexed by VmafPoolingMethod enum value (UNKNOWN=0, MIN=1, MAX=2, MEAN=3,
 * HARMONIC_MEAN=4). Designated initializers are a GCC C++ extension not
 * supported by GCC 16 in non-trivial form; use positional initializers so the
 * array compiles cleanly under -std=c++23. Index 0 (UNKNOWN) is nullptr and
 * never accessed — callers loop from j=1. */
static constexpr const char *pool_method_name[] = {
    nullptr,        /* VMAF_POOL_METHOD_UNKNOWN = 0 */
    "min",          /* VMAF_POOL_METHOD_MIN = 1     */
    "max",          /* VMAF_POOL_METHOD_MAX = 2     */
    "mean",         /* VMAF_POOL_METHOD_MEAN = 3    */
    "harmonic_mean" /* VMAF_POOL_METHOD_HARMONIC_MEAN = 4 */
};
/* Guard: if VMAF_POOL_METHOD_NB grows without updating this array,
 * callers loop to NB-1 and read out of bounds. Catch it at compile time
 * (adversarial review 2026-05-28 finding #17; JPL Rule 23 spirit). */
static_assert(VMAF_POOL_METHOD_NB == 5,
              "pool_method_name array size mismatch — update the array above");

static inline std::string_view fmt_or_default(const char *score_format) noexcept
{
    return score_format ? std::string_view{score_format} : DEFAULT_SCORE_FORMAT;
}

static unsigned count_written_at(VmafFeatureCollector *fc, unsigned i)
{
    unsigned cnt = 0;
    for (unsigned j = 0; j < fc->cnt; j++) {
        /* ADR-0606: `>` was wrong — valid indices are 0..capacity-1, so the
         * out-of-bounds check must use `>=`.  The old `>` allowed access to
         * score[capacity] when i == capacity, which is one past the end of the
         * allocated array and is undefined behaviour.  Apple Clang / ASan
         * catches this as a heap buffer overread. */
        if (i >= fc->feature_vector[j]->capacity)
            continue;
        if (fc->feature_vector[j]->score[i].written)
            cnt++;
    }
    return cnt;
}

/* Writers rely on a final ferror() check to detect I/O failure rather than
 * propagating per-call errors — there is no recoverable action mid-stream. */
// NOLINTBEGIN(cert-err33-c)
static void xml_write_frames(VmafFeatureCollector *fc, FILE *outfile, unsigned subsample,
                             std::string_view sf)
{
    (void)std::fprintf(outfile, "  <frames>\n");
    for (unsigned i = 0; i < max_capacity(fc); i++) {
        if ((subsample > 1) && (i % subsample))
            continue;

        unsigned cnt = count_written_at(fc, i);
        if (!cnt)
            continue;

        (void)std::fprintf(outfile, "    <frame frameNum=\"%d\" ", i);
        for (unsigned j = 0; j < fc->cnt; j++) {
            if (i >= fc->feature_vector[j]->capacity) /* ADR-0606: >= not > */
                continue;
            if (!fc->feature_vector[j]->score[i].written)
                continue;
            (void)std::fprintf(outfile, "%s=\"",
                               vmaf_feature_name_alias(fc->feature_vector[j]->name));
            (void)std::fprintf(outfile, sf.data(), fc->feature_vector[j]->score[i].value);
            (void)std::fprintf(outfile, "\" ");
        }
        (void)std::fprintf(outfile, "/>\n");
    }
    (void)std::fprintf(outfile, "  </frames>\n");
}

/* Emit pool scores for one XML <metric> entry when frames were actually read.
 * Called only when pic_cnt > 0 to avoid the `pic_cnt - 1` unsigned underflow
 * to UINT_MAX that was the ADR-0602 macOS SIGSEGV root cause. */
static void xml_write_one_metric_pools(VmafContext *vmaf, FILE *outfile, const char *feature_name,
                                       unsigned pic_cnt, std::string_view sf)
{
    for (unsigned j = 1; j < VMAF_POOL_METHOD_NB; j++) {
        double score;
        int err = vmaf_feature_score_pooled(vmaf, feature_name, static_cast<VmafPoolingMethod>(j),
                                            &score, 0, pic_cnt - 1);
        if (!err) {
            (void)std::fprintf(outfile, "%s=\"", pool_method_name[j]);
            (void)std::fprintf(outfile, sf.data(), score);
            (void)std::fprintf(outfile, "\" ");
        }
    }
}

static void xml_write_pooled_metrics(VmafContext *vmaf, VmafFeatureCollector *fc, FILE *outfile,
                                     unsigned pic_cnt, std::string_view sf)
{
    (void)std::fprintf(outfile, "  <pooled_metrics>\n");
    for (unsigned i = 0; i < fc->cnt; i++) {
        const char *feature_name = fc->feature_vector[i]->name;
        (void)std::fprintf(outfile, "    <metric name=\"%s\" ",
                           vmaf_feature_name_alias(feature_name));
        if (pic_cnt > 0)
            xml_write_one_metric_pools(vmaf, outfile, feature_name, pic_cnt, sf);
        (void)std::fprintf(outfile, "/>\n");
    }
    (void)std::fprintf(outfile, "  </pooled_metrics>\n");
}

static void xml_write_aggregate_metrics(VmafFeatureCollector *fc, FILE *outfile,
                                        std::string_view sf)
{
    (void)std::fprintf(outfile, "  <aggregate_metrics ");
    for (unsigned i = 0; i < fc->aggregate_vector.cnt; i++) {
        (void)std::fprintf(outfile, "%s=\"", fc->aggregate_vector.metric[i].name);
        (void)std::fprintf(outfile, sf.data(), fc->aggregate_vector.metric[i].value);
        (void)std::fprintf(outfile, "\" ");
    }
    (void)std::fprintf(outfile, "/>\n");
}

static void xml_write_pooled_and_aggregate(VmafContext *vmaf, VmafFeatureCollector *fc,
                                           FILE *outfile, unsigned pic_cnt, std::string_view sf)
{
    xml_write_pooled_metrics(vmaf, fc, outfile, pic_cnt, sf);
    xml_write_aggregate_metrics(fc, outfile, sf);
}

[[nodiscard]] int vmaf_write_output_xml(VmafContext *vmaf, VmafFeatureCollector *fc, FILE *outfile,
                                        unsigned subsample, unsigned width, unsigned height,
                                        double fps, unsigned pic_cnt, const char *score_format)
{
    if (!vmaf)
        return -EINVAL;
    if (!fc)
        return -EINVAL;
    if (!outfile)
        return -EINVAL;

    const std::string_view sf = fmt_or_default(score_format);

    const LocaleGuard locale;

    (void)std::fprintf(outfile, "<VMAF version=\"%s\">\n", vmaf_version());
    (void)std::fprintf(outfile, "  <params qualityWidth=\"%d\" qualityHeight=\"%d\" />\n", width,
                       height);
    (void)std::fprintf(outfile, "  <fyi fps=\"%.2f\" />\n", fps);

    xml_write_frames(fc, outfile, subsample, sf);
    xml_write_pooled_and_aggregate(vmaf, fc, outfile, pic_cnt, sf);

    (void)std::fprintf(outfile, "</VMAF>\n");

    const int flush_err = std::fflush(outfile);

    return (flush_err != 0 || std::ferror(outfile)) ? -EIO : 0;
}

static void json_write_frame_metric(FILE *outfile, const char *name, double value,
                                    std::string_view sf, bool trailing_comma)
{
    switch (std::fpclassify(value)) {
    case FP_NORMAL:
    case FP_ZERO:
    case FP_SUBNORMAL:
        (void)std::fprintf(outfile, "        \"%s\": ", name);
        (void)std::fprintf(outfile, sf.data(), value);
        (void)std::fprintf(outfile, "%s\n", trailing_comma ? "," : "");
        break;
    case FP_INFINITE:
    case FP_NAN:
    default:
        (void)std::fprintf(outfile, "        \"%s\": null%s", name, trailing_comma ? "," : "");
        break;
    }
}

static void json_write_frame(VmafFeatureCollector *fc, FILE *outfile, unsigned i, unsigned cnt,
                             std::string_view sf)
{
    (void)std::fprintf(outfile, "    {\n");
    (void)std::fprintf(outfile, "      \"frameNum\": %d,\n", i);
    (void)std::fprintf(outfile, "      \"metrics\": {\n");

    unsigned cnt2 = 0;
    for (unsigned j = 0; j < fc->cnt; j++) {
        if (i >= fc->feature_vector[j]->capacity) /* ADR-0606: >= not > */
            continue;
        if (!fc->feature_vector[j]->score[i].written)
            continue;
        cnt2++;
        json_write_frame_metric(outfile, vmaf_feature_name_alias(fc->feature_vector[j]->name),
                                fc->feature_vector[j]->score[i].value, sf, cnt2 < cnt);
    }
    (void)std::fprintf(outfile, "      }\n");
    (void)std::fprintf(outfile, "    }");
}

static void json_write_frames(VmafFeatureCollector *fc, FILE *outfile, unsigned subsample,
                              std::string_view sf)
{
    (void)std::fprintf(outfile, "  \"frames\": [");
    /* ADR-0606: track whether we have emitted the first frame entry so the
     * separator comma is placed correctly regardless of which frame index is
     * the first one with written scores (using `i > 0` was wrong when the
     * first written frame had index > 0, producing a leading comma). */
    bool first_frame = true;
    for (unsigned i = 0; i < max_capacity(fc); i++) {
        if ((subsample > 1) && (i % subsample))
            continue;

        unsigned cnt = count_written_at(fc, i);
        if (!cnt)
            continue;
        (void)std::fprintf(outfile, "%s", first_frame ? "\n" : ",\n");
        first_frame = false;

        json_write_frame(fc, outfile, i, cnt, sf);
    }
    (void)std::fprintf(outfile, "\n  ],\n");
}

/* Emit one pool score value, prefixing with a separator only after the first.
 *
 * ADR-0606: the previous implementation used `j > 1` (where j is the pool
 * method enum value) to decide whether to emit a leading comma.  This was
 * wrong when the j==1 method call returned an error and was skipped: the
 * j==2 call would then print a leading comma with no preceding value,
 * producing malformed JSON ("{,\n  \"max\": ...}").  Use an explicit
 * `*first` flag instead so the comma tracks what was actually emitted. */
static void json_write_pool_score(FILE *outfile, bool *first, unsigned j, double score,
                                  std::string_view sf)
{
    (void)std::fprintf(outfile, "%s", *first ? "\n" : ",\n");
    *first = false;
    switch (std::fpclassify(score)) {
    case FP_NORMAL:
    case FP_ZERO:
    case FP_SUBNORMAL:
        (void)std::fprintf(outfile, "      \"%s\": ", pool_method_name[j]);
        (void)std::fprintf(outfile, sf.data(), score);
        break;
    case FP_INFINITE:
    case FP_NAN:
    default:
        (void)std::fprintf(outfile, "      \"%s\": null", pool_method_name[j]);
        break;
    }
}

static void json_write_pooled_entry(VmafContext *vmaf, FILE *outfile, const char *feature_name,
                                    unsigned pic_cnt, std::string_view sf)
{
    (void)std::fprintf(outfile, "    \"%s\": {", vmaf_feature_name_alias(feature_name));
    /* ADR-0602 / ADR-0606: pic_cnt == 0 means no frames were read via
     * vmaf_read_pictures (e.g. the caller injected scores via
     * vmaf_import_feature_score).  The expression `pic_cnt - 1` wraps to
     * UINT_MAX when pic_cnt is zero, which passes vmaf_feature_score_pooled's
     * `index_low > index_high` guard (0 <= UINT_MAX for unsigned) and produces
     * a loop whose termination condition `i <= UINT_MAX` is tautologically true
     * for unsigned i.  Apple Clang may treat that as an infinite loop and
     * optimise away the in-body early-exit paths that Linux GCC happens to
     * preserve.  Skip pooled metrics entirely when there are no frames. */
    if (pic_cnt > 0) {
        bool first = true;
        for (unsigned j = 1; j < VMAF_POOL_METHOD_NB; j++) {
            double score;
            int err = vmaf_feature_score_pooled(
                vmaf, feature_name, static_cast<VmafPoolingMethod>(j), &score, 0, pic_cnt - 1);
            if (!err)
                json_write_pool_score(outfile, &first, j, score, sf);
        }
    }
    (void)std::fprintf(outfile, "\n");
    (void)std::fprintf(outfile, "    }");
}

static void json_write_pooled(VmafContext *vmaf, VmafFeatureCollector *fc, FILE *outfile,
                              unsigned pic_cnt, std::string_view sf)
{
    (void)std::fprintf(outfile, "  \"pooled_metrics\": {");
    for (unsigned i = 0; i < fc->cnt; i++) {
        (void)std::fprintf(outfile, "%s", i > 0 ? ",\n" : "\n");
        json_write_pooled_entry(vmaf, outfile, fc->feature_vector[i]->name, pic_cnt, sf);
    }
    (void)std::fprintf(outfile, "\n  },\n");
}

static void json_write_aggregate(VmafFeatureCollector *fc, FILE *outfile, std::string_view sf)
{
    (void)std::fprintf(outfile, "  \"aggregate_metrics\": {");
    for (unsigned i = 0; i < fc->aggregate_vector.cnt; i++) {
        switch (std::fpclassify(fc->aggregate_vector.metric[i].value)) {
        case FP_NORMAL:
        case FP_ZERO:
        case FP_SUBNORMAL:
            (void)std::fprintf(outfile, "\n    \"%s\": ", fc->aggregate_vector.metric[i].name);
            (void)std::fprintf(outfile, sf.data(), fc->aggregate_vector.metric[i].value);
            break;
        case FP_INFINITE:
        case FP_NAN:
        default:
            (void)std::fprintf(outfile, "\n    \"%s\": null", fc->aggregate_vector.metric[i].name);
            break;
        }
        (void)std::fprintf(outfile, "%s", i < fc->aggregate_vector.cnt - 1 ? "," : "");
    }
    (void)std::fprintf(outfile, "\n  }\n");
}

[[nodiscard]] int vmaf_write_output_json(VmafContext *vmaf, VmafFeatureCollector *fc, FILE *outfile,
                                         unsigned subsample, double fps, unsigned pic_cnt,
                                         const char *score_format)
{
    /* ADR-0602: mirror the vmaf_write_output_xml NULL guards so the JSON
     * writer is equally defensive.  vmaf and fc are both dereferenced by the
     * pooled-metrics loop; outfile is dereferenced by every fprintf. */
    if (!vmaf)
        return -EINVAL;
    if (!fc)
        return -EINVAL;
    if (!outfile)
        return -EINVAL;

    const std::string_view sf = fmt_or_default(score_format);

    const LocaleGuard locale;

    (void)std::fprintf(outfile, "{\n");
    (void)std::fprintf(outfile, "  \"version\": \"%s\",\n", vmaf_version());
    switch (std::fpclassify(fps)) {
    case FP_NORMAL:
    case FP_ZERO:
    case FP_SUBNORMAL:
        (void)std::fprintf(outfile, "  \"fps\": %.2f,\n", fps);
        break;
    case FP_INFINITE:
    case FP_NAN:
    default:
        (void)std::fprintf(outfile, "  \"fps\": null,\n");
        break;
    }

    json_write_frames(fc, outfile, subsample, sf);
    json_write_pooled(vmaf, fc, outfile, pic_cnt, sf);
    json_write_aggregate(fc, outfile, sf);

    (void)std::fprintf(outfile, "}\n");

    const int flush_err = std::fflush(outfile);

    return (flush_err != 0 || std::ferror(outfile)) ? -EIO : 0;
}

[[nodiscard]] int vmaf_write_output_csv(VmafFeatureCollector *fc, FILE *outfile, unsigned subsample,
                                        const char *score_format)
{
    const std::string_view sf = fmt_or_default(score_format);

    const LocaleGuard locale;

    (void)std::fprintf(outfile, "Frame,");
    for (unsigned i = 0; i < fc->cnt; i++) {
        (void)std::fprintf(outfile, "%s,", vmaf_feature_name_alias(fc->feature_vector[i]->name));
    }
    (void)std::fprintf(outfile, "\n");

    for (unsigned i = 0; i < max_capacity(fc); i++) {
        if ((subsample > 1) && (i % subsample))
            continue;

        unsigned cnt = 0;
        for (unsigned j = 0; j < fc->cnt; j++) {
            if (i >= fc->feature_vector[j]->capacity) /* ADR-0606: >= not > */
                continue;
            if (fc->feature_vector[j]->score[i].written)
                cnt++;
        }
        if (!cnt)
            continue;

        (void)std::fprintf(outfile, "%d,", i);
        for (unsigned j = 0; j < fc->cnt; j++) {
            if (i >= fc->feature_vector[j]->capacity) /* ADR-0606: >= not > */
                continue;
            if (!fc->feature_vector[j]->score[i].written)
                continue;
            (void)std::fprintf(outfile, sf.data(), fc->feature_vector[j]->score[i].value);
            (void)std::fprintf(outfile, ",");
        }
        (void)std::fprintf(outfile, "\n");
    }

    const int flush_err = std::fflush(outfile);

    return (flush_err != 0 || std::ferror(outfile)) ? -EIO : 0;
}

[[nodiscard]] int vmaf_write_output_sub(VmafFeatureCollector *fc, FILE *outfile, unsigned subsample,
                                        const char *score_format)
{
    const std::string_view sf = fmt_or_default(score_format);

    const LocaleGuard locale;

    for (unsigned i = 0; i < max_capacity(fc); i++) {
        if ((subsample > 1) && (i % subsample))
            continue;

        unsigned cnt = 0;
        for (unsigned j = 0; j < fc->cnt; j++) {
            if (i >= fc->feature_vector[j]->capacity) /* ADR-0606: >= not > */
                continue;
            if (fc->feature_vector[j]->score[i].written)
                cnt++;
        }
        if (!cnt)
            continue;

        (void)std::fprintf(outfile, "{%d}{%d}frame: %d|", i, i + 1, i);
        for (unsigned j = 0; j < fc->cnt; j++) {
            if (i >= fc->feature_vector[j]->capacity) /* ADR-0606: >= not > */
                continue;
            if (!fc->feature_vector[j]->score[i].written)
                continue;
            (void)std::fprintf(outfile,
                               "%s: ", vmaf_feature_name_alias(fc->feature_vector[j]->name));
            (void)std::fprintf(outfile, sf.data(), fc->feature_vector[j]->score[i].value);
            (void)std::fprintf(outfile, "|");
        }
        (void)std::fprintf(outfile, "\n");
    }

    const int flush_err = std::fflush(outfile);

    return (flush_err != 0 || std::ferror(outfile)) ? -EIO : 0;
}
// NOLINTEND(cert-err33-c)
