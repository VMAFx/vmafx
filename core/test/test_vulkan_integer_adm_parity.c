/**
 *
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * integer_adm CPU vs. Vulkan parity under the ADR-3402 gate contract.
 *
 * The Vulkan twin (integer_adm_vulkan, opt-in behind -Denable_vulkan)
 * must reproduce the unmodified CPU extractor bit-for-bit. Per the
 * ADR-3402 parity-gate contract the values are exchanged as C %a
 * hex-float strings and compared as EXACT strings — tolerance 0, every
 * bit recoverable. The references committed under
 * core/test/data/vulkan-adm/refs/ were produced by the unmodified CPU
 * extractor at the anchor commit af5ec5c94 (evidence #3302 comment
 * 6689093406); the PoC proved the GPU path byte-identical on RTX 5080
 * and RTX 4060 for both gate fixtures.
 *
 * Three tests:
 *   - comparator self-test (always runs, even in Vulkan-less builds):
 *     proves the comparator is not vacuous — a planted single-digit
 *     flip is caught and named, a dropped frame is caught, malformed
 *     input is refused (exit-2 path). Offline from any GPU.
 *   - parity on the 576x324_48f gate fixture (needs the twin).
 *   - parity on the 640x480_48f gate fixture (needs the twin).
 *
 * Skip behaviour: a build without `-Denable_vulkan=enabled` has no
 * `integer_adm_vulkan` extractor registered, and a build with it still
 * skips when no Vulkan loader / compute-capable device answers at init.
 * Both skips state the reason and pass (the SYCL parity-lane posture:
 * hardware evidence stays manual, committed as refs/ + out/).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* Fixture + reference locations (meson passes the absolute paths). */
#ifndef VULKAN_ADM_TESTDATA_DIR
#define VULKAN_ADM_TESTDATA_DIR "../testdata"
#endif
#ifndef VULKAN_ADM_REF_DIR
#define VULKAN_ADM_REF_DIR "data/vulkan-adm/refs"
#endif

#define FRAMES 48
#define N_VALUE_FIELDS 11

static const char *column_names[N_VALUE_FIELDS] = {
    "s0num", "s0den", "s1num", "s1den", "s2num", "s2den",
    "s3num", "s3den", "num",   "den",   "adm2",
};

static const char *feature_names[N_VALUE_FIELDS] = {
    "integer_adm_num_scale0", "integer_adm_den_scale0",
    "integer_adm_num_scale1", "integer_adm_den_scale1",
    "integer_adm_num_scale2", "integer_adm_den_scale2",
    "integer_adm_num_scale3", "integer_adm_den_scale3",
    "integer_adm_num",        "integer_adm_den",
    "integer_adm",
};

/* ------------------------------------------------------------------ */
/* Comparator: C port of the PoC's tools/compare.py contract. Every    */
/* payload field is a hex-float string; fields are compared as EXACT   */
/* strings — a 1-ULP difference is a FAIL.                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int frame;
    char *values[N_VALUE_FIELDS + 1]; /* malloc'd strings, NULL = absent */
} ContractRow;

typedef struct {
    char fields[N_VALUE_FIELDS + 1][32];
    int n_fields;
    ContractRow *rows;
    int n_rows;
    int cap_rows;
} Contract;

static void contract_free(Contract *c)
{
    for (int i = 0; i < c->n_rows; i++)
        for (int k = 0; k < N_VALUE_FIELDS + 1; k++)
            free(c->rows[i].values[k]);
    free(c->rows);
    memset(c, 0, sizeof(*c));
}

static int field_index(const Contract *c, const char *name)
{
    for (int k = 0; k < c->n_fields; k++)
        if (!strcmp(c->fields[k], name))
            return k;
    return -1;
}

/* Parse one contract held in memory as an array of lines.
 * Returns 0, or -1 with `err` set to a static message (the parse-error
 * path; compare.py's exit-2). */
