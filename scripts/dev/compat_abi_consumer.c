/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Unmodified upstream libvmaf consumer for ABI drop-in parity verification
 *  (ADR-3339, issue #2473).
 *
 *  Compiled once against upstream Netflix headers and linked against
 *  upstream's libvmaf.so.3. Swapped at runtime via LD_LIBRARY_PATH against
 *  this fork's libvmaf.so.3 to prove drop-in binary compatibility.
 *
 *  usage: consumer <ref> <dis> <w> <h> <420|422|444|400> <bpc> <cpumask>
 *                  <frames> <spec>...
 *    F:<extractor>[:<option>=<value>...]   vmaf_use_feature()
 *    M:<model.json>[:noclip|:transform]    vmaf_model_load_from_path()
 *    B:<version>[:noclip|:transform]       vmaf_model_load() (built-in)
 *    C:<collection.json>                   vmaf_model_collection_load_from_path()
 *
 *  output lines match upstream_parity_harness.c exactly:
 *    <frame> <name> <%.17g>
 *    pool <name> <mean> <harmonic mean>
 *    agg <name> <value>
 *    END <frames read>
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>

#define PARITY_MAX_MODELS 16
#define PARITY_MAX_OPTIONS 32
#define PARITY_MAX_SPEC 4096
#define PARITY_MAX_ROW (2u * 16384u)
#define PARITY_MAX_FRAMES 100000u
#define PARITY_FIRST_SPEC 9
#define PARITY_MAX_METRICS 128
#define PARITY_NAME_LEN 64
#define PARITY_LINE_BUF 4096

enum ParityExit {
    PARITY_OK = 0,
    PARITY_USAGE = 1,
    PARITY_ALLOC = 2,
    PARITY_INIT = 3,
    PARITY_OPEN = 4,
    PARITY_SPEC = 5,
    PARITY_FEATURE = 6,
    PARITY_MODEL = 7,
    PARITY_READ = 8,
    PARITY_FLUSH = 9,
    PARITY_PRINT = 10,
};

typedef struct ParityRun {
    VmafContext *vmaf;
    VmafModel *models[PARITY_MAX_MODELS];
    VmafModelCollection *collections[PARITY_MAX_MODELS];
    char names[2 * PARITY_MAX_MODELS][16];
    char metric_names[PARITY_MAX_METRICS][PARITY_NAME_LEN];
    unsigned n_models;
    unsigned n_collections;
    unsigned n_metrics;
    enum VmafPixelFormat pix_fmt;
    unsigned bpc;
    unsigned w;
    unsigned h;
    unsigned frame_limit;
    unsigned frames;
} ParityRun;

static int g_print_failed;

static void emit(int printed)
{
    if (printed < 0) {
        g_print_failed = 1;
    }
}

static int parse_unsigned(const char *text, unsigned long long max, unsigned long long *out)
{
    char *end = NULL;
    const unsigned long long value = strtoull(text, &end, 0);
    if (end == text || *end != '\0' || value > max) {
        return -1;
    }
    *out = value;
    return 0;
}

static int parse_pix_fmt(const char *text, enum VmafPixelFormat *out)
{
    static const struct {
        const char *name;
        enum VmafPixelFormat fmt;
    } table[] = {
        {"420", VMAF_PIX_FMT_YUV420P},
        {"422", VMAF_PIX_FMT_YUV422P},
        {"444", VMAF_PIX_FMT_YUV444P},
        {"400", VMAF_PIX_FMT_YUV400P},
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!strcmp(text, table[i].name)) {
            *out = table[i].fmt;
            return 0;
        }
    }
    return -1;
}

static int split_fields(char *spec, char **fields, int max)
{
    int n = 0;
    char *cursor = spec;
    for (int i = 0; i < max; i++) {
        fields[n++] = cursor;
        char *colon = strchr(cursor, ':');
        if (!colon) {
            return n;
        }
        *colon = '\0';
        cursor = colon + 1;
    }
    return -1;
}

