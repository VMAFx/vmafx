/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  T6-9 / ADR-0211 — exercise the failure modes of
 *  vmaf_dnn_verify_signature() without requiring `cosign` on the test
 *  host. The function fails closed, so every "missing X" path returns a
 *  negative errno and short-circuits before posix_spawnp is reached.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "test.h"

#include "dnn/model_loader.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

static char *test_verify_null_path(void)
{
    const int err = vmaf_dnn_verify_signature(NULL, NULL);
    mu_assert("NULL onnx path -> -EINVAL", err == -EINVAL);
    return NULL;
}

#ifndef _WIN32
/* Helper: write a tiny scratch file + return its path inside `out_path`.
 *
 * Uses open(O_CREAT|O_WRONLY|O_TRUNC, 0600) instead of fopen("wb") so the
 * fixture creates the file with owner-only permissions. fopen() defers
 * mode to the umask (typically 0022, yielding 0644 — and 0666 in some
 * containerised CI environments), which CodeQL flags as
 * cpp/world-writable-file-creation. Tightening to 0600 keeps the test's
 * scratch output out of any other user's reach on shared CI hosts. */
static int write_tmp(const char *suffix, const char *body, char *out_path, size_t out_sz)
{
    const int n = snprintf(out_path, out_sz, "/tmp/vmaf_verify_test_%d_%s", (int)getpid(), suffix);
    if (n < 0 || (size_t)n >= out_sz)
        return -1;
    const int fd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return -1;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        (void)close(fd);
        return -1;
    }
    if (body && fputs(body, f) < 0) {
        (void)fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

static char *test_verify_missing_registry(void)
{
    /* No registry beside /tmp/foo.onnx — expect -ENOENT. */
    char onnx[256];
    const int wrc = write_tmp("missing_reg.onnx", "fake", onnx, sizeof(onnx));
    mu_assert("write_tmp succeeded", wrc == 0);
    const int err = vmaf_dnn_verify_signature(onnx, "/nonexistent/registry.json");
    mu_assert("missing registry -> -ENOENT", err == -ENOENT);
    (void)unlink(onnx);
    return NULL;
}

static char *test_verify_no_matching_entry(void)
{
    /* Registry exists but has no entry for `unknown.onnx`. */
    char reg_path[256];
    const int wrc = write_tmp(
        "reg.json", "{\"models\":[{\"id\":\"x\",\"onnx\":\"other.onnx\",\"sha256\":\"00\"}]}",
        reg_path, sizeof(reg_path));
    mu_assert("write_tmp reg succeeded", wrc == 0);
    const int err = vmaf_dnn_verify_signature("/tmp/unknown.onnx", reg_path);
    mu_assert("no entry -> -ENOENT", err == -ENOENT);
    (void)unlink(reg_path);
    return NULL;
}

static char *test_verify_missing_bundle(void)
{
    /* Registry has the entry, but the sigstore_bundle file does not exist. */
    char reg_path[256];
    const char *body = "{\"models\":[{\"id\":\"y\",\"onnx\":\"y.onnx\",\"sha256\":\"00\","
                       "\"sigstore_bundle\":\"y.onnx.sigstore.json\"}]}";
    const int wrc = write_tmp("reg2.json", body, reg_path, sizeof(reg_path));
    mu_assert("write_tmp reg2 succeeded", wrc == 0);
    const int err = vmaf_dnn_verify_signature("/tmp/y.onnx", reg_path);
    mu_assert("missing bundle -> -ENOENT", err == -ENOENT);
    (void)unlink(reg_path);
    return NULL;
}

static char *test_verify_entry_without_bundle(void)
{
    /* Entry exists but has no `sigstore_bundle` key — refuse to load
     * (unsigned model). */
    char reg_path[256];
    const char *body = "{\"models\":[{\"id\":\"z\",\"onnx\":\"z.onnx\",\"sha256\":\"00\"}]}";
    const int wrc = write_tmp("reg3.json", body, reg_path, sizeof(reg_path));
    mu_assert("write_tmp reg3 succeeded", wrc == 0);
    const int err = vmaf_dnn_verify_signature("/tmp/z.onnx", reg_path);
    mu_assert("no bundle key -> -ENOENT", err == -ENOENT);
    (void)unlink(reg_path);
    return NULL;
}

/* Helper: set up a per-test scratch directory under /tmp containing
 * registry.json + bundle file beside the model. Permissions are 0700 on
 * the directory and 0600 on the files (CodeQL fix for shared CI hosts).
 * Returns 0 on success, -1 on any setup failure. */
static int setup_scratch_dir(char *out_dir, size_t out_sz, const char *suffix)
{
    const int n = snprintf(out_dir, out_sz, "/tmp/vmaf_verify_dir_%d_%s", (int)getpid(), suffix);
    if (n < 0 || (size_t)n >= out_sz)
        return -1;
    /* Idempotent: clean up if a previous run left it behind. */
    (void)mkdir(out_dir, S_IRWXU);
    if (chmod(out_dir, S_IRWXU) != 0)
        return -1;
    return 0;
}

/* Write `body` into <dir>/<name> with mode 0600. */
static int write_in_dir(const char *dir, const char *name, const char *body)
{
    char path[512];
    const int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= (int)sizeof(path))
        return -1;
    const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return -1;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        (void)close(fd);
        return -1;
    }
    if (body && fputs(body, f) < 0) {
        (void)fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

/* Best-effort cleanup of <dir>/<name>. Errors ignored — these are
 * scratch files inside a per-pid /tmp dir. */
static void cleanup_in_dir(const char *dir, const char *name)
{
    char path[512];
    const int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= (int)sizeof(path))
        return;
    (void)unlink(path);
}