static int contract_parse(const char *const *lines, int n_lines, Contract *c, const char **err)
{
    memset(c, 0, sizeof(*c));
    int fields_seen = 0;
    for (int li = 0; li < n_lines; li++) {
        const char *p = lines[li];
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            continue;
        if (*p == '#') {
            if (!strncmp(p, "# fields:", 9)) {
                char buf[512];
                snprintf(buf, sizeof(buf), "%s", p + 9);
                int nf = 0;
                for (char *tok = strtok(buf, " \t"); tok; tok = strtok(NULL, " \t")) {
                    if (nf >= N_VALUE_FIELDS + 1) {
                        *err = "too many fields";
                        goto fail;
                    }
                    snprintf(c->fields[nf], sizeof(c->fields[0]), "%s", tok);
                    nf++;
                }
                if (nf < 1 || strcmp(c->fields[0], "frame")) {
                    *err = "'# fields:' must start with 'frame'";
                    goto fail;
                }
                if (fields_seen && c->n_fields != nf) {
                    *err = "multiple conflicting '# fields:' lines";
                    goto fail;
                }
                c->n_fields = nf;
                fields_seen = 1;
            }
            continue; /* any other '#...' line is ignored */
        }

        /* Payload line: drop the decimal mirror after '|'. */
        char payload[2048];
        snprintf(payload, sizeof(payload), "%s", p);
        char *bar = strchr(payload, '|');
        if (bar)
            *bar = '\0';

        if (!fields_seen) {
            *err = "'# fields:' header line not found before data";
            goto fail;
        }

        char *save = NULL;
        char *tok = strtok_r(payload, " \t", &save);
        if (!tok || strncmp(tok, "frame=", 6)) {
            *err = "line must start with 'frame=<i>'";
            goto fail;
        }
        /* Digits only — compare.py's int() would reject anything else,
         * and atoi("x") silently yields 0. */
        const char *num = tok + 6;
        if (!*num) {
            *err = "bad frame index";
            goto fail;
        }
        for (const char *d = num; *d; d++) {
            if (*d < '0' || *d > '9') {
                *err = "bad frame index";
                goto fail;
            }
        }
        const int fnum = atoi(num);
        for (int i = 0; i < c->n_rows; i++) {
            if (c->rows[i].frame == fnum) {
                *err = "duplicate frame";
                goto fail;
            }
        }

        if (c->n_rows == c->cap_rows) {
            c->cap_rows = c->cap_rows ? c->cap_rows * 2 : 64;
            ContractRow *nr = realloc(c->rows, (size_t)c->cap_rows * sizeof(*c->rows));
            if (!nr) {
                *err = "out of memory";
                goto fail;
            }
            c->rows = nr;
        }
        ContractRow *row = &c->rows[c->n_rows];
        memset(row, 0, sizeof(*row));
        row->frame = fnum;

        while ((tok = strtok_r(NULL, " \t", &save))) {
            char *eq = strchr(tok, '=');
            if (!eq) {
                *err = "malformed token";
                goto fail;
            }
            *eq = '\0';
            const int k = field_index(c, tok);
            if (k <= 0) { /* 0 is 'frame', -1 unknown */
                *err = "token not declared in '# fields:' header";
                goto fail;
            }
            if (row->values[k]) {
                *err = "duplicate field in frame";
                goto fail;
            }
            row->values[k] = strdup(eq + 1);
        }
        for (int k = 1; k < c->n_fields; k++) {
            if (!row->values[k]) {
                *err = "frame missing a declared field";
                goto fail;
            }
        }
        c->n_rows++;
    }

    if (!fields_seen) {
        *err = "no '# fields:' header line found";
        goto fail;
    }
    if (c->n_rows == 0) {
        *err = "no frame lines found";
        goto fail;
    }
    return 0;

fail:
    contract_free(c);
    return -1;
}

/* Compare two parsed contracts. Returns the number of mismatches
 * (including frame-set differences), or -1 when the two contracts'
 * '# fields:' lists disagree — the parse-degeneration path the
 * ADR-3402 non-vacuousness clause requires to be a hard refusal
 * (compare.py's exit-2). The first `max_report` mismatches are printed
 * to `fp`. */
