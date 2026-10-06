/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Reading a JSON report back and verifying it (#2142, ADR-2073, RC4 WP5).
 *
 * vmafx_report_open() flattens the report into its scalar members in
 * document order, each with a path (`provenance.models[0].sha256`,
 * `frames[3].metrics.vmaf`), and on the way writes the provenance object
 * again in the RFC 8785 form the library digests: the writer emitted it in
 * that form, so the text is rebuilt from the tokens (keys must come in byte
 * order, numbers must be plain integers) and its SHA-256 must equal the
 * recorded `digest`.
 *
 * vmafx_report_verify() compares a recorded report with a re-run member by
 * member, skipping what describes the environment rather than the
 * configuration (ENVIRONMENT below), and names the first difference; then it
 * checks each report's digests.
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/path_utf8.h"
#include "error_internal.h"
#include "internal.h"
#include "pdjson.h"
#include "provenance_json.h"
#include "sha256.h"
#include "thread_locale.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define REPORT_BYTES_MAX ((size_t)1u << 30)
#define REPORT_ENTRIES_MAX (1u << 24)
#define REPORT_DEPTH_MAX 32u
#define REPORT_PATH_MAX 1024u
#define REPORT_KEY_MAX 64u
#define LOSSLESS_FORMAT "%.17g"

enum EntryKind { ENTRY_STRING, ENTRY_NUMBER, ENTRY_LITERAL };

typedef struct ReportEntry {
    char *path;
    char *text;
    enum EntryKind kind;
} ReportEntry;

struct VmafxReportFile {
    ReportEntry *entries;
    uint32_t n;
    uint32_t cap;
    bool has_provenance;
    bool canonical; /* the provenance object was in RFC 8785 form */
    char canonical_where[REPORT_PATH_MAX];
    char digest[VMAFX_DIGEST_TEXT_SIZE]; /* recomputed from the provenance object */
};

/* ---- Walking the tokens --------------------------------------------------------- */

typedef struct Frame {
    bool object;
    bool first;     /* nothing emitted into the canonical text yet */
    uint32_t index; /* next array element */
    size_t prefix;  /* path length of the container */
    char key[REPORT_KEY_MAX];
    char last_key[REPORT_KEY_MAX];
} Frame;

typedef struct Walk {
    json_stream json;
    VmafxReportFile *file;
    char path[REPORT_PATH_MAX];
    size_t path_len;
    Frame frames[REPORT_DEPTH_MAX];
    uint32_t depth;
    uint32_t canon_depth; /* depth of the provenance object's frame; 0 outside it */
    VmafxJsonText canon;
    bool failed;
} Walk;

static char *copy_bytes(const char *bytes, size_t len)
{
    char *const copy = malloc(len + 1u);
    if (copy) {
        memcpy(copy, bytes, len);
        copy[len] = '\0';
    }
    return copy;
}

static bool add_entry(Walk *walk, const char *text, size_t len, enum EntryKind kind)
{
    VmafxReportFile *const file = walk->file;
    if (file->n == file->cap) {
        const uint32_t cap = file->cap ? file->cap * 2u : 256u;
        ReportEntry *const grown =
            cap <= REPORT_ENTRIES_MAX ? realloc(file->entries, sizeof(*grown) * cap) : NULL;
        if (!grown) {
            return false;
        }
        file->entries = grown;
        file->cap = cap;
    }
    ReportEntry entry = {.path = copy_bytes(walk->path, walk->path_len),
                         .text = copy_bytes(text, len),
                         .kind = kind};
    if (!entry.path || !entry.text) {
        free(entry.path);
        free(entry.text);
        return false;
    }
    file->entries[file->n++] = entry;
    return true;
}

/* Append the path segment of the next value of the innermost container. */
static bool push_segment(Walk *walk)
{
    if (walk->depth == 0) {
        return true;
    }
    Frame *const top = &walk->frames[walk->depth - 1u];
    walk->path_len = top->prefix;
    const size_t room = sizeof(walk->path) - walk->path_len;
    const int n = top->object ?
                      snprintf(walk->path + walk->path_len, room, "%s%s", walk->path_len ? "." : "",
                               top->key) :
                      snprintf(walk->path + walk->path_len, room, "[%" PRIu32 "]", top->index);
    if (n < 0 || (size_t)n >= room) {
        return false;
    }
    walk->path_len += (size_t)n;
    return true;
}

static bool in_canon(const Walk *walk)
{
    return walk->canon_depth && walk->depth >= walk->canon_depth;
}

/* The digest covers neither itself nor the timing. */
static bool canon_skipped(const Walk *walk)
{
    const Frame *const top = &walk->frames[walk->depth - 1u];
    return walk->depth == walk->canon_depth &&
           (strcmp(top->key, "digest") == 0 || strcmp(top->key, "elapsed_ns") == 0);
}

static void not_canonical(Walk *walk)
{
    if (walk->file->canonical) {
        walk->file->canonical = false;
        (void)snprintf(walk->file->canonical_where, sizeof(walk->file->canonical_where), "%.*s",
                       (int)walk->path_len, walk->path);
    }
}

/* Emit the separator and key of the next canonical value. */
static void canon_lead(Walk *walk)
{
    Frame *const top = &walk->frames[walk->depth - 1u];
    vmafx_json_text_puts(&walk->canon, top->first ? "" : ",");
    top->first = false;
    if (top->object) {
        vmafx_json_text_string(&walk->canon, top->key);
        vmafx_json_text_puts(&walk->canon, ":");
    }
}

static bool plain_integer(const char *text, size_t len)
{
    size_t i = text[0] == '-' ? 1u : 0u;
    if (i >= len || (text[i] == '0' && len > i + 1u)) {
        return false;
    }
    for (; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

static void canon_scalar(Walk *walk, enum json_type type, const char *text, size_t len)
{
    if (!in_canon(walk) || canon_skipped(walk)) {
        return;
    }
    canon_lead(walk);
    if (type == JSON_STRING) {
        vmafx_json_text_string(&walk->canon, text);
        return;
    }
    if (type == JSON_NUMBER && !plain_integer(text, len)) {
        not_canonical(walk);
    }
    vmafx_json_text_put(&walk->canon, text, len);
}

/* After a value: the container moves on. */
static void value_done(Walk *walk)
{
    if (walk->depth == 0) {
        return;
    }
    Frame *const top = &walk->frames[walk->depth - 1u];
    if (top->object) {
        top->key[0] = '\0';
    } else {
        top->index++;
    }
}

static bool scalar(Walk *walk, enum json_type type)
{
    size_t len = 0;
    const char *text = json_get_string(&walk->json, &len);
    if (type == JSON_TRUE || type == JSON_FALSE || type == JSON_NULL) {
        text = type == JSON_TRUE ? "true" : type == JSON_FALSE ? "false" : "null";
        len = strlen(text);
    } else if (len) {
        len -= 1u; /* json_get_string() counts the terminator */
    }
    const enum EntryKind kind = type == JSON_STRING ? ENTRY_STRING :
                                type == JSON_NUMBER ? ENTRY_NUMBER :
                                                      ENTRY_LITERAL;
    if (!push_segment(walk) || !add_entry(walk, text, len, kind)) {
        return false;
    }
    canon_scalar(walk, type, text, len);
    value_done(walk);
    return true;
}

static bool open_container(Walk *walk, bool object)
{
    if (walk->depth >= REPORT_DEPTH_MAX || !push_segment(walk)) {
        return false;
    }
    const bool provenance =
        walk->depth == 1u && object && strcmp(walk->frames[0].key, "provenance") == 0;
    if (in_canon(walk)) {
        canon_lead(walk);
    }
    Frame *const frame = &walk->frames[walk->depth++];
    memset(frame, 0, sizeof(*frame));
    frame->object = object;
    frame->first = true;
    frame->prefix = walk->path_len;
    if (provenance) {
        walk->canon_depth = walk->depth;
        walk->file->has_provenance = true;
    }
    if (in_canon(walk)) {
        vmafx_json_text_puts(&walk->canon, object ? "{" : "[");
    }
    return true;
}

static void close_container(Walk *walk)
{
    const bool object = walk->frames[walk->depth - 1u].object;
    if (in_canon(walk)) {
        vmafx_json_text_puts(&walk->canon, object ? "}" : "]");
    }
    if (walk->depth == walk->canon_depth) {
        walk->canon_depth = 0;
    }
    walk->depth--;
    value_done(walk);
}

/* A member key: remembered for the value, checked for byte order. */
static bool member_key(Walk *walk)
{
    Frame *const top = &walk->frames[walk->depth - 1u];
    size_t len = 0;
    const char *const key = json_get_string(&walk->json, &len);
    (void)snprintf(top->key, sizeof(top->key), "%s", key);
    if (in_canon(walk)) {
        if (top->last_key[0] && strcmp(top->last_key, top->key) >= 0) {
            (void)push_segment(walk);
            not_canonical(walk);
        }
        memcpy(top->last_key, top->key, sizeof(top->last_key));
    }
    return true;
}

static bool expects_key(const Walk *walk)
{
    const Frame *const top = walk->depth ? &walk->frames[walk->depth - 1u] : NULL;
    return top && top->object && top->key[0] == '\0';
}

static bool step(Walk *walk, enum json_type type)
{
    switch (type) {
    case JSON_OBJECT:
    case JSON_ARRAY:
        return open_container(walk, type == JSON_OBJECT);
    case JSON_OBJECT_END:
    case JSON_ARRAY_END:
        if (walk->depth == 0) {
            return false;
        }
        close_container(walk);
        return true;
    case JSON_STRING:
        if (expects_key(walk)) {
            return member_key(walk);
        }
        return scalar(walk, type);
    case JSON_NUMBER:
    case JSON_TRUE:
    case JSON_FALSE:
    case JSON_NULL:
        return scalar(walk, type);
    default:
        return false;
    }
}

/* Walk every token of `data`: 0, or -EINVAL (not JSON), -ENOMEM. */
static int walk_report(VmafxReportFile *file, const char *data, size_t len)
{
    Walk *const walk = calloc(1, sizeof(*walk));
    if (!walk) {
        return -ENOMEM;
    }
    walk->file = file;
    file->canonical = true;
    json_open_buffer(&walk->json, data, len);
    int err = 0;
    for (size_t tokens = 0; tokens < (size_t)REPORT_ENTRIES_MAX * 4u; tokens++) {
        const enum json_type type = json_next(&walk->json);
        if (type == JSON_DONE) {
            break;
        }
        if (type == JSON_ERROR || !step(walk, type)) {
            err = type == JSON_ERROR ? -EINVAL : -ENOMEM;
            break;
        }
    }
    json_close(&walk->json);
    char *const canon = vmafx_json_text_take(&walk->canon);
    if (!err && file->has_provenance && canon) {
        vmafx_digest_text(canon, strlen(canon), file->digest);
    }
    err = !err && file->has_provenance && !canon ? -ENOMEM : err;
    free(canon);
    free(walk);
    return err;
}

/* ---- Opening ---------------------------------------------------------------------- */

static int read_file(const char *path, char **data, size_t *len)
{
    FILE *const file = vmaf_fopen_utf8(path, "rb");
    if (!file) {
        return -EIO;
    }
    size_t cap = 65536u;
    char *buf = malloc(cap);
    size_t n = 0;
    while (buf && !ferror(file) && !feof(file) && cap <= REPORT_BYTES_MAX) {
        n += fread(buf + n, 1u, cap - n, file);
        if (n == cap && cap < REPORT_BYTES_MAX) {
            char *const grown = realloc(buf, cap * 2u);
            if (!grown) {
                free(buf);
                buf = NULL;
            }
            buf = grown;
            cap *= 2u;
        } else if (n == cap) {
            break;
        }
    }
    const int failed = !buf || ferror(file) || !feof(file);
    (void)fclose(file);
    if (failed) {
        free(buf);
        return buf ? -EIO : -ENOMEM;
    }
    *data = buf;
    *len = n;
    return 0;
}

void vmafx_report_close(VmafxReportFile *file)
{
    if (!file) {
        return;
    }
    for (uint32_t i = 0; i < file->n; i++) {
        free(file->entries[i].path);
        free(file->entries[i].text);
    }
    free(file->entries);
    free(file);
}

static VmafxStatus open_failure(const VmafxReport *report, const char *path, int err)
{
    if (err == -EIO) {
        return VMAFX_FAIL(report, VMAFX_E_IO, err, VMAFX_SUBJECT_PATH, path,
                          "cannot read the report");
    }
    if (err == -ENOMEM) {
        return VMAFX_FAIL(report, VMAFX_E_NOMEM, err, VMAFX_SUBJECT_PATH, path,
                          "cannot hold the report");
    }
    return VMAFX_FAIL(report, VMAFX_E_INVALID, err, VMAFX_SUBJECT_PATH, path,
                      "not a JSON report with a provenance object");
}

VmafxStatus vmafx_report_open(const char *path, VmafxReportFile **out, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!path || !out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER,
                          path ? "out" : "path", "NULL argument");
    }
    *out = NULL;
    char *data = NULL;
    size_t len = 0;
    int err = read_file(path, &data, &len);
    VmafxReportFile *const file = err ? NULL : calloc(1, sizeof(*file));
    err = !err && !file ? -ENOMEM : err;
    if (!err) {
        assert(file && data);
        err = walk_report(file, data, len);
    }
    free(data);
    if (!err && !file->has_provenance) {
        err = -EINVAL;
    }
    if (err) {
        vmafx_report_close(file);
        return open_failure(&report, path, err);
    }
    *out = file;
    return VMAFX_OK;
}

const char *vmafx_report_field(const VmafxReportFile *file, const char *path)
{
    if (!file || !path) {
        return NULL;
    }
    for (uint32_t i = 0; i < file->n; i++) {
        if (strcmp(file->entries[i].path, path) == 0) {
            return file->entries[i].text;
        }
    }
    return NULL;
}

/* ---- The score digest of a lossless report ----------------------------------------- */

typedef struct ScoreLine {
    const char *name;
    unsigned index;
    double value;
} ScoreLine;

static int compare_lines(const void *a, const void *b)
{
    const ScoreLine *const x = a;
    const ScoreLine *const y = b;
    const int by_name = strcmp(x->name, y->name);
    if (by_name) {
        return by_name;
    }
    return (x->index > y->index) - (x->index < y->index);
}

/* The frame a `frames[i]...` path belongs to: `frames[i]` and its length. */
static size_t frame_prefix(const char *path)
{
    const char *const close = strncmp(path, "frames[", 7) == 0 ? strchr(path, ']') : NULL;
    return close ? (size_t)(close - path) + 1u : 0u;
}

/* The frame number of the frame whose `frameNum` member is `entry`. */
static bool frame_number(const ReportEntry *entry, size_t prefix, unsigned *index)
{
    char *end = NULL;
    const unsigned long value = strtoul(entry->text, &end, 10);
    *index = (unsigned)value;
    return strcmp(entry->path + prefix, ".frameNum") == 0 && end && *end == '\0';
}

typedef struct FrameCursor {
    const char *path; /* the frames[i] the number belongs to */
    size_t prefix;
    unsigned index;
    bool known;
} FrameCursor;

/* One entry of the walk: a frame number moves the cursor, a metric of the
 * cursor's frame becomes a line; false for a null or misplaced score. */
static bool collect_entry(const ReportEntry *entry, FrameCursor *cursor, ScoreLine *line,
                          bool *added)
{
    static const char marker[] = ".metrics.";
    *added = false;
    const size_t prefix = frame_prefix(entry->path);
    if (!prefix) {
        return true;
    }
    if (strncmp(entry->path + prefix, ".frameNum", 9) == 0) {
        cursor->path = entry->path;
        cursor->prefix = prefix;
        cursor->known = frame_number(entry, prefix, &cursor->index);
        return cursor->known;
    }
    if (strncmp(entry->path + prefix, marker, sizeof(marker) - 1u) != 0) {
        return true;
    }
    const bool same_frame = cursor->known && cursor->prefix == prefix &&
                            strncmp(cursor->path, entry->path, prefix) == 0;
    if (entry->kind != ENTRY_NUMBER || !same_frame) {
        return false;
    }
    line->name = entry->path + prefix + sizeof(marker) - 1u;
    line->index = cursor->index;
    line->value = strtod(entry->text, NULL);
    *added = true;
    return true;
}

/* Every per-frame score of the report, at most `cap`; false for a null score
 * or a metric before its frame's number. */
static bool collect_lines(const VmafxReportFile *file, ScoreLine *lines, uint32_t cap, uint32_t *n)
{
    FrameCursor cursor = {.path = NULL, .prefix = 0, .index = 0, .known = false};
    *n = 0;
    bool ok = true;
    for (uint32_t i = 0; ok && i < file->n && *n < cap; i++) {
        bool added = false;
        ok = collect_entry(&file->entries[i], &cursor, &lines[*n], &added);
        *n += added ? 1u : 0u;
    }
    return ok;
}

/* `sha256:...` of the report's scores as the library digests them; "" when
 * the report cannot reproduce the bits (not lossless, a null score). */
static void report_scores_digest(const VmafxReportFile *file, char out[VMAFX_DIGEST_TEXT_SIZE])
{
    out[0] = '\0';
    const char *const format = vmafx_report_field(file, "score_format");
    ScoreLine *const lines = malloc(sizeof(*lines) * (file->n ? file->n : 1u));
    uint32_t n = 0;
    VmafThreadLocaleState *const locale = vmaf_thread_locale_push_c();
    const bool ok = lines && format && strcmp(format, LOSSLESS_FORMAT) == 0 &&
                    collect_lines(file, lines, file->n, &n);
    vmaf_thread_locale_pop(locale);
    if (ok) {
        qsort(lines, n, sizeof(*lines), compare_lines);
        VmafxSha256 sha;
        vmafx_sha256_init(&sha);
        for (uint32_t i = 0; i < n; i++) {
            uint64_t bits = 0;
            memcpy(&bits, &lines[i].value, sizeof(bits));
            char line[48];
            const int len =
                snprintf(line, sizeof(line), " %u %016" PRIx64 "\n", lines[i].index, bits);
            vmafx_sha256_update(&sha, lines[i].name, strlen(lines[i].name));
            vmafx_sha256_update(&sha, line, len > 0 ? (size_t)len : 0u);
        }
        char hex[VMAFX_SHA256_HEX_CHARS];
        vmafx_sha256_final_hex(&sha, hex);
        (void)snprintf(out, VMAFX_DIGEST_TEXT_SIZE, "sha256:%s", hex);
    }
    free(lines);
}

/* ---- Verification ------------------------------------------------------------------ */

static VmafxStatus mismatch(const VmafxReport *report, const char *field, const char *recorded,
                            const char *other, const char *other_label)
{
    return VMAFX_FAIL(report, VMAFX_E_MISMATCH, 0, VMAFX_SUBJECT_PARAMETER, field,
                      "recorded %s, %s %s", recorded ? recorded : "(absent)", other_label,
                      other ? other : "(absent)");
}

/* A report's own digests: the provenance object and, for a lossless report,
 * the scores. */
static VmafxStatus self_check(const VmafxReport *report, const VmafxReportFile *file)
{
    if (!file->canonical) {
        return VMAFX_FAIL(report, VMAFX_E_MISMATCH, 0, VMAFX_SUBJECT_PARAMETER,
                          file->canonical_where,
                          "the provenance object is not in the form it was digested in");
    }
    const char *const digest = vmafx_report_field(file, "provenance.digest");
    if (!digest || strcmp(digest, file->digest) != 0) {
        return mismatch(report, "provenance.digest", digest, file->digest, "recomputed");
    }
    char scores[VMAFX_DIGEST_TEXT_SIZE];
    report_scores_digest(file, scores);
    const char *const recorded = vmafx_report_field(file, "provenance.scores_digest");
    if (scores[0] && (!recorded || strcmp(recorded, scores) != 0)) {
        return mismatch(report, "provenance.scores_digest", recorded, scores, "recomputed");
    }
    return VMAFX_OK;
}

/* Members that describe where and when a report was made, not what was
 * scored: a re-run on another build or device may differ in them. */
static bool environment(const char *path)
{
    static const char *const exact[] = {
        "version",
        "fps",
        "provenance.version",
        "provenance.commit",
        "provenance.build_id",
        "provenance.compiler",
        "provenance.build_flags",
        "provenance.fp_policy",
        "provenance.backends",
        "provenance.rust_twins",
        "provenance.simd",
        "provenance.device_index",
        "provenance.device_name",
        "provenance.device_runtime",
        "provenance.elapsed_ns",
        "provenance.digest",
    };
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        if (strcmp(path, exact[i]) == 0) {
            return true;
        }
    }
    const char *const leaf = strrchr(path, '.');
    return strncmp(path, "provenance.features[", 20) == 0 && leaf &&
           (strcmp(leaf, ".device") == 0 || strcmp(leaf, ".runtime") == 0 ||
            strcmp(leaf, ".exactness") == 0);
}

