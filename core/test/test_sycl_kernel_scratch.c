/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* ADR-1395: SYCL kernels use no scratch memory on Intel GPUs. Under the Linux
 * xe kernel driver an Arc A-series GPU returns wrong values from a kernel with
 * a private array in memory or with spilled registers.
 *
 * The audit builds every kernel registered in this program (libvmaf's device
 * images) for the default GPU. A kernel with a non-zero private_mem_size or
 * spill_memory_size fails the test unless core/src/sycl/scratch_ratchet.txt
 * lists it; a listed kernel that no longer uses scratch memory only prints a
 * note asking for its line to be removed. The test also checks that the
 * ratchet file and vmaf_sycl_scratch_extractors() name the same extractors,
 * and that the self-test's private-array probe really uses private memory.
 * Without a GPU it skips (exit 77).
 *
 * The ratchet list is read from the path meson bakes in, unless the environment
 * names another file in VMAF_SYCL_SCRATCH_RATCHET_FILE: a test program copied
 * off the build machine (the Windows SYCL tester zip, ADR-1566) carries the list
 * next to it, and the baked path names nothing on the tester's PC. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#if HAVE_SYCL

#include "gpu_dispatch_env.h"
#include "libvmaf/libvmaf_sycl.h"
#include "sycl/common.h"
#include "sycl/scratch_check.h"
#include "compat/path_utf8.h"

#ifndef VMAF_SYCL_SCRATCH_RATCHET
#error "meson passes the ratchet list path as VMAF_SYCL_SCRATCH_RATCHET"
#endif

enum {
    MAX_ENTRIES = 64,
    MAX_FILE_LINES = 512,
    MAX_LINE = 1024,
    MAX_KERNEL = 512,
    MAX_NAME = 64,
    MAX_NAMES = 32,
    RATCHET_FIELDS = 4,
};

struct RatchetEntry {
    char extractors[MAX_LINE];
    char kernel[MAX_KERNEL];
    int registered;
    int uses_scratch;
};

static struct {
    struct RatchetEntry entries[MAX_ENTRIES];
    int n_entries;
    int kernels;
    int with_scratch;
    int unlisted;
} audit;

static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0';
}

/* Copies whitespace-separated field `index` of `line[0..len)` into `out`. */
static int get_field(const char *line, size_t len, int index, char *out, size_t cap)
{
    size_t i = 0;
    for (int field = 0; field <= index && i < len; field++) {
        while (i < len && (line[i] == ' ' || line[i] == '\t'))
            i++;
        const size_t start = i;
        while (i < len && !is_blank(line[i]))
            i++;
        const size_t n = i - start;
        if (field == index && n > 0 && n < cap) {
            memcpy(out, &line[start], n);
            out[n] = '\0';
            return 0;
        }
    }
    return -EINVAL;
}

/* 1: comment or blank line, 0: entry parsed, -EINVAL: malformed. */
static int parse_line(const char *line, struct RatchetEntry *e)
{
    const size_t len = strnlen(line, MAX_LINE);
    size_t first = 0;
    while (first < len && (line[first] == ' ' || line[first] == '\t'))
        first++;
    if (first == len || line[first] == '#' || is_blank(line[first]))
        return 1;
    char scratch_bytes[MAX_NAME];
    if (get_field(line, len, 0, e->extractors, sizeof(e->extractors)) ||
        get_field(line, len, 1, scratch_bytes, sizeof(scratch_bytes)) ||
        get_field(line, len, 2, scratch_bytes, sizeof(scratch_bytes)) ||
        get_field(line, len, RATCHET_FIELDS - 1, e->kernel, sizeof(e->kernel))) {
        return -EINVAL;
    }
    return 0;
}

static char *load_ratchet(void)
{
    /* VMAF_SYCL_SCRATCH_RATCHET_FILE when set and not empty, else the path of the
     * source tree this program was built from; read through the once-only
     * environment snapshot (ADR-0488) rather than a getenv of this thread. */
    const char *const override = vmaf_gpu_dispatch_env_get("VMAF_SYCL_SCRATCH_RATCHET_FILE");
    const char *const path =
        (override != NULL && override[0] != '\0') ? override : VMAF_SYCL_SCRATCH_RATCHET;
    FILE *f = vmaf_fopen_utf8(path, "r");
    mu_assert("cannot open core/src/sycl/scratch_ratchet.txt", f != NULL);
    char line[MAX_LINE];
    int bad = 0;
    for (int n = 0; n < MAX_FILE_LINES && !bad && fgets(line, sizeof(line), f) != NULL; n++) {
        if (audit.n_entries == MAX_ENTRIES) {
            bad = 1;
            break;
        }
        const int rc = parse_line(line, &audit.entries[audit.n_entries]);
        bad = rc < 0;
        audit.n_entries += (rc == 0);
    }
    const int closed = fclose(f);
    mu_assert("malformed or oversized scratch_ratchet.txt", !bad);
    mu_assert("fclose(scratch_ratchet.txt) failed", closed == 0);
    return NULL;
}

static struct RatchetEntry *find_entry(const char *kernel)
{
    for (int i = 0; i < audit.n_entries; i++) {
        if (strcmp(audit.entries[i].kernel, kernel) == 0)
            return &audit.entries[i];
    }
    return NULL;
}