static int contract_compare(const Contract *ref, const Contract *test, int max_report, FILE *fp)
{
    if (ref->n_fields != test->n_fields) {
        fprintf(fp, "field lists disagree: ref=%d fields test=%d fields\n", ref->n_fields,
                test->n_fields);
        return -1;
    }
    for (int k = 0; k < ref->n_fields; k++) {
        if (strcmp(ref->fields[k], test->fields[k])) {
            fprintf(fp, "field lists disagree at %d: ref=%s test=%s\n", k, ref->fields[k],
                    test->fields[k]);
            return -1;
        }
    }
    int mismatches = 0;
    unsigned ref_only = 0, test_only = 0;
    for (int i = 0; i < ref->n_rows; i++) {
        const ContractRow *r = &ref->rows[i];
        const ContractRow *t = NULL;
        for (int j = 0; j < test->n_rows; j++) {
            if (test->rows[j].frame == r->frame) {
                t = &test->rows[j];
                break;
            }
        }
        if (!t) {
            ref_only++;
            if (mismatches++ < max_report)
                fprintf(fp, "frame=%d field=<frame-missing> ref=present test=absent-in-test\n",
                        r->frame);
            continue;
        }
        for (int k = 1; k < ref->n_fields && k < N_VALUE_FIELDS + 1; k++) {
            if (strcmp(r->values[k], t->values[k])) {
                if (mismatches++ < max_report)
                    fprintf(fp, "frame=%d field=%s ref=%s test=%s\n", r->frame, ref->fields[k],
                            r->values[k], t->values[k]);
            }
        }
    }
    for (int j = 0; j < test->n_rows; j++) {
        int found = 0;
        for (int i = 0; i < ref->n_rows; i++)
            if (ref->rows[i].frame == test->rows[j].frame) {
                found = 1;
                break;
            }
        if (!found) {
            test_only++;
            if (mismatches++ < max_report)
                fprintf(fp, "frame=%d field=<frame-missing> ref=absent-in-ref test=present\n",
                        test->rows[j].frame);
        }
    }
    fprintf(fp,
            "frames: ref=%d test=%d compared=%d (ref-only=%u, test-only=%u) mismatches=%d\n",
            ref->n_rows, test->n_rows,
            ref->n_rows < test->n_rows ? ref->n_rows : test->n_rows, ref_only, test_only,
            mismatches);
    return mismatches;
}

/* ------------------------------------------------------------------ */
/* Comparator self-test (runs in every build — offline, no GPU).       */
/* ------------------------------------------------------------------ */