static char *test_verify_default_registry_missing(void)
{
    /* registry_path = NULL forces the default-registry derivation:
     *   <dirname(onnx_path)>/registry.json
     * The onnx file lives in our scratch dir, but registry.json is
     * intentionally absent — exercises slurp_registry()'s fopen-fail
     * branch through the default-path lookup logic (lines 547-560). */
    char dir[256];
    mu_assert("setup scratch dir", setup_scratch_dir(dir, sizeof(dir), "defreg") == 0);
    mu_assert("write onnx", write_in_dir(dir, "model.onnx", "fake") == 0);

    char onnx_path[512];
    const int n = snprintf(onnx_path, sizeof(onnx_path), "%s/model.onnx", dir);
    mu_assert("snprintf onnx_path", n > 0 && (size_t)n < sizeof(onnx_path));

    const int err = vmaf_dnn_verify_signature(onnx_path, NULL);
    /* fopen() of the missing default registry returns -ENOENT. */
    mu_assert("default registry missing -> -ENOENT", err == -ENOENT);

    cleanup_in_dir(dir, "model.onnx");
    (void)rmdir(dir);
    return NULL;
}

static char *test_verify_default_registry_no_slash_in_onnx(void)
{
    /* onnx_path has no '/' separator -> falls through to the
     * model/tiny/registry.json default. That path won't exist in /tmp's
     * cwd, so we expect a slurp_registry() failure (-ENOENT). Exercises
     * lines 555-558 (the "no slash in onnx_path" branch). */
    const int err = vmaf_dnn_verify_signature("nonexistent.onnx", NULL);
    /* Whatever cwd resolves "model/tiny/registry.json" to, it must not
     * exist for an absolute-no-slash test argument run from the meson
     * test harness. Accept any negative errno that means "not found". */
    mu_assert("no-slash onnx + missing default reg -> negative", err < 0);
    return NULL;
}