static int use_feature(ParityRun *run, char **fields, int n_fields)
{
    VmafFeatureDictionary *dict = NULL;
    for (int i = 1; i < n_fields; i++) {
        char *equals = strchr(fields[i], '=');
        if (!equals) {
            return PARITY_SPEC;
        }
        *equals = '\0';
        if (vmaf_feature_dictionary_set(&dict, fields[i], equals + 1)) {
            emit(printf("ERR dictionary_set %s\n", fields[i]));
            return PARITY_SPEC;
        }
    }
    const int err = vmaf_use_feature(run->vmaf, fields[0], dict);
    if (err) {
        emit(printf("ERR use_feature %d\n", err));
        return PARITY_FEATURE;
    }
    return PARITY_OK;
}

static int model_flags(char **fields, int n_fields, uint64_t *flags)
{
    *flags = VMAF_MODEL_FLAGS_DEFAULT;
    for (int i = 1; i < n_fields; i++) {
        if (!strcmp(fields[i], "noclip")) {
            *flags |= VMAF_MODEL_FLAG_DISABLE_CLIP;
        } else if (!strcmp(fields[i], "transform")) {
            *flags |= VMAF_MODEL_FLAG_ENABLE_TRANSFORM;
        } else {
            return -1;
        }
    }
    return 0;
}

static int use_collection(ParityRun *run, VmafModelConfig *cfg, const char *path)
{
    VmafModel *first = NULL;
    VmafModelCollection **slot = &run->collections[run->n_collections];
    int err = vmaf_model_collection_load_from_path(&first, slot, cfg, path);
    if (!err) {
        err = vmaf_use_features_from_model_collection(run->vmaf, *slot);
    }
    if (!err) {
        run->n_collections++;
    }
    return err;
}

static int use_model(ParityRun *run, char kind, char **fields, int n_fields)
{
    const unsigned slot = run->n_models + run->n_collections;
    uint64_t flags = 0;
    if (slot >= PARITY_MAX_MODELS || model_flags(fields, n_fields, &flags)) {
        return PARITY_SPEC;
    }
    emit(snprintf(run->names[slot], sizeof(run->names[slot]), "model%u", slot));
    VmafModelConfig cfg = {.name = run->names[slot], .flags = flags};
    int err = 0;
    if (kind == 'C') {
        err = use_collection(run, &cfg, fields[0]);
    } else {
        VmafModel **model = &run->models[run->n_models];
        err = (kind == 'M') ? vmaf_model_load_from_path(model, &cfg, fields[0]) :
                              vmaf_model_load(model, &cfg, fields[0]);
        if (!err) {
            err = vmaf_use_features_from_model(run->vmaf, *model);
        }
        if (!err) {
            run->n_models++;
        }
    }
    if (err) {
        emit(printf("ERR model %d\n", err));
        return PARITY_MODEL;
    }
    return PARITY_OK;
}

static int apply_spec(ParityRun *run, const char *text)
{
    static char spec[PARITY_MAX_SPEC];
    char *fields[PARITY_MAX_OPTIONS];
    const size_t length = strlen(text);
    if (length < 3 || length >= sizeof(spec) || text[1] != ':') {
        return PARITY_SPEC;
    }
    memcpy(spec, text, length + 1);
    const int n_fields = split_fields(spec + 2, fields, PARITY_MAX_OPTIONS);
    if (n_fields < 1) {
        return PARITY_SPEC;
    }
    switch (spec[0]) {
    case 'F':
        return use_feature(run, fields, n_fields);
    case 'M':
    case 'B':
    case 'C':
        return use_model(run, spec[0], fields, n_fields);
    default:
        return PARITY_SPEC;
    }
}

static void file_plane_size(const ParityRun *run, unsigned plane, unsigned *w, unsigned *h)
{
    const unsigned ss_hor = run->pix_fmt != VMAF_PIX_FMT_YUV444P;
    const unsigned ss_ver = run->pix_fmt == VMAF_PIX_FMT_YUV420P;
    *w = plane ? (run->w + ss_hor) >> ss_hor : run->w;
    *h = plane ? (run->h + ss_ver) >> ss_ver : run->h;
}