static char *test_vulkan_adm_comparator_selftest(void)
{
    static const char *base[] = {
        "# tool=selftest",
        "# fields: frame s0num s0den adm2",
        "frame=0 s0num=0x1.d904dep+4 s0den=0x1.f064p+4 adm2=0x1p+0 | 29.56 (ignored)",
        "frame=1 s0num=0x1.d904dep+4 s0den=0x1.f064p+4 adm2=0x1p+0 | 29.56 (ignored)",
        "frame=2 s0num=0x1.d904dep+4 s0den=0x1.f064p+4 adm2=0x1p+0 | 29.56 (ignored)",
    };
    /* One flipped hex digit on frame=1 s0num. */
    static const char *flipped[] = {
        "# tool=selftest",
        "# fields: frame s0num s0den adm2",
        "frame=0 s0num=0x1.d904dep+4 s0den=0x1.f064p+4 adm2=0x1p+0 | x",
        "frame=1 s0num=0x1.d904cfp+4 s0den=0x1.f064p+4 adm2=0x1p+0 | x",
        "frame=2 s0num=0x1.d904dep+4 s0den=0x1.f064p+4 adm2=0x1p+0 | x",
    };
    /* frame=1 dropped. */
    static const char *dropped[] = {
        "# tool=selftest",
        "# fields: frame s0num s0den adm2",
        "frame=0 s0num=0x1.d904dep+4 s0den=0x1.f064p+4 adm2=0x1p+0 | x",
        "frame=2 s0num=0x1.d904dep+4 s0den=0x1.f064p+4 adm2=0x1p+0 | x",
    };
    /* Disagreeing '# fields:' lists -> parse error. */
    static const char *short_fields[] = {
        "# fields: frame s0num s0den",
        "frame=0 s0num=0x1p+0 s0den=0x1p+0",
    };
    /* Malformed frame index -> parse error. */
    static const char *malformed[] = {
        "# fields: frame s0num",
        "frame=x s0num=0x1p+0",
    };
    /* No frame lines -> parse error. */
    static const char *empty[] = {
        "# tool=selftest",
    };

    Contract ref, test;
    const char *err = NULL;

    mu_assert("selftest: base parse", !contract_parse(base, 5, &ref, &err));
    mu_assert("selftest: identical -> 0 mismatches",
              !contract_compare(&ref, &ref, 10, stderr));
    contract_free(&ref);

    mu_assert("selftest: ref parse (flipped)", !contract_parse(base, 5, &ref, &err));
    mu_assert("selftest: flipped parse", !contract_parse(flipped, 5, &test, &err));
    mu_assert("selftest: 1-hex-digit flip -> exactly 1 mismatch",
              contract_compare(&ref, &test, 10, stderr) == 1);
    contract_free(&ref);
    contract_free(&test);

    mu_assert("selftest: ref parse (dropped)", !contract_parse(base, 5, &ref, &err));
    mu_assert("selftest: dropped parse", !contract_parse(dropped, 4, &test, &err));
    mu_assert("selftest: dropped frame -> mismatch", contract_compare(&ref, &test, 10, stderr));
    contract_free(&ref);
    contract_free(&test);

    mu_assert("selftest: base parse (fields case)", !contract_parse(base, 5, &ref, &err));
    mu_assert("selftest: short-fields parse", !contract_parse(short_fields, 2, &test, &err));
    mu_assert("selftest: disagreeing '# fields:' lists -> hard refusal",
              contract_compare(&ref, &test, 10, stderr) == -1);
    contract_free(&ref);
    contract_free(&test);

    mu_assert("selftest: malformed frame index -> parse error",
              contract_parse(malformed, 2, &test, &err) == -1 && err);
    contract_free(&test);

    mu_assert("selftest: no frame lines -> parse error",
              contract_parse(empty, 1, &test, &err) == -1 && err);
    contract_free(&test);

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Vulkan leg.                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned w;
    unsigned h;
    const char *fixture_name;
    const char *ref_path;
} GateFixture;

static const GateFixture gate_fixtures[] = {
    { 576, 324, "576x324_48f", VULKAN_ADM_REF_DIR "/cpu-adm-576x324-48f.txt" },
    { 640, 480, "640x480_48f", VULKAN_ADM_REF_DIR "/cpu-adm-640x480-48f.txt" },
};

/* Feed one yuv420p8 frame pair from `fref`/`fdis`. Returns 0, 1 on
 * short read (end of input), negative on error. */
static int read_and_feed(VmafContext *vmaf, FILE *fref, FILE *fdis, unsigned w, unsigned h,
                         unsigned index)
{
    const size_t luma = (size_t)w * h;
    const size_t chroma = (size_t)(w / 2) * (h / 2);
    const size_t frame_bytes = luma + 2 * chroma;

    uint8_t *buf = malloc(frame_bytes);
    if (!buf)
        return -ENOMEM;
    int rc = 0;
    VmafPicture pics[2];
    memset(pics, 0, sizeof(pics));

    for (int side = 0; side < 2; side++) {
        FILE *f = side ? fdis : fref;
        if (fread(buf, 1, frame_bytes, f) != frame_bytes) {
            rc = 1;
            goto out;
        }
        int err = vmaf_picture_alloc(&pics[side], VMAF_PIX_FMT_YUV420P, 8, w, h);
        if (err) {
            rc = err;
            goto out;
        }
        uint8_t *dst_y = (uint8_t *)pics[side].data[0];
        for (unsigned r = 0; r < h; r++)
            memcpy(dst_y + (size_t)r * pics[side].stride[0], buf + (size_t)r * w, w);
        uint8_t *dst_u = (uint8_t *)pics[side].data[1];
        for (unsigned r = 0; r < h / 2; r++)
            memcpy(dst_u + (size_t)r * pics[side].stride[1],
                   buf + luma + (size_t)r * (w / 2), w / 2);
        uint8_t *dst_v = (uint8_t *)pics[side].data[2];
        for (unsigned r = 0; r < h / 2; r++)
            memcpy(dst_v + (size_t)r * pics[side].stride[2],
                   buf + luma + chroma + (size_t)r * (w / 2), w / 2);
    }
    rc = vmaf_read_pictures(vmaf, &pics[0], &pics[1], index);
    /* The context owns the pictures whatever the call returns
     * (libvmaf.h:397), so there is nothing to free on the error path. */
out:
    free(buf);
    return rc;
}

