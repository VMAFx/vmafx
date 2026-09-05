/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <stdlib.h> /* _fullpath */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
/* MSVC's <sys/stat.h> ships the S_IFDIR / S_IFREG bit masks but not the
 * POSIX classification macros. Provide them so the single-source path
 * checks below compile on both sides. */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#endif

#include "libvmaf/model.h"

#include "model_loader.h"
#include "onnx_scan.h"

/* Portable realpath wrapper: POSIX realpath() on Linux/macOS, _fullpath()
 * on MinGW/Windows. Both resolve symlinks and canonicalise the path in
 * place, returning NULL on failure. */
static char *resolve_path(const char *path, char *resolved)
{
#ifdef _WIN32
    return _fullpath(resolved, path, PATH_MAX);
#else
    return realpath(path, resolved);
#endif
}

/* Optional chroot-style path jail: when VMAF_TINY_MODEL_DIR is set in the
 * environment, the caller-supplied (already resolved) model path must sit
 * under the jail directory after both are canonicalised. Returns 0 when
 * the env is unset/empty (no-op) or the path is inside the jail, and
 * -EACCES otherwise. Fails closed on a misconfigured jail (env points at
 * a non-directory or an unresolvable path) — defensive default.
 *
 * The @p jail_dir parameter (may be NULL) is expected to be pre-cached from
 * getenv("VMAF_TINY_MODEL_DIR") by the caller to avoid repeated unsafe
 * getenv() calls in multithreaded contexts. */
static int enforce_tiny_model_jail(const char *resolved_model, const char *jail_dir)
{
    if (!jail_dir || jail_dir[0] == '\0')
        return 0;

    char jail_resolved[PATH_MAX];
    if (resolve_path(jail_dir, jail_resolved) == nullptr)
        return -EACCES;

    struct stat jst;
    if (stat(jail_resolved, &jst) != 0)
        return -EACCES;
    if (!S_ISDIR(jst.st_mode))
        return -EACCES;

    const size_t jlen = strlen(jail_resolved);
    if (jlen == 0u || jlen >= PATH_MAX - 1u)
        return -EACCES;

    /* Require a trailing separator on the jail prefix so "/foo" does not
     * match a sibling "/foobar". Normalise by appending one when absent. */
    char jail_prefix[PATH_MAX];
    size_t plen = jlen;
    memcpy(jail_prefix, jail_resolved, jlen);
    if (jail_prefix[plen - 1u] != '/') {
        jail_prefix[plen++] = '/';
    }
    jail_prefix[plen] = '\0';

    const size_t mlen = strlen(resolved_model);
    if (mlen < plen)
        return -EACCES;
    if (strncmp(resolved_model, jail_prefix, plen) != 0)
        return -EACCES;
    return 0;
}

/* ONNX files are protobuf-serialised graph messages. We sniff by extension +
 * a loose leading-byte pattern — protobuf varints start with a field tag
 * byte, so the first byte is rarely '{' (JSON) or '\x80' (pickle). */

static bool has_suffix(const char *s, const char *suf)
{
    size_t ls = strlen(s);
    size_t lu = strlen(suf);
    if (ls < lu)
        return false;
    return strcmp(s + ls - lu, suf) == 0;
}

static bool json_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int vmaf_dnn_sniff_kind(const char *path)
{
    if (!path)
        return -1;
    if (has_suffix(path, ".json") || has_suffix(path, ".pkl")) {
        return VMAF_MODEL_KIND_SVM;
    }
    if (has_suffix(path, ".onnx")) {
        return VMAF_MODEL_KIND_DNN_FR; /* default; sidecar may upgrade to NR */
    }
    return -1;
}

/* Locate the first occurrence of "key" that is a JSON key (i.e. followed by
 * ':' with optional whitespace) rather than a JSON value.  Returns a pointer
 * to the character immediately after the ':' on success, or NULL when no
 * matching key is found.
 *
 * A plain strstr(doc, needle) false-positives when a JSON *value* string
 * equals the key name (e.g. {"kind": "name", "name": "actual"} — searching
 * for key "name" would land on the value "name" first).  The fix: after each
 * strstr hit, skip whitespace past the needle and check that the next
 * non-space character is ':'.  If not, advance past this occurrence and
 * retry. */
static const char *find_key_in_doc(const char *doc, const char *needle, size_t needle_len)
{
    const char *cursor = doc;
    while ((cursor = strstr(cursor, needle)) != nullptr) {
        const char *after = cursor + needle_len;
        /* Skip whitespace after the needle. */
        while (*after && json_is_space(*after))
            after++;
        if (*after == ':') {
            /* Confirmed: this is a key occurrence.  Return the position
             * just after the ':' so the caller can parse the value. */
            return after + 1;
        }
        /* This occurrence is inside a value string — skip past it. */
        cursor++;
    }
    return nullptr;
}

/* Ultra-small JSON-value extractor: supports "key": "value" and "key": number.
 * Sidecars are written by vmaf-train so we know the exact shape and can avoid
 * pulling a JSON dependency into libvmaf. */
static char *extract_string(const char *doc, const char *key)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return nullptr;
    const char *p = find_key_in_doc(doc, needle, (size_t)n);
    if (!p)
        return nullptr;
    while (*p && json_is_space(*p))
        p++;
    if (*p != '"')
        return nullptr;
    p++;
    const char *q = strchr(p, '"');
    if (!q)
        return nullptr;
    size_t len = (size_t)(q - p);
    char *out = (char *)malloc(len + 1);
    if (!out)
        return nullptr;
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

/* Extract a JSON array of string values:  "key": ["a", "b", "c"]
 *
 * Writes up to @p max strings into @p out[]; the caller owns each
 * allocation. The actual count is returned via @p *out_n. Returns 0
 * when the key is found and parsed (even if the array is empty),
 * -ENOENT when the key is absent, -ERANGE when the array exceeds
 * @p max, -ENOMEM on allocation failure. */
/* Helper for extract_string_array(): free [0..cnt) entries in @p out and
 * NULL each slot, so the caller can rely on `*out_n == 0` after an error
 * return meaning "no live allocations in @p out". */