static int read_plane(FILE *file, const ParityRun *run, VmafPicture *pic, unsigned plane)
{
    static unsigned char row[PARITY_MAX_ROW];
    const size_t sample = run->bpc > 8 ? 2 : 1;
    unsigned file_w = 0;
    unsigned file_h = 0;
    file_plane_size(run, plane, &file_w, &file_h);
    const unsigned copy_w = pic->w[plane] < file_w ? pic->w[plane] : file_w;
    for (unsigned y = 0; y < file_h; y++) {
        if (fread(row, sample, file_w, file) != file_w) {
            return 1;
        }
        if (y < pic->h[plane]) {
            memcpy((unsigned char *)pic->data[plane] + (size_t)y * (size_t)pic->stride[plane], row,
                   sample * copy_w);
        }
    }
    return 0;
}

static int read_picture(FILE *file, const ParityRun *run, VmafPicture *pic)
{
    if (vmaf_picture_alloc(pic, run->pix_fmt, run->bpc, run->w, run->h)) {
        emit(printf("ERR picture_alloc\n"));
        return PARITY_ALLOC;
    }
    const unsigned planes = run->pix_fmt == VMAF_PIX_FMT_YUV400P ? 1 : 3;
    for (unsigned plane = 0; plane < planes; plane++) {
        if (read_plane(file, run, pic, plane)) {
            if (vmaf_picture_unref(pic)) {
                return PARITY_ALLOC;
            }
            return 1;
        }
    }
    return 0;
}

static int read_frames(ParityRun *run, FILE *ref_file, FILE *dis_file)
{
    for (unsigned n = 0; n < run->frame_limit; n++) {
        VmafPicture ref;
        VmafPicture dis;
        int status = read_picture(ref_file, run, &ref);
        if (status) {
            return status == 1 ? PARITY_OK : status;
        }
        status = read_picture(dis_file, run, &dis);
        if (status) {
            if (vmaf_picture_unref(&ref)) {
                return PARITY_ALLOC;
            }
            return status == 1 ? PARITY_OK : status;
        }
        const int err = vmaf_read_pictures(run->vmaf, &ref, &dis, n);
        if (err) {
            emit(printf("ERR read_pictures %d frame %u\n", err, n));
            return PARITY_READ;
        }
        run->frames = n + 1;
    }
    return PARITY_OK;
}

static void score_models(const ParityRun *run)
{
    for (unsigned i = 0; i < run->frames; i++) {
        for (unsigned k = 0; k < run->n_models; k++) {
            double score = 0.0;
            const int err = vmaf_score_at_index(run->vmaf, run->models[k], &score, i);
            if (err) {
                emit(printf("ERR score_at_index %d model%u frame %u\n", err, k, i));
            }
        }
        for (unsigned k = 0; k < run->n_collections; k++) {
            VmafModelCollectionScore score;
            const int err =
                vmaf_score_at_index_model_collection(run->vmaf, run->collections[k], &score, i);
            if (err) {
                emit(printf("ERR score_at_index_collection %d frame %u\n", err, i));
            }
        }
    }
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static int match_metric_entry(const char *line, char *out, size_t max_len)
{
    const char *p = skip_ws(line);
    if (*p != '"') {
        return 0;
    }
    p++;
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= max_len) {
        return 0;
    }
    const char *colon = skip_ws(end + 1);
    if (colon[0] != ':' || (colon[1] != ' ' && colon[1] != '\t')) {
        return 0;
    }
    if (!strchr(colon + 1, '{')) {
        return 0;
    }
    const size_t len = (size_t)(end - p);
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int match_aggregate_entry(const char *line, char *name_out, size_t max_name, char *val_out,
                                 size_t max_val)
{
    const char *p = skip_ws(line);
    if (*p != '"') {
        return 0;
    }
    p++;
    const char *end_name = strchr(p, '"');
    if (!end_name || (size_t)(end_name - p) >= max_name) {
        return 0;
    }
    const char *colon = skip_ws(end_name + 1);
    if (*colon != ':') {
        return 0;
    }
    const char *val_start = skip_ws(colon + 1);
    const char *val_end = val_start;
    while (*val_end && *val_end != ',' && *val_end != '\r' && *val_end != '\n') {
        val_end++;
    }
    while (val_end > val_start && (*(val_end - 1) == ' ' || *(val_end - 1) == '\t')) {
        val_end--;
    }
    if (val_end == val_start || (size_t)(val_end - val_start) >= max_val) {
        return 0;
    }
    const size_t name_len = (size_t)(end_name - p);
    memcpy(name_out, p, name_len);
    name_out[name_len] = '\0';
    const size_t val_len = (size_t)(val_end - val_start);
    memcpy(val_out, val_start, val_len);
    val_out[val_len] = '\0';
    return 1;
}

static int discover_metrics(ParityRun *run, const char *json_path)
{
    FILE *f = fopen(json_path, "r");
    if (!f) {
        return -1;
    }
    char line[PARITY_LINE_BUF];
    int in_pooled = 0;
    run->n_metrics = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!in_pooled) {
            if (strstr(line, "\"pooled_metrics\":")) {
                in_pooled = 1;
            }
            continue;
        }
        if (line[0] == ' ' && line[1] == ' ' &&
            (line[2] == '}' || (line[2] == ' ' && line[3] == '}'))) {
            break;
        }
        if (run->n_metrics >= PARITY_MAX_METRICS) {
            break;
        }
        if (match_metric_entry(line, run->metric_names[run->n_metrics], PARITY_NAME_LEN)) {
            run->n_metrics++;
        }
    }
    if (fclose(f)) {
        return -1;
    }
    return 0;
}

