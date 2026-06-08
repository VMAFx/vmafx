/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "test.h"

#include "dnn/model_loader.h"

static char *test_sniff_by_extension(void)
{
    mu_assert("json → SVM", vmaf_dnn_sniff_kind("foo.json") == VMAF_MODEL_KIND_SVM);
    mu_assert("pkl → SVM", vmaf_dnn_sniff_kind("foo.pkl") == VMAF_MODEL_KIND_SVM);
    mu_assert("onnx → DNN_FR", vmaf_dnn_sniff_kind("foo.onnx") == VMAF_MODEL_KIND_DNN_FR);
    mu_assert("unknown ext → -1", vmaf_dnn_sniff_kind("foo.bin") == -1);
    mu_assert("NULL → -1", vmaf_dnn_sniff_kind(NULL) == -1);
    return NULL;
}

static char *test_size_cap(void)
{
    /* Use a regular file that exists on every supported host as a proxy for
     * "regular file, within limits". /etc/passwd ships on Linux and macOS;
     * /etc/hostname is Linux-only and made the macOS leg fail with -ENOENT
     * when this assertion expects -E2BIG (size cap) or 0 (success). */
#ifdef _WIN32
    const char *probe = "C:\\Windows\\System32\\cmd.exe";
#else
    const char *probe = "/etc/passwd";
#endif
    int err = vmaf_dnn_validate_onnx(probe, 1);
    mu_assert("expected -E2BIG for 1-byte cap", err == -E2BIG || err == 0);
    err = vmaf_dnn_validate_onnx("/definitely/does/not/exist.onnx", 0);
    mu_assert("expected errno for missing file", err < 0);
    return NULL;
}

static char *test_validate_null_path(void)
{
    const int err = vmaf_dnn_validate_onnx(NULL, 0);
    mu_assert("NULL path → -EINVAL", err == -EINVAL);
    return NULL;
}

#ifndef _WIN32
/* Write @p len bytes of @p data to a fresh temp file; returns malloc'd path. */
static char *write_temp(const unsigned char *data, size_t len)
{
    char tmpl[] = "/tmp/vmaf-dnn-validate-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return NULL;
    const ssize_t w = write(fd, data, len);
    close(fd);
    if (w != (ssize_t)len)
        return NULL;
    return strdup(tmpl);
}

/* Write @p len bytes of @p data to an exact path with user-only perms (0600).
 * Unlike fopen(path, "wb") this doesn't inherit umask and can't race into a
 * world-writable state (CodeQL cpp/world-writable-file-creation). */
static int write_file_600(const char *path, const unsigned char *data, size_t len)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    const ssize_t w = write(fd, data, len);
    const int rc = close(fd);
    if (w != (ssize_t)len || rc != 0)
        return -1;
    return 0;
}
#endif /* !_WIN32 */

/* Open @p path for writing with user-only perms (0600), returning a buffered
 * FILE* via fdopen so existing fprintf-based test code keeps working. Same
 * umask-safety motivation as write_file_600. Returns NULL on any error.
 * Available on both POSIX and MinGW: open()/close() come via <fcntl.h> +
 * <io.h>, fdopen() via <stdio.h>. The 0600 mode is a no-op on Windows
 * (NTFS uses ACLs), but harmless. */
static FILE *fopen_w_600(const char *path)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return NULL;
    FILE *fp = fdopen(fd, "w");
    if (!fp)
        (void)close(fd);
    return fp;
}

#ifndef _WIN32

static char *test_validate_zero_byte(void)
{
    char *path = write_temp((const unsigned char *)"", 0);
    mu_assert("temp file creation failed", path != NULL);
    const int err = vmaf_dnn_validate_onnx(path, 0);
    (void)remove(path);
    free(path);
    mu_assert("empty file → -EBADMSG", err == -EBADMSG);
    return NULL;
}

static char *test_validate_allowed_onnx(void)
{
    /* Minimal ModelProto { graph { node { op_type = "Conv" } } } */
    const unsigned char buf[] = {0x3A, 0x08, 0x0A, 0x06, 0x22, 0x04, 'C', 'o', 'n', 'v'};
    char *path = write_temp(buf, sizeof(buf));
    mu_assert("temp file creation failed", path != NULL);
    const int err = vmaf_dnn_validate_onnx(path, 0);
    (void)remove(path);
    free(path);
    mu_assert("allowed Conv onnx → 0", err == 0);
    return NULL;
}

static char *test_validate_disallowed_onnx(void)
{
    /* ADR-0169 / T6-5: Loop joined the allowlist, so a bare Loop is no
     * longer an example of a forbidden op. Use Scan, which stays
     * rejected by design (see ADR-0169 § Alternatives considered). */
    const unsigned char buf[] = {0x3A, 0x08, 0x0A, 0x06, 0x22, 0x04, 'S', 'c', 'a', 'n'};
    char *path = write_temp(buf, sizeof(buf));
    mu_assert("temp file creation failed", path != NULL);
    const int err = vmaf_dnn_validate_onnx(path, 0);
    (void)remove(path);
    free(path);
    mu_assert("disallowed Scan onnx → -EPERM", err == -EPERM);
    return NULL;
}

static char *test_validate_symlink_to_dir(void)
{
    /* Path hardening: realpath() must resolve a symlink to /tmp (a
     * directory) before the S_ISREG gate, so validate must reject. */
    char link_path[] = "/tmp/vmaf-dnn-dirlink-XXXXXX";
    int fd = mkstemp(link_path);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    (void)remove(link_path);
    mu_assert("symlink() failed", symlink("/tmp", link_path) == 0);
    const int err = vmaf_dnn_validate_onnx(link_path, 0);
    (void)remove(link_path);
    /* /tmp is not a regular file → stat_regular returns -ENOENT. */
    mu_assert("symlink-to-dir → -ENOENT", err == -ENOENT);
    return NULL;
}

/* Allowed ONNX payload (single Conv node) reused across jail tests. */
static const unsigned char kAllowedOnnx[] = {0x3A, 0x08, 0x0A, 0x06, 0x22,
                                             0x04, 'C',  'o',  'n',  'v'};

static char *test_jail_unset_accepts_anywhere(void)
{
    /* With VMAF_TINY_MODEL_DIR unset the jail is a no-op; a valid model
     * anywhere in the filesystem must still validate. */
    unsetenv("VMAF_TINY_MODEL_DIR");
    char *path = write_temp(kAllowedOnnx, sizeof(kAllowedOnnx));
    mu_assert("temp file creation failed", path != NULL);
    const int err = vmaf_dnn_validate_onnx(path, 0);
    (void)remove(path);
    free(path);
    mu_assert("jail unset → allowed model validates", err == 0);
    return NULL;
}

static char *test_jail_accepts_model_inside(void)
{
    /* Model sits inside the jail dir → must validate. */
    char jail[] = "/tmp/vmaf-dnn-jail-XXXXXX";
    mu_assert("mkdtemp failed", mkdtemp(jail) != NULL);

    char model_path[PATH_MAX];
    (void)snprintf(model_path, sizeof(model_path), "%s/allowed.onnx", jail);
    mu_assert("write_file_600 model failed",
              write_file_600(model_path, kAllowedOnnx, sizeof(kAllowedOnnx)) == 0);

    mu_assert("setenv failed", setenv("VMAF_TINY_MODEL_DIR", jail, 1) == 0);
    const int err = vmaf_dnn_validate_onnx(model_path, 0);
    unsetenv("VMAF_TINY_MODEL_DIR");
    (void)remove(model_path);
    rmdir(jail);
    mu_assert("model inside jail → 0", err == 0);
    return NULL;
}

static char *test_jail_rejects_model_outside(void)
{
    /* Model sits outside the jail dir → must return -EACCES before any
     * stat() of the model path happens. */
    char jail[] = "/tmp/vmaf-dnn-jail-XXXXXX";
    mu_assert("mkdtemp failed", mkdtemp(jail) != NULL);

    char *outside = write_temp(kAllowedOnnx, sizeof(kAllowedOnnx));
    mu_assert("write_temp failed", outside != NULL);

    mu_assert("setenv failed", setenv("VMAF_TINY_MODEL_DIR", jail, 1) == 0);
    const int err = vmaf_dnn_validate_onnx(outside, 0);
    unsetenv("VMAF_TINY_MODEL_DIR");
    (void)remove(outside);
    free(outside);
    rmdir(jail);
    mu_assert("model outside jail → -EACCES", err == -EACCES);
    return NULL;
}