static void free_partial_string_array(char **out, size_t cnt)
{
    for (size_t i = 0; i < cnt; ++i) {
        free(out[i]);
        out[i] = nullptr;
    }
}

/* Position @p *out_p just past the '[' of the JSON array bound to @p key.
 * Returns 0 on success, -ENOENT when the key is absent, -EINVAL when the key
 * name is too long or its value is not an array. Shared by
 * extract_string_array() and extract_float_array(). */
static int json_array_begin(const char *doc, const char *key, const char **out_p)
{
    char needle[64];
    const int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -EINVAL;
    const char *p = find_key_in_doc(doc, needle, (size_t)n);
    if (!p)
        return -ENOENT;
    while (*p && json_is_space(*p))
        p++;
    if (*p != '[')
        return -EINVAL;
    *out_p = p + 1;
    return 0;
}

static int extract_string_array(const char *doc, const char *key, char **out, size_t max,
                                size_t *out_n)
{
    /* Always initialise the out-count so callers can assume it is valid
     * after every return — including error paths. Previously the -EINVAL
     * / -ERANGE / -ENOMEM branches left `*out_n` untouched while leaving
     * partially-allocated entries in `out[]`; the call sites in
     * `vmaf_dnn_sidecar_load` then iterated `0..*out_n` (== 0) for
     * cleanup and silently leaked every string allocated before the
     * error fired. */
    *out_n = 0u;

    const char *p = nullptr;
    const int rc = json_array_begin(doc, key, &p);
    if (rc != 0)
        return rc;

    size_t cnt = 0u;
    while (*p) {
        while (*p && json_is_space(*p))
            p++;
        if (*p == ']') {
            *out_n = cnt;
            return 0;
        }
        if (*p != '"') {
            free_partial_string_array(out, cnt);
            return -EINVAL;
        }
        ++p;
        const char *q = strchr(p, '"');
        if (!q) {
            free_partial_string_array(out, cnt);
            return -EINVAL;
        }
        const size_t len = (size_t)(q - p);
        if (cnt >= max) {
            free_partial_string_array(out, cnt);
            return -ERANGE;
        }
        char *s = (char *)malloc(len + 1u);
        if (!s) {
            free_partial_string_array(out, cnt);
            return -ENOMEM;
        }
        memcpy(s, p, len);
        s[len] = '\0';
        out[cnt++] = s;
        p = q + 1;
        while (*p && json_is_space(*p))
            p++;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == ']') {
            *out_n = cnt;
            return 0;
        }
        free_partial_string_array(out, cnt);
        return -EINVAL;
    }
    free_partial_string_array(out, cnt);
    return -EINVAL;
}

/* Extract a JSON array of numeric values:  "key": [1.0, 2, 3.5e-1]
 *
 * Writes up to @p max floats into @p out[]; @p *out_n receives the
 * actual count. Returns 0 / -ENOENT / -ERANGE on the same axes as
 * extract_string_array. */
static int extract_float_array(const char *doc, const char *key, float *out, size_t max,
                               size_t *out_n)
{
    const char *p = nullptr;
    const int rc = json_array_begin(doc, key, &p);
    if (rc != 0)
        return rc;

    size_t cnt = 0u;
    while (*p) {
        while (*p && json_is_space(*p))
            p++;
        if (*p == ']') {
            *out_n = cnt;
            return 0;
        }
        char *endp = nullptr;
        errno = 0;
        const double v = strtod(p, &endp);
        if (endp == p)
            return -EINVAL;
        if (errno == ERANGE || !isfinite(v))
            return -ERANGE;
        if (cnt >= max)
            return -ERANGE;
        out[cnt++] = (float)v;
        p = endp;
        while (*p && json_is_space(*p))
            p++;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p == ']') {
            *out_n = cnt;
            return 0;
        }
        return -EINVAL;
    }
    return -EINVAL;
}

static int extract_int(const char *doc, const char *key, int *out)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -EINVAL;
    const char *p = find_key_in_doc(doc, needle, (size_t)n);
    if (!p)
        return -ENOENT;
    while (*p && json_is_space(*p))
        p++;
    errno = 0;
    char *endp = nullptr;
    long v = strtol(p, &endp, 10);
    if (endp == p)
        return -EINVAL;
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX)
        return -ERANGE;
    *out = (int)v;
    return 0;
}

/* Derive the sidecar JSON path from @p onnx_path: ".onnx" is replaced by
 * ".json", any other suffix gets ".json" appended. Returns 0, or
 * -ENAMETOOLONG when the result does not fit in @p out_sz. */
static int sidecar_json_path(const char *onnx_path, char *out, size_t out_sz)
{
    const size_t len = strlen(onnx_path);
    if (len + 6 > out_sz)
        return -ENAMETOOLONG;
    memcpy(out, onnx_path, len);
    /* replace ".onnx" with ".json" */
    if (len >= 5 && strcmp(onnx_path + len - 5, ".onnx") == 0) {
        memcpy(out + len - 5, ".json", 5);
        out[len] = '\0';
    } else {
        memcpy(out + len, ".json", 6);
    }
    return 0;
}

/* Read the whole sidecar JSON into a NUL-terminated heap buffer owned by the
 * caller. Returns 0 on success, or a negative errno. */
static int slurp_sidecar_json(const char *sidecar, char **out_buf)
{
    struct stat st;
    if (stat(sidecar, &st) == 0) {
        if (!S_ISREG(st.st_mode))
            return -EINVAL;
        if ((size_t)st.st_size > DNN_SIDECAR_JSON_MAX)
            return -EFBIG;
    }

    FILE *f = fopen(sidecar, "rb");
    if (!f)
        return -errno;
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return -EIO;
    }
    const long sz_raw = ftell(f);
    if (sz_raw < 0 || (size_t)sz_raw > DNN_SIDECAR_JSON_MAX) {
        (void)fclose(f);
        return -EFBIG;
    }
    const size_t sz = (size_t)sz_raw;
    if (fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return -EIO;
    }
    char *buf = (char *)calloc(sz + 1u, 1u);
    if (!buf) {
        (void)fclose(f);
        return -ENOMEM;
    }
    const size_t r = fread(buf, 1u, sz, f);
    (void)fclose(f);
    if (r != sz) {
        free(buf);
        return -EIO;
    }
    assert(sz <= DNN_SIDECAR_JSON_MAX);
    /* buf was allocated as sz + 1u bytes just above, so buf[sz] is valid. The
     * analyzer loses this relationship across the fread path. */
    buf[sz] = '\0'; // NOLINT(clang-analyzer-security.ArrayBound)
    *out_buf = buf;
    return 0;
}