static void print_frames(const ParityRun *run)
{
    for (unsigned j = 0; j < run->n_metrics; j++) {
        const char *name = run->metric_names[j];
        for (unsigned i = 0; i < run->frames; i++) {
            double score = 0.0;
            const int err = vmaf_feature_score_at_index(run->vmaf, name, &score, i);
            if (!err) {
                emit(printf("%u %s %.17g\n", i, name, score));
            }
        }
    }
}

static void print_pooled(const ParityRun *run)
{
    if (!run->frames) {
        return;
    }
    for (unsigned j = 0; j < run->n_metrics; j++) {
        const char *name = run->metric_names[j];
        double mean = 0.0;
        double harmonic = 0.0;
        const int mean_err = vmaf_feature_score_pooled(run->vmaf, name, VMAF_POOL_METHOD_MEAN,
                                                       &mean, 0, run->frames - 1);
        const int harmonic_err = vmaf_feature_score_pooled(
            run->vmaf, name, VMAF_POOL_METHOD_HARMONIC_MEAN, &harmonic, 0, run->frames - 1);
        if (!mean_err && !harmonic_err) {
            emit(printf("pool %s %.17g %.17g\n", name, mean, harmonic));
        }
    }
}

static void print_aggregate_line(const char *line)
{
    char name[PARITY_NAME_LEN];
    char val_str[PARITY_NAME_LEN];
    if (!match_aggregate_entry(line, name, sizeof(name), val_str, sizeof(val_str))) {
        return;
    }
    if (!strcmp(val_str, "null")) {
        emit(printf("agg %s -inf\n", name));
        return;
    }
    char *endptr = NULL;
    errno = 0;
    const double val = strtod(val_str, &endptr);
    if (endptr != val_str && errno == 0) {
        emit(printf("agg %s %.17g\n", name, val));
    }
}

static int print_aggregates(const char *json_path)
{
    FILE *f = fopen(json_path, "r");
    if (!f) {
        return -1;
    }
    char line[PARITY_LINE_BUF];
    int in_agg = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!in_agg) {
            if (strstr(line, "\"aggregate_metrics\":")) {
                in_agg = 1;
            }
            continue;
        }
        const char *k = skip_ws(line);
        if (*k == '}') {
            break;
        }
        print_aggregate_line(line);
    }
    if (fclose(f)) {
        return -1;
    }
    return 0;
}

static int make_temp_path(char *out, size_t sz)
{
    const int written = snprintf(out, sz, "/tmp/vmaf_consumer_XXXXXX");
    if (written < 0 || (size_t)written >= sz) {
        return -1;
    }
    const int fd = mkstemp(out);
    if (fd < 0) {
        return -1;
    }
    return close(fd);
}