static char *test_verify_registry_no_slash_in_path(void)
{
    /* registry_path has no '/' — exercises the bundle_abs `else` branch
     * (lines 601-607) where we just memcpy the relative bundle path. We
     * expect -ENOENT because the bundle won't exist in cwd. Use chdir to
     * a known scratch dir for a deterministic outcome. */
    char dir[256];
    mu_assert("setup scratch dir", setup_scratch_dir(dir, sizeof(dir), "noslash") == 0);
    const char *body = "{\"models\":[{\"id\":\"r\",\"onnx\":\"r.onnx\",\"sha256\":\"00\","
                       "\"sigstore_bundle\":\"r.onnx.sigstore.json\"}]}";
    mu_assert("write registry", write_in_dir(dir, "reg_noslash.json", body) == 0);

    /* Save + restore cwd so the test is hermetic. */
    char cwd_save[512];
    mu_assert("getcwd", getcwd(cwd_save, sizeof(cwd_save)) != NULL);
    mu_assert("chdir into scratch", chdir(dir) == 0);

    /* registry_path is a basename only — no '/'. */
    const int err = vmaf_dnn_verify_signature("/tmp/r.onnx", "reg_noslash.json");

    (void)chdir(cwd_save);
    mu_assert("no-slash registry + missing bundle -> -ENOENT", err == -ENOENT);

    cleanup_in_dir(dir, "reg_noslash.json");
    (void)rmdir(dir);
    return NULL;
}

/* Build a fake `cosign` shell script under <dir>/cosign that exits
 * `exit_code`. Used to exercise the posix_spawnp + waitpid paths
 * without requiring the real Sigstore toolchain. Mode 0700 (owner
 * read/write/exec; CodeQL-clean). */