/* "kind": "nr" | "fr" | "filter" — anything else keeps the caller's default. */
static void parse_sidecar_kind(const char *doc, VmafModelSidecar *out)
{
    char *kind_str = extract_string(doc, "kind");
    if (!kind_str)
        return;
    if (strcmp(kind_str, "nr") == 0) {
        out->kind = VMAF_MODEL_KIND_DNN_NR;
    } else if (strcmp(kind_str, "fr") == 0) {
        out->kind = VMAF_MODEL_KIND_DNN_FR;
    } else if (strcmp(kind_str, "filter") == 0) {
        out->kind = VMAF_MODEL_KIND_DNN_FILTER;
    }
    free(kind_str);
}

/* Optional "output_names" array. A malformed array is non-fatal: the partial
 * allocations are wiped and the model stays single-output. */
static void parse_sidecar_output_names(const char *doc, VmafModelSidecar *out)
{
    out->n_output_names = 0u;
    size_t n_output_names = 0u;
    const int orc = extract_string_array(doc, "output_names", out->output_names,
                                         VMAF_DNN_MAX_OUTPUT_NAMES, &n_output_names);
    if (orc == 0 && n_output_names > 0u) {
        out->n_output_names = n_output_names;
    } else if (orc != -ENOENT && orc != 0) {
        free_partial_string_array(out->output_names, n_output_names);
        out->n_output_names = 0u;
    }
}

/* ADR-0173 / T5-3: optional quant_mode field (default fp32). */
static void parse_sidecar_quant_mode(const char *doc, VmafModelSidecar *out)
{
    out->quant_mode = VMAF_QUANT_FP32;
    char *quant_str = extract_string(doc, "quant_mode");
    if (!quant_str)
        return;
    if (strcmp(quant_str, "dynamic") == 0) {
        out->quant_mode = VMAF_QUANT_DYNAMIC;
    } else if (strcmp(quant_str, "static") == 0) {
        out->quant_mode = VMAF_QUANT_STATIC;
    } else if (strcmp(quant_str, "qat") == 0) {
        out->quant_mode = VMAF_QUANT_QAT;
    }
    /* Anything else (including "fp32" or junk) keeps the default. */
    free(quant_str);
}

/* Per-feature scaler arrays for the schema parsed by
 * parse_sidecar_feature_schema(). Sets out->has_feature_scaler when both
 * arrays are present and their lengths match @p n_names. */
static void parse_sidecar_feature_scaler(const char *doc, VmafModelSidecar *out, size_t n_names)
{
    size_t n_mean = 0u;
    size_t n_std = 0u;
    int mrc = extract_float_array(doc, "feature_mean", out->feature_mean,
                                  VMAF_DNN_MAX_FEATURE_NAMES, &n_mean);
    if (mrc == -ENOENT) {
        mrc = extract_float_array(doc, "input_mean", out->feature_mean, VMAF_DNN_MAX_FEATURE_NAMES,
                                  &n_mean);
    }
    int src = extract_float_array(doc, "feature_std", out->feature_std, VMAF_DNN_MAX_FEATURE_NAMES,
                                  &n_std);
    if (src == -ENOENT) {
        src = extract_float_array(doc, "input_std", out->feature_std, VMAF_DNN_MAX_FEATURE_NAMES,
                                  &n_std);
    }
    if (mrc == 0 && src == 0 && n_mean == n_names && n_std == n_names)
        out->has_feature_scaler = true;
}

/* ADR-0518: feature-vector tiny models carry their feature schema in
 * the sidecar — feature names (in input-tensor order), per-feature
 * scaler mean, per-feature scaler std. Two field-name conventions
 * are accepted:
 *
 *   - ``feature_order`` / ``feature_mean`` / ``feature_std`` —
 *     written by ``ai/scripts/train_fr_regressor_v2.py`` and the
 *     v1 trainer.
 *   - ``features`` / ``input_mean`` / ``input_std`` — written by
 *     the ``vmaf_tiny_v*`` trainers (the scaler is baked into the
 *     ONNX graph as Constant nodes, but the sidecar still echoes
 *     the per-feature values for downstream tooling).
 *
 * Missing schema is non-fatal: the loader treats the model as a
 * rank-4 NCHW image model and ``vmaf_ctx_dnn_attach`` enforces
 * the rank-4 contract. */
static void parse_sidecar_feature_schema(const char *doc, VmafModelSidecar *out)
{
    out->n_features = 0u;
    out->has_feature_scaler = false;
    size_t n_names = 0u;
    int frc = extract_string_array(doc, "feature_order", out->feature_names,
                                   VMAF_DNN_MAX_FEATURE_NAMES, &n_names);
    if (frc == -ENOENT) {
        frc = extract_string_array(doc, "features", out->feature_names, VMAF_DNN_MAX_FEATURE_NAMES,
                                   &n_names);
    }
    if (frc == 0 && n_names > 0u) {
        out->n_features = n_names;
        parse_sidecar_feature_scaler(doc, out, n_names);
    } else if (frc != -ENOENT && frc != 0) {
        /* Malformed array — wipe partial allocations to keep the
         * sidecar consistent. */
        free_partial_string_array(out->feature_names, n_names);
        out->n_features = 0u;
    }
}

/* "onnx_has_scaler": true — the ONNX graph already applies the
 * StandardScaler as baked Constant nodes (ADR-0244 / vmaf_tiny_v2..v4).
 * When present, the C runtime must skip re-applying has_feature_scaler to
 * avoid double-scaling. The field is parsed as a bare JSON boolean; any
 * non-"false" token in the value position is treated as true (the only
 * value the trainers ever write is "true"). */