static int parse_geometry(ParityRun *run, char **argv, unsigned long long *cpumask)
{
    unsigned long long w = 0;
    unsigned long long h = 0;
    unsigned long long bpc = 0;
    unsigned long long frames = 0;
    if (parse_unsigned(argv[3], PARITY_MAX_ROW / 2u, &w) ||
        parse_unsigned(argv[4], PARITY_MAX_ROW / 2u, &h) || parse_pix_fmt(argv[5], &run->pix_fmt) ||
        parse_unsigned(argv[6], 16, &bpc) || parse_unsigned(argv[7], UINT64_MAX, cpumask) ||
        parse_unsigned(argv[8], PARITY_MAX_FRAMES, &frames) || !w || !h || bpc < 8 || !frames) {
        return -1;
    }
    run->w = (unsigned)w;
    run->h = (unsigned)h;
    run->bpc = (unsigned)bpc;
    run->frame_limit = (unsigned)frames;
    return 0;
}

static int run_request(ParityRun *run, int argc, char **argv)
{
    for (int a = PARITY_FIRST_SPEC; a < argc; a++) {
        const int status = apply_spec(run, argv[a]);
        if (status) {
            return status;
        }
    }
    FILE *ref_file = fopen(argv[1], "rb");
    FILE *dis_file = fopen(argv[2], "rb");
    int status = PARITY_OPEN;
    if (ref_file && dis_file) {
        status = read_frames(run, ref_file, dis_file);
    }
    if (ref_file && fclose(ref_file)) {
        status = status ? status : PARITY_OPEN;
    }
    if (dis_file && fclose(dis_file)) {
        status = status ? status : PARITY_OPEN;
    }
    if (status) {
        return status;
    }
    const int err = vmaf_read_pictures(run->vmaf, NULL, NULL, 0);
    if (err) {
        emit(printf("ERR flush %d\n", err));
        return PARITY_FLUSH;
    }
    score_models(run);
    char tmp_path[256];
    if (make_temp_path(tmp_path, sizeof(tmp_path))) {
        return PARITY_PRINT;
    }
    const int write_err = vmaf_write_output(run->vmaf, tmp_path, VMAF_OUTPUT_FORMAT_JSON);
    int agg_err = 0;
    if (!write_err) {
        discover_metrics(run, tmp_path);
    }
    print_frames(run);
    print_pooled(run);
    if (!write_err) {
        agg_err = print_aggregates(tmp_path);
    }
    if (unlink(tmp_path)) {
        return PARITY_PRINT;
    }
    if (write_err || agg_err) {
        return PARITY_PRINT;
    }
    emit(printf("END %u\n", run->frames));
    return PARITY_OK;
}

static void cleanup_run(ParityRun *run)
{
    for (unsigned k = 0; k < run->n_models; k++) {
        if (run->models[k]) {
            vmaf_model_destroy(run->models[k]);
        }
    }
    for (unsigned k = 0; k < run->n_collections; k++) {
        if (run->collections[k]) {
            vmaf_model_collection_destroy(run->collections[k]);
        }
    }
    if (run->vmaf) {
        vmaf_close(run->vmaf);
    }
}

int main(int argc, char **argv)
{
    static ParityRun run;
    unsigned long long cpumask = 0;
    if (argc <= PARITY_FIRST_SPEC || parse_geometry(&run, argv, &cpumask)) {
        emit(fprintf(stderr, "usage: %s ref dis w h 420|422|444|400 bpc cpumask frames spec...\n",
                     argc > 0 ? argv[0] : "consumer"));
        return PARITY_USAGE;
    }
    const VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_ERROR,
        .n_threads = 1,
        .n_subsample = 1,
        .cpumask = cpumask,
    };
    const int init_err = vmaf_init(&run.vmaf, cfg);
    if (init_err) {
        emit(printf("ERR init %d\n", init_err));
        return PARITY_INIT;
    }
    const int status = run_request(&run, argc, argv);
    cleanup_run(&run);
    if (fflush(stdout) || g_print_failed) {
        return PARITY_PRINT;
    }
    return status;
}
