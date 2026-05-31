/**
 *
 *  Copyright 2026 Lusoris.
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

/* Coverage push for core/src/feature/mkdirp.c (0% baseline).
 *
 * mkdirp is the recursive-mkdir helper consumed by cambi.c's heatmap
 * sidecar (cambi.c:708).  Before this test, the file had 0/35 line
 * coverage because cambi_test never exercises the
 * `heatmaps_path` option.  This test drives the public symbol
 * directly with the same four input shapes that the cambi path
 * indirectly relies on: NULL guard, single-level mkdir, idempotent
 * re-call (EEXIST), and nested-path normalization (collapses
 * consecutive '/').
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "feature/mkdirp.h"
#include "test.h"

static char *test_mkdirp_null_path(void)
{
    int rc = mkdirp(NULL, 0770);
    mu_assert("mkdirp(NULL) must return -1", rc == -1);
    return NULL;
}

static char *test_mkdirp_single_level(void)
{
    /* Use mkdtemp to get a unique parent the test owns. */
    char tmpl[] = "/tmp/vmaf_mkdirp_test_XXXXXX";
    char *parent = mkdtemp(tmpl);
    mu_assert("mkdtemp must succeed", parent != NULL);

    char path[256];
    (void)snprintf(path, sizeof(path), "%s/leaf", parent);

    int rc = mkdirp(path, 0770);
    mu_assert("mkdirp single-level must succeed", rc == 0);

    struct stat st;
    int rc_stat = stat(path, &st);
    mu_assert("mkdirp must actually create the directory", rc_stat == 0 && S_ISDIR(st.st_mode));

    /* Cleanup. */
    (void)rmdir(path);
    (void)rmdir(parent);
    return NULL;
}

static char *test_mkdirp_idempotent_eexist(void)
{
    char tmpl[] = "/tmp/vmaf_mkdirp_eexist_XXXXXX";
    char *parent = mkdtemp(tmpl);
    mu_assert("mkdtemp must succeed", parent != NULL);

    char path[256];
    (void)snprintf(path, sizeof(path), "%s/once", parent);

    int rc = mkdirp(path, 0770);
    mu_assert("first mkdirp must succeed", rc == 0);

    /* Second call must also return success (EEXIST is treated as 0). */
    rc = mkdirp(path, 0770);
    mu_assert("re-running mkdirp on an existing path must return 0 (EEXIST)", rc == 0);

    (void)rmdir(path);
    (void)rmdir(parent);
    return NULL;
}

static char *test_mkdirp_normalize_double_slash(void)
{
    /* path_normalize must collapse consecutive slashes; verify the
     * recursive walk produces a single nested directory regardless. */
    char tmpl[] = "/tmp/vmaf_mkdirp_norm_XXXXXX";
    char *parent = mkdtemp(tmpl);
    mu_assert("mkdtemp must succeed", parent != NULL);

    char path[512];
    (void)snprintf(path, sizeof(path), "%s//a///b///c", parent);

    int rc = mkdirp(path, 0770);
    mu_assert("mkdirp with redundant '/' must succeed", rc == 0);

    char clean[512];
    (void)snprintf(clean, sizeof(clean), "%s/a/b/c", parent);

    struct stat st;
    int rc_stat = stat(clean, &st);
    mu_assert("mkdirp must create the normalized nested path", rc_stat == 0 && S_ISDIR(st.st_mode));

    /* Cleanup deepest-first. */
    char d2[512];
    char d1[512];
    (void)snprintf(d2, sizeof(d2), "%s/a/b", parent);
    (void)snprintf(d1, sizeof(d1), "%s/a", parent);
    (void)rmdir(clean);
    (void)rmdir(d2);
    (void)rmdir(d1);
    (void)rmdir(parent);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_mkdirp_null_path);
    mu_run_test(test_mkdirp_single_level);
    mu_run_test(test_mkdirp_idempotent_eexist);
    mu_run_test(test_mkdirp_normalize_double_slash);
    return NULL;
}