static void parse_sidecar_onnx_has_scaler(const char *doc, VmafModelSidecar *out)
{
    out->onnx_has_scaler = false;
    const char *p_os = strstr(doc, "\"onnx_has_scaler\"");
    if (!p_os)
        return;
    const char *colon_os = strchr(p_os, ':');
    if (!colon_os)
        return;
    const char *v_os = colon_os + 1;
    while (*v_os && json_is_space(*v_os))
        v_os++;
    if (strncmp(v_os, "true", 4) == 0)
        out->onnx_has_scaler = true;
}

/* ADR-0519: codec-aware models carry an encoder vocabulary in the
 * sidecar so the CLI can validate --tiny-codec names and build the
 * correct one-hot block. Missing = not codec-aware (non-fatal). */
static void parse_sidecar_encoder_vocab(const char *doc, VmafModelSidecar *out)
{
    out->n_encoder_vocab = 0u;
    out->codec_aware = false;
    size_t n_vocab = 0u;
    const int vrc = extract_string_array(doc, "encoder_vocab", out->encoder_vocab,
                                         VMAF_DNN_MAX_ENCODER_VOCAB, &n_vocab);
    if (vrc == 0 && n_vocab > 0u) {
        out->n_encoder_vocab = n_vocab;
        out->codec_aware = true;
    } else if (vrc != -ENOENT && vrc != 0) {
        /* Malformed array — wipe partial allocations to keep the
         * sidecar consistent. */
        free_partial_string_array(out->encoder_vocab, n_vocab);
        out->n_encoder_vocab = 0u;
    }
}

int vmaf_dnn_sidecar_load(const char *onnx_path, VmafModelSidecar *out)
{
    if (!onnx_path || !out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->kind = VMAF_MODEL_KIND_DNN_FR;

    char sidecar[4096];
    int rc = sidecar_json_path(onnx_path, sidecar, sizeof(sidecar));
    if (rc != 0)
        return rc;

    char *buf = nullptr;
    rc = slurp_sidecar_json(sidecar, &buf);
    if (rc != 0)
        return rc;

    parse_sidecar_kind(buf, out);
    out->name = extract_string(buf, "name");
    out->input_name = extract_string(buf, "input_name");
    out->output_name = extract_string(buf, "output_name");
    parse_sidecar_output_names(buf, out);
    (void)extract_int(buf, "onnx_opset", &out->opset);
    parse_sidecar_quant_mode(buf, out);
    parse_sidecar_feature_schema(buf, out);
    parse_sidecar_onnx_has_scaler(buf, out);
    parse_sidecar_encoder_vocab(buf, out);

    free(buf);
    return 0;
}

/* ============================================================
 * ADR-0522 — codec block helper
 *
 * The trainer (ai/scripts/train_fr_regressor_v2.py) pins the layout
 * `[encoder_onehot(N_ENCODERS), preset_norm, crf_norm]` with
 * `PRESET_MAX_ORDINAL = 9.0` and `CRF_MAX = 63.0`. The PRESET_ORDINAL
 * table below mirrors lines 169..234 of that file. When the trainer
 * changes either constant, update both sides — see the AGENTS.md
 * note under libvmaf/src/dnn/.
 * ============================================================ */

/* One (preset name → raw ordinal) row of an encoder's preset vocabulary. */
typedef struct PresetOrdinal {
    const char *preset; /* lower-case preset token */
    float ordinal;      /* raw ordinal in [0, PRESET_MAX_ORD] */
} PresetOrdinal;

/* One encoder family: the encoder names that share @presets. Both arrays are
 * nullptr-terminated. */
typedef struct EncoderPresetTable {
    const char *const *encoders;
    const PresetOrdinal *presets;
} EncoderPresetTable;

/* Highest ordinal in the normalised [0, 1] preset scale (see the trainer's
 * `train_fr_regressor_v2.py::_preset_ordinal`). */
#define PRESET_MAX_ORD 9.0f
/* "medium" — the ordinal used whenever the encoder or the preset is unknown. */
#define PRESET_DEFAULT_ORD 5.0f

/* libx264 / libx265 share the same preset vocabulary. */
static const char *const kEncX26x[] = {"libx264", "libx265", nullptr};
static const PresetOrdinal kPresetsX26x[] = {
    {"ultrafast", 0.0f}, {"superfast", 1.0f}, {"veryfast", 2.0f}, {"faster", 3.0f},
    {"fast", 4.0f},      {"medium", 5.0f},    {"slow", 6.0f},     {"slower", 7.0f},
    {"veryslow", 8.0f},  {"placebo", 9.0f},   {nullptr, 0.0f},
};

static const char *const kEncVvenc[] = {"libvvenc", nullptr};
static const PresetOrdinal kPresetsVvenc[] = {
    {"faster", 1.0f}, {"fast", 3.0f},   {"medium", 5.0f},
    {"slow", 7.0f},   {"slower", 8.0f}, {nullptr, 0.0f},
};

/* libvpx-vp9 deadline strings. */
static const char *const kEncVp9[] = {"libvpx-vp9", nullptr};
static const PresetOrdinal kPresetsVp9[] = {
    {"realtime", 0.0f},
    {"good", 5.0f},
    {"best", 9.0f},
    {nullptr, 0.0f},
};

/* NVENC p1..p7. */
static const char *const kEncNvenc[] = {"h264_nvenc", "hevc_nvenc", "av1_nvenc", nullptr};
static const PresetOrdinal kPresetsNvenc[] = {
    {"p1", 0.0f}, {"p2", 2.0f}, {"p3", 3.0f}, {"p4", 5.0f},
    {"p5", 6.0f}, {"p6", 7.0f}, {"p7", 9.0f}, {nullptr, 0.0f},
};

/* Intel QSV (h264_qsv / hevc_qsv / av1_qsv). */
static const char *const kEncQsv[] = {"h264_qsv", "hevc_qsv", "av1_qsv", nullptr};
static const PresetOrdinal kPresetsQsv[] = {
    {"veryfast", 2.0f}, {"faster", 3.0f}, {"fast", 4.0f},     {"medium", 5.0f},
    {"slow", 6.0f},     {"slower", 7.0f}, {"veryslow", 8.0f}, {nullptr, 0.0f},
};

static const EncoderPresetTable kPresetTables[] = {
    {kEncX26x, kPresetsX26x},   {kEncVvenc, kPresetsVvenc}, {kEncVp9, kPresetsVp9},
    {kEncNvenc, kPresetsNvenc}, {kEncQsv, kPresetsQsv},     {nullptr, nullptr},
};

/* True when @p enc_lc appears in the nullptr-terminated @p encoders list. */
static bool encoder_in_family(const char *const *encoders, const char *enc_lc)
{
    for (size_t i = 0; encoders[i] != nullptr; ++i) {
        if (strcmp(encoders[i], enc_lc) == 0)
            return true;
    }
    return false;
}

/* Return the preset vocabulary for @p enc_lc, or nullptr when the encoder has
 * no string preset table (unknown encoder, or libsvtav1's numeric presets). */
static const PresetOrdinal *preset_table_for(const char *enc_lc)
{
    for (size_t t = 0; kPresetTables[t].encoders != nullptr; ++t) {
        if (encoder_in_family(kPresetTables[t].encoders, enc_lc))
            return kPresetTables[t].presets;
    }
    return nullptr;
}

/* libsvtav1 uses numeric presets 0..13; the trainer squashes them to 0..9.
 * Returns the raw ordinal, or PRESET_DEFAULT_ORD when @p preset_lc is not a
 * number in range. */
static float svtav1_preset_ordinal(const char *preset_lc)
{
    char *endp = nullptr;
    errno = 0;
    const long v = strtol(preset_lc, &endp, 10);
    if (endp != preset_lc && errno == 0 && v >= 0 && v <= 13) {
        const long clamped = v < 9 ? v : 9;
        return (float)clamped;
    }
    return PRESET_DEFAULT_ORD;
}

/* Per-encoder preset table. Keys are lower-case; lookup returns the
 * normalised value in [0, 1]. Falls back to ordinal 5 (medium) when
 * either the encoder or the preset string is not known. */
static float codec_block_preset_ordinal(const char *enc_lc, const char *preset_lc)
{
    if (!enc_lc || !preset_lc)
        return PRESET_DEFAULT_ORD / PRESET_MAX_ORD;

    if (strcmp(enc_lc, "libsvtav1") == 0)
        return svtav1_preset_ordinal(preset_lc) / PRESET_MAX_ORD;

    const PresetOrdinal *table = preset_table_for(enc_lc);
    if (table == nullptr)
        return PRESET_DEFAULT_ORD / PRESET_MAX_ORD;

    for (size_t i = 0; table[i].preset != nullptr; ++i) {
        if (strcmp(table[i].preset, preset_lc) == 0)
            return table[i].ordinal / PRESET_MAX_ORD;
    }
    return PRESET_DEFAULT_ORD / PRESET_MAX_ORD;
}

/* Lower-case @p n bytes of @p s in place.
 *
 * `(tolower)` is deliberately parenthesised: glibc's <ctype.h> defines
 * `tolower` as a five-level nested macro, which pushes this loop past the
 * ADR-1142 readability-function-size nesting budget purely as an artefact of
 * the expansion. The parenthesised form suppresses macro expansion and calls
 * the library function, which is behaviourally identical. */
static void str_to_lower(char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        s[i] = (char)(tolower)((unsigned char)s[i]);
    }
}

