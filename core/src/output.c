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

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "feature/alias.h"
#include "feature/feature_collector.h"
#include "output.h"
#include "thread_locale.h"

#include "libvmaf/libvmaf.h"

/* Library default matches Netflix's pre-fork output exactly so consumers of
 * vmaf_write_output_with_format(..., NULL) get golden-compatible numbers
 * without an explicit format. Round-trip lossless is opt-in via "%.17g".
 * See ADR-0119 (supersedes ADR-0006). */
#define DEFAULT_SCORE_FORMAT "%.6f"

static unsigned max_capacity(VmafFeatureCollector *fc)
{
    unsigned capacity = 0;

    for (unsigned j = 0; j < fc->cnt; j++) {
        if (fc->feature_vector[j]->capacity > capacity)
            capacity = fc->feature_vector[j]->capacity;
    }

    return capacity;
}

static const char *pool_method_name[] = {
    [VMAF_POOL_METHOD_MIN] = "min",
    [VMAF_POOL_METHOD_MAX] = "max",
    [VMAF_POOL_METHOD_MEAN] = "mean",
    [VMAF_POOL_METHOD_HARMONIC_MEAN] = "harmonic_mean",
};

static inline const char *fmt_or_default(const char *score_format)
{
    return score_format ? score_format : DEFAULT_SCORE_FORMAT;
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
 * propagating per-call errors — there is no recoverable action mid-stream.
 * Output-format writer pattern per ADR-0141 §2; ADR-0278 cite form. */
// NOLINTBEGIN(cert-err33-c) — ADR-0141 / ADR-0278
static void xml_write_frames(VmafFeatureCollector *fc, FILE *outfile, unsigned subsample,
                             const char *sf)
{
    fprintf(outfile, "  <frames>\n");
    for (unsigned i = 0; i < max_capacity(fc); i++) {
        if ((subsample > 1) && (i % subsample))
            continue;

        unsigned cnt = count_written_at(fc, i);
        if (!cnt)
            continue;

        fprintf(outfile, "    <frame frameNum=\"%d\" ", i);
        for (unsigned j = 0; j < fc->cnt; j++) {
            if (i >= fc->feature_vector[j]->capacity) /* ADR-0606: >= not > */
                continue;
            if (!fc->feature_vector[j]->score[i].written)
                continue;
            fprintf(outfile, "%s=\"", vmaf_feature_name_alias(fc->feature_vector[j]->name));
            fprintf(outfile, sf, fc->feature_vector[j]->score[i].value);
            fprintf(outfile, "\" ");
        }
        fprintf(outfile, "/>\n");
    }
    fprintf(outfile, "  </frames>\n");
}

/* Emit pool scores for one XML <metric> entry when frames were actually read.
 * Called only when pic_cnt > 0 to avoid the `pic_cnt - 1` unsigned underflow
 * to UINT_MAX that was the ADR-0602 macOS SIGSEGV root cause. */
static void xml_write_one_metric_pools(VmafContext *vmaf, FILE *outfile, const char *feature_name,
                                       unsigned pic_cnt, const char *sf)
{
    for (unsigned j = 1; j < VMAF_POOL_METHOD_NB; j++) {
        double score;
        int err = vmaf_feature_score_pooled(vmaf, feature_name, j, &score, 0, pic_cnt - 1);
        if (!err) {
            fprintf(outfile, "%s=\"", pool_method_name[j]);
            fprintf(outfile, sf, score);
            fprintf(outfile, "\" ");
        }
    }
}

static void xml_write_pooled_metrics(VmafContext *vmaf, VmafFeatureCollector *fc, FILE *outfile,
                                     unsigned pic_cnt, const char *sf)
{
    fprintf(outfile, "  <pooled_metrics>\n");
    for (unsigned i = 0; i < fc->cnt; i++) {
        const char *feature_name = fc->feature_vector[i]->name;
        fprintf(outfile, "    <metric name=\"%s\" ", vmaf_feature_name_alias(feature_name));
        if (pic_cnt > 0)
            xml_write_one_metric_pools(vmaf, outfile, feature_name, pic_cnt, sf);
        fprintf(outfile, "/>\n");
    }
    fprintf(outfile, "  </pooled_metrics>\n");
}

static void xml_write_aggregate_metrics(VmafFeatureCollector *fc, FILE *outfile, const char *sf)
{
    fprintf(outfile, "  <aggregate_metrics ");
    for (unsigned i = 0; i < fc->aggregate_vector.cnt; i++) {
        fprintf(outfile, "%s=\"", fc->aggregate_vector.metric[i].name);
        fprintf(outfile, sf, fc->aggregate_vector.metric[i].value);
        fprintf(outfile, "\" ");
    }
    fprintf(outfile, "/>\n");
}

static void xml_write_pooled_and_aggregate(VmafContext *vmaf, VmafFeatureCollector *fc,
                                           FILE *outfile, unsigned pic_cnt, const char *sf)
{
    xml_write_pooled_metrics(vmaf, fc, outfile, pic_cnt, sf);
    xml_write_aggregate_metrics(fc, outfile, sf);
}

int vmaf_write_output_xml(VmafContext *vmaf, VmafFeatureCollector *fc, FILE *outfile,
                          unsigned subsample, unsigned width, unsigned height, double fps,
                          unsigned pic_cnt, const char *score_format)
{
    if (!vmaf)
        return -EINVAL;
    if (!fc)
        return -EINVAL;
    if (!outfile)
        return -EINVAL;

    const char *sf = fmt_or_default(score_format);

    VmafThreadLocaleState *locale_state = vmaf_thread_locale_push_c();

    fprintf(outfile, "<VMAF version=\"%s\">\n", vmaf_version());
    fprintf(outfile, "  <params qualityWidth=\"%d\" qualityHeight=\"%d\" />\n", width, height);
    fprintf(outfile, "  <fyi fps=\"%.2f\" />\n", fps);

    xml_write_frames(fc, outfile, subsample, sf);
    xml_write_pooled_and_aggregate(vmaf, fc, outfile, pic_cnt, sf);

    fprintf(outfile, "</VMAF>\n");

    const int flush_err = fflush(outfile);
    vmaf_thread_locale_pop(locale_state);

    return (flush_err != 0 || ferror(outfile)) ? -EIO : 0;
}

static void json_write_frame_metric(FILE *outfile, const char *name, double value, const char *sf,
                                    bool trailing_comma)
{
    switch (fpclassify(value)) {
    case FP_NORMAL:
    case FP_ZERO:
    case FP_SUBNORMAL:
        fprintf(outfile, "        \"%s\": ", name);
        fprintf(outfile, sf, value);
        fprintf(outfile, "%s\n", trailing_comma ? "," : "");
        break;
    case FP_INFINITE:
    case FP_NAN:
    default:
        fprintf(outfile, "        \"%s\": null%s", name, trailing_comma ? "," : "");
        break;
    }
}

static void json_write_frame(VmafFeatureCollector *fc, FILE *outfile, unsigned i, unsigned cnt,
                             const char *sf)
{
    fprintf(outfile, "    {\n");
    fprintf(outfile, "      \"frameNum\": %d,\n", i);
    fprintf(outfile, "      \"metrics\": {\n");

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
    fprintf(outfile, "      }\n");
    fprintf(outfile, "    }");
}

static void json_write_frames(VmafFeatureCollector *fc, FILE *outfile, unsigned subsample,
                              const char *sf)
{
    fprintf(outfile, "  \"frames\": [");
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
        fprintf(outfile, "%s", first_frame ? "\n" : ",\n");
        first_frame = false;

        json_write_frame(fc, outfile, i, cnt, sf);
    }
    fprintf(outfile, "\n  ],\n");
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
                                  const char *sf)
{
    fprintf(outfile, "%s", *first ? "\n" : ",\n");
    *first = false;
    switch (fpclassify(score)) {
    case FP_NORMAL:
    case FP_ZERO:
    case FP_SUBNORMAL:
        fprintf(outfile, "      \"%s\": ", pool_method_name[j]);
        fprintf(outfile, sf, score);
        break;
    case FP_INFINITE:
    case FP_NAN:
    default:
        fprintf(outfile, "      \"%s\": null", pool_method_name[j]);
        break;
    }
}

