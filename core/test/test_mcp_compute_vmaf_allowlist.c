/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  R2-4 regression — the in-process C MCP `compute_vmaf` scoring binding
 *  (core/src/mcp/compute_vmaf.c) must reject caller-supplied reference /
 *  distorted YUV paths that resolve OUTSIDE the allowlisted roots, exactly
 *  like the Python (mcp-server/vmaf-mcp/src/vmaf_mcp/server.py) and Go
 *  (pkg/libvmaf/paths.go) MCP servers already do.
 *
 *  Before the fix `score_yuv_pair()` stat()'d and open()'d any path the
 *  caller passed — a path-traversal / arbitrary-file-read hole. The fix
 *  canonicalises with realpath(3) and requires the result to sit under one
 *  of <repo>/testdata, <repo>/python/test/resource, <repo>/model,
 *  /workspace/python/test/resource, or a $VMAF_MCP_ALLOW entry.
 *
 *  White-box strategy: `validate_path()` is `static`, so this test
 *  #include's the compilation unit directly (the established pattern — see
 *  test_feature_collector.c which #include's feature_collector.c). The
 *  public entry point `vmaf_mcp_compute_vmaf` is macro-renamed before the
 *  include so it does not clash with the copy already linked from libvmaf
 *  when enable_mcp=true.
 *
 *  Load-bearing proof: with the guard removed, validate_path() does not
 *  exist and the out-of-root assertions below fail to compile / reject;
 *  with the guard present every assertion passes. Toggle confirmed in the
 *  PR description.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"

/* Dodge the duplicate-symbol clash with libvmaf's own copy of the public
 * entry point (present when the library is built with enable_mcp=true). */
#define vmaf_mcp_compute_vmaf vmaf_mcp_compute_vmaf__allowlist_test_dup
/* White-box include of the CU under test to reach the static validate_path();
 * established pattern, see test_feature_collector.c. */
/* NOLINTNEXTLINE(bugprone-suspicious-include) — white-box test, see above (ADR-0141 / ADR-0278). */
#include "mcp/compute_vmaf.c"
#undef vmaf_mcp_compute_vmaf

/* Return the repo root via the same marker-walk validate_path() uses, so
 * the test can build an in-allowlist path that actually exists on disk. */
static int test_repo_root(char *out, size_t out_sz)
{
    return discover_repo_root(out, out_sz);
}

/* A path that resolves outside every allowlisted root must be rejected
 * with -EACCES and a non-NULL owned error message. /etc/passwd exists on
 * every CI host and is never under an allowlisted root. */
static char *test_rejects_outside_allowlist(void)
{
    char real[PATH_MAX];
    char *err = NULL;
    int rc = validate_path("/etc/passwd", real, sizeof(real), &err);
    int had_err = err != NULL;
    free(err); /* Free before any (short-circuiting) assert — no leak path. */
    mu_assert("out-of-root path must be rejected with -EACCES", rc == -EACCES);
    mu_assert("rejection must set an owned error message", had_err);
    return NULL;
}

/* A `../`-traversal that escapes an allowlisted root after canonicalisation
 * must also be rejected. realpath() collapses the `..` so the resolved
 * target lands outside testdata/. */
static char *test_rejects_traversal_escape(void)
{
    char root[PATH_MAX];
    if (test_repo_root(root, sizeof(root)) != 0) {
        /* Cannot locate repo root in this environment — skip rather than
         * false-fail. The /etc/passwd case already proves rejection. */
        (void)fprintf(stderr, "(skip: repo root not found) ");
        return NULL;
    }
    /* testdata/../README.md resolves to <repo>/README.md, outside testdata. */
    char traversal[PATH_MAX];
    int n = snprintf(traversal, sizeof(traversal), "%s/testdata/../README.md", root);
    mu_assert("snprintf overflow building traversal path", n > 0 && (size_t)n < sizeof(traversal));
    char real[PATH_MAX];
    char *err = NULL;
    int rc = validate_path(traversal, real, sizeof(real), &err);
    mu_assert("`..`-escape path must be rejected with -EACCES", rc == -EACCES);
    free(err);
    return NULL;
}