/* Resolve common ffprobe codec aliases (h264 → libx264, hevc → libx265,
 * av1 → libsvtav1, vp9 → libvpx-vp9, vvc → libvvenc) so the trainer
 * `train_fr_regressor_v2.py::_encoder_index` aliases work from the CLI
 * too. Returns @p name_lc when no alias matches. */
static const char *resolve_codec_alias(const char *name_lc)
{
    if (strcmp(name_lc, "h264") == 0 || strcmp(name_lc, "avc") == 0)
        return "libx264";
    if (strcmp(name_lc, "hevc") == 0 || strcmp(name_lc, "h265") == 0)
        return "libx265";
    if (strcmp(name_lc, "av1") == 0)
        return "libsvtav1";
    if (strcmp(name_lc, "vp9") == 0)
        return "libvpx-vp9";
    if (strcmp(name_lc, "vvc") == 0 || strcmp(name_lc, "h266") == 0)
        return "libvvenc";
    return name_lc;
}

/* Copy @p src into @p dst (capacity @p dst_sz) lower-cased.
 * Returns @p dst, or nullptr when @p src is absent/empty or does not fit. */
static const char *lower_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!src || src[0] == '\0')
        return nullptr;
    const size_t len = strlen(src);
    if (len >= dst_sz)
        return nullptr;
    memcpy(dst, src, len + 1u);
    str_to_lower(dst, len);
    return dst;
}

/* Find the one-hot slot for the alias-resolved codec key @p codec_lc.
 * Returns the vocab index, or @p n_vocab when no slot matches. */
static size_t codec_vocab_index(const char *const *vocab, size_t n_vocab, const char *codec_lc)
{
    for (size_t i = 0; i < n_vocab; ++i) {
        if (vocab[i] && strcmp(vocab[i], codec_lc) == 0)
            return i;
    }
    return n_vocab;
}