static void json_write_pooled_entry(VmafContext *vmaf, FILE *outfile, const char *feature_name,
                                    unsigned pic_cnt, const char *sf)
{
    fprintf(outfile, "    \"%s\": {", vmaf_feature_name_alias(feature_name));
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
            int err = vmaf_feature_score_pooled(vmaf, feature_name, j, &score, 0, pic_cnt - 1);
            if (!err)
                json_write_pool_score(outfile, &first, j, score, sf);
        }
    }
    fprintf(outfile, "\n");
    fprintf(outfile, "    }");
}

static void json_write_pooled(VmafContext *vmaf, VmafFeatureCollector *fc, FILE *outfile,
                              unsigned pic_cnt, const char *sf)
{
    fprintf(outfile, "  \"pooled_metrics\": {");
    for (unsigned i = 0; i < fc->cnt; i++) {
        fprintf(outfile, "%s", i > 0 ? ",\n" : "\n");
        json_write_pooled_entry(vmaf, outfile, fc->feature_vector[i]->name, pic_cnt, sf);
    }
    fprintf(outfile, "\n  },\n");
}

static void json_write_aggregate(VmafFeatureCollector *fc, FILE *outfile, const char *sf)
{
    fprintf(outfile, "  \"aggregate_metrics\": {");
    for (unsigned i = 0; i < fc->aggregate_vector.cnt; i++) {
        switch (fpclassify(fc->aggregate_vector.metric[i].value)) {
        case FP_NORMAL:
        case FP_ZERO:
        case FP_SUBNORMAL:
            fprintf(outfile, "\n    \"%s\": ", fc->aggregate_vector.metric[i].name);
            fprintf(outfile, sf, fc->aggregate_vector.metric[i].value);
            break;
        case FP_INFINITE:
        case FP_NAN:
        default:
            fprintf(outfile, "\n    \"%s\": null", fc->aggregate_vector.metric[i].name);
            break;
        }
        fprintf(outfile, "%s", i < fc->aggregate_vector.cnt - 1 ? "," : "");
    }
    fprintf(outfile, "\n  }\n");
}