static uint32_t next_compared(const VmafxReportFile *file, uint32_t i)
{
    while (i < file->n && environment(file->entries[i].path)) {
        i++;
    }
    return i;
}

static VmafxStatus compare(const VmafxReport *report, const VmafxReportFile *recorded,
                           const VmafxReportFile *rerun)
{
    uint32_t a = next_compared(recorded, 0);
    uint32_t b = next_compared(rerun, 0);
    for (; a < recorded->n && b < rerun->n;
         a = next_compared(recorded, a + 1u), b = next_compared(rerun, b + 1u)) {
        const ReportEntry *const x = &recorded->entries[a];
        const ReportEntry *const y = &rerun->entries[b];
        if (strcmp(x->path, y->path) != 0) {
            return mismatch(report, x->path, x->text, NULL, "re-run");
        }
        if (x->kind != y->kind || strcmp(x->text, y->text) != 0) {
            return mismatch(report, x->path, x->text, y->text, "re-run");
        }
    }
    if (a < recorded->n) {
        return mismatch(report, recorded->entries[a].path, recorded->entries[a].text, NULL,
                        "re-run");
    }
    if (b < rerun->n) {
        return mismatch(report, rerun->entries[b].path, NULL, rerun->entries[b].text, "re-run");
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_report_verify(const VmafxReportFile *recorded, const VmafxReportFile *rerun,
                                VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!recorded) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "recorded",
                          "NULL argument");
    }
    VmafxStatus status = rerun ? compare(&report, recorded, rerun) : VMAFX_OK;
    if (status == VMAFX_OK) {
        status = self_check(&report, recorded);
    }
    if (status == VMAFX_OK && rerun) {
        status = self_check(&report, rerun);
    }
    return status;
}

/* NOLINTEND(modernize-use-nullptr) */