int vmaf_dnn_codec_block_fill(float *buf, size_t buf_len, const char *const *vocab, size_t n_vocab,
                              const char *codec_name, const char *preset, int crf)
{
    if (!buf || !vocab || n_vocab == 0u)
        return -EINVAL;
    /* Layout: [one-hot(n_vocab), preset_norm, crf_norm]. */
    if (buf_len != n_vocab + 2u)
        return -EINVAL;

    /* Zero the whole block first so exactly one slot ends up set. */
    for (size_t i = 0; i < buf_len; ++i) {
        buf[i] = 0.0f;
    }

    /* Default to "unknown" — the trainer guarantees the last slot is
     * the "unknown" bucket (see train_fr_regressor_v2.py
     * `UNKNOWN_ENCODER_INDEX = ENCODER_VOCAB.index("unknown")` with
     * "unknown" appended last). */
    size_t codec_idx = n_vocab - 1u;
    int found = 0;

    char name_lc[64];
    const char *codec_lc = lower_copy(name_lc, sizeof(name_lc), codec_name);

    if (codec_lc) {
        codec_lc = resolve_codec_alias(codec_lc);
        const size_t hit = codec_vocab_index(vocab, n_vocab, codec_lc);
        if (hit != n_vocab) {
            codec_idx = hit;
            found = 1;
        }
    } else if (!codec_name || codec_name[0] == '\0') {
        /* nullptr or empty codec name is a legitimate "unknown" tag. */
        found = 1;
    }

    buf[codec_idx] = 1.0f;

    /* preset_norm: encoder-specific ordinal table, normalised by 9.0. */
    char preset_lc[64];
    const char *preset_ptr = lower_copy(preset_lc, sizeof(preset_lc), preset);

    const char *enc_key = codec_lc ? codec_lc : "";
    buf[n_vocab] = codec_block_preset_ordinal(enc_key, preset_ptr);

    /* crf_norm: clamp to [0, 63] then divide. */
    const int crf_clamped = crf < 0 ? 0 : (crf > 63 ? 63 : crf);
    buf[n_vocab + 1u] = (float)crf_clamped / 63.0f;

    return found ? 0 : -ENOENT;
}

void vmaf_dnn_sidecar_free(VmafModelSidecar *s)
{
    if (!s)
        return;
    /* Array-count fields must not exceed their compile-time maximums. */
    assert(s->n_output_names <= VMAF_DNN_MAX_OUTPUT_NAMES);
    assert(s->n_features <= VMAF_DNN_MAX_FEATURE_NAMES);
    assert(s->n_encoder_vocab <= VMAF_DNN_MAX_ENCODER_VOCAB);
    free(s->name);
    free(s->input_name);
    free(s->output_name);
    for (size_t i = 0; i < s->n_output_names && i < VMAF_DNN_MAX_OUTPUT_NAMES; ++i) {
        free(s->output_names[i]);
    }
    for (size_t i = 0; i < s->n_features && i < VMAF_DNN_MAX_FEATURE_NAMES; ++i) {
        free(s->feature_names[i]);
    }
    for (size_t i = 0; i < s->n_encoder_vocab && i < VMAF_DNN_MAX_ENCODER_VOCAB; ++i) {
        free(s->encoder_vocab[i]);
    }
    memset(s, 0, sizeof(*s));
}

/* Size + kind check on the resolved path. Returns 0 + st_size in *out_size
 * on success, or a negative errno on failure. */
static int stat_regular(const char *path, size_t max_bytes, size_t *out_size)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -errno;
    if (!S_ISREG(st.st_mode))
        return -ENOENT;
    if ((size_t)st.st_size > max_bytes)
        return -E2BIG;
    *out_size = (size_t)st.st_size;
    return 0;
}

/* Read @p sz bytes of @p path into a freshly-allocated buffer. Caller frees. */
static int slurp_file(const char *path, size_t sz, unsigned char **out_buf)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -errno;
    unsigned char *buf = (unsigned char *)malloc(sz);
    if (!buf) {
        (void)fclose(f);
        return -ENOMEM;
    }
    const size_t r = fread(buf, 1u, sz, f);
    (void)fclose(f);
    if (r != sz) {
        free(buf);
        return -EIO;
    }
    *out_buf = buf;
    return 0;
}

int vmaf_dnn_validate_onnx(const char *path, size_t max_bytes)
{
    if (!path)
        return -EINVAL;
    assert(path != nullptr);
    if (max_bytes == 0)
        max_bytes = VMAF_DNN_DEFAULT_MAX_BYTES;
    assert(max_bytes > 0u);

    /* Resolve symlinks and normalise the path before any stat / open. An
     * adversarial --tiny-model value could point at a symlink to a non-
     * regular file; resolve_path() dereferences the symlink so the
     * subsequent S_ISREG check reflects the actual target. */
    char resolved[PATH_MAX];
    if (resolve_path(path, resolved) == nullptr)
        return -errno;
    assert(resolved[0] != '\0');

    /* Cache environment variables once per function call to avoid repeated
     * unsafe getenv() calls in multithreaded contexts (getenv is not required
     * to be thread-safe by C99, and glibc's implementation is known to race).
     * ADR-0461 caller-contract: no other thread calls setenv("VMAF_*")
     * concurrently with this read. A pthread_once snapshot (the
     * gpu_dispatch_env posture) is deliberately NOT used here — the tiny-model
     * tests setenv() this variable between cases and must observe each value. */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-0461 caller-contract. */
    const char *jail_dir = getenv("VMAF_TINY_MODEL_DIR");

    /* Optional chroot-style path jail via VMAF_TINY_MODEL_DIR. Applied
     * before any I/O on the target so a jail violation can't even trigger
     * a stat() of the would-be path. */
    int err = enforce_tiny_model_jail(resolved, jail_dir);
    if (err != 0)
        return err;

    size_t sz = 0;
    err = stat_regular(resolved, max_bytes, &sz);
    if (err != 0)
        return err;
    assert(sz <= max_bytes);
    /* Degenerate zero-byte file cannot be a valid ONNX ModelProto. */
    if (sz == 0)
        return -EBADMSG;

    unsigned char *buf = nullptr;
    err = slurp_file(resolved, sz, &buf);
    if (err != 0)
        return err;
    assert(buf != nullptr);

    /* Deep op-allowlist walk: parse the ONNX protobuf for NodeProto.op_type
     * strings and reject any that are not in the allowlist. This runs
     * before ORT's CreateSession, so a disallowed op short-circuits load. */
    err = vmaf_dnn_scan_onnx(buf, sz, nullptr);
    free(buf);
    return err;
}