/* One parity run over a gate fixture. `ref` is the committed CPU
 * contract; `test_contract` the twin's, built frame by frame. */
static char *check_parity(const GateFixture *fx)
{
    /* The committed reference. */
    FILE *rf = fopen(fx->ref_path, "r");
    if (!rf) {
        mu_assert("committed reference file missing", 0);
        return NULL;
    }
    char *ref_lines[FRAMES + 8];
    int n_ref = 0;
    char line[2048];
    while (n_ref < FRAMES + 8 && fgets(line, sizeof(line), rf)) {
        line[strcspn(line, "\r\n")] = '\0';
        ref_lines[n_ref] = strdup(line);
        if (!ref_lines[n_ref++]) {
            mu_assert("out of memory", 0);
        }
    }
    fclose(rf);

    Contract ref_contract;
    const char *err = NULL;
    mu_assert("reference contract parse", !contract_parse((const char *const *)ref_lines, n_ref,
                                                          &ref_contract, &err));
    for (int i = 0; i < n_ref; i++)
        free(ref_lines[i]);

    /* The Vulkan twin. Registration exists only in
     * -Denable_vulkan=enabled builds: an unregistered name makes
     * vmaf_use_feature fail, and that skip states the build shape. */
    VmafFeatureDictionary *opts = NULL;
    mu_assert("feature dictionary alloc", !vmaf_feature_dictionary_set(&opts, "debug", "true"));
    VmafConfiguration cfg = { .log_level = VMAF_LOG_LEVEL_NONE };
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", !vmaf_init(&vmaf, cfg));
    const int use_err = vmaf_use_feature(vmaf, "integer_adm_vulkan", opts);
    if (use_err) {
        (void)vmaf_feature_dictionary_free(&opts);
        (void)vmaf_close(vmaf);
        (void)fprintf(stderr, "[skip: integer_adm_vulkan not built (enable_vulkan off)] ");
        mu_skipped = 1;
        contract_free(&ref_contract);
        return NULL;
    }

    char ref_yuv[512];
    char dis_yuv[512];
    snprintf(ref_yuv, sizeof(ref_yuv), VULKAN_ADM_TESTDATA_DIR "/ref_%s.yuv", fx->fixture_name);
    snprintf(dis_yuv, sizeof(dis_yuv), VULKAN_ADM_TESTDATA_DIR "/dis_%s.yuv", fx->fixture_name);
    FILE *fref = fopen(ref_yuv, "rb");
    FILE *fdis = fopen(dis_yuv, "rb");
    if (!fref || !fdis) {
        (void)vmaf_feature_dictionary_free(&opts);
        (void)vmaf_close(vmaf);
        if (fref)
            fclose(fref);
        (void)fprintf(stderr, "[skip: gate fixture yuv files not present] ");
        mu_skipped = 1;
        contract_free(&ref_contract);
        return NULL;
    }

    /* Feed the whole fixture pair, flush, then build the twin's
     * contract (the cpu_ref.c producer reads every score after the
     * end-of-sequence flush, so the same order keeps the score
     * storage path identical). */
    char *test_lines[FRAMES + 1];
    int produced = 0;
    int run_err = 0;
    /* The comparator is contract-degenerating: both sides must carry a
     * declared '# fields:' list (compare.py reads it from the files it
     * compares). The committed refs carry theirs; the twin's contract
     * declares the same columns here. */
    {
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr), "# fields: frame");
        for (int k = 0; k < N_VALUE_FIELDS && hlen > 0 && hlen < (int)sizeof(hdr); k++)
            hlen += snprintf(hdr + hlen, sizeof(hdr) - (size_t)hlen, " %s", column_names[k]);
        test_lines[produced] = strdup(hdr);
        if (!test_lines[produced++])
            run_err = -ENOMEM;
    }
    for (unsigned i = 0; i < FRAMES && !run_err; i++) {
        run_err = read_and_feed(vmaf, fref, fdis, fx->w, fx->h, i);
        if (run_err == 1)
            run_err = -EINVAL; /* fewer frames than the gate ran: fixture mismatch */
    }
    if (!run_err)
        run_err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    fclose(fref);
    fclose(fdis);
    for (unsigned i = 0; i < FRAMES && !run_err; i++) {
        double vals[N_VALUE_FIELDS];
        char payload[2048];
        int len = snprintf(payload, sizeof(payload), "frame=%u", i);
        for (int k = 0; k < N_VALUE_FIELDS; k++) {
            if (vmaf_feature_score_at_index(vmaf, feature_names[k], &vals[k], i)) {
                run_err = -EINVAL;
                break;
            }
            len += snprintf(payload + len, sizeof(payload) - (size_t)len, " %s=%a",
                            column_names[k], vals[k]);
        }
        if (!run_err) {
            snprintf(payload + len, sizeof(payload) - (size_t)len, " | (decimal mirror)");
            test_lines[produced] = strdup(payload);
            if (!test_lines[produced++])
                run_err = -ENOMEM;
        }
    }

    const int device_err = (run_err == -ENODEV || run_err == -ENOSYS || run_err == -ENOTSUP);
    if (device_err) {
        for (int i = 0; i < produced; i++)
            free(test_lines[i]);
        (void)vmaf_feature_dictionary_free(&opts);
        (void)vmaf_close(vmaf);
        (void)fprintf(stderr, "[skip: no Vulkan compute device at init (%d)] ", run_err);
        mu_skipped = 1;
        contract_free(&ref_contract);
        return NULL;
    }
    mu_assert("vulkan twin run failed", !run_err);

    /* Comparing per frame as the frames were produced avoids holding a
     * second full contract; keep it simple: parse the produced lines. */
    Contract test_contract;
    if (contract_parse((const char *const *)test_lines, produced, &test_contract, &err)) {
        (void)fprintf(stderr, "produced=%d parse err=%s first line=%.120s\n", produced,
                      err ? err : "(null)", produced > 0 ? test_lines[0] : "");
        mu_assert("produced contract parse", 0);
    }
    for (int i = 0; i < produced; i++)
        free(test_lines[i]);

    (void)vmaf_feature_dictionary_free(&opts);

    mu_assert("vmaf_close failed", !vmaf_close(vmaf));

    const int mismatches = contract_compare(&ref_contract, &test_contract, 8, stderr);
    contract_free(&ref_contract);
    contract_free(&test_contract);
    mu_assert("contract field lists disagree between the committed reference and the twin "
              "output (parse degeneration)",
              mismatches >= 0);
    mu_assert("vulkan integer_adm is not bit-exact against the committed CPU references "
              "(see stderr for the first mismatching frame/field pairs)",
              mismatches == 0);
    return NULL;
}

static char *test_vulkan_adm_parity_576x324(void)
{
    return check_parity(&gate_fixtures[0]);
}

static char *test_vulkan_adm_parity_640x480(void)
{
    return check_parity(&gate_fixtures[1]);
}

char *run_tests(void)
{
    /* The comparator self-test runs in EVERY build: the gate must be
     * provably non-vacuous offline (ADR-3402 parity-gate contract,
     * non-vacuousness clause). */
    mu_run_test(test_vulkan_adm_comparator_selftest);
    mu_run_test(test_vulkan_adm_parity_576x324);
    mu_run_test(test_vulkan_adm_parity_640x480);
    return NULL;
}
