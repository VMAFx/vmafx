/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * cambi.c's heatmap writers as cambi_internal.h exports them
 * (vmaf_cambi_open_heatmaps() / _dump_c_values() / _close_heatmaps()): the
 * `heatmaps_path` files of the CPU extractor, and of the Metal twin, which
 * writes its host-resident c-values through the same three functions
 * (T-METAL-CAMBI-SCORE-NAME-SUFFIXED-2026-10-05). The round trip pins the
 * file names, the 16-bit scaling and the per-frame offsets; the edge cases are
 * no path (nothing opened) and a path under a regular file (refused).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define HEATMAP_PATH_SEPARATOR '\\'
#else
#include <unistd.h>
#define HEATMAP_PATH_SEPARATOR '/'
#endif

#include "test.h"
#include "compat/path_utf8.h"
#include "feature/cambi_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

static unsigned long process_id(void)
{
#ifdef _WIN32
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

static int remove_directory(const char *path)
{
#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

#define HEATMAP_ENC_SIDE 216
#define HEATMAP_SCALE 4
#define HEATMAP_SIDE 14 /* 216 halved four times, rounding up */
#define HEATMAP_WINDOW 9
#define HEATMAP_PIXELS (HEATMAP_SIDE * HEATMAP_SIDE)

static const int heatmap_weights[4] = {1, 2, 3, 4};

/* dump_c_values()'s 16-bit sample for c-value `c`: the same scaling, written
 * out with the test's window and weights. */
static uint16_t heatmap_sample(float c)
{
    const int max_c_value = 4 * HEATMAP_WINDOW * HEATMAP_WINDOW / 4;
    const double scaling_value = 65535.0 / max_c_value;
    return (uint16_t)(scaling_value * c);
}

static float heatmap_c_value(int frame, int i)
{
    return (float)((i * 7 + frame * 13) % 82) * 0.987f;
}

static int write_heatmap_frames(FILE *files[VMAF_CAMBI_NUM_SCALES])
{
    float c_values[HEATMAP_PIXELS];
    int err = 0;
    /* Frame 1 before frame 0: each frame lands at its own offset. */
    for (int frame = 1; frame >= 0 && !err; frame--) {
        for (int i = 0; i < HEATMAP_PIXELS; i++)
            c_values[i] = heatmap_c_value(frame, i);
        err = vmaf_cambi_dump_c_values(files, c_values, HEATMAP_SIDE, HEATMAP_SIDE, HEATMAP_SCALE,
                                       HEATMAP_WINDOW, 4, heatmap_weights, frame);
    }
    return err;
}

static int heatmap_path(char *path, size_t size, const char *directory, int scale, int side)
{
    const int len = snprintf(path, size, "%s%ccambi_heatmap_scale_%d_%dx%d_16b.gray", directory,
                             HEATMAP_PATH_SEPARATOR, scale, side, side);
    return len > 0 && (size_t)len < size;
}

/* 1 when the scale's file holds both frames' samples and nothing else. */
static int heatmap_file_matches(const char *directory)
{
    char path[512];
    if (!heatmap_path(path, sizeof(path), directory, HEATMAP_SCALE, HEATMAP_SIDE))
        return 0;
    FILE *file = vmaf_fopen_utf8(path, "rb");
    if (!file)
        return 0;
    uint16_t samples[(2 * HEATMAP_PIXELS) + 1];
    const size_t read =
        fread(samples, sizeof(samples[0]), sizeof(samples) / sizeof(samples[0]), file);
    int ok = fclose(file) == 0 && read == (size_t)(2 * HEATMAP_PIXELS);
    for (int frame = 0; frame < 2 && ok; frame++) {
        for (int i = 0; i < HEATMAP_PIXELS && ok; i++)
            ok = samples[(frame * HEATMAP_PIXELS) + i] == heatmap_sample(heatmap_c_value(frame, i));
    }
    return ok;
}

/* 1 when the five files and the directory are gone. */
static int remove_heatmap_directory(const char *directory)
{
    int removed = 1;
    int side = HEATMAP_ENC_SIDE;
    for (int scale = 0; scale < VMAF_CAMBI_NUM_SCALES; scale++) {
        char path[512];
        removed = heatmap_path(path, sizeof(path), directory, scale, side) &&
                  vmaf_remove_utf8(path) == 0 && removed;
        side = (side + 1) / 2;
    }
    return remove_directory(directory) == 0 && removed;
}

typedef struct HeatmapRoundTrip {
    int open_rc;
    int dump_rc;
    int closed;
    int matches;
    int removed;
} HeatmapRoundTrip;

/* Open, write two frames, close (twice), read back, remove. */
static HeatmapRoundTrip run_heatmap_round_trip(const char *directory)
{
    FILE *files[VMAF_CAMBI_NUM_SCALES] = {NULL};
    HeatmapRoundTrip r;
    r.open_rc = vmaf_cambi_open_heatmaps(directory, HEATMAP_ENC_SIDE, HEATMAP_ENC_SIDE, files);
    r.dump_rc = r.open_rc ? r.open_rc : write_heatmap_frames(files);
    r.closed = vmaf_cambi_close_heatmaps(directory, files) == 0;
    for (int scale = 0; scale < VMAF_CAMBI_NUM_SCALES; scale++)
        r.closed = r.closed && files[scale] == NULL;
    r.closed = r.closed && vmaf_cambi_close_heatmaps(directory, files) == 0;
    r.matches = heatmap_file_matches(directory);
    r.removed = remove_heatmap_directory(directory);
    return r;
}

static char *test_heatmap_writers_round_trip(void)
{
    char directory[256];
    const int len =
        snprintf(directory, sizeof(directory), "vmaf_cambi_heatmap_writers_%lu", process_id());
    mu_assert("heatmap directory path overflow", len > 0 && (size_t)len < sizeof(directory));
    const HeatmapRoundTrip r = run_heatmap_round_trip(directory);
    mu_assert("vmaf_cambi_open_heatmaps() failed", r.open_rc == 0);
    mu_assert("vmaf_cambi_dump_c_values() failed", r.dump_rc == 0);
    mu_assert("vmaf_cambi_close_heatmaps() failed, left a slot set or failed again", r.closed);
    mu_assert("the heatmap file does not hold both frames' samples", r.matches);
    mu_assert("the heatmap files or directory could not be removed", r.removed);
    return NULL;
}

/* No path: nothing is opened. */
static char *test_heatmap_writers_without_path(void)
{
    FILE *files[VMAF_CAMBI_NUM_SCALES] = {NULL};
    const int rc = vmaf_cambi_open_heatmaps(NULL, 216, 216, files);
    int none = 1;
    for (int scale = 0; scale < VMAF_CAMBI_NUM_SCALES; scale++)
        none = none && files[scale] == NULL;
    mu_assert("no heatmaps_path must succeed and open nothing", rc == 0 && none);
    mu_assert("closing unopened heatmaps must succeed",
              vmaf_cambi_close_heatmaps(NULL, files) == 0);
    return NULL;
}

/* A heatmaps_path under a regular file is refused, and closing after it works. */
static char *test_heatmap_writers_bad_path(void)
{
    char blocker[256];
    char path[300];
    const int len =
        snprintf(blocker, sizeof(blocker), "vmaf_cambi_heatmap_blocker_%lu", process_id());
    const int path_len = snprintf(path, sizeof(path), "%s%csub", blocker, HEATMAP_PATH_SEPARATOR);
    mu_assert("blocker path overflow", len > 0 && (size_t)len < sizeof(blocker) && path_len > 0 &&
                                           (size_t)path_len < sizeof(path));
    FILE *file = vmaf_fopen_utf8(blocker, "wb");
    mu_assert("could not create the blocking file", file != NULL);
    const int closed = fclose(file) == 0;
    FILE *files[VMAF_CAMBI_NUM_SCALES] = {NULL};
    const int open_rc = vmaf_cambi_open_heatmaps(path, 216, 216, files);
    const int close_rc = vmaf_cambi_close_heatmaps(path, files);
    const int removed = vmaf_remove_utf8(blocker) == 0;
    mu_assert("could not close or remove the blocking file", closed && removed);
    mu_assert("a heatmaps_path under a regular file must be refused", open_rc == -EINVAL);
    mu_assert("closing after a refused open must succeed", close_rc == 0);
    return NULL;
}

static int flush_heatmap_files(FILE *files[VMAF_CAMBI_NUM_SCALES])
{
    int ok = 1;
    for (int scale = 0; scale < VMAF_CAMBI_NUM_SCALES; scale++)
        ok = files[scale] != NULL && fflush(files[scale]) == 0 && ok;
    return ok;
}

typedef struct HeatmapSharedPath {
    int first_ok;
    int second_open_rc;
    int closed;
    int kept;
    int reopen_truncates;
    int removed;
} HeatmapSharedPath;

/* Context A writes and flushes; context B opens the same directory while A is
 * open and must not truncate A's samples. Once both closed, the next open
 * starts a new run and truncates again (Netflix/vmaf PR 1707). */
static HeatmapSharedPath run_heatmap_shared_path(const char *directory)
{
    FILE *first[VMAF_CAMBI_NUM_SCALES] = {NULL};
    FILE *second[VMAF_CAMBI_NUM_SCALES] = {NULL};
    FILE *third[VMAF_CAMBI_NUM_SCALES] = {NULL};
    HeatmapSharedPath r;
    r.first_ok =
        vmaf_cambi_open_heatmaps(directory, HEATMAP_ENC_SIDE, HEATMAP_ENC_SIDE, first) == 0 &&
        write_heatmap_frames(first) == 0 && flush_heatmap_files(first);
    r.second_open_rc =
        vmaf_cambi_open_heatmaps(directory, HEATMAP_ENC_SIDE, HEATMAP_ENC_SIDE, second);
    r.closed = vmaf_cambi_close_heatmaps(directory, first) == 0;
    r.closed = vmaf_cambi_close_heatmaps(directory, second) == 0 && r.closed;
    r.kept = heatmap_file_matches(directory);
    r.reopen_truncates =
        vmaf_cambi_open_heatmaps(directory, HEATMAP_ENC_SIDE, HEATMAP_ENC_SIDE, third) == 0;
    r.reopen_truncates = vmaf_cambi_close_heatmaps(directory, third) == 0 && r.reopen_truncates &&
                         !heatmap_file_matches(directory);
    r.removed = remove_heatmap_directory(directory);
    return r;
}

static char *test_heatmap_writers_shared_path(void)
{
    char directory[256];
    const int len =
        snprintf(directory, sizeof(directory), "vmaf_cambi_heatmap_shared_%lu", process_id());
    mu_assert("heatmap directory path overflow", len > 0 && (size_t)len < sizeof(directory));
    const HeatmapSharedPath r = run_heatmap_shared_path(directory);
    mu_assert("the first context could not open, write or flush", r.first_ok);
    mu_assert("the second context could not open the shared path", r.second_open_rc == 0);
    mu_assert("closing the two contexts failed", r.closed);
    mu_assert("the second open truncated the first context's samples", r.kept);
    mu_assert("an open after every context closed must truncate again", r.reopen_truncates);
    mu_assert("the heatmap files or directory could not be removed", r.removed);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_heatmap_writers_shared_path);
    mu_run_test(test_heatmap_writers_round_trip);
    mu_run_test(test_heatmap_writers_without_path);
    mu_run_test(test_heatmap_writers_bad_path);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