static char *test_jail_rejects_sibling_prefix(void)
{
    /* Classic prefix-matching trap: if the jail prefix "/tmp/foo" accepted
     * "/tmp/foobar/x.onnx" via naive strncmp, a sibling dir could escape.
     * The trailing-separator normalisation in enforce_tiny_model_jail must
     * reject this. */
    char jail[] = "/tmp/vmaf-dnn-sibprefix-XXXXXX";
    mu_assert("mkdtemp failed", mkdtemp(jail) != NULL);

    char sibling_dir[PATH_MAX];
    (void)snprintf(sibling_dir, sizeof(sibling_dir), "%s-sibling", jail);
    mu_assert("sibling mkdir failed", mkdir(sibling_dir, 0700) == 0);

    char model_path[PATH_MAX];
    (void)snprintf(model_path, sizeof(model_path), "%s/escape.onnx", sibling_dir);
    mu_assert("write_file_600 sibling model failed",
              write_file_600(model_path, kAllowedOnnx, sizeof(kAllowedOnnx)) == 0);

    mu_assert("setenv failed", setenv("VMAF_TINY_MODEL_DIR", jail, 1) == 0);
    const int err = vmaf_dnn_validate_onnx(model_path, 0);
    unsetenv("VMAF_TINY_MODEL_DIR");
    (void)remove(model_path);
    rmdir(sibling_dir);
    rmdir(jail);
    mu_assert("sibling prefix must be rejected → -EACCES", err == -EACCES);
    return NULL;
}

static char *test_jail_rejects_symlink_escape(void)
{
    /* Symlink-escape attempt: place a symlink *inside* the jail pointing
     * at a file *outside* the jail. realpath() on the model argument
     * resolves the symlink to the outside target, which must then fail
     * the prefix check. */
    char jail[] = "/tmp/vmaf-dnn-jailsym-XXXXXX";
    mu_assert("mkdtemp failed", mkdtemp(jail) != NULL);

    char *outside = write_temp(kAllowedOnnx, sizeof(kAllowedOnnx));
    mu_assert("write_temp failed", outside != NULL);

    char link_path[PATH_MAX];
    (void)snprintf(link_path, sizeof(link_path), "%s/escape.onnx", jail);
    mu_assert("symlink() failed", symlink(outside, link_path) == 0);

    mu_assert("setenv failed", setenv("VMAF_TINY_MODEL_DIR", jail, 1) == 0);
    const int err = vmaf_dnn_validate_onnx(link_path, 0);
    unsetenv("VMAF_TINY_MODEL_DIR");
    (void)remove(link_path);
    (void)remove(outside);
    free(outside);
    rmdir(jail);
    mu_assert("symlink escape must be rejected → -EACCES", err == -EACCES);
    return NULL;
}

static char *test_jail_rejects_nonexistent_jail(void)
{
    /* Fails closed on a misconfigured jail: if VMAF_TINY_MODEL_DIR points
     * at a path that does not exist, every validation returns -EACCES. */
    char *model = write_temp(kAllowedOnnx, sizeof(kAllowedOnnx));
    mu_assert("write_temp failed", model != NULL);

    mu_assert("setenv failed",
              setenv("VMAF_TINY_MODEL_DIR", "/tmp/vmaf-does-not-exist-zzzyx", 1) == 0);
    const int err = vmaf_dnn_validate_onnx(model, 0);
    unsetenv("VMAF_TINY_MODEL_DIR");
    (void)remove(model);
    free(model);
    mu_assert("nonexistent jail dir → -EACCES", err == -EACCES);
    return NULL;
}

static char *test_jail_rejects_non_directory(void)
{
    /* Jail env pointing at a regular file (not a directory) must also fail
     * closed — a file is not a prefix anything can sit under. */
    char *jail_file = write_temp((const unsigned char *)"x", 1u);
    mu_assert("write_temp failed", jail_file != NULL);
    char *model = write_temp(kAllowedOnnx, sizeof(kAllowedOnnx));
    mu_assert("write_temp failed", model != NULL);

    mu_assert("setenv failed", setenv("VMAF_TINY_MODEL_DIR", jail_file, 1) == 0);
    const int err = vmaf_dnn_validate_onnx(model, 0);
    unsetenv("VMAF_TINY_MODEL_DIR");
    (void)remove(model);
    (void)remove(jail_file);
    free(model);
    free(jail_file);
    mu_assert("jail-is-file → -EACCES", err == -EACCES);
    return NULL;
}

static char *test_jail_accepts_trailing_slash(void)
{
    /* Normalisation check: a trailing '/' on the env value must not
     * introduce a double-separator that breaks the prefix match. */
    char jail[] = "/tmp/vmaf-dnn-jailslash-XXXXXX";
    mu_assert("mkdtemp failed", mkdtemp(jail) != NULL);

    char jail_with_slash[PATH_MAX];
    (void)snprintf(jail_with_slash, sizeof(jail_with_slash), "%s/", jail);

    char model_path[PATH_MAX];
    (void)snprintf(model_path, sizeof(model_path), "%s/allowed.onnx", jail);
    mu_assert("write_file_600 model failed",
              write_file_600(model_path, kAllowedOnnx, sizeof(kAllowedOnnx)) == 0);

    mu_assert("setenv failed", setenv("VMAF_TINY_MODEL_DIR", jail_with_slash, 1) == 0);
    const int err = vmaf_dnn_validate_onnx(model_path, 0);
    unsetenv("VMAF_TINY_MODEL_DIR");
    (void)remove(model_path);
    rmdir(jail);
    mu_assert("jail with trailing slash → 0", err == 0);
    return NULL;
}
#endif /* !_WIN32 */

static char *test_sidecar_parses(void)
{
#ifdef _WIN32
    char tmpl[MAX_PATH];
    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    snprintf(tmpl, sizeof tmpl, "%svmaf-dnn-sidecar-test", tmpdir);
    FILE *tmpf = fopen(tmpl, "w");
    mu_assert("temp file creation failed", tmpf != NULL);
    fclose(tmpf);
#else
    char tmpl[] = "/tmp/vmaf-dnn-sidecar-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
#endif

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    /* Touch an empty onnx so sidecar_load doesn't key off its existence. */
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\n"
                     "  \"name\": \"vmaf_tiny_fr_v1\",\n"
                     "  \"kind\": \"fr\",\n"
                     "  \"onnx_opset\": 17,\n"
                     "  \"input_name\":  \"features\",\n"
                     "  \"output_name\": \"score\",\n"
                     "  \"output_names\": [\"score\", \"uncertainty\"]\n"
                     "}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load failed", err == 0);
    mu_assert("kind FR", meta.kind == VMAF_MODEL_KIND_DNN_FR);
    mu_assert("opset 17", meta.opset == 17);
    mu_assert("name set", meta.name && !strcmp(meta.name, "vmaf_tiny_fr_v1"));
    mu_assert("input set", meta.input_name && !strcmp(meta.input_name, "features"));
    mu_assert("output set", meta.output_name && !strcmp(meta.output_name, "score"));
    mu_assert("output_names count", meta.n_output_names == 2u);
    mu_assert("output_names[0] set",
              meta.output_names[0] && !strcmp(meta.output_names[0], "score"));
    mu_assert("output_names[1] set",
              meta.output_names[1] && !strcmp(meta.output_names[1], "uncertainty"));
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

static char *test_sidecar_rejects_null_args(void)
{
    /* NULL onnx_path or NULL out → -EINVAL (line 171). */
    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(NULL, &meta);
    mu_assert("NULL onnx_path rejected", err == -EINVAL);
    err = vmaf_dnn_sidecar_load("model.onnx", NULL);
    mu_assert("NULL out rejected", err == -EINVAL);
    return NULL;
}

static char *test_sidecar_free_null_is_noop(void)
{
    /* free(NULL) must be a no-op (line 241). */
    vmaf_dnn_sidecar_free(NULL);
    mu_assert("sidecar_free(NULL) returned without crashing", 1);
    return NULL;
}