int vmaf_write_output_json(VmafContext *vmaf, VmafFeatureCollector *fc, FILE *outfile,
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

    const char *sf = fmt_or_default(score_format);

    VmafThreadLocaleState *locale_state = vmaf_thread_locale_push_c();

    fprintf(outfile, "{\n");
    fprintf(outfile, "  \"version\": \"%s\",\n", vmaf_version());
    switch (fpclassify(fps)) {
    case FP_NORMAL:
    case FP_ZERO:
    case FP_SUBNORMAL:
        fprintf(outfile, "  \"fps\": %.2f,\n", fps);
        break;
    case FP_INFINITE:
    case FP_NAN:
    default:
        fprintf(outfile, "  \"fps\": null,\n");
        break;
    }

    json_write_frames(fc, outfile, subsample, sf);
    json_write_pooled(vmaf, fc, outfile, pic_cnt, sf);
    json_write_aggregate(fc, outfile, sf);

    fprintf(outfile, "}\n");

    const int flush_err = fflush(outfile);
    vmaf_thread_locale_pop(locale_state);

    return (flush_err != 0 || ferror(outfile)) ? -EIO : 0;
}

int vmaf_write_output_csv(VmafFeatureCollector *fc, FILE *outfile, unsigned subsample,
                          const char *score_format)
{
    /* Mirror the XML/JSON writers' NULL guards (ADR-0602).  Both fc and
     * outfile are dereferenced unconditionally below; previously this writer
     * would SIGSEGV on NULL input where the sibling writers returned -EINVAL.
     * Adversarial audit 2026-05-31. */
    if (!fc)
        return -EINVAL;
    if (!outfile)
        return -EINVAL;

    const char *sf = fmt_or_default(score_format);

    VmafThreadLocaleState *locale_state = vmaf_thread_locale_push_c();

    fprintf(outfile, "Frame,");
    for (unsigned i = 0; i < fc->cnt; i++) {
        fprintf(outfile, "%s,", vmaf_feature_name_alias(fc->feature_vector[i]->name));
    }
    fprintf(outfile, "\n");

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

        fprintf(outfile, "%d,", i);
        for (unsigned j = 0; j < fc->cnt; j++) {
            if (i >= fc->feature_vector[j]->capacity) /* ADR-0606: >= not > */
                continue;
            if (!fc->feature_vector[j]->score[i].written)
                continue;
            fprintf(outfile, sf, fc->feature_vector[j]->score[i].value);
            fprintf(outfile, ",");
        }
        fprintf(outfile, "\n");
    }

    const int flush_err = fflush(outfile);
    vmaf_thread_locale_pop(locale_state);

    return (flush_err != 0 || ferror(outfile)) ? -EIO : 0;
}

int vmaf_write_output_sub(VmafFeatureCollector *fc, FILE *outfile, unsigned subsample,
                          const char *score_format)
{
    /* Mirror the XML/JSON writers' NULL guards (ADR-0602).  See the CSV
     * writer above for rationale. */
    if (!fc)
        return -EINVAL;
    if (!outfile)
        return -EINVAL;

    const char *sf = fmt_or_default(score_format);

    VmafThreadLocaleState *locale_state = vmaf_thread_locale_push_c();

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

        fprintf(outfile, "{%d}{%d}frame: %d|", i, i + 1, i);
        for (unsigned j = 0; j < fc->cnt; j++) {
            if (i >= fc->feature_vector[j]->capacity) /* ADR-0606: >= not > */
                continue;
            if (!fc->feature_vector[j]->score[i].written)
                continue;
            fprintf(outfile, "%s: ", vmaf_feature_name_alias(fc->feature_vector[j]->name));
            fprintf(outfile, sf, fc->feature_vector[j]->score[i].value);
            fprintf(outfile, "|");
        }
        fprintf(outfile, "\n");
    }

    const int flush_err = fflush(outfile);
    vmaf_thread_locale_pop(locale_state);

    return (flush_err != 0 || ferror(outfile)) ? -EIO : 0;
}
// NOLINTEND(cert-err33-c)