/* A genuine in-allowlist file must be accepted (rc==0) and the resolved
 * canonical path returned. Uses a real file under <repo>/python/test/resource. */
static char *test_accepts_in_allowlist(void)
{
    char root[PATH_MAX];
    if (test_repo_root(root, sizeof(root)) != 0) {
        (void)fprintf(stderr, "(skip: repo root not found) ");
        return NULL;
    }
    /* Pick a file that exists under an allowlisted root. testdata/ is a
     * default root and ships committed .yuv fixtures; dis_576x324_48f.yuv is
     * one. The Netflix golden YUV tree under python/test/resource/yuv/ may be
     * absent in a sparse / LFS-skipped checkout, so testdata/ is more
     * reliable for CI. */
    char in_root[PATH_MAX];
    int n = snprintf(in_root, sizeof(in_root), "%s/testdata/dis_576x324_48f.yuv", root);
    mu_assert("snprintf overflow building in-root path", n > 0 && (size_t)n < sizeof(in_root));

    /* Confirm the fixture exists; if not (sparse checkout), skip cleanly. */
    if (access(in_root, R_OK) != 0) {
        (void)fprintf(stderr, "(skip: testdata YUV fixture absent) ");
        return NULL;
    }

    char real[PATH_MAX];
    char *err = NULL;
    int rc = validate_path(in_root, real, sizeof(real), &err);
    if (err != NULL)
        free(err);
    mu_assert("in-allowlist existing file must be accepted (rc==0)", rc == 0);
    mu_assert("accepted path must be non-empty", real[0] != '\0');
    return NULL;
}

/* VMAF_MCP_ALLOW must extend the root set: a path under a directory named
 * only in the env variable is accepted. Proves env parity with Python/Go. */
static char *test_vmaf_mcp_allow_extends_roots(void)
{
    /* /tmp is never a default root. Add it via VMAF_MCP_ALLOW and confirm a
     * file created under it validates. */
    char tmpl[] = "/tmp/vmaf_allowlist_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    mu_assert("mkdtemp failed", dir != NULL);

    char file_path[PATH_MAX];
    int n = snprintf(file_path, sizeof(file_path), "%s/probe.yuv", dir);
    mu_assert("snprintf overflow building tmp file path", n > 0 && (size_t)n < sizeof(file_path));
    FILE *f = fopen(file_path, "wb");
    mu_assert("fopen probe file failed", f != NULL);
    (void)fputc('x', f);
    (void)fclose(f);

    /* Without the env var the path is rejected. */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — single-thread test setup (ADR-0141 / ADR-0278). */
    (void)unsetenv("VMAF_MCP_ALLOW");
    char real[PATH_MAX];
    char *err = NULL;
    int rc_before = validate_path(file_path, real, sizeof(real), &err);
    free(err);
    mu_assert("tmp path must be rejected without VMAF_MCP_ALLOW", rc_before == -EACCES);

    /* With the env var pointing at the temp dir the path is accepted. */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — single-thread test setup (ADR-0141 / ADR-0278). */
    int se = setenv("VMAF_MCP_ALLOW", dir, 1);
    mu_assert("setenv VMAF_MCP_ALLOW failed", se == 0);
    err = NULL;
    int rc_after = validate_path(file_path, real, sizeof(real), &err);
    free(err);
    mu_assert("tmp path must be accepted with VMAF_MCP_ALLOW", rc_after == 0);

    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — single-thread test setup (ADR-0141 / ADR-0278). */
    (void)unsetenv("VMAF_MCP_ALLOW");
    (void)unlink(file_path);
    (void)rmdir(dir);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_rejects_outside_allowlist);
    mu_run_test(test_rejects_traversal_escape);
    mu_run_test(test_accepts_in_allowlist);
    mu_run_test(test_vmaf_mcp_allow_extends_roots);
    return NULL;
}