static char *test_sidecar_missing_returns_enoent(void)
{
    /* Sidecar absent → -ENOENT (errno propagation through fopen). */
    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load("/tmp/vmaf-no-such-model-zzz.onnx", &meta);
    mu_assert("missing sidecar → -ENOENT", err == -ENOENT);
    return NULL;
}

#ifndef _WIN32
static char *test_sidecar_non_regular_returns_einval(void)
{
    /* sidecar path exists but is a directory (not a regular file) →
     * S_ISREG gate added by the PR #143 fix must return -EINVAL,
     * not silently fall through to fopen which would then give -EISDIR
     * or -EIO depending on the host. */
    char dir_json[] = "/tmp/vmaf-dnn-dirjson-XXXXXX.json";
    /* mkdtemp requires trailing XXXXXX with no suffix; use a two-step
     * approach: mkstemp + remove + mkdir. */
    char tmp[] = "/tmp/vmaf-dnn-dirjson-XXXXXX";
    int fd = mkstemp(tmp);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    (void)remove(tmp);
    mu_assert("mkdir for dir sidecar failed", mkdir(tmp, 0700) == 0);
    /* Build a fake onnx path whose sidecar ("<base>.json") resolves to
     * the directory we just created.  "<tmp>.onnx" → sidecar = "<tmp>.json";
     * rename the dir to match the sidecar name. */
    (void)snprintf(dir_json, sizeof(dir_json), "%s.json", tmp);
    mu_assert("rename to .json dir failed", rename(tmp, dir_json) == 0);
    char onnx[256];
    (void)snprintf(onnx, sizeof(onnx), "%s.onnx", tmp);
    VmafModelSidecar meta;
    const int err = vmaf_dnn_sidecar_load(onnx, &meta);
    rmdir(dir_json);
    mu_assert("non-regular sidecar (dir) → -EINVAL", err == -EINVAL);
    return NULL;
}

static char *test_sidecar_parses_kind_nr(void)
{
    /* kind == "nr" branch (line 224). */
    char tmpl[] = "/tmp/vmaf-dnn-sidecar-nr-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\"kind\": \"nr\"}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load nr failed", err == 0);
    mu_assert("kind NR", meta.kind == VMAF_MODEL_KIND_DNN_NR);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* ADR-0173 / T5-3: optional `quant_mode` field defaults to FP32 when
 * absent, parses the four valid strings, and falls back to FP32 on any
 * unknown value. */