static int write_fake_cosign(const char *dir, int exit_code)
{
    char path[512];
    const int n = snprintf(path, sizeof(path), "%s/cosign", dir);
    if (n < 0 || (size_t)n >= (int)sizeof(path))
        return -1;
    const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IXUSR);
    if (fd < 0)
        return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        (void)close(fd);
        return -1;
    }
    /* Minimal shebang stub. The fork's verify-blob argv gets ignored
     * — only the exit code matters for branch coverage. */
    if (fprintf(f, "#!/bin/sh\nexit %d\n", exit_code) < 0) {
        (void)fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

static char *run_with_fake_cosign(int exit_code, int expected_err)
{
    char dir[256];
    mu_assert("setup scratch dir",
              setup_scratch_dir(dir, sizeof(dir), exit_code == 0 ? "cosign_ok" : "cosign_fail") ==
                  0);
    /* Registry + bundle + onnx all under <dir>. The bundle must be a
     * regular file (any contents — the fake cosign ignores them). */
    const char *reg_body = "{\"models\":[{\"id\":\"k\",\"onnx\":\"k.onnx\",\"sha256\":\"00\","
                           "\"sigstore_bundle\":\"k.onnx.sigstore.json\"}]}";
    mu_assert("write registry", write_in_dir(dir, "registry.json", reg_body) == 0);
    mu_assert("write bundle", write_in_dir(dir, "k.onnx.sigstore.json", "{}") == 0);
    mu_assert("write onnx", write_in_dir(dir, "k.onnx", "fake") == 0);
    mu_assert("write fake cosign", write_fake_cosign(dir, exit_code) == 0);

    /* Save + override PATH so locate_cosign() resolves to our stub. */
    char path_save[2048];
    const char *path_env = getenv("PATH");
    const int pn = snprintf(path_save, sizeof(path_save), "%s", path_env ? path_env : "");
    mu_assert("snprintf PATH save", pn >= 0 && (size_t)pn < sizeof(path_save));
    mu_assert("setenv PATH override", setenv("PATH", dir, 1) == 0);

    char onnx_path[512];
    const int n = snprintf(onnx_path, sizeof(onnx_path), "%s/k.onnx", dir);
    mu_assert("snprintf onnx_path", n > 0 && (size_t)n < (int)sizeof(onnx_path));
    char reg_path[512];
    const int rn = snprintf(reg_path, sizeof(reg_path), "%s/registry.json", dir);
    mu_assert("snprintf reg_path", rn > 0 && (size_t)rn < (int)sizeof(reg_path));

    const int err = vmaf_dnn_verify_signature(onnx_path, reg_path);

    /* Restore PATH before assertion so a fail doesn't leak state. */
    (void)setenv("PATH", path_save, 1);
    cleanup_in_dir(dir, "registry.json");
    cleanup_in_dir(dir, "k.onnx.sigstore.json");
    cleanup_in_dir(dir, "k.onnx");
    cleanup_in_dir(dir, "cosign");
    (void)rmdir(dir);

    mu_assert("verify_signature returned expected status", err == expected_err);
    return NULL;
}

static char *test_verify_cosign_success(void)
{
    /* Fake cosign exits 0 -> verify_signature() returns 0. Exercises
     * locate_cosign() success (lines 513-534), posix_spawnp() success
     * (line 648), waitpid() (line 653), and the WIFEXITED+0 success
     * tail (lines 657-659). */
    return run_with_fake_cosign(0, 0);
}

static char *test_verify_cosign_nonzero_exit(void)
{
    /* Fake cosign exits 1 -> verify_signature() returns -EPROTO.
     * Exercises the same locate/spawn/wait path plus the failure tail
     * at line 657. */
    return run_with_fake_cosign(1, -EPROTO);
}

static char *test_verify_cosign_not_on_path(void)
{
    /* PATH set to an empty directory with no `cosign` binary. The
     * locate_cosign() walk completes without finding a match and
     * returns -EACCES, which propagates through verify_signature().
     * Exercises the locate_cosign() failure tail (line 534). */
    char dir[256];
    mu_assert("setup scratch dir", setup_scratch_dir(dir, sizeof(dir), "nopath") == 0);
    const char *reg_body = "{\"models\":[{\"id\":\"q\",\"onnx\":\"q.onnx\",\"sha256\":\"00\","
                           "\"sigstore_bundle\":\"q.onnx.sigstore.json\"}]}";
    mu_assert("write registry", write_in_dir(dir, "registry.json", reg_body) == 0);
    mu_assert("write bundle", write_in_dir(dir, "q.onnx.sigstore.json", "{}") == 0);
    mu_assert("write onnx", write_in_dir(dir, "q.onnx", "fake") == 0);

    char path_save[2048];
    const char *path_env = getenv("PATH");
    const int pn = snprintf(path_save, sizeof(path_save), "%s", path_env ? path_env : "");
    mu_assert("snprintf PATH save", pn >= 0 && (size_t)pn < sizeof(path_save));
    /* Point PATH at a directory we control that does NOT contain
     * cosign. (Using "" would tickle the empty-string short-circuit
     * at the top of locate_cosign — we want the full PATH walk to
     * exit without a hit.) */
    mu_assert("setenv PATH override", setenv("PATH", dir, 1) == 0);

    char onnx_path[512];
    const int n = snprintf(onnx_path, sizeof(onnx_path), "%s/q.onnx", dir);
    mu_assert("snprintf onnx_path", n > 0 && (size_t)n < (int)sizeof(onnx_path));
    char reg_path[512];
    const int rn = snprintf(reg_path, sizeof(reg_path), "%s/registry.json", dir);
    mu_assert("snprintf reg_path", rn > 0 && (size_t)rn < (int)sizeof(reg_path));

    const int err = vmaf_dnn_verify_signature(onnx_path, reg_path);

    (void)setenv("PATH", path_save, 1);
    cleanup_in_dir(dir, "registry.json");
    cleanup_in_dir(dir, "q.onnx.sigstore.json");
    cleanup_in_dir(dir, "q.onnx");
    (void)rmdir(dir);

    mu_assert("cosign not on PATH -> -EACCES", err == -EACCES);
    return NULL;
}

static char *test_verify_cosign_path_empty_string(void)
{
    /* Empty PATH string short-circuits locate_cosign() at line 517 with
     * -EACCES before any directory walk. Distinct branch from the
     * "PATH set but cosign not present" case above. */
    char dir[256];
    mu_assert("setup scratch dir", setup_scratch_dir(dir, sizeof(dir), "emptypath") == 0);
    const char *reg_body = "{\"models\":[{\"id\":\"e\",\"onnx\":\"e.onnx\",\"sha256\":\"00\","
                           "\"sigstore_bundle\":\"e.onnx.sigstore.json\"}]}";
    mu_assert("write registry", write_in_dir(dir, "registry.json", reg_body) == 0);
    mu_assert("write bundle", write_in_dir(dir, "e.onnx.sigstore.json", "{}") == 0);
    mu_assert("write onnx", write_in_dir(dir, "e.onnx", "fake") == 0);

    char path_save[2048];
    const char *path_env = getenv("PATH");
    const int pn = snprintf(path_save, sizeof(path_save), "%s", path_env ? path_env : "");
    mu_assert("snprintf PATH save", pn >= 0 && (size_t)pn < sizeof(path_save));
    mu_assert("setenv PATH=''", setenv("PATH", "", 1) == 0);

    char onnx_path[512];
    const int n = snprintf(onnx_path, sizeof(onnx_path), "%s/e.onnx", dir);
    mu_assert("snprintf onnx_path", n > 0 && (size_t)n < (int)sizeof(onnx_path));
    char reg_path[512];
    const int rn = snprintf(reg_path, sizeof(reg_path), "%s/registry.json", dir);
    mu_assert("snprintf reg_path", rn > 0 && (size_t)rn < (int)sizeof(reg_path));

    const int err = vmaf_dnn_verify_signature(onnx_path, reg_path);

    (void)setenv("PATH", path_save, 1);
    cleanup_in_dir(dir, "registry.json");
    cleanup_in_dir(dir, "e.onnx.sigstore.json");
    cleanup_in_dir(dir, "e.onnx");
    (void)rmdir(dir);

    mu_assert("empty PATH -> -EACCES", err == -EACCES);
    return NULL;
}

static char *run_with_registry_body(const char *body, const char *onnx_path, int expected_err,
                                    const char *suffix)
{
    char reg_path[256];
    const int wrc = write_tmp(suffix, body, reg_path, sizeof(reg_path));
    mu_assert("write_tmp succeeded", wrc == 0);
    const int err = vmaf_dnn_verify_signature(onnx_path, reg_path);
    (void)unlink(reg_path);
    mu_assert("verify_signature returned expected status", err == expected_err);
    return NULL;
}

static char *test_verify_malformed_onnx_no_colon(void)
{
    /* `"onnx"` key with no ':' after it -> -ENOENT (line 402). */
    const char *body = "{\"onnx\" \"x.onnx\"}";
    return run_with_registry_body(body, "/tmp/x.onnx", -ENOENT, "mal_nocolon.json");
}

static char *test_verify_malformed_onnx_no_open_quote(void)
{
    /* `"onnx":` followed by a non-quoted token -> -EBADMSG (line 407). */
    const char *body = "{\"onnx\": x.onnx}";
    return run_with_registry_body(body, "/tmp/x.onnx", -EBADMSG, "mal_noopen.json");
}

static char *test_verify_malformed_onnx_no_close_quote(void)
{
    /* `"onnx": "value` with no closing quote -> -EBADMSG (line 411). */
    const char *body = "{\"onnx\": \"x.onnx";
    return run_with_registry_body(body, "/tmp/x.onnx", -EBADMSG, "mal_noclose.json");
}

static char *test_verify_malformed_bundle_no_colon(void)
{
    /* Matching entry but `"sigstore_bundle"` key has no ':' -> -EBADMSG
     * (line 424). Construct the value string carefully so that the
     * malformed-key fragment occurs *before* the next entry. */
    const char *body = "{\"models\":[{\"id\":\"x\",\"onnx\":\"m.onnx\",\"sigstore_bundle\""
                       " \"m.bundle\"}]}";
    return run_with_registry_body(body, "/tmp/m.onnx", -EBADMSG, "mal_bcolon.json");
}

static char *test_verify_malformed_bundle_no_open_quote(void)
{
    /* `"sigstore_bundle":` followed by a non-quoted token -> -EBADMSG
     * (line 429). */
    const char *body = "{\"models\":[{\"id\":\"x\",\"onnx\":\"m.onnx\",\"sigstore_bundle\":"
                       " m.bundle}]}";
    return run_with_registry_body(body, "/tmp/m.onnx", -EBADMSG, "mal_bopen.json");
}

static char *test_verify_malformed_bundle_no_close_quote(void)
{
    /* `"sigstore_bundle": "value` with no closing quote -> -EBADMSG
     * (line 433). The terminator file body is intentionally truncated. */
    const char *body = "{\"models\":[{\"id\":\"x\",\"onnx\":\"m.onnx\",\"sigstore_bundle\":"
                       " \"m.bundle";
    return run_with_registry_body(body, "/tmp/m.onnx", -EBADMSG, "mal_bclose.json");
}

static char *test_verify_bundle_is_directory(void)
{
    /* Bundle path resolves to a directory (not a regular file). Exercises
     * the !S_ISREG branch (line 616) — distinct from the stat-fails
     * branch already covered by test_verify_missing_bundle. */
    char dir[256];
    mu_assert("setup scratch dir", setup_scratch_dir(dir, sizeof(dir), "bundle_isdir") == 0);
    const char *reg_body = "{\"models\":[{\"id\":\"d\",\"onnx\":\"d.onnx\",\"sha256\":\"00\","
                           "\"sigstore_bundle\":\"bundle_dir\"}]}";
    mu_assert("write registry", write_in_dir(dir, "registry.json", reg_body) == 0);
    /* Make `bundle_dir` an actual directory inside <dir>. */
    char bundle_path[512];
    const int bn = snprintf(bundle_path, sizeof(bundle_path), "%s/bundle_dir", dir);
    mu_assert("snprintf bundle_path", bn > 0 && (size_t)bn < (int)sizeof(bundle_path));
    mu_assert("mkdir bundle_dir", mkdir(bundle_path, S_IRWXU) == 0);

    char reg_path[512];
    const int rn = snprintf(reg_path, sizeof(reg_path), "%s/registry.json", dir);
    mu_assert("snprintf reg_path", rn > 0 && (size_t)rn < (int)sizeof(reg_path));

    const int err = vmaf_dnn_verify_signature("/tmp/d.onnx", reg_path);
    mu_assert("bundle is directory -> -ENOENT", err == -ENOENT);

    (void)rmdir(bundle_path);
    cleanup_in_dir(dir, "registry.json");
    (void)rmdir(dir);
    return NULL;
}
#endif /* !_WIN32 */

#ifdef _WIN32
static char *test_verify_windows_returns_enosys(void)
{
    const int err = vmaf_dnn_verify_signature("C:\\fake.onnx", NULL);
    mu_assert("Windows path -> -ENOSYS", err == -ENOSYS);
    return NULL;
}
#endif

char *run_tests(void)
{
    mu_run_test(test_verify_null_path);
#ifndef _WIN32
    mu_run_test(test_verify_missing_registry);
    mu_run_test(test_verify_no_matching_entry);
    mu_run_test(test_verify_missing_bundle);
    mu_run_test(test_verify_entry_without_bundle);
    mu_run_test(test_verify_default_registry_missing);
    mu_run_test(test_verify_default_registry_no_slash_in_onnx);
    mu_run_test(test_verify_registry_no_slash_in_path);
    mu_run_test(test_verify_bundle_is_directory);
    mu_run_test(test_verify_malformed_onnx_no_colon);
    mu_run_test(test_verify_malformed_onnx_no_open_quote);
    mu_run_test(test_verify_malformed_onnx_no_close_quote);
    mu_run_test(test_verify_malformed_bundle_no_colon);
    mu_run_test(test_verify_malformed_bundle_no_open_quote);
    mu_run_test(test_verify_malformed_bundle_no_close_quote);
    mu_run_test(test_verify_cosign_path_empty_string);
    mu_run_test(test_verify_cosign_not_on_path);
    mu_run_test(test_verify_cosign_success);
    mu_run_test(test_verify_cosign_nonzero_exit);
#else
    mu_run_test(test_verify_windows_returns_enosys);
#endif
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