/* ============================================================
 * T6-9 / ADR-0211 — Sigstore-bundle verification of tiny models.
 *
 * Operates on the fork's tiny-model registry (model/tiny/registry.json):
 *   1. Resolve the registry alongside the ONNX (or use the caller path).
 *   2. Find the entry whose `onnx` basename matches the user-supplied
 *      ONNX path and pull `sigstore_bundle`.
 *   3. Spawn `cosign verify-blob --bundle=<...> <onnx>` via posix_spawnp.
 *
 * Banned-function note (CLAUDE.md §6): system(3) is forbidden. We use
 * posix_spawnp(3p), check every return value, and pass argv as an array
 * (no shell, no quoting concerns). Windows builds return -ENOSYS — the
 * supply-chain workflow runs on Linux/macOS only today.
 * ============================================================ */

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
/* environ is a POSIX extension; the variable is only accessed in the
 * POSIX (non-Windows) build path via posix_spawnp().  Confine the
 * extern declaration here to match the actual usage site. */
extern char **environ;
#endif

/* Find the sigstore_bundle path for the model entry whose onnx basename
 * matches `onnx_basename`. Writes a registry-relative path into `out`
 * (size `out_sz`) on success, or returns -ENOENT when no match. The
 * parser reuses the ultra-small extract_string()/strstr() helpers above
 * to avoid pulling a JSON dep into libvmaf. */
static int find_bundle_for_onnx(const char *registry_doc, const char *onnx_basename, char *out,
                                size_t out_sz)
{
    if (!registry_doc || !onnx_basename || !out || out_sz == 0u)
        return -EINVAL;

    /* Iterate over each "id"/"onnx"/"sigstore_bundle" triple. The registry
     * is small (< 64 entries) so a linear scan is fine. We anchor on the
     * `"onnx"` key, then read the matching `"sigstore_bundle"` if it
     * occurs within the same JSON object (i.e. before the next `"onnx"`
     * or end-of-doc). */
    const char *cursor = registry_doc;
    while ((cursor = strstr(cursor, "\"onnx\"")) != nullptr) {
        const char *colon = strchr(cursor, ':');
        if (!colon)
            return -ENOENT;
        const char *p = colon + 1;
        while (*p && json_is_space(*p))
            p++;
        if (*p != '"')
            return -EBADMSG;
        p++;
        const char *q = strchr(p, '"');
        if (!q)
            return -EBADMSG;
        const size_t len = (size_t)(q - p);

        /* Compare against onnx_basename. */
        if (strlen(onnx_basename) == len && strncmp(p, onnx_basename, len) == 0) {
            /* Found the entry — search forward for sigstore_bundle within
             * the bounded window (next "onnx" key or end-of-doc). */
            const char *next_onnx = strstr(q, "\"onnx\"");
            const char *bundle_key = strstr(q, "\"sigstore_bundle\"");
            if (!bundle_key || (next_onnx && bundle_key > next_onnx))
                return -ENOENT;
            const char *bcolon = strchr(bundle_key, ':');
            if (!bcolon)
                return -EBADMSG;
            const char *bp = bcolon + 1;
            while (*bp && json_is_space(*bp))
                bp++;
            if (*bp != '"')
                return -EBADMSG;
            bp++;
            const char *bq = strchr(bp, '"');
            if (!bq)
                return -EBADMSG;
            const size_t blen = (size_t)(bq - bp);
            if (blen + 1u > out_sz)
                return -ENAMETOOLONG;
            memcpy(out, bp, blen);
            out[blen] = '\0';
            return 0;
        }
        cursor = q + 1;
    }
    return -ENOENT;
}

/* Slurp the registry JSON into a freshly-allocated NUL-terminated buffer.
 * Bounded to 1 MiB — the registry is < 8 KiB today; the cap is a defensive
 * sanity bound. Caller frees. */
static int slurp_registry(const char *registry_path, char **out_buf)
{
    FILE *f = fopen(registry_path, "rb");
    if (!f)
        return -errno;
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return -EIO;
    }
    long sz_raw = ftell(f);
    if (sz_raw < 0 || sz_raw > (1L << 20)) {
        (void)fclose(f);
        return -EFBIG;
    }
    const size_t sz = (size_t)sz_raw;
    if (fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return -EIO;
    }
    char *buf = (char *)calloc(sz + 1u, 1u);
    if (!buf) {
        (void)fclose(f);
        return -ENOMEM;
    }
    const size_t r = fread(buf, 1u, sz, f);
    (void)fclose(f);
    if (r != sz) {
        free(buf);
        return -EIO;
    }
    *out_buf = buf;
    return 0;
}

#ifdef _WIN32
int vmaf_dnn_verify_signature(const char *onnx_path, const char *registry_path)
{
    /* Argument validation runs before the platform-availability probe so
     * callers get a deterministic -EINVAL on misuse regardless of OS. The
     * NULL-path contract is part of the public API and must not be masked
     * by the Windows -ENOSYS short-circuit below. */
    if (!onnx_path)
        return -EINVAL;
    (void)registry_path;
    /* posix_spawn / cosign supply-chain path is Linux/macOS-only today.
     * The supply-chain workflow does not run on Windows; document and
     * fail loud rather than silently bypass. */
    return -ENOSYS;
}
#else
/* Compute the basename portion of `path` (no allocation; returns a pointer
 * into the input). Defensive against trailing slashes by stopping at the
 * first non-slash from the right. */
static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Locate `cosign` on PATH. Returns 0 on success and writes the resolved
 * absolute path into `out`; returns -EACCES otherwise. Implemented by
 * walking PATH manually and stat-checking each candidate so we never need
 * a shell.
 *
 * The @p path_env parameter (may be NULL) is expected to be pre-cached from
 * getenv("PATH") by the caller to avoid repeated unsafe getenv() calls in
 * multithreaded contexts. */
static int locate_cosign(const char *path_env, char *out, size_t out_sz)
{
    if (!path_env || path_env[0] == '\0')
        return -EACCES;
    const char *p = path_env;
    while (*p) {
        const char *colon = strchr(p, ':');
        const size_t seg_len = colon ? (size_t)(colon - p) : strlen(p);
        if (seg_len > 0u && seg_len + sizeof("/cosign") <= out_sz) {
            memcpy(out, p, seg_len);
            (void)snprintf(out + seg_len, out_sz - seg_len, "/cosign");
            struct stat st;
            if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
                return 0;
            }
        }
        if (!colon)
            break;
        p = colon + 1;
    }
    return -EACCES;
}