static void on_kernel(const struct VmafSyclKernelScratch *k, void *user)
{
    (void)user;
    const int scratch = (k->private_bytes != 0) || (k->spill_bytes != 0);
    audit.kernels++;
    audit.with_scratch += scratch;
    struct RatchetEntry *e = find_entry(k->name);
    if (e != NULL) {
        e->registered = 1;
        e->uses_scratch = scratch;
        return;
    }
    if (!scratch)
        return;
    audit.unlisted++;
    (void)fprintf(stderr,
                  "  FAIL: %s uses scratch memory (private %zu B, spill %zu B) and is not in "
                  "core/src/sycl/scratch_ratchet.txt. Remove the scratch use (ADR-1395).\n"
                  "        kernel id: %s\n",
                  k->pretty, k->private_bytes, k->spill_bytes, k->name);
}

static void report_removable_entries(void)
{
    for (int i = 0; i < audit.n_entries; i++) {
        const struct RatchetEntry *e = &audit.entries[i];
        if (e->registered && e->uses_scratch)
            continue;
        (void)fprintf(stderr,
                      "  NOTE: remove this line from core/src/sycl/scratch_ratchet.txt; the "
                      "kernel %s: %s\n",
                      e->registered ? "no longer uses scratch memory" : "is not registered",
                      e->kernel);
    }
}

static char *test_kernels_outside_ratchet_use_no_scratch(void)
{
    mu_assert_msg(load_ratchet());

    /* Rejecting NULL arguments is all this call does; it also keeps
     * dmabuf_import.o, whose de-tiling kernels the audit covers, in this
     * statically linked program. */
    mu_assert("a VA surface import without a state must fail",
              vmaf_sycl_import_va_surface(NULL, NULL, 0, 0, 0, 0, 0) < 0);

    const int n = vmaf_sycl_kernel_scratch_audit(on_kernel, NULL);
    if (n == -ENODEV || n == -ENOSYS) {
        (void)fprintf(stderr, "  [SKIP] no SYCL GPU to audit (%d)\n", n);
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("kernel scratch audit failed", n > 0);
    (void)fprintf(stderr, "  audited %d kernels: %d use scratch memory, %d listed in the ratchet\n",
                  audit.kernels, audit.with_scratch, audit.n_entries);
    report_removable_entries();
    mu_assert("a kernel outside the ratchet list uses scratch memory (see FAIL lines)",
              audit.unlisted == 0);
    return NULL;
}

/* Splits a list of names separated by commas and spaces into `names`;
 * returns the count, or -1 when one does not fit. */
static int split_names(const char *list, char names[][MAX_NAME], int cap)
{
    const size_t len = strnlen(list, MAX_LINE);
    int n = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && (list[i] == ',' || list[i] == ' '))
            i++;
        const size_t start = i;
        while (i < len && list[i] != ',' && list[i] != ' ')
            i++;
        const size_t name_len = i - start;
        if (name_len == 0)
            break;
        if (n == cap || name_len >= MAX_NAME)
            return -1;
        memcpy(names[n], &list[start], name_len);
        names[n][name_len] = '\0';
        n++;
    }
    return n;
}

static int contains(char names[][MAX_NAME], int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0)
            return 1;
    }
    return 0;
}

static char *test_ratchet_names_the_logged_extractors(void)
{
    static char logged[MAX_NAMES][MAX_NAME];
    static char listed[MAX_NAMES][MAX_NAME];
    static char names[MAX_NAMES][MAX_NAME];
    const int n_logged = split_names(vmaf_sycl_scratch_extractors(), logged, MAX_NAMES);
    mu_assert("vmaf_sycl_scratch_extractors() is malformed", n_logged >= 0);
    int n_listed = 0;
    for (int i = 0; i < audit.n_entries; i++) {
        const int n = split_names(audit.entries[i].extractors, names, MAX_NAMES);
        mu_assert("a ratchet extractor column is malformed", n > 0);
        for (int j = 0; j < n; j++) {
            mu_assert("a ratchet extractor is missing from vmaf_sycl_scratch_extractors()",
                      contains(logged, n_logged, names[j]));
            if (!contains(listed, n_listed, names[j]) && n_listed < MAX_NAMES)
                memcpy(listed[n_listed++], names[j], MAX_NAME);
        }
    }
    mu_assert("vmaf_sycl_scratch_extractors() names an extractor the ratchet list does not",
              n_listed == n_logged);
    return NULL;
}

static char *test_selftest_probe_uses_private_memory(void)
{
    VmafSyclState *state = NULL;
    const VmafSyclConfiguration cfg = {.device_index = -1};
    if (vmaf_sycl_state_init(&state, cfg) != 0) {
        mu_skipped = 1;
        return NULL;
    }
    struct VmafSyclScratchProbe r;
    const int err = vmaf_sycl_scratch_probe(vmaf_sycl_get_queue_ptr(state), &r);
    vmaf_sycl_state_free(&state);
    mu_assert("scratch probe failed to run", err == 0);
    (void)fprintf(stderr,
                  "  probes: private array %zu B, %u/%u wrong; spill %s%zu B, %u/%u wrong\n",
                  r.private_bytes, r.private_wrong, r.work_items, r.spill_ran ? "" : "(not run) ",
                  r.spill_bytes, r.spill_wrong, r.work_items);
    mu_assert("the private-array probe must use private memory, or the self-test proves nothing",
              r.private_bytes > 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_kernels_outside_ratchet_use_no_scratch);
    if (mu_skipped)
        return NULL;
    mu_run_test(test_ratchet_names_the_logged_extractors);
    mu_run_test(test_selftest_probe_uses_private_memory);
    return NULL;
}

#else

char *run_tests(void)
{
    return NULL;
}

#endif

/* NOLINTEND(modernize-use-nullptr) */