static char *test_sidecar_quant_mode_default_fp32(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-quant-default-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    /* No quant_mode field — default branch. */
    (void)fprintf(s, "{\"kind\": \"fr\"}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load default failed", err == 0);
    mu_assert("absent quant_mode → FP32", meta.quant_mode == VMAF_QUANT_FP32);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

static char *test_sidecar_quant_mode_dynamic(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-quant-dyn-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\"kind\": \"fr\", \"quant_mode\": \"dynamic\"}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load dynamic failed", err == 0);
    mu_assert("quant_mode dynamic", meta.quant_mode == VMAF_QUANT_DYNAMIC);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

static char *test_sidecar_quant_mode_unknown_falls_back(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-quant-bad-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\"kind\": \"fr\", \"quant_mode\": \"int4\"}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load unknown_mode failed", err == 0);
    mu_assert("unknown quant_mode → FP32 (fail-safe)", meta.quant_mode == VMAF_QUANT_FP32);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

static char *test_sidecar_no_dot_onnx_extension(void)
{
    /* Path that does not end in ".onnx" → falls through to the
     * memcpy(sidecar + len, ".json", 6) branch (line 185). With this
     * branch, foo.bin → foo.bin.json. */
    char tmpl[] = "/tmp/vmaf-dnn-noext-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char model[1024];
    char sidecar[1024];
    (void)snprintf(model, sizeof model, "%s.bin", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.bin.json", tmpl);
    FILE *f = fopen_w_600(model);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\"kind\": \"fr\"}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(model, &meta);
    mu_assert("non-onnx sidecar suffix branch", err == 0);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(model);
    (void)remove(tmpl);
    return NULL;
}

static char *test_sidecar_oversized_path(void)
{
    /* Path of length > sizeof(sidecar) - 6 → -ENAMETOOLONG (line 178).
     * sizeof(sidecar) is 4096; we need a path > 4090 chars. */
    char *huge = (char *)malloc(4100u);
    mu_assert("alloc failed", huge != NULL);
    memset(huge, 'a', 4096u);
    huge[4096] = '\0';
    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(huge, &meta);
    free(huge);
    mu_assert("oversized path → -ENAMETOOLONG", err == -ENAMETOOLONG);
    return NULL;
}

static char *test_sidecar_malformed_keys_default(void)
{
    /* Sidecar JSON missing keys / malformed values: extract_string and
     * extract_int return NULL/error and the loader falls back to defaults
     * without erroring. Drives the no-key / non-string / non-int branches
     * in extract_string (lines 117-138) and extract_int (lines 147-163). */
    char tmpl[] = "/tmp/vmaf-dnn-malformed-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    /* "kind" present but not a string (number) → extract_string returns
     * NULL via "no opening quote" branch. "name" missing entirely →
     * extract_string returns NULL via strstr-miss branch. "onnx_opset"
     * present but not a number → extract_int returns -EINVAL via the
     * endp == p branch. */
    (void)fprintf(s, "{\"kind\": 42, \"onnx_opset\": \"abc\"}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("malformed sidecar still loads with defaults", err == 0);
    /* kind defaults to FR when "kind" is non-string. */
    mu_assert("kind defaults to FR", meta.kind == VMAF_MODEL_KIND_DNN_FR);
    /* opset stays 0 when extract_int rejects the value. */
    mu_assert("opset defaults to 0", meta.opset == 0);
    /* No name in JSON → out->name is NULL. */
    mu_assert("missing name stays NULL", meta.name == NULL);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

static char *test_sidecar_extract_string_no_close_quote(void)
{
    /* extract_string with a key that has an opening quote on the value but
     * no closing quote → strchr(p, '"') returns NULL, line 132 branch. */
    char tmpl[] = "/tmp/vmaf-dnn-noclose-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    /* "name" opens a quote that never closes before EOF. */
    (void)fputs("{\"name\": \"unterminated", s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("malformed sidecar still loads", err == 0);
    mu_assert("unterminated string returns NULL", meta.name == NULL);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* ADR-0976 regression: extract_string_array() must clean up its own
 * partial allocations on every error path (-EINVAL, -ERANGE, -ENOMEM)
 * and reset *out_n to 0. Previously the error branches left
 * partially-allocated entries in out[] but never wrote *out_n, so the
 * caller's `for (i = 0; i < *out_n; ++i) free(out[i])` cleanup looped
 * zero times and the strings leaked.
 *
 * The shipping symptom: an attacker dropping a malformed sidecar JSON
 * next to a tiny ONNX (e.g. "output_names": ["a", "b", garbage_no_close)
 * leaked ~7 string allocations per `vmaf_dnn_session_open()`. With this
 * fix the producer free()s its own partial work before returning -EINVAL,
 * and *out_n is always 0 on error so a defensive caller loop is a no-op
 * rather than a missed cleanup. */
static char *test_sidecar_string_array_malformed_no_leak(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-arr-malformed-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    /* output_names array has two valid entries then a non-quote / EOF —
     * extract_string_array() must return -EINVAL after allocating "a"
     * and "b", and the loader must observe n_output_names == 0 with
     * NULL slots so the subsequent vmaf_dnn_sidecar_free() does not
     * see stale pointers (would double-free or leak). */
    (void)fputs("{\n"
                "  \"kind\": \"fr\",\n"
                "  \"output_names\": [\"a\", \"b\", garbage\n"
                "}\n",
                s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("malformed array sidecar still loads (non-fatal)", err == 0);
    /* The malformed array must NOT leave a non-zero count nor live
     * pointers in the slots — otherwise the producer is leaking and the
     * caller's fallback cleanup is silently wrong. */
    mu_assert("n_output_names == 0 after malformed parse", meta.n_output_names == 0u);
    mu_assert("output_names[0] is NULL", meta.output_names[0] == NULL);
    mu_assert("output_names[1] is NULL", meta.output_names[1] == NULL);
    /* Safe to free — would crash / report leaks under ASan if the
     * producer left stale pointers. */
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* ADR-0976 regression: same invariant for feature_names (drives the
 * second extract_string_array() call site in vmaf_dnn_sidecar_load).
 * A malformed feature_order array must not leave partial allocations
 * dangling in meta.feature_names[]. */
static char *test_sidecar_feature_names_malformed_no_leak(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-fn-malformed-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    /* Two valid entries then an unterminated string — exercises the
     * "no close quote" branch of extract_string_array(). */
    (void)fputs("{\n"
                "  \"kind\": \"fr\",\n"
                "  \"feature_order\": [\"adm2\", \"vif_scale0\", \"unterminated\n"
                "}\n",
                s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("malformed feature_order sidecar still loads", err == 0);
    mu_assert("n_features == 0 after malformed parse", meta.n_features == 0u);
    mu_assert("feature_names[0] is NULL", meta.feature_names[0] == NULL);
    mu_assert("feature_names[1] is NULL", meta.feature_names[1] == NULL);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* ADR-0517 regression: sidecar carrying a feature-vector schema
 * (feature_order / feature_mean / feature_std as written by
 * train_fr_regressor_v2.py) populates VmafModelSidecar.n_features,
 * feature_names[], feature_mean[], feature_std[], and
 * has_feature_scaler. */
static char *test_sidecar_feature_vector_canonical6(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-fv-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\n"
                     "  \"kind\": \"fr\",\n"
                     "  \"feature_order\": [\"adm2\", \"vif_scale0\", "
                     "\"vif_scale1\", \"vif_scale2\", \"vif_scale3\", \"motion2\"],\n"
                     "  \"feature_mean\": [0.86, 0.37, 0.73, 0.82, 0.87, 8.95],\n"
                     "  \"feature_std\":  [0.10, 0.16, 0.19, 0.17, 0.14, 6.24]\n"
                     "}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load canonical-6 failed", err == 0);
    mu_assert("n_features == 6", meta.n_features == 6u);
    mu_assert("feature_names[0] == adm2",
              meta.feature_names[0] && strcmp(meta.feature_names[0], "adm2") == 0);
    mu_assert("feature_names[5] == motion2",
              meta.feature_names[5] && strcmp(meta.feature_names[5], "motion2") == 0);
    mu_assert("has_feature_scaler", meta.has_feature_scaler);
    mu_assert("feature_mean[0] ~ 0.86",
              meta.feature_mean[0] > 0.85f && meta.feature_mean[0] < 0.87f);
    mu_assert("feature_std[5] ~ 6.24", meta.feature_std[5] > 6.2f && meta.feature_std[5] < 6.3f);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* ADR-0517: the alternative `features` / `input_mean` / `input_std`
 * field names (used by vmaf_tiny_v* trainers) populate the same
 * VmafModelSidecar fields. */
static char *test_sidecar_feature_vector_vmaf_tiny_field_names(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-fv2-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\n"
                     "  \"kind\": \"fr\",\n"
                     "  \"features\": [\"adm2\", \"vif_scale0\"],\n"
                     "  \"input_mean\": [0.5, 0.5],\n"
                     "  \"input_std\":  [0.1, 0.1]\n"
                     "}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load tiny-v4-style failed", err == 0);
    mu_assert("n_features == 2", meta.n_features == 2u);
    mu_assert("alt-field has_feature_scaler", meta.has_feature_scaler);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* ADR-0517: sidecar with feature_order but no scaler arrays — the
 * loader honours the names but flags has_feature_scaler false so the
 * runtime knows to skip the (mean, std) transform. */
static char *test_sidecar_feature_vector_no_scaler(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-fv3-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\n"
                     "  \"kind\": \"fr\",\n"
                     "  \"feature_order\": [\"adm2\", \"vif_scale0\"]\n"
                     "}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load no-scaler failed", err == 0);
    mu_assert("n_features == 2", meta.n_features == 2u);
    mu_assert("no scaler arrays → has_feature_scaler false", meta.has_feature_scaler == false);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* vmaf_tiny_v2/v3/v4 pattern: sidecar with features + input_mean/std
 * AND "onnx_has_scaler": true.  The loader must set has_feature_scaler
 * (for tooling) AND onnx_has_scaler (so the C runtime skips the
 * redundant second application of the scaler). */
static char *test_sidecar_onnx_has_scaler_flag(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-ons-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\n"
                     "  \"kind\": \"fr\",\n"
                     "  \"features\": [\"adm2\", \"vif_scale0\"],\n"
                     "  \"input_mean\": [0.9, 0.5],\n"
                     "  \"input_std\":  [0.03, 0.15],\n"
                     "  \"onnx_has_scaler\": true\n"
                     "}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load onnx_has_scaler failed", err == 0);
    mu_assert("n_features == 2", meta.n_features == 2u);
    mu_assert("has_feature_scaler still set for tooling", meta.has_feature_scaler);
    mu_assert("onnx_has_scaler flag set", meta.onnx_has_scaler);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* Absence of onnx_has_scaler must leave the flag false (backwards
 * compatibility: fr_regressor_v1 etc. rely on the C-side scaler). */
static char *test_sidecar_onnx_has_scaler_absent(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-ons2-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\n"
                     "  \"kind\": \"fr\",\n"
                     "  \"feature_order\": [\"adm2\", \"vif_scale0\"],\n"
                     "  \"feature_mean\": [0.8, 0.4],\n"
                     "  \"feature_std\":  [0.1, 0.1]\n"
                     "}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load no-onnx-scaler failed", err == 0);
    mu_assert("has_feature_scaler true", meta.has_feature_scaler);
    mu_assert("onnx_has_scaler false when absent", meta.onnx_has_scaler == false);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* ADR-0519: sidecar carrying an encoder_vocab array populates
 * VmafModelSidecar.encoder_vocab / n_encoder_vocab and flips
 * codec_aware. Models without it stay codec_aware == false. */
static char *test_sidecar_encoder_vocab_v2(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-vocab-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\n"
                     "  \"kind\": \"fr\",\n"
                     "  \"encoder_vocab\": [\"libx264\", \"libx265\", \"libsvtav1\", "
                     "\"libvvenc\", \"libvpx-vp9\", \"h264_nvenc\", \"hevc_nvenc\", "
                     "\"av1_nvenc\", \"h264_qsv\", \"hevc_qsv\", \"av1_qsv\", \"unknown\"],\n"
                     "  \"encoder_vocab_version\": 2\n"
                     "}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load codec-aware failed", err == 0);
    mu_assert("codec_aware true", meta.codec_aware);
    mu_assert("n_encoder_vocab == 12", meta.n_encoder_vocab == 12u);
    mu_assert("encoder_vocab[0] == libx264",
              meta.encoder_vocab[0] && strcmp(meta.encoder_vocab[0], "libx264") == 0);
    mu_assert("encoder_vocab[11] == unknown",
              meta.encoder_vocab[11] && strcmp(meta.encoder_vocab[11], "unknown") == 0);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* ADR-0519: sidecar without encoder_vocab keeps codec_aware false
 * (existing non-v2 models — fr_regressor_v1, vmaf_tiny_v4, dists_sq). */
static char *test_sidecar_no_encoder_vocab(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-novocab-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\n"
                     "  \"kind\": \"fr\",\n"
                     "  \"feature_order\": [\"adm2\"]\n"
                     "}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load no-vocab failed", err == 0);
    mu_assert("codec_aware false", meta.codec_aware == false);
    mu_assert("n_encoder_vocab == 0", meta.n_encoder_vocab == 0u);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* quant_mode "static" — drives the model_loader.c:434-435 branch in
 * vmaf_dnn_sidecar_load that maps the literal string to
 * VMAF_QUANT_STATIC. Sibling to test_sidecar_quant_mode_dynamic. */
static char *test_sidecar_quant_mode_static(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-quant-static-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\"kind\": \"fr\", \"quant_mode\": \"static\"}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load static failed", err == 0);
    mu_assert("quant_mode static", meta.quant_mode == VMAF_QUANT_STATIC);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* quant_mode "qat" — drives the model_loader.c:436-437 branch (the
 * third literal-string case) inside vmaf_dnn_sidecar_load. */
static char *test_sidecar_quant_mode_qat(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-quant-qat-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\"kind\": \"fr\", \"quant_mode\": \"qat\"}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load qat failed", err == 0);
    mu_assert("quant_mode qat", meta.quant_mode == VMAF_QUANT_QAT);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* sidecar kind == "filter" — drives the model_loader.c:405-406 branch
 * inside vmaf_dnn_sidecar_load that maps the literal string to
 * VMAF_MODEL_KIND_DNN_FILTER. Sibling to test_sidecar_parses_kind_nr. */
static char *test_sidecar_kind_filter(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-kind-filter-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);
    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);
    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fprintf(s, "{\"kind\": \"filter\"}\n");
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("sidecar_load kind=filter failed", err == 0);
    mu_assert("kind == DNN_FILTER", meta.kind == VMAF_MODEL_KIND_DNN_FILTER);
    vmaf_dnn_sidecar_free(&meta);
    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* ADR-0976 regression sibling for encoder_vocab. Companion to the
 * output_names + feature_order leak-no-leak tests above: drives the
 * third call site of extract_string_array() in vmaf_dnn_sidecar_load
 * so the encoder_vocab wipe loop (model_loader.c:511-515) executes.
 * Without this, a malformed encoder_vocab array would leak whatever
 * entries were parsed before the syntax error fires. */
static char *test_sidecar_encoder_vocab_malformed_no_leak(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-vocab-malformed-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    /* Two valid encoder entries then an unterminated third entry —
     * extract_string_array returns -EINVAL after allocating
     * "libx264" + "libx265"; the loader's cleanup loop must observe
     * n_encoder_vocab == 0 and NULL slots. */
    (void)fputs("{\n"
                "  \"kind\": \"fr\",\n"
                "  \"encoder_vocab\": [\"libx264\", \"libx265\", unterminated\n"
                "}\n",
                s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("malformed encoder_vocab sidecar still loads", err == 0);
    mu_assert("n_encoder_vocab == 0 after malformed parse", meta.n_encoder_vocab == 0u);
    mu_assert("codec_aware stays false on malformed array", meta.codec_aware == false);
    mu_assert("encoder_vocab[0] is NULL", meta.encoder_vocab[0] == NULL);
    mu_assert("encoder_vocab[1] is NULL", meta.encoder_vocab[1] == NULL);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* extract_string_array empty-array branch (model_loader.c:210-212):
 * a literal `[]` is a valid empty list that returns 0 with *out_n=0.
 * The loader treats absent vs empty distinctly — empty must NOT set
 * codec_aware. Mirrors the equivalent feature_order absent-vs-empty
 * contract for ADR-0518 producers. */
static char *test_sidecar_empty_arrays_are_valid(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-empty-arr-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    /* All three string arrays explicitly empty. Each parser returns 0
     * with *out_n == 0; the loader treats *out_n == 0 the same as
     * absent and does not promote codec_aware / has_feature_scaler. */
    (void)fputs("{\n"
                "  \"kind\": \"fr\",\n"
                "  \"output_names\": [],\n"
                "  \"feature_order\": [],\n"
                "  \"encoder_vocab\": []\n"
                "}\n",
                s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("empty-array sidecar loads ok", err == 0);
    mu_assert("empty output_names → n_output_names 0", meta.n_output_names == 0u);
    mu_assert("empty feature_order → n_features 0", meta.n_features == 0u);
    mu_assert("empty encoder_vocab → not codec-aware", meta.codec_aware == false);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* extract_string_array ERANGE branch (model_loader.c:225-227): when
 * the JSON array carries more than the destination's @p max entries.
 * encoder_vocab caps at VMAF_DNN_MAX_ENCODER_VOCAB (32); pad the
 * array past that bound. */
static char *test_sidecar_encoder_vocab_over_max_returns_erange(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-vocab-over-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fputs("{\n  \"kind\": \"fr\",\n  \"encoder_vocab\": [", s);
    /* Emit 33 distinct entries — one over the 32-entry cap. */
    for (int i = 0; i < 33; ++i) {
        if (i > 0)
            (void)fputs(",", s);
        (void)fprintf(s, "\"enc_%d\"", i);
    }
    (void)fputs("]\n}\n", s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    /* The outer load remains successful — encoder_vocab over-cap is
     * a wipe-and-continue condition, not a hard fail. */
    mu_assert("over-cap encoder_vocab sidecar still loads", err == 0);
    mu_assert("over-cap → codec_aware false", meta.codec_aware == false);
    mu_assert("over-cap → n_encoder_vocab 0", meta.n_encoder_vocab == 0u);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* extract_string_array trailing-junk branch (model_loader.c:248-249):
 * a JSON array that ends with a non-`,`/non-`]` token after a
 * successful entry must surface -EINVAL through the partial-cleanup
 * path. The shipped consumer (vmaf_dnn_sidecar_load) absorbs the
 * error and returns 0 with the array wiped; this test asserts the
 * absorbed-state contract. */
static char *test_sidecar_array_trailing_junk_wipes(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-arr-junk-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    /* `"a" Z` after the first entry is neither comma nor `]` — exercises
     * the trailing-junk -EINVAL return inside extract_string_array. */
    (void)fputs("{\n"
                "  \"kind\": \"fr\",\n"
                "  \"output_names\": [\"a\" Z]\n"
                "}\n",
                s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("trailing-junk sidecar still loads", err == 0);
    mu_assert("trailing-junk → n_output_names 0", meta.n_output_names == 0u);
    mu_assert("trailing-junk → slot 0 NULL", meta.output_names[0] == NULL);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* extract_string_array non-string element branch
 * (model_loader.c:214-217): an array entry that doesn't open with `"`
 * is rejected. The shipping symptom we care about: a sidecar with
 * `"output_names": [42]` (integer, not string) must not be parsed
 * as a valid name list. */
static char *test_sidecar_array_non_string_element_wipes(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-arr-int-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fputs("{\n"
                "  \"kind\": \"fr\",\n"
                "  \"output_names\": [42]\n"
                "}\n",
                s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("int-element sidecar still loads (loader absorbs)", err == 0);
    mu_assert("int-element → n_output_names 0", meta.n_output_names == 0u);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* extract_int -ERANGE branch (model_loader.c:332-333): a numeric
 * onnx_opset value that overflows int triggers the strtol ERANGE /
 * INT_MAX guard. */
static char *test_sidecar_opset_overflow_returns_default(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-opset-over-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fputs("{\"kind\": \"fr\", \"onnx_opset\": 99999999999999999999999}\n", s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("overflow opset sidecar still loads", err == 0);
    /* extract_int's -ERANGE branch is ignored downstream → opset
     * stays at the zeroed default. */
    mu_assert("overflow opset → default 0", meta.opset == 0);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}

/* extract_float_array trailing-junk -EINVAL branch
 * (model_loader.c:306-307): when a feature_mean entry parses cleanly
 * then is followed by neither `,` nor `]`. The shipped loader treats
 * feature_mean / feature_std as paired with feature_order; a bad
 * scaler vector falls back to has_feature_scaler=false. */
static char *test_sidecar_feature_mean_trailing_junk(void)
{
    char tmpl[] = "/tmp/vmaf-dnn-mean-junk-XXXXXX";
    int fd = mkstemp(tmpl);
    mu_assert("mkstemp failed", fd >= 0);
    close(fd);

    char onnx[1024];
    char sidecar[1024];
    (void)snprintf(onnx, sizeof onnx, "%s.onnx", tmpl);
    (void)snprintf(sidecar, sizeof sidecar, "%s.json", tmpl);
    FILE *f = fopen_w_600(onnx);
    if (f)
        (void)fclose(f);

    FILE *s = fopen_w_600(sidecar);
    mu_assert("fopen sidecar failed", s != NULL);
    (void)fputs("{\n"
                "  \"kind\": \"fr\",\n"
                "  \"feature_order\": [\"adm2\"],\n"
                "  \"feature_mean\": [1.5 Z],\n"
                "  \"feature_std\": [0.5]\n"
                "}\n",
                s);
    (void)fclose(s);

    VmafModelSidecar meta;
    int err = vmaf_dnn_sidecar_load(onnx, &meta);
    mu_assert("malformed feature_mean sidecar still loads", err == 0);
    /* Bad scaler → has_feature_scaler stays false even though
     * feature_order parsed successfully. */
    mu_assert("malformed feature_mean → has_feature_scaler false",
              meta.has_feature_scaler == false);
    vmaf_dnn_sidecar_free(&meta);

    (void)remove(sidecar);
    (void)remove(onnx);
    (void)remove(tmpl);
    return NULL;
}
#endif /* !_WIN32 */

/* ADR-0519: vmaf_dnn_codec_block_fill — known codec produces the right
 * one-hot + preset_norm + crf_norm. */
static char *test_codec_block_fill_libx264_medium_28(void)
{
    static const char *VOCAB[] = {"libx264",    "libx265",    "libsvtav1",  "libvvenc",
                                  "libvpx-vp9", "h264_nvenc", "hevc_nvenc", "av1_nvenc",
                                  "h264_qsv",   "hevc_qsv",   "av1_qsv",    "unknown"};
    const size_t n_vocab = 12u;
    float buf[14] = {0};
    int rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, n_vocab, "libx264", "medium", 28);
    mu_assert("rc == 0 for known codec", rc == 0);
    /* one-hot: index 0 (libx264) set, all others zero */
    mu_assert("buf[0] == 1.0 (libx264)", buf[0] > 0.999f && buf[0] < 1.001f);
    for (size_t i = 1; i < n_vocab; ++i) {
        mu_assert("non-selected encoder slots are zero", buf[i] == 0.0f);
    }
    /* preset_norm: 5/9 = 0.5555... */
    mu_assert("preset_norm ~ 5/9", buf[n_vocab] > 0.555f && buf[n_vocab] < 0.556f);
    /* crf_norm: 28/63 = 0.444... */
    mu_assert("crf_norm ~ 28/63", buf[n_vocab + 1u] > 0.444f && buf[n_vocab + 1u] < 0.445f);
    return NULL;
}

/* ADR-0519: vmaf_dnn_codec_block_fill — unknown codec name returns
 * -ENOENT but still writes the "unknown" bucket so the model can
 * still produce a finite score if the caller decides to ignore. */
static char *test_codec_block_fill_unknown_returns_enoent(void)
{
    static const char *VOCAB[] = {"libx264", "libx265", "unknown"};
    float buf[5] = {0};
    int rc = vmaf_dnn_codec_block_fill(buf, 5u, VOCAB, 3u, "MY_UNKNOWN_ENC", "medium", 28);
    mu_assert("rc == -ENOENT for unknown codec", rc == -ENOENT);
    mu_assert("buf[2] == 1.0 (unknown bucket)", buf[2] > 0.999f && buf[2] < 1.001f);
    mu_assert("buf[0] == 0", buf[0] == 0.0f);
    mu_assert("buf[1] == 0", buf[1] == 0.0f);
    return NULL;
}

/* ADR-0519: vmaf_dnn_codec_block_fill — NULL codec name selects
 * "unknown" but returns 0 (legitimate "I don't know the codec" tag,
 * not a typo). */
static char *test_codec_block_fill_null_codec_is_ok(void)
{
    static const char *VOCAB[] = {"libx264", "unknown"};
    float buf[4] = {0};
    int rc = vmaf_dnn_codec_block_fill(buf, 4u, VOCAB, 2u, NULL, NULL, 0);
    mu_assert("rc == 0 for NULL codec", rc == 0);
    mu_assert("unknown bucket set", buf[1] > 0.999f && buf[1] < 1.001f);
    return NULL;
}

/* ADR-0519: vmaf_dnn_codec_block_fill — ffprobe alias "h264" is
 * remapped to libx264 (not bucketed to unknown). */
static char *test_codec_block_fill_h264_alias(void)
{
    static const char *VOCAB[] = {"libx264", "libx265", "unknown"};
    float buf[5] = {0};
    int rc = vmaf_dnn_codec_block_fill(buf, 5u, VOCAB, 3u, "h264", "medium", 0);
    mu_assert("rc == 0 for h264 alias", rc == 0);
    mu_assert("buf[0] == 1.0 (libx264 via alias)", buf[0] > 0.999f && buf[0] < 1.001f);
    return NULL;
}

/* resolve_codec_alias coverage: hevc/h265 → libx265 (model_loader.c:660-661),
 * av1 → libsvtav1 (662-663), vp9 → libvpx-vp9 (664-665), vvc/h266 → libvvenc
 * (666-667). Each alias takes a separate branch in the chained strcmp ladder
 * and was uncovered when only the libx264 alias test ran. */
static char *test_codec_block_fill_aliases_hevc_av1_vp9_vvc(void)
{
    static const char *VOCAB[] = {"libx264",  "libx265",    "libsvtav1",
                                  "libvvenc", "libvpx-vp9", "unknown"};
    const size_t n_vocab = 6u;
    float buf[8] = {0};
    /* hevc → libx265 (slot 1). */
    int rc = vmaf_dnn_codec_block_fill(buf, n_vocab + 2u, VOCAB, n_vocab, "hevc", "medium", 28);
    mu_assert("hevc alias rc == 0", rc == 0);
    mu_assert("hevc → libx265 (buf[1])", buf[1] > 0.999f && buf[1] < 1.001f);

    /* h265 (synonym of hevc) → libx265. */
    memset(buf, 0, sizeof(buf));
    rc = vmaf_dnn_codec_block_fill(buf, n_vocab + 2u, VOCAB, n_vocab, "h265", "medium", 28);
    mu_assert("h265 alias rc == 0", rc == 0);
    mu_assert("h265 → libx265 (buf[1])", buf[1] > 0.999f && buf[1] < 1.001f);

    /* av1 → libsvtav1 (slot 2). */
    memset(buf, 0, sizeof(buf));
    rc = vmaf_dnn_codec_block_fill(buf, n_vocab + 2u, VOCAB, n_vocab, "av1", "medium", 28);
    mu_assert("av1 alias rc == 0", rc == 0);
    mu_assert("av1 → libsvtav1 (buf[2])", buf[2] > 0.999f && buf[2] < 1.001f);

    /* vp9 → libvpx-vp9 (slot 4). */
    memset(buf, 0, sizeof(buf));
    rc = vmaf_dnn_codec_block_fill(buf, n_vocab + 2u, VOCAB, n_vocab, "vp9", "medium", 28);
    mu_assert("vp9 alias rc == 0", rc == 0);
    mu_assert("vp9 → libvpx-vp9 (buf[4])", buf[4] > 0.999f && buf[4] < 1.001f);

    /* vvc → libvvenc (slot 3). */
    memset(buf, 0, sizeof(buf));
    rc = vmaf_dnn_codec_block_fill(buf, n_vocab + 2u, VOCAB, n_vocab, "vvc", "medium", 28);
    mu_assert("vvc alias rc == 0", rc == 0);
    mu_assert("vvc → libvvenc (buf[3])", buf[3] > 0.999f && buf[3] < 1.001f);

    /* h266 (synonym of vvc) → libvvenc. */
    memset(buf, 0, sizeof(buf));
    rc = vmaf_dnn_codec_block_fill(buf, n_vocab + 2u, VOCAB, n_vocab, "h266", "medium", 28);
    mu_assert("h266 alias rc == 0", rc == 0);
    mu_assert("h266 → libvvenc (buf[3])", buf[3] > 0.999f && buf[3] < 1.001f);

    /* avc (synonym of h264) → libx264 — covers the second arm of the
     * h264-side branch. */
    memset(buf, 0, sizeof(buf));
    rc = vmaf_dnn_codec_block_fill(buf, n_vocab + 2u, VOCAB, n_vocab, "avc", "medium", 28);
    mu_assert("avc alias rc == 0", rc == 0);
    mu_assert("avc → libx264 (buf[0])", buf[0] > 0.999f && buf[0] < 1.001f);
    return NULL;
}

/* `slower` preset selector — model_loader.c:635-636 is the only preset
 * ordinal that lacks a dedicated test in the existing
 * test_codec_block_fill_preset_tables grid (the table covers
 * ultrafast..veryslow but skips slower's distinct slot). */
static char *test_codec_block_fill_preset_slower(void)
{
    static const char *VOCAB[] = {"libx264", "unknown"};
    float buf[4] = {0};
    int rc = vmaf_dnn_codec_block_fill(buf, 4u, VOCAB, 2u, "libx264", "slower", 28);
    mu_assert("slower preset rc == 0", rc == 0);
    /* preset_norm = 7 / 9 = 0.777... */
    mu_assert("slower preset_norm ~ 7/9", buf[2] > 0.777f && buf[2] < 0.778f);
    return NULL;
}

/* NULL vocab entry skip branch — model_loader.c:702-704. When the
 * vocab table carries a NULL entry the inner loop must continue past
 * it rather than dereferencing. The existing tests use fully-populated
 * vocabs so the NULL-skip branch is uncovered. */
static char *test_codec_block_fill_null_vocab_entry_is_skipped(void)
{
    /* Mid-table NULL slot — the loop must skip it and still match
     * "libx264" at slot 2. */
    static const char *VOCAB[] = {"libx265", NULL, "libx264", "unknown"};
    float buf[6] = {0};
    int rc = vmaf_dnn_codec_block_fill(buf, 6u, VOCAB, 4u, "libx264", "medium", 28);
    mu_assert("rc == 0 when matching past a NULL slot", rc == 0);
    mu_assert("buf[2] == 1.0", buf[2] > 0.999f && buf[2] < 1.001f);
    mu_assert("buf[1] (NULL slot) untouched", buf[1] == 0.0f);
    return NULL;
}

/* ADR-0519: vmaf_dnn_codec_block_fill — CRF clamped to [0, 63]. */
static char *test_codec_block_fill_crf_clamp(void)
{
    static const char *VOCAB[] = {"libx264", "unknown"};
    float buf[4] = {0};
    /* CRF 100 → clamped to 63 → crf_norm == 1.0 */
    int rc = vmaf_dnn_codec_block_fill(buf, 4u, VOCAB, 2u, "libx264", "medium", 100);
    mu_assert("rc == 0", rc == 0);
    mu_assert("crf_norm clamped to 1.0", buf[3] > 0.999f && buf[3] < 1.001f);
    /* CRF -5 → clamped to 0 → crf_norm == 0.0 */
    rc = vmaf_dnn_codec_block_fill(buf, 4u, VOCAB, 2u, "libx264", "medium", -5);
    mu_assert("rc == 0 (negative crf)", rc == 0);
    mu_assert("crf_norm clamped to 0.0", buf[3] == 0.0f);
    return NULL;
}

static char *test_codec_block_fill_preset_tables(void)
{
    static const char *VOCAB[] = {"libx264",    "libx265",    "libsvtav1",  "libvvenc",
                                  "libvpx-vp9", "h264_nvenc", "hevc_nvenc", "av1_nvenc",
                                  "h264_qsv",   "hevc_qsv",   "av1_qsv",    "unknown"};
    float buf[14] = {0};
    int rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libx265", "placebo", 0);
    mu_assert("x265 placebo ok", rc == 0);
    mu_assert("x265 placebo preset = 1", buf[12] > 0.999f && buf[12] < 1.001f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libsvtav1", "13", 0);
    mu_assert("svtav1 numeric preset ok", rc == 0);
    mu_assert("svtav1 preset clamped to 1", buf[12] > 0.999f && buf[12] < 1.001f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libvvenc", "slower", 0);
    mu_assert("vvenc slower ok", rc == 0);
    mu_assert("vvenc slower preset", buf[12] > 0.888f && buf[12] < 0.890f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libvpx-vp9", "best", 0);
    mu_assert("vp9 best ok", rc == 0);
    mu_assert("vp9 best preset = 1", buf[12] > 0.999f && buf[12] < 1.001f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_nvenc", "p7", 0);
    mu_assert("nvenc p7 ok", rc == 0);
    mu_assert("nvenc p7 preset = 1", buf[12] > 0.999f && buf[12] < 1.001f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "hevc_qsv", "veryslow", 0);
    mu_assert("qsv veryslow ok", rc == 0);
    mu_assert("qsv veryslow preset", buf[12] > 0.888f && buf[12] < 0.890f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libx264", "ultrafast", 0);
    mu_assert("x264 ultrafast ok", rc == 0);
    mu_assert("x264 ultrafast preset", buf[12] < 0.001f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libx264", "superfast", 0);
    mu_assert("x264 superfast ok", rc == 0);
    mu_assert("x264 superfast preset", buf[12] > 0.110f && buf[12] < 0.112f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libx264", "veryfast", 0);
    mu_assert("x264 veryfast ok", rc == 0);
    mu_assert("x264 veryfast preset", buf[12] > 0.221f && buf[12] < 0.223f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libx264", "faster", 0);
    mu_assert("x264 faster ok", rc == 0);
    mu_assert("x264 faster preset", buf[12] > 0.332f && buf[12] < 0.334f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libx264", "fast", 0);
    mu_assert("x264 fast ok", rc == 0);
    mu_assert("x264 fast preset", buf[12] > 0.443f && buf[12] < 0.445f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libx264", "slow", 0);
    mu_assert("x264 slow ok", rc == 0);
    mu_assert("x264 slow preset", buf[12] > 0.666f && buf[12] < 0.668f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libx264", "slower", 0);
    mu_assert("x264 slower ok", rc == 0);
    mu_assert("x264 slower preset", buf[12] > 0.777f && buf[12] < 0.779f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libx264", "veryslow", 0);
    mu_assert("x264 veryslow ok", rc == 0);
    mu_assert("x264 veryslow preset", buf[12] > 0.888f && buf[12] < 0.890f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libvvenc", "faster", 0);
    mu_assert("vvenc faster ok", rc == 0);
    mu_assert("vvenc faster preset", buf[12] > 0.110f && buf[12] < 0.112f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libvvenc", "fast", 0);
    mu_assert("vvenc fast ok", rc == 0);
    mu_assert("vvenc fast preset", buf[12] > 0.332f && buf[12] < 0.334f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libvvenc", "medium", 0);
    mu_assert("vvenc medium ok", rc == 0);
    mu_assert("vvenc medium preset", buf[12] > 0.555f && buf[12] < 0.556f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libvvenc", "slow", 0);
    mu_assert("vvenc slow ok", rc == 0);
    mu_assert("vvenc slow preset", buf[12] > 0.777f && buf[12] < 0.779f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libvpx-vp9", "realtime", 0);
    mu_assert("vp9 realtime ok", rc == 0);
    mu_assert("vp9 realtime preset", buf[12] < 0.001f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "libvpx-vp9", "good", 0);
    mu_assert("vp9 good ok", rc == 0);
    mu_assert("vp9 good preset", buf[12] > 0.555f && buf[12] < 0.556f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_nvenc", "p1", 0);
    mu_assert("nvenc p1 ok", rc == 0);
    mu_assert("nvenc p1 preset", buf[12] < 0.001f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_nvenc", "p2", 0);
    mu_assert("nvenc p2 ok", rc == 0);
    mu_assert("nvenc p2 preset", buf[12] > 0.221f && buf[12] < 0.223f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_nvenc", "p3", 0);
    mu_assert("nvenc p3 ok", rc == 0);
    mu_assert("nvenc p3 preset", buf[12] > 0.332f && buf[12] < 0.334f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_nvenc", "p4", 0);
    mu_assert("nvenc p4 ok", rc == 0);
    mu_assert("nvenc p4 preset", buf[12] > 0.555f && buf[12] < 0.556f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_nvenc", "p5", 0);
    mu_assert("nvenc p5 ok", rc == 0);
    mu_assert("nvenc p5 preset", buf[12] > 0.666f && buf[12] < 0.668f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_nvenc", "p6", 0);
    mu_assert("nvenc p6 ok", rc == 0);
    mu_assert("nvenc p6 preset", buf[12] > 0.777f && buf[12] < 0.779f);

    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_qsv", "veryfast", 0);
    mu_assert("qsv veryfast ok", rc == 0);
    mu_assert("qsv veryfast preset", buf[12] > 0.221f && buf[12] < 0.223f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_qsv", "faster", 0);
    mu_assert("qsv faster ok", rc == 0);
    mu_assert("qsv faster preset", buf[12] > 0.332f && buf[12] < 0.334f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_qsv", "fast", 0);
    mu_assert("qsv fast ok", rc == 0);
    mu_assert("qsv fast preset", buf[12] > 0.443f && buf[12] < 0.445f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_qsv", "medium", 0);
    mu_assert("qsv medium ok", rc == 0);
    mu_assert("qsv medium preset", buf[12] > 0.555f && buf[12] < 0.556f);
    rc = vmaf_dnn_codec_block_fill(buf, 14u, VOCAB, 12u, "h264_qsv", "slow", 0);
    mu_assert("qsv slow ok", rc == 0);
    mu_assert("qsv slow preset", buf[12] > 0.666f && buf[12] < 0.668f);
    return NULL;
}

static char *test_codec_block_fill_unknown_presets_default(void)
{
    static const char *VOCAB[] = {"libx264",   "libsvtav1", "libvvenc", "libvpx-vp9",
                                  "av1_nvenc", "av1_qsv",   "unknown"};
    float buf[9] = {0};
    int rc = vmaf_dnn_codec_block_fill(buf, 9u, VOCAB, 7u, "libx264", "not-a-preset", 0);
    mu_assert("x264 unknown preset ok", rc == 0);
    mu_assert("x264 unknown preset defaults medium", buf[7] > 0.555f && buf[7] < 0.556f);

    rc = vmaf_dnn_codec_block_fill(buf, 9u, VOCAB, 7u, "libsvtav1", "notnumeric", 0);
    mu_assert("svtav1 unknown preset ok", rc == 0);
    mu_assert("svtav1 unknown preset defaults medium", buf[7] > 0.555f && buf[7] < 0.556f);

    rc = vmaf_dnn_codec_block_fill(buf, 9u, VOCAB, 7u, "libvvenc", "not-a-preset", 0);
    mu_assert("vvenc unknown preset ok", rc == 0);
    mu_assert("vvenc unknown preset defaults medium", buf[7] > 0.555f && buf[7] < 0.556f);

    rc = vmaf_dnn_codec_block_fill(buf, 9u, VOCAB, 7u, "libvpx-vp9", "not-a-deadline", 0);
    mu_assert("vp9 unknown preset ok", rc == 0);
    mu_assert("vp9 unknown preset defaults medium", buf[7] > 0.555f && buf[7] < 0.556f);

    rc = vmaf_dnn_codec_block_fill(buf, 9u, VOCAB, 7u, "av1_nvenc", "p9", 0);
    mu_assert("nvenc unknown preset ok", rc == 0);
    mu_assert("nvenc unknown preset defaults medium", buf[7] > 0.555f && buf[7] < 0.556f);

    rc = vmaf_dnn_codec_block_fill(buf, 9u, VOCAB, 7u, "av1_qsv", "not-a-preset", 0);
    mu_assert("qsv unknown preset ok", rc == 0);
    mu_assert("qsv unknown preset defaults medium", buf[7] > 0.555f && buf[7] < 0.556f);
    return NULL;
}

static char *test_codec_block_fill_rejects_bad_args(void)
{
    static const char *VOCAB[] = {"libx264", "unknown"};
    float buf[4] = {0};
    int rc = vmaf_dnn_codec_block_fill(NULL, 4u, VOCAB, 2u, "libx264", "medium", 28);
    mu_assert("NULL buf rejected", rc == -EINVAL);
    rc = vmaf_dnn_codec_block_fill(buf, 4u, NULL, 2u, "libx264", "medium", 28);
    mu_assert("NULL vocab rejected", rc == -EINVAL);
    rc = vmaf_dnn_codec_block_fill(buf, 4u, VOCAB, 0u, "libx264", "medium", 28);
    mu_assert("empty vocab rejected", rc == -EINVAL);
    return NULL;
}

/* ADR-0519: vmaf_dnn_codec_block_fill — wrong buf_len returns -EINVAL. */
static char *test_codec_block_fill_bad_len(void)
{
    static const char *VOCAB[] = {"libx264", "unknown"};
    float buf[5] = {0};
    /* expected len = 2 + 2 = 4; pass 5 */
    int rc = vmaf_dnn_codec_block_fill(buf, 5u, VOCAB, 2u, "libx264", "medium", 28);
    mu_assert("rc == -EINVAL for wrong buf_len", rc == -EINVAL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_sniff_by_extension);
    mu_run_test(test_size_cap);
    mu_run_test(test_validate_null_path);
#ifndef _WIN32
    mu_run_test(test_validate_zero_byte);
    mu_run_test(test_validate_allowed_onnx);
    mu_run_test(test_validate_disallowed_onnx);
    mu_run_test(test_validate_symlink_to_dir);
    mu_run_test(test_jail_unset_accepts_anywhere);
    mu_run_test(test_jail_accepts_model_inside);
    mu_run_test(test_jail_rejects_model_outside);
    mu_run_test(test_jail_rejects_sibling_prefix);
    mu_run_test(test_jail_rejects_symlink_escape);
    mu_run_test(test_jail_rejects_nonexistent_jail);
    mu_run_test(test_jail_rejects_non_directory);
    mu_run_test(test_jail_accepts_trailing_slash);
#endif
    mu_run_test(test_sidecar_parses);
    mu_run_test(test_sidecar_rejects_null_args);
    mu_run_test(test_sidecar_free_null_is_noop);
    mu_run_test(test_sidecar_missing_returns_enoent);
#ifndef _WIN32
    mu_run_test(test_sidecar_non_regular_returns_einval);
    mu_run_test(test_sidecar_parses_kind_nr);
    mu_run_test(test_sidecar_quant_mode_default_fp32);
    mu_run_test(test_sidecar_quant_mode_dynamic);
    mu_run_test(test_sidecar_quant_mode_unknown_falls_back);
    mu_run_test(test_sidecar_no_dot_onnx_extension);
    mu_run_test(test_sidecar_oversized_path);
    mu_run_test(test_sidecar_malformed_keys_default);
    mu_run_test(test_sidecar_extract_string_no_close_quote);
    mu_run_test(test_sidecar_string_array_malformed_no_leak);
    mu_run_test(test_sidecar_feature_names_malformed_no_leak);
    mu_run_test(test_sidecar_feature_vector_canonical6);
    mu_run_test(test_sidecar_feature_vector_vmaf_tiny_field_names);
    mu_run_test(test_sidecar_feature_vector_no_scaler);
    mu_run_test(test_sidecar_onnx_has_scaler_flag);
    mu_run_test(test_sidecar_onnx_has_scaler_absent);
    mu_run_test(test_sidecar_encoder_vocab_v2);
    mu_run_test(test_sidecar_no_encoder_vocab);
    mu_run_test(test_sidecar_encoder_vocab_malformed_no_leak);
    mu_run_test(test_sidecar_empty_arrays_are_valid);
    mu_run_test(test_sidecar_encoder_vocab_over_max_returns_erange);
    mu_run_test(test_sidecar_array_trailing_junk_wipes);
    mu_run_test(test_sidecar_array_non_string_element_wipes);
    mu_run_test(test_sidecar_opset_overflow_returns_default);
    mu_run_test(test_sidecar_feature_mean_trailing_junk);
    mu_run_test(test_sidecar_quant_mode_static);
    mu_run_test(test_sidecar_quant_mode_qat);
    mu_run_test(test_sidecar_kind_filter);
#endif
    mu_run_test(test_codec_block_fill_libx264_medium_28);
    mu_run_test(test_codec_block_fill_unknown_returns_enoent);
    mu_run_test(test_codec_block_fill_null_codec_is_ok);
    mu_run_test(test_codec_block_fill_h264_alias);
    mu_run_test(test_codec_block_fill_aliases_hevc_av1_vp9_vvc);
    mu_run_test(test_codec_block_fill_preset_slower);
    mu_run_test(test_codec_block_fill_null_vocab_entry_is_skipped);
    mu_run_test(test_codec_block_fill_crf_clamp);
    mu_run_test(test_codec_block_fill_preset_tables);
    mu_run_test(test_codec_block_fill_unknown_presets_default);
    mu_run_test(test_codec_block_fill_rejects_bad_args);
    mu_run_test(test_codec_block_fill_bad_len);
    return NULL;
}