/* Registry path used when the caller passes registry_path == nullptr:
 * <dirname(onnx_path)>/registry.json, or model/tiny/registry.json when
 * @p onnx_path carries no directory component. */
static int default_registry_path(const char *onnx_path, char *out, size_t out_sz)
{
    const char *slash = strrchr(onnx_path, '/');
    if (slash) {
        const size_t dlen = (size_t)(slash - onnx_path);
        if (dlen + sizeof("/registry.json") > out_sz)
            return -ENAMETOOLONG;
        assert(dlen < out_sz);
        memcpy(out, onnx_path, dlen);
        (void)snprintf(out + dlen, out_sz - dlen, "/registry.json");
        return 0;
    }
    const int n = snprintf(out, out_sz, "model/tiny/registry.json");
    if (n < 0 || (size_t)n >= out_sz)
        return -ENAMETOOLONG;
    return 0;
}

/* Resolve @p bundle_rel against the directory of @p reg_path.
 *
 * We use memcpy() instead of snprintf("%s", ...) for the rel-component
 * copy: gcc's -Wformat-truncation cannot prove the destination is wide
 * enough (it sees `bundle_rel` as a PATH_MAX-sized buffer and the
 * destination tail as up to `out_sz - 1`, then warns about a theoretical
 * 4095-byte truncation even though the explicit length-precondition here
 * rules it out at runtime). memcpy + an explicit NUL keeps the bounds check
 * load-bearing without the false positive. */
static int resolve_bundle_abs(const char *reg_path, const char *bundle_rel, char *out,
                              size_t out_sz)
{
    const size_t blen = strlen(bundle_rel);
    const char *reg_slash = strrchr(reg_path, '/');
    if (!reg_slash) {
        if (blen + 1u > out_sz)
            return -ENAMETOOLONG;
        memcpy(out, bundle_rel, blen);
        out[blen] = '\0';
        return 0;
    }
    const size_t dlen = (size_t)(reg_slash - reg_path);
    if (dlen + 1u + blen + 1u > out_sz)
        return -ENAMETOOLONG;
    assert(dlen < out_sz);
    memcpy(out, reg_path, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1u, bundle_rel, blen);
    out[dlen + 1u + blen] = '\0';
    return 0;
}

/* Spawn `cosign verify-blob` on @p onnx_path with @p bundle_abs and wait for
 * it. Returns 0 only when cosign exits 0 (fail-closed).
 *
 * The certificate-identity-regexp + oidc-issuer mirror docs/ai/security.md;
 * they pin verification to VMAFx/vmafx's supply-chain workflow identity. */
static int run_cosign_verify(const char *cosign_path, const char *bundle_abs, const char *onnx_path)
{
    /* Build the --bundle=<path> argument. */
    char bundle_arg[PATH_MAX + 16];
    const int n = snprintf(bundle_arg, sizeof(bundle_arg), "--bundle=%s", bundle_abs);
    if (n < 0 || (size_t)n >= sizeof(bundle_arg))
        return -ENAMETOOLONG;
    assert((size_t)n < sizeof(bundle_arg));

    char *argv[] = {
        (char *)"cosign",
        (char *)"verify-blob",
        bundle_arg,
        (char *)"--certificate-identity-regexp",
        (char *)"https://github.com/VMAFx/vmafx/.github/workflows/.+",
        (char *)"--certificate-oidc-issuer",
        (char *)"https://token.actions.githubusercontent.com",
        (char *)onnx_path,
        nullptr,
    };

    pid_t pid = 0;
    const int sp = posix_spawnp(&pid, cosign_path, nullptr, nullptr, argv, environ);
    if (sp != 0)
        return -sp;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -errno;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -EPROTO;
    return 0;
}

int vmaf_dnn_verify_signature(const char *onnx_path, const char *registry_path)
{
    if (!onnx_path)
        return -EINVAL;
    assert(onnx_path != nullptr);

    /* Default registry: <dirname(onnx_path)>/registry.json. */
    char default_reg[PATH_MAX];
    const char *reg_path = registry_path;
    if (!reg_path) {
        const int prc = default_registry_path(onnx_path, default_reg, sizeof(default_reg));
        if (prc != 0)
            return prc;
        reg_path = default_reg;
    }
    assert(reg_path != nullptr);

    char *reg_buf = nullptr;
    int err = slurp_registry(reg_path, &reg_buf);
    if (err != 0)
        return err;
    assert(reg_buf != nullptr);

    const char *base = path_basename(onnx_path);
    assert(base != nullptr);
    char bundle_rel[PATH_MAX];
    err = find_bundle_for_onnx(reg_buf, base, bundle_rel, sizeof(bundle_rel));
    free(reg_buf);
    if (err != 0)
        return err;
    assert(bundle_rel[0] != '\0');

    char bundle_abs[PATH_MAX];
    err = resolve_bundle_abs(reg_path, bundle_rel, bundle_abs, sizeof(bundle_abs));
    if (err != 0)
        return err;
    assert(bundle_abs[0] != '\0');

    /* The bundle file must exist before we even invoke cosign — otherwise
     * cosign's error message is opaque. Fail-closed: missing bundle = no
     * trust. */
    struct stat bst;
    if (stat(bundle_abs, &bst) != 0)
        return -ENOENT;
    if (!S_ISREG(bst.st_mode))
        return -ENOENT;

    /* Cache environment variables once per function call to avoid repeated
     * unsafe getenv() calls in multithreaded contexts (getenv is not required
     * to be thread-safe by C99, and glibc's implementation is known to race).
     * ADR-0461 caller-contract: no other thread calls setenv("PATH")
     * concurrently with this read. */
    /* NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-0461 caller-contract. */
    const char *path_env = getenv("PATH");

    char cosign_path[PATH_MAX];
    err = locate_cosign(path_env, cosign_path, sizeof(cosign_path));
    if (err != 0)
        return err;
    assert(cosign_path[0] != '\0');

    return run_cosign_verify(cosign_path, bundle_abs, onnx_path);
}
#endif /* !_WIN32 */
