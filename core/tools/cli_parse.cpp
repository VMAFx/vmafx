/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

/* ADR-0809 — C++23 Wave 8: cli_parse.c → cli_parse.cpp.
 * Conservative idioms only: nullptr, static_cast, std::string_view for
 * option-string comparisons (removes pointer arithmetic / strlen calls),
 * [[nodiscard]] on helpers that return error-coded values.
 * The public ABI (cli_parse.h) is unchanged; extern "C" guards added to
 * that header keep all C callers compiling without modification. */

#include "config.h"

#ifdef _WIN32
#include "compat/win32/getopt.h"
#include <windows.h>
#else
#include <getopt.h>
#include <unistd.h>
#endif
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

#include "cli_parse.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "vmafx/import_layouts_gen.h"

namespace
{

/* The getopt short option string (short_opts), the long-only option
 * identifiers (ARG_*), the long option table (long_opts) and the usage lines
 * are generated from the option groups of core/api/vmafx.toml (ADR-1852, RC4
 * WP8): a flag, an alias or a help line is added there, never here. */
#include "cli_options.gen.inc"

/* Default matches Netflix's pre-fork output exactly so the CPU golden
 * gate passes without explicit flags (CLAUDE.md §8). Round-trip lossless
 * formatting is opt-in via --precision=max. See ADR-0119 (supersedes
 * ADR-0006). */
#define VMAF_DEFAULT_PRECISION_FMT "%.6f"
#define VMAF_LOSSLESS_PRECISION_FMT "%.17g"

char precision_fmt_buf[16];

const char *resolve_precision_fmt(const char *optarg, const char *app, CLISettings *s)
{
    using sv = std::string_view;
    const sv arg{optarg};
    if (arg == "max" || arg == "full") {
        s->precision_max = true;
        return VMAF_LOSSLESS_PRECISION_FMT;
    }
    if (arg == "legacy") {
        /* `legacy` is now the default; keep the alias accepted so existing
         * scripts that pass it explicitly do not break. */
        s->precision_legacy = true;
        return VMAF_DEFAULT_PRECISION_FMT;
    }
    char *end = nullptr;
    const long n = strtol(optarg, &end, 10);
    if (*end || end == optarg || n < 1 || n > 17) {
        (void)fprintf(stderr,
                      "%s: --precision must be an integer 1..17, "
                      "or one of: max, full, legacy (got: %s)\n",
                      app, optarg);
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-1155: CLI error exit
        exit(1);
    }
    s->precision_n = static_cast<int>(n);
    (void)snprintf(precision_fmt_buf, sizeof precision_fmt_buf, "%%.%ldg", n);
    return precision_fmt_buf;
}

void print_usage(FILE *const out, const char *const app)
{
    (void)fprintf(out, "Usage: %s [options]\n\n", app);
    for (const char *const line : usage_lines) {
        (void)fprintf(out, "%s\n", line);
    }
}

[[noreturn]] void usage_exit(bool is_error)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-1155: CLI usage exit
    exit(is_error ? 1 : 0);
}

[[noreturn]] void usage(const char *const app, const char *const reason)
{
    FILE *const out = reason ? stderr : stdout;
    if (reason) {
        (void)fputs(reason, stderr);
        (void)fprintf(stderr, "\n\n");
    }
    print_usage(out, app);
    usage_exit(reason != nullptr);
}

template <typename Arg>
[[noreturn]] void usage(const char *const app, const char *const reason, Arg &&arg)
{
    FILE *const out = reason ? stderr : stdout;
    if (reason) {
        (void)fprintf(stderr, reason, std::forward<Arg>(arg));
        (void)fprintf(stderr, "\n\n");
    }
    print_usage(out, app);
    usage_exit(reason != nullptr);
}

template <typename Arg1, typename Arg2>
[[noreturn]] void usage(const char *const app, const char *const reason, Arg1 &&arg1, Arg2 &&arg2)
{
    FILE *const out = reason ? stderr : stdout;
    if (reason) {
        (void)fprintf(stderr, reason, std::forward<Arg1>(arg1), std::forward<Arg2>(arg2));
        (void)fprintf(stderr, "\n\n");
    }
    print_usage(out, app);
    usage_exit(reason != nullptr);
}

template <typename Arg1, typename Arg2, typename Arg3>
[[noreturn]] void usage(const char *const app, const char *const reason, Arg1 &&arg1, Arg2 &&arg2,
                        Arg3 &&arg3)
{
    FILE *const out = reason ? stderr : stdout;
    if (reason) {
        (void)fprintf(stderr, reason, std::forward<Arg1>(arg1), std::forward<Arg2>(arg2),
                      std::forward<Arg3>(arg3));
        (void)fprintf(stderr, "\n\n");
    }
    print_usage(out, app);
    usage_exit(reason != nullptr);
}

[[noreturn]] void usage(const char *const app, std::nullptr_t)
{
    print_usage(stdout, app);
    usage_exit(false);
}

template <typename T>
void checked_append(T *const arr, unsigned &cnt, const T &val, const char *const app,
                    const char *const desc)
{
    if (cnt == CLI_SETTINGS_STATIC_ARRAY_LEN) {
        usage(app, "A maximum of %d %s are supported\n", CLI_SETTINGS_STATIC_ARRAY_LEN, desc);
    }
    arr[cnt++] = val;
}

void checked_replace_feature(CLIFeatureConfig *const arr, unsigned &cnt,
                             const CLIFeatureConfig &val, const char *const app,
                             const char *const desc)
{
    unsigned i = 0;
    for (i = 0; i < cnt; i++) {
        if (!strcmp(arr[i].name, val.name)) {
            free(arr[i].buf);
            vmaf_feature_dictionary_free(&arr[i].opts_dict);
            arr[i] = val;
            break;
        }
    }
    if (i == cnt) {
        checked_append(arr, cnt, val, app, desc);
    }
}

void error(const char *const app, const char *const optarg, const int option,
           const char *const shouldbe)
{
    char optname[256];
    int n = 0;

    for (n = 0; long_opts[n].name; n++) {
        if (long_opts[n].val == option)
            break;
    }
    /* Replace assert(long_opts[n].name) with an explicit check: the
     * banned-function audit (ADR-0523) found that assert() here (a)
     * uses a banned macro per docs/principles.md §1.2 rule 30 in
     * analysis and (b) with GCC 16 + -O3 -flto the assert→SIGABRT path
     * was transformed into SIGSEGV via sprintf(NULL) on the next line,
     * making the failure non-deterministic and harder to diagnose.
     * Replace with a clean error-exit that never invokes UB. */
    if (!long_opts[n].name) {
        usage(app,
              "Invalid argument \"%s\" for unrecognised option (internal error: "
              "option code %d not in long_opts[])",
              optarg, option);
        return; /* unreachable — usage() calls exit(1); satisfies [[noreturn]] analysis */
    }
    if (long_opts[n].val < 256) {
        (void)snprintf(optname, sizeof(optname), "-%c/--%s", long_opts[n].val, long_opts[n].name);
    } else {
        (void)snprintf(optname, sizeof(optname), "--%s", long_opts[n].name);
    }

    usage(app, "Invalid argument \"%s\" for option %s; should be %s", optarg, optname, shouldbe);
}

[[nodiscard]] unsigned parse_unsigned(const char *const optarg, const int option,
                                      const char *const app)
{
    /* Reject negative strings before calling strtoul: POSIX strtoul silently
     * converts "-1" to ULONG_MAX via unsigned wrapping, which would then be
     * truncated to UINT_MAX and silently accepted. */
    if (optarg[0] == '-')
        error(app, optarg, option, "a non-negative integer");
    char *end = nullptr;
    errno = 0;
    const unsigned long ul = strtoul(optarg, &end, 0);
    if (*end || end == optarg || errno == ERANGE || ul > UINT_MAX)
        error(app, optarg, option, "an integer in [0, 2^32-1]");
    return static_cast<unsigned>(ul);
}

[[nodiscard]] unsigned parse_bitdepth(const char *const optarg, const int option,
                                      const char *const app)
{
    const unsigned bitdepth = parse_unsigned(optarg, option, app);
    if (bitdepth < 8 || bitdepth > 16)
        error(app, optarg, option, "a valid bitdepth (8 to 16)");
    return bitdepth;
}

/* A layout of the import table named on the command line (core/api/vmafx.toml
 * `[[pixel_formats]]`, ADR-2145): its row, or nullptr. The planar rows (yuv420p ... gray) are
 * accepted as names too. */
[[nodiscard]] const VmafxImportLayout *find_layout(const std::string_view name)
{
    for (const VmafxImportLayout &row : vmafx_import_layouts) {
        if (name == row.name)
            return &row;
    }
    return nullptr;
}

/* A plain planar row: the frame is the file's, no conversion. */
[[nodiscard]] bool is_plain_planar(const VmafxImportLayout &row)
{
    return row.packed == VMAFX_IMPORT_PACKED_NONE && !row.interleaved && !row.msb &&
           row.pix_fmt == row.planar_fmt;
}

/* `--pixel_format`: 400 / 420 / 422 / 444, or a layout of the import table. Sets the planar frame
 * (`pix_fmt`), the layout of a raw file that is not planar (`input_layout`) and the bit depth a
 * layout fixes (a later --bitdepth is checked against it). */
void parse_pixel_format(const char *const optarg, const int option, const char *const app,
                        CLISettings *const settings)
{
    const std::string_view arg{optarg};
    settings->input_layout = 0;
    settings->pix_fmt = VMAF_PIX_FMT_UNKNOWN;
    if (arg == "400")
        settings->pix_fmt = VMAF_PIX_FMT_YUV400P;
    if (arg == "420")
        settings->pix_fmt = VMAF_PIX_FMT_YUV420P;
    if (arg == "422")
        settings->pix_fmt = VMAF_PIX_FMT_YUV422P;
    if (arg == "444")
        settings->pix_fmt = VMAF_PIX_FMT_YUV444P;
    if (const VmafxImportLayout *const row = find_layout(arg); row && !settings->pix_fmt) {
        settings->pix_fmt = static_cast<enum VmafPixelFormat>(row->planar_fmt);
        if (!is_plain_planar(*row))
            settings->input_layout = row->pix_fmt;
        if (row->bpc_min == row->bpc_max)
            settings->bitdepth = row->bpc_min;
    }
    if (!settings->pix_fmt) {
        error(app, optarg, option,
              "a valid pixel format (400/420/422/444, or a layout: nv12 nv16 nv24 p010 p016 p210 "
              "p216 p410 p416 yuyv422 uyvy422 v210 y210 y212 xv30 xv36 vuyx ayuv yuv444p16msb "
              "rgb rgba bgra)");
    }
}

/* A name and its VmafxColor* value. */
struct NamedColor {
    const char *name;
    unsigned value;
};

[[nodiscard]] unsigned parse_named_color(const NamedColor *const table, const size_t n,
                                         const char *const optarg, const int option,
                                         const char *const app, const char *const expected)
{
    for (size_t i = 0; i < n; i++) {
        if (std::string_view{optarg} == table[i].name)
            return table[i].value;
    }
    error(app, optarg, option, expected);
    return 0;
}

/* The statement of a raw RGB input (ADR-2146). Values the integer conversion does not make
 * (bt2020cl, ictcp, linear) are parsed so that the refusal can name them. */
void handle_rgb_flag(const int o, const char *const optarg, const char *const app,
                     CLISettings *const settings)
{
    static const NamedColor matrices[] = {{"bt601", VMAF_COLOR_MATRIX_BT601},
                                          {"bt709", VMAF_COLOR_MATRIX_BT709},
                                          {"bt2020ncl", VMAF_COLOR_MATRIX_BT2020_NCL},
                                          {"bt2020cl", VMAF_COLOR_MATRIX_BT2020_CL},
                                          {"ictcp", VMAF_COLOR_MATRIX_ICTCP}};
    static const NamedColor ranges[] = {{"limited", VMAF_COLOR_RANGE_LIMITED},
                                        {"full", VMAF_COLOR_RANGE_FULL}};
    static const NamedColor transfers[] = {{"bt709", VMAF_COLOR_TRC_BT709},
                                           {"srgb", VMAF_COLOR_TRC_SRGB},
                                           {"pq", VMAF_COLOR_TRC_SMPTE2084},
                                           {"hlg", VMAF_COLOR_TRC_HLG},
                                           {"linear", VMAF_COLOR_TRC_LINEAR}};
    switch (o) {
    case ARG_RGB_MATRIX:
        settings->rgb_matrix = parse_named_color(matrices, std::size(matrices), optarg, o, app,
                                                 "a matrix: bt601, bt709 or bt2020ncl");
        break;
    case ARG_RGB_RANGE:
        settings->rgb_range = parse_named_color(ranges, std::size(ranges), optarg, o, app,
                                                "a range: limited or full");
        break;
    case ARG_RGB_TRANSFER:
        settings->rgb_transfer = parse_named_color(transfers, std::size(transfers), optarg, o, app,
                                                   "a transfer: bt709, srgb, pq or hlg");
        break;
    default:
        settings->rgb_out_range = parse_named_color(ranges, std::size(ranges), optarg, o, app,
                                                    "a range: limited or full");
        break;
    }
}

/* ADR-1190 — escape-aware splitting of the `--model` / `--feature` option
 * strings (Netflix/vmaf#766).
 *
 * Every split site used to call `strsep`, so any ':' or '=' inside a value was
 * a separator no matter what the user meant: `-m 'path=C:\models\m.json'` died
 * with `bad option string "\models\m.json"`, and `-m 'path=/a/dir=eq/m.json'`
 * was silently truncated to `/a/dir` — a phantom path nobody typed.
 *
 * `cli_split()` replaces `strsep` at all nine sites. It breaks on the first
 * UNESCAPED separator and leaves backslash sequences intact, so a `\:` written
 * for the ':' pass is still literal when the '=' pass runs; the leaf token is
 * unescaped exactly once, after the last split — keys and feature names by
 * `cli_unescape_key()`, values by `cli_unescape_value()` (ADR-1355). A ':' that
 * spells a Windows drive letter is data rather than a separator, so the common
 * `path=C:\...` form needs no escaping at all.
 *
 * The `strsep` shim is gone with the call sites: it existed only as a
 * POSIX/MSVC portability wrapper (MSVC's UCRT declares `strsep` as `extern
 * "C"`, which is why it needed a distinct name), and `cli_split` is
 * self-contained on every platform. */

/* True when s[i] — known to be ':' — is the drive-letter colon of a Windows
 * path: exactly one ASCII letter precedes it, that letter starts the segment
 * (string start, or immediately after a '=' or ':'), and a path separator
 * follows. `path=C:\models\m.json` therefore parses as one key/value pair. */
[[nodiscard]] bool cli_is_drive_colon(const char *const s, const size_t i)
{
    if (i == 0U)
        return false;
    if ((i > 1U) && (s[i - 2U] != '=') && (s[i - 2U] != ':'))
        return false;
    const char letter = s[i - 1U];
    const bool is_alpha =
        ((letter >= 'A') && (letter <= 'Z')) || ((letter >= 'a') && (letter <= 'z'));
    return is_alpha && ((s[i + 1U] == '\\') || (s[i + 1U] == '/'));
}

/* strsep() semantics — returns the token, advances *sp past the separator or to
 * nullptr when the token runs to the end — except that a backslash-escaped
 * separator and a Windows drive-letter colon are literal. Backslashes are
 * preserved here; the leaf unescaper decides which of them are escapes. A
 * separator after an odd run of backslashes is therefore literal and one after
 * an even run is a separator, which is the pairing cli_unescape_value() uses. */
char *cli_split(char **sp, const char sep)
{
    if (!sp || !*sp)
        return nullptr;
    char *const s = *sp;
    size_t i = 0U;
    while (s[i] != '\0') {
        if ((s[i] == '\\') && (s[i + 1U] != '\0')) {
            i += 2U; /* skip the escaped byte; the leaf unescaper handles the backslash */
            continue;
        }
        if (s[i] != sep) {
            i++;
            continue;
        }
        if ((sep == ':') && cli_is_drive_colon(s, i)) {
            i++;
            continue;
        }
        s[i] = '\0';
        *sp = s + i + 1U;
        return s;
    }
    *sp = nullptr;
    return s;
}

/* In-place removal of the escaping backslash in `\:`, `\=`, `\.` and `\\`, for
 * keys and feature names only: a model overload key is split on '.', so all
 * four are escapes there. Every other backslash is data. Values go through
 * cli_unescape_value() instead. Call once, on a token that will not be split
 * again. */
void cli_unescape_key(char *const s)
{
    if (!s)
        return;
    char *w = s;
    const char *r = s;
    while (*r != '\0') {
        const char nxt = r[1];
        if ((*r == '\\') && ((nxt == ':') || (nxt == '=') || (nxt == '.') || (nxt == '\\')))
            r++;
        *w = *r;
        w++;
        r++;
    }
    *w = '\0';
}

/* Length of the run of backslashes that starts at s. */
[[nodiscard]] size_t cli_backslash_run(const char *const s)
{
    size_t n = 0U;
    while (s[n] == '\\')
        n++;
    return n;
}

/* ADR-1355 — in-place unescape of a value: a path, a model name, an option
 * value. Values are where Windows paths live, so a backslash is data unless it
 * belongs to a run that sits directly before ':' or '=' or ends the value.
 * Such a run is read in pairs, `\\` standing for one backslash; a lone
 * backslash left over escapes the ':' or '=' after it, or, at the end of the
 * value, stands for itself. `..\m.json`, `\\server\share` and
 * `C:\models\.cache` pass through byte for byte, while `\:` and `\=` keep their
 * ADR-1190 meaning and a literal backslash can still precede ':' or '='. */
void cli_unescape_value(char *const s)
{
    if (!s)
        return;
    char *w = s;
    const char *r = s;
    while (*r != '\0') {
        if (*r != '\\') {
            *w = *r;
            w++;
            r++;
            continue;
        }
        const size_t run = cli_backslash_run(r);
        const char next = r[run];
        const bool paired = (next == ':') || (next == '=') || (next == '\0');
        size_t keep = paired ? (run / 2U) : run;
        if ((next == '\0') && ((run % 2U) != 0U))
            keep++; /* a lone trailing backslash is data */
        /* keep <= run, so the write cursor never passes the read cursor. */
        for (size_t k = 0U; k < keep; k++) {
            *w = '\\';
            w++;
        }
        r += run;
    }
    *w = '\0';
}

void apply_model_opt(CLIModelConfig &model_cfg, char *key, char *val, const char *const app)
{
    if (!strcmp(key, "path")) {
        model_cfg.path = val;
    } else if (!strcmp(key, "name")) {
        model_cfg.cfg.name = val;
    } else if (!strcmp(key, "version")) {
        model_cfg.version = val;
    } else if (!strcmp(key, "disable_clip")) {
        model_cfg.cfg.flags |= !strcmp(val, "true") ? VMAF_MODEL_FLAG_DISABLE_CLIP : 0;
    } else if (!strcmp(key, "enable_transform")) {
        model_cfg.cfg.flags |= !strcmp(val, "true") ? VMAF_MODEL_FLAG_ENABLE_TRANSFORM : 0;
    } else {
        if (model_cfg.overload_cnt == CLI_SETTINGS_STATIC_ARRAY_LEN) {
            usage(app,
                  "A maximum of %d feature overloads per model"
                  " are supported\n",
                  CLI_SETTINGS_STATIC_ARRAY_LEN);
        }
        /* The overload key is `<feature>.<option>`; split before unescaping so
         * a `\.` inside either half stays data instead of becoming the
         * separator. */
        char *const name = cli_split(&key, '.');
        char *const opt = cli_split(&key, '.');
        cli_unescape_key(name);
        cli_unescape_key(opt);
        model_cfg.feature_overload[model_cfg.overload_cnt].name = name;
        const int err = vmaf_feature_dictionary_set(
            &model_cfg.feature_overload[model_cfg.overload_cnt].opts_dict, opt, val);
        if (err)
            usage(app, "Problem parsing model: \"%s\"\n", name);

        model_cfg.overload_cnt++;
    }
}

/* Release the option-string buffer before reporting a parse error.
 *
 * `usage()` is `_Noreturn` and ends the process in the shipped CLI, so this
 * free is unobservable there. It matters for the libFuzzer harness, which
 * intercepts `exit` via `-Wl,--wrap=exit` and longjmps back into its own
 * frame: the half-built CLIModelConfig / CLIFeatureConfig never reaches
 * `CLISettings`, so `cli_free()` cannot release its buffer and 32 bytes leak.
 * LeakSanitizer only reports it when its conservative scan no longer sees the
 * stale pointer, which made the nightly fuzz job fail intermittently rather
 * than reproducibly. Freeing here also keeps the function correct if `usage()`
 * ever stops exiting. */
[[noreturn]] static void usage_free(void *buf, const char *const app, const char *const fmt,
                                    const char *const arg)
{
    /* `arg` points INTO `buf` — it is a slice of the option string, not a
     * separate allocation — so it must be copied before the buffer goes away.
     * Freeing first and passing the original pointer prints freed memory: the
     * message came out as `bad option string ""`. */
    char arg_copy[256];
    (void)snprintf(arg_copy, sizeof(arg_copy), "%s", (arg != nullptr) ? arg : "");
    free(buf);
    usage(app, fmt, arg_copy);
}

CLIModelConfig parse_model_config(const char *const optarg, const char *const app)
{
    const size_t optarg_sz = strnlen(optarg, 1024);
    char *optarg_copy = static_cast<char *>(malloc(optarg_sz + 1));
    if (!optarg_copy)
        usage(app, "error while parsing model option: %s", optarg);
    (void)memset(optarg_copy, 0, optarg_sz + 1);
    (void)strncpy(optarg_copy, optarg, optarg_sz);

    CLIModelConfig model_cfg = {
        .path = nullptr,
        .version = nullptr,
        .cfg =
            {
                .name = "vmaf",
                .flags = VMAF_MODEL_FLAGS_DEFAULT,
            },
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = optarg_copy,
        .is_default = false,
    };

    char *key_val = nullptr;
    while ((key_val = cli_split(&optarg_copy, ':')) != nullptr) {
        char *const key = cli_split(&key_val, '=');
        /* Everything after the first unescaped '=' is the value, so a second
         * '=' inside a path no longer truncates it. The key keeps its
         * backslashes: apply_model_opt() may still split it on '.'. */
        char *val = key_val;
        cli_unescape_value(val);
        if (!val) {
            if (!strcmp(key, "disable_clip") || !strcmp(key, "enable_transform")) {
                val = const_cast<char *>("true");
            } else {
                usage_free(model_cfg.buf, app,
                           "Problem parsing model, "
                           "bad option string \"%s\".",
                           key);
            }
        }
        apply_model_opt(model_cfg, key, val, app);
    }

    return model_cfg;
}

/* CLI alias map: user-facing "integer_*" names to the internal extractor
 * registration names.  Extractors register without the "integer_" prefix (or
 * with a completely different name), so passing the prefix verbatim yields
 * "problem loading feature extractor".  Adding the map here — at the parse
 * layer — keeps the rewrite in one place and leaves the extractor registry
 * unchanged.  See the commit that introduced this table for the full list of
 * affected names. */
const struct {
    const char *alias;
    const char *target;
} cli_feature_aliases[] = {
    {.alias = "integer_motion", .target = "motion"},
    {.alias = "integer_motion2", .target = "motion_v2"},
    {.alias = "integer_ssim", .target = "ssim"},
    {.alias = "integer_ms_ssim", .target = "float_ms_ssim"},
    {.alias = "integer_psnr", .target = "psnr"},
};

/* Report a bad feature option string, releasing the half-built config first.
 *
 * Same ownership problem as parse_model_config(): the config has not reached
 * CLISettings yet, so cli_free() cannot release it. Harmless in the shipped
 * CLI, where usage() exits; load-bearing under the fuzz harness, which
 * longjmps out of exit(). Both the feature name and the key point INTO the
 * buffer, so they are copied before it is released — freeing first and
 * formatting afterwards prints freed memory. */
[[noreturn]] static void feature_usage_free(CLIFeatureConfig *const cfg, const char *const app,
                                            const char *const key)
{
    char name_copy[256];
    char key_copy[256];
    (void)snprintf(name_copy, sizeof(name_copy), "%s", (cfg->name != nullptr) ? cfg->name : "");
    (void)snprintf(key_copy, sizeof(key_copy), "%s", (key != nullptr) ? key : "");
    (void)vmaf_feature_dictionary_free(&cfg->opts_dict);
    free(cfg->buf);
    usage(app,
          "Problem parsing feature \"%s\", "
          "bad option string \"%s\".\n",
          name_copy, key_copy);
}

CLIFeatureConfig parse_feature_config(const char *const optarg, const char *const app)
{
    const size_t optarg_sz = strnlen(optarg, 1024);
    char *optarg_copy = static_cast<char *>(malloc(optarg_sz + 1));
    if (!optarg_copy)
        usage(app, "error while parsing feature option: %s", optarg);
    (void)memset(optarg_copy, 0, optarg_sz + 1);
    (void)strncpy(optarg_copy, optarg, optarg_sz);
    void *buf = optarg_copy;

    char *const feature_name = cli_split(&optarg_copy, '=');
    cli_unescape_key(feature_name);

    CLIFeatureConfig feature_cfg = {
        .name = feature_name,
        .opts_dict = nullptr,
        .buf = buf,
    };

    /* Rewrite user-facing "integer_*" aliases to the names the extractor
     * registry actually uses.  The rewrite only touches the name field; any
     * key=value options that follow the "=" separator are unaffected. */
    for (const auto &feature_alias : cli_feature_aliases) {
        if (!strcmp(feature_cfg.name, feature_alias.alias)) {
            feature_cfg.name = feature_alias.target;
            break;
        }
    }

    char *key_val = nullptr;
    while ((key_val = cli_split(&optarg_copy, ':')) != nullptr) {
        char *const key = cli_split(&key_val, '=');
        /* Value = the whole remainder after the first unescaped '='. */
        char *const val = key_val;
        cli_unescape_key(key);
        cli_unescape_value(val);
        if (!val)
            feature_usage_free(&feature_cfg, app, key);
        const int err = vmaf_feature_dictionary_set(&feature_cfg.opts_dict, key, val);
        if (err) {
            /* `optarg` is the caller's argv string, not our buffer, so it
             * stays valid across the free. */
            (void)vmaf_feature_dictionary_free(&feature_cfg.opts_dict);
            free(feature_cfg.buf);
            usage(app, "Problem parsing feature \"%s\"\n", optarg);
        }
    }

    return feature_cfg;
}

void aom_ctc_v1_0(CLISettings *const settings, const char *const app)
{
    const CLIModelConfig cfg = {
        .path = nullptr,
        .version = "vmaf_v0.6.1", /* vmaf-model-pin: AOM CTC v1.0 mandates this exact model */
        .cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT},
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = nullptr,
        .is_default = false,
    };
    checked_append(settings->model_config, settings->model_cnt, cfg, app, "models");

    const CLIModelConfig cfg_neg = {
        .path = nullptr,
        .version = "vmaf_v0.6.1neg", /* vmaf-model-pin: AOM CTC v1.0 mandates this exact model */
        .cfg = {.name = "vmaf_neg", .flags = VMAF_MODEL_FLAGS_DEFAULT},
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = nullptr,
        .is_default = false,
    };
    checked_append(settings->model_config, settings->model_cnt, cfg_neg, app, "models");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("psnr=reduced_hbd_peak=true:"
                                        "enable_apsnr=true:min_sse=0.5",
                                        app),
                   app, "features");

    checked_append(settings->feature_cfg, settings->feature_cnt, parse_feature_config("ciede", app),
                   app, "features");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("float_ssim=enable_db=true:clip_db=true", app), app,
                   "features");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("float_ms_ssim=enable_db=true:clip_db=true", app), app,
                   "features");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("psnr_hvs", app), app, "features");
}

void aom_ctc_v2_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v1_0(settings, app);
}

void aom_ctc_v3_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v2_0(settings, app);
    checked_append(settings->feature_cfg, settings->feature_cnt, parse_feature_config("cambi", app),
                   app, "features");
}

void aom_ctc_v4_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v3_0(settings, app);
}

void aom_ctc_v5_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v4_0(settings, app);
}

void aom_ctc_v6_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v5_0(settings, app);
    settings->common_bitdepth = true;
}

void aom_ctc_v7_0(CLISettings *const settings, const char *const app)
{
    aom_ctc_v6_0(settings, app);
    checked_replace_feature(
        settings->feature_cfg, settings->feature_cnt,
        parse_feature_config("float_ssim=scale=1:enable_db=true:clip_db=true", app), app,
        "features");
}

void parse_aom_ctc(CLISettings *const settings, const char *const optarg, const char *const app)
{
    using sv = std::string_view;
    const sv arg{optarg};

    if (arg == "proposed")
        usage(app, "`--aom_ctc proposed` is deprecated.");

    if (arg == "v1.0") {
        aom_ctc_v1_0(settings, app);
        return;
    }
    if (arg == "v2.0") {
        aom_ctc_v2_0(settings, app);
        return;
    }
    if (arg == "v3.0") {
        aom_ctc_v3_0(settings, app);
        return;
    }
    if (arg == "v4.0") {
        aom_ctc_v4_0(settings, app);
        return;
    }
    if (arg == "v5.0") {
        aom_ctc_v5_0(settings, app);
        return;
    }
    if (arg == "v6.0") {
        aom_ctc_v6_0(settings, app);
        return;
    }
    if (arg == "v7.0") {
        aom_ctc_v7_0(settings, app);
        return;
    }

    usage(app, "bad aom_ctc version \"%s\"", optarg);
}

void nflx_ctc_v1_0(CLISettings *const settings, const char *const app)
{
    const CLIModelConfig cfg = {
        .path = nullptr,
        .version = "vmaf_4k_v0.6.1",
        .cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT},
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = nullptr,
        .is_default = false,
    };
    checked_append(settings->model_config, settings->model_cnt, cfg, app, "models");

    const CLIModelConfig cfg_neg = {
        .path = nullptr,
        .version = "vmaf_4k_v0.6.1neg",
        .cfg = {.name = "vmaf_neg", .flags = VMAF_MODEL_FLAGS_DEFAULT},
        .feature_overload = {},
        .overload_cnt = 0,
        .buf = nullptr,
        .is_default = false,
    };
    checked_append(settings->model_config, settings->model_cnt, cfg_neg, app, "models");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("psnr=enable_chroma=true:enable_apsnr=true", app), app,
                   "features");

    checked_append(settings->feature_cfg, settings->feature_cnt,
                   parse_feature_config("float_ssim=enable_db=true:clip_db=true", app), app,
                   "features");

    checked_append(settings->feature_cfg, settings->feature_cnt, parse_feature_config("cambi", app),
                   app, "features");
}

void parse_nflx_ctc(CLISettings *const settings, const char *const optarg, const char *const app)
{
    if (std::string_view{optarg} == "v1.0") {
        nflx_ctc_v1_0(settings, app);
        return;
    }
    usage(app, "bad nflx_ctc version \"%s\"", optarg);
}

void handle_video_input_flag(const int o, const char *const optarg, const char *const app,
                             CLISettings *const settings)
{
    switch (o) {
    case 'r':
        settings->path_ref = const_cast<char *>(optarg);
        break;
    case 'd':
        settings->path_dist = const_cast<char *>(optarg);
        break;
    case 'w':
        settings->width = parse_unsigned(optarg, 'w', app);
        settings->use_yuv = true;
        break;
    case 'h':
        settings->height = parse_unsigned(optarg, 'h', app);
        settings->use_yuv = true;
        break;
    case 'p':
        parse_pixel_format(optarg, 'p', app, settings);
        settings->use_yuv = true;
        break;
    case 'b':
        settings->bitdepth = parse_bitdepth(optarg, 'b', app);
        settings->use_yuv = true;
        break;
    case 'o':
        settings->output_path = const_cast<char *>(optarg);
        break;
    default:
        break;
    }
}

void handle_threads_flag(const char *const optarg, const char *const app,
                         CLISettings *const settings)
{
    settings->thread_cnt = parse_unsigned(optarg, ARG_THREADS, app);
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const unsigned hw_threads = static_cast<unsigned>(si.dwNumberOfProcessors);
#else
    const long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    const unsigned hw_threads = (nproc > 0) ? static_cast<unsigned>(nproc) : 0u;
#endif
    if (hw_threads > 0u && settings->thread_cnt > hw_threads) {
        (void)std::fprintf(stderr, "warning: --threads %u capped to %u (hardware cores)\n",
                           settings->thread_cnt, hw_threads);
        settings->thread_cnt = hw_threads;
    }
}

void handle_frame_flag(const int o, const char *const optarg, const char *const app,
                       CLISettings *const settings)
{
    switch (o) {
    case ARG_FRAME_CNT:
        settings->frame_cnt = parse_unsigned(optarg, ARG_FRAME_CNT, app);
        break;
    case ARG_FRAME_SKIP_REF:
        settings->frame_skip_ref = parse_unsigned(optarg, ARG_FRAME_SKIP_REF, app);
        break;
    case ARG_FRAME_SKIP_DIST:
        settings->frame_skip_dist = parse_unsigned(optarg, ARG_FRAME_SKIP_DIST, app);
        break;
    default:
        break;
    }
}

void handle_tiny_flag(const int o, const char *const optarg, const char *const app,
                      CLISettings *const settings)
{
    switch (o) {
    case ARG_TINY_MODEL:
        settings->tiny_model_path = optarg;
        break;
    case ARG_TINY_DEVICE:
    case ARG_DNN_EP: {
        using sv = std::string_view;
        const sv dev{optarg};
        if (dev != "auto" && dev != "cpu" && dev != "cuda" && dev != "openvino" &&
            dev != "openvino-npu" && dev != "openvino-cpu" && dev != "openvino-gpu" &&
            dev != "coreml" && dev != "coreml-ane" && dev != "coreml-gpu" && dev != "coreml-cpu" &&
            dev != "rocm") {
            error(app, optarg, o == ARG_DNN_EP ? ARG_DNN_EP : ARG_TINY_DEVICE,
                  "one of auto|cpu|cuda|openvino|openvino-npu|openvino-cpu|"
                  "openvino-gpu|coreml|coreml-ane|coreml-gpu|coreml-cpu|rocm");
        }
        settings->tiny_device = optarg;
        break;
    }
    case ARG_TINY_THREADS:
        settings->tiny_threads = static_cast<int>(parse_unsigned(optarg, ARG_TINY_THREADS, app));
        break;
    case ARG_TINY_FP16:
        settings->tiny_fp16 = true;
        break;
    case ARG_TINY_MODEL_VERIFY:
        settings->tiny_model_verify = true;
        break;
    case ARG_TINY_CODEC:
        settings->tiny_codec = optarg;
        break;
    case ARG_TINY_PRESET:
        settings->tiny_preset = optarg;
        break;
    case ARG_TINY_CRF: {
        const unsigned long crf = parse_unsigned(optarg, ARG_TINY_CRF, app);
        settings->tiny_crf = static_cast<int>(crf > 63u ? 63u : crf);
        break;
    }
    case ARG_NO_REFERENCE:
        settings->no_reference = true;
        break;
    case ARG_TINY_RESIZE: {
        using sv = std::string_view;
        const sv rsz{optarg};
        if (rsz != "bilinear" && rsz != "nearest" && rsz != "bicubic" && rsz != "disabled") {
            error(app, optarg, ARG_TINY_RESIZE,
                  "--tiny-resize must be one of: bilinear, nearest, bicubic, disabled");
        }
        settings->tiny_resize = optarg;
        break;
    }
    default:
        break;
    }
}

void handle_backend_device_flag(const int o, const char *const optarg, const char *const app,
                                CLISettings *const settings)
{
    switch (o) {
    case ARG_NO_CUDA:
        settings->no_cuda = true;
        break;
    case ARG_NO_SYCL:
        settings->no_sycl = true;
        break;
    case ARG_SYCL_DEVICE:
        settings->sycl_device = static_cast<int>(parse_unsigned(optarg, ARG_SYCL_DEVICE, app));
        break;
    case ARG_NO_HIP:
        settings->no_hip = true;
        break;
    case ARG_HIP_DEVICE:
        settings->hip_device = static_cast<int>(parse_unsigned(optarg, ARG_HIP_DEVICE, app));
        break;
    case ARG_NO_METAL:
        settings->no_metal = true;
        break;
    case ARG_METAL_DEVICE:
        settings->metal_device = static_cast<int>(parse_unsigned(optarg, ARG_METAL_DEVICE, app));
        break;
    case ARG_BACKEND:
        settings->backend = optarg;
        break;
    default:
        break;
    }
}

void apply_backend_settings(const char *const app, CLISettings *const settings)
{
    if (settings->backend) {
        using sv = std::string_view;
        const sv be{settings->backend};
        if (be == "auto") {
            if (!settings->no_cuda && !settings->use_gpumask) {
                settings->gpumask = 0;
                settings->use_gpumask = true;
            }
        } else if (be == "cpu") {
            settings->no_cuda = true;
            settings->no_sycl = true;
            settings->no_hip = true;
            settings->no_metal = true;
        } else if (be == "cuda") {
            settings->no_sycl = true;
            settings->no_hip = true;
            settings->no_metal = true;
            if (!settings->use_gpumask) {
                settings->gpumask = 0;
                settings->use_gpumask = true;
            }
        } else if (be == "sycl") {
            settings->no_cuda = true;
            settings->no_hip = true;
            settings->no_metal = true;
            if (settings->sycl_device < 0)
                settings->sycl_device = 0;
        } else if (be == "hip") {
            settings->no_cuda = true;
            settings->no_sycl = true;
            settings->no_metal = true;
            if (settings->hip_device < 0)
                settings->hip_device = 0;
        } else if (be == "metal") {
            settings->no_cuda = true;
            settings->no_sycl = true;
            settings->no_hip = true;
            if (settings->metal_device < 0)
                settings->metal_device = 0;
        } else {
            usage(app,
                  "Unknown --backend value '%s' "
                  "(expected: auto|cpu|cuda|sycl|hip|metal)",
                  settings->backend);
        }
    } else {
        if (!settings->no_cuda && !settings->use_gpumask) {
            settings->gpumask = 0;
            settings->use_gpumask = true;
        }
    }
}

/* A layout of the import table holds the bit depths of its row. */
static void validate_layout_depth(const char *const app, const CLISettings *const settings)
{
    for (const VmafxImportLayout &row : vmafx_import_layouts) {
        if (row.pix_fmt != settings->input_layout)
            continue;
        if (settings->bitdepth >= row.bpc_min && settings->bitdepth <= row.bpc_max)
            continue;
        char range[16];
        (void)snprintf(range, sizeof range, "%u to %u", row.bpc_min, row.bpc_max);
        usage(app, "--pixel_format %s holds %s bits per component, not --bitdepth %u", row.name,
              static_cast<const char *>(range), settings->bitdepth);
    }
}

/* An RGB layout is converted to Y'CbCr with a matrix, range and transfer the user states
 * (ADR-2146): each one missing is refused by its flag, a matrix or transfer the integer
 * conversion does not make is refused by name. Nothing is assumed. */
static void validate_rgb_statement(const char *const app, const CLISettings *const settings)
{
    for (const VmafxImportLayout &row : vmafx_import_layouts) {
        if (row.pix_fmt != settings->input_layout || !row.needs_statement)
            continue;
        const char *const need = "%s is converted to Y'CbCr with a matrix, range and transfer you "
                                 "state; none is assumed: --%s is missing";
        if (!settings->rgb_matrix)
            usage(app, need, row.name, "rgb_matrix (bt601, bt709 or bt2020ncl)");
        if (!settings->rgb_range)
            usage(app, need, row.name, "rgb_range (limited or full)");
        if (!settings->rgb_transfer)
            usage(app, need, row.name, "rgb_transfer (bt709, srgb, pq or hlg)");
        if (!settings->rgb_out_range)
            usage(app, need, row.name, "rgb_out_range (limited or full)");
        if (settings->rgb_matrix == VMAF_COLOR_MATRIX_BT2020_CL ||
            settings->rgb_matrix == VMAF_COLOR_MATRIX_ICTCP)
            usage(app, "--rgb_matrix: the integer conversion covers bt601, bt709 and bt2020ncl; "
                       "bt2020cl and ictcp need the transfer function in the conversion");
        if (settings->rgb_transfer == VMAF_COLOR_TRC_LINEAR)
            usage(app, "--rgb_transfer linear: the matrix applies to non-linear R'G'B'; encode "
                       "the samples first");
    }
}

static void validate_yuv_settings(const char *const app, const CLISettings *const settings)
{
    if (!settings->use_yuv) {
        return;
    }
    if (settings->width == 0 && (settings->height || settings->pix_fmt || settings->bitdepth)) {
        usage(app, "--width must be > 0");
    }
    if (settings->height == 0 && (settings->width || settings->pix_fmt || settings->bitdepth)) {
        usage(app, "--height must be > 0");
    }
    if (!(settings->width && settings->height && settings->pix_fmt && settings->bitdepth)) {
        usage(app, "The following options are required for .yuv input:\n"
                   "  --width/-w\n"
                   "  --height/-h\n"
                   "  --pixel_format/-p\n"
                   "  --bitdepth/-b (implied by most layouts)\n");
    }
    validate_layout_depth(app, settings);
    validate_rgb_statement(app, settings);
}

void validate_cli_settings(const char *const app, CLISettings *const settings)
{
    if (settings->no_reference) {
        if (!settings->tiny_model_path) {
            usage(app, "--no-reference requires --tiny-model; no classic NR scorer exists");
        }
        settings->no_prediction = true;
    } else if (!settings->path_ref) {
        usage(app, "Reference .y4m or .yuv (-r/--reference) is required");
    }
    if (!settings->path_dist)
        usage(app, "Distorted .y4m or .yuv (-d/--distorted) is required");

    validate_yuv_settings(app, settings);

    if (settings->model_cnt == 0 && !settings->no_prediction) {
#if VMAF_BUILT_IN_MODELS
        const CLIModelConfig cfg = {
            .path = nullptr,
            .version =
                settings->netflix_compat ?
                    VMAF_NETFLIX_COMPAT_MODEL_VERSION :
                    VMAF_DEFAULT_MODEL_VERSION, /* vmaf-model-pin: Netflix upstream compat restores v0.6.1 default model */
            .cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT},
            .feature_overload = {},
            .overload_cnt = 0,
            .buf = nullptr,
            .is_default = true,
        };
        checked_append(settings->model_config, settings->model_cnt, cfg, app, "models");
#else
        usage(app, "At least one model (-m/--model) is required "
                   "unless no prediction (-n/--no_prediction) is set");
#endif
    }

    for (unsigned i = 0; i < settings->model_cnt; i++) {
        for (unsigned j = 0; j < settings->model_cnt; j++) {
            if (i == j)
                continue;
            if (!strcmp(settings->model_config[i].cfg.name, settings->model_config[j].cfg.name)) {
                usage(app, "Each model should be uniquely named. "
                           "Set using `--model` via the `name=...` param.");
            }
        }
    }
}

void handle_output_flag(const int o, CLISettings *const settings)
{
    switch (o) {
    case ARG_OUTPUT_XML:
        settings->output_fmt = VMAF_OUTPUT_FORMAT_XML;
        break;
    case ARG_OUTPUT_JSON:
        settings->output_fmt = VMAF_OUTPUT_FORMAT_JSON;
        break;
    case ARG_OUTPUT_CSV:
        settings->output_fmt = VMAF_OUTPUT_FORMAT_CSV;
        break;
    case ARG_OUTPUT_SUB:
        settings->output_fmt = VMAF_OUTPUT_FORMAT_SUB;
        break;
    default:
        break;
    }
}

void handle_feature_model_flag(const int o, const char *const optarg, const char *const app,
                               CLISettings *const settings)
{
    switch (o) {
    case 'm':
        checked_append(settings->model_config, settings->model_cnt, parse_model_config(optarg, app),
                       app, "models");
        break;
    case ARG_FEATURE:
        checked_append(settings->feature_cfg, settings->feature_cnt,
                       parse_feature_config(optarg, app), app, "features");
        break;
    case ARG_THREADS:
        handle_threads_flag(optarg, app, settings);
        break;
    case ARG_SUBSAMPLE:
        settings->subsample = parse_unsigned(optarg, ARG_SUBSAMPLE, app);
        break;
    case 'c':
    case ARG_CPUMASK:
        settings->cpumask = parse_unsigned(optarg, ARG_CPUMASK, app);
        break;
    case ARG_GPUMASK:
        settings->gpumask = parse_unsigned(optarg, ARG_GPUMASK, app);
        settings->use_gpumask = true;
        break;
    case ARG_AOM_CTC:
        parse_aom_ctc(settings, optarg, app);
        break;
    case ARG_NFLX_CTC:
        parse_nflx_ctc(settings, optarg, app);
        break;
    default:
        break;
    }
}

void handle_misc_flag(const int o, const char *const app, CLISettings *const settings)
{
    switch (o) {
    case ARG_HELP:
        usage(app, nullptr);
        break;
    case 'n':
        settings->no_prediction = true;
        break;
    case 'q':
        settings->quiet = true;
        break;
    case ARG_NETFLIX_COMPAT:
        settings->netflix_compat = true;
        break;
    case ARG_PROVENANCE_SIDECAR:
        settings->provenance_sidecar = true;
        break;
    case 'v':
        if (settings->vmafx_mode) {
            (void)fprintf(stderr, "VMAFX %s (auto-backend, precision=max)\n", vmaf_version());
        } else {
            (void)fprintf(stderr, "%s\n", vmaf_version());
        }
        // NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-1155: CLI version exit
        exit(0);
    default:
        break;
    }
}

bool handle_primary_cli_opt(const int o, const char *const optarg, const char *const app,
                            CLISettings *const settings)
{
    switch (o) {
    case 'r':
    case 'd':
    case 'w':
    case 'h':
    case 'p':
    case 'b':
    case 'o':
        handle_video_input_flag(o, optarg, app, settings);
        return true;
    case ARG_OUTPUT_XML:
    case ARG_OUTPUT_JSON:
    case ARG_OUTPUT_CSV:
    case ARG_OUTPUT_SUB:
        handle_output_flag(o, settings);
        return true;
    case 'm':
    case ARG_FEATURE:
    case ARG_THREADS:
    case ARG_SUBSAMPLE:
    case 'c':
    case ARG_CPUMASK:
    case ARG_GPUMASK:
    case ARG_AOM_CTC:
    case ARG_NFLX_CTC:
        handle_feature_model_flag(o, optarg, app, settings);
        return true;
    default:
        return false;
    }
}

void process_single_cli_opt(const int o, const char *const optarg, const char *const app,
                            CLISettings *const settings)
{
    if (handle_primary_cli_opt(o, optarg, app, settings))
        return;

    switch (o) {
    case ARG_FRAME_CNT:
    case ARG_FRAME_SKIP_REF:
    case ARG_FRAME_SKIP_DIST:
        handle_frame_flag(o, optarg, app, settings);
        break;
    case ARG_NO_CUDA:
    case ARG_NO_SYCL:
    case ARG_SYCL_DEVICE:
    case ARG_NO_HIP:
    case ARG_HIP_DEVICE:
    case ARG_NO_METAL:
    case ARG_METAL_DEVICE:
    case ARG_BACKEND:
        handle_backend_device_flag(o, optarg, app, settings);
        break;
    case ARG_PRECISION:
        settings->precision_fmt = resolve_precision_fmt(optarg, app, settings);
        break;
    case ARG_TINY_MODEL:
    case ARG_TINY_DEVICE:
    case ARG_DNN_EP:
    case ARG_TINY_THREADS:
    case ARG_TINY_FP16:
    case ARG_TINY_MODEL_VERIFY:
    case ARG_TINY_CODEC:
    case ARG_TINY_PRESET:
    case ARG_TINY_CRF:
    case ARG_NO_REFERENCE:
    case ARG_TINY_RESIZE:
        handle_tiny_flag(o, optarg, app, settings);
        break;
    case ARG_RGB_MATRIX:
    case ARG_RGB_RANGE:
    case ARG_RGB_TRANSFER:
    case ARG_RGB_OUT_RANGE:
        handle_rgb_flag(o, optarg, app, settings);
        break;
    case ARG_HELP:
    case 'n':
    case 'q':
    case 'v':
    case ARG_NETFLIX_COMPAT:
    case ARG_PROVENANCE_SIDECAR:
        handle_misc_flag(o, app, settings);
        break;
    case ARG_VERIFY_PROVENANCE:
        settings->verify_provenance = optarg;
        break;
    default:
        break;
    }
}

} // namespace

extern "C" bool detect_vmafx_mode(const char *const argv0)
{
    if (!argv0)
        return false;
    using sv = std::string_view;
    const sv s{argv0};
    const auto slash = s.find_last_of("/\\");
    const sv base = (slash != sv::npos) ? s.substr(slash + 1) : s;
    return (base == "vmafx" || base == "vmafx.exe");
}

void cli_parse(const int argc, char *const *const argv, CLISettings *const settings)
{
    (void)memset(settings, 0, sizeof(*settings));
    settings->vmafx_mode = detect_vmafx_mode(argv[0]);
    settings->sycl_device = -1;  // auto-select by default
    settings->hip_device = -1;   // auto-select by default
    settings->metal_device = -1; // auto-select by default
    settings->precision_n = -1;
    settings->precision_fmt = VMAF_DEFAULT_PRECISION_FMT;
    settings->tiny_device = "auto";
    settings->tiny_crf = -1; /* ADR-0522: -1 = unset; 0..63 user-supplied */
    int o = 0;

    // NOLINTNEXTLINE(concurrency-mt-unsafe) — ADR-1155: single-threaded CLI entry point before worker threads spawn
    while ((o = getopt_long(argc, argv, short_opts, long_opts, nullptr)) >= 0) {
        process_single_cli_opt(o, optarg, argv[0], settings);
    }

    if (settings->vmafx_mode) {
        /* ADR-0690: apply modernized defaults (precision=max) unless explicit --precision given */
        if (!settings->precision_max && !settings->precision_legacy &&
            (settings->precision_n == -1)) {
            settings->precision_max = true;
            settings->precision_fmt = VMAF_LOSSLESS_PRECISION_FMT;
        }
    }

    if (settings->netflix_compat) {
        /* ADR-0696: Final post-parse pass overriding any modernizations back to legacy defaults */
        settings->backend = "cpu";
        settings->no_cuda = true;
        settings->no_sycl = true;
        settings->no_hip = true;
        settings->no_metal = true;
        settings->precision_max = false;
        settings->precision_legacy = true;
        settings->precision_n = -1;
        settings->precision_fmt = VMAF_DEFAULT_PRECISION_FMT;
    }

    if (!settings->output_fmt)
        settings->output_fmt = VMAF_OUTPUT_FORMAT_XML;

    /* RC4 WP5: a verification run takes its configuration from the report. */
    if (settings->verify_provenance)
        return;
    apply_backend_settings(argv[0], settings);
    validate_cli_settings(argv[0], settings);
}

const char *cli_verify_provenance_arg(const int argc, char *const *const argv)
{
    static constexpr std::string_view option = "--verify-provenance";
    for (int i = 1; i < argc; i++) {
        const std::string_view arg{argv[i]};
        if (arg == option)
            return i + 1 < argc ? argv[i + 1] : nullptr;
        if (arg.starts_with(option) && arg.size() > option.size() && arg[option.size()] == '=')
            return argv[i] + option.size() + 1;
    }
    return nullptr;
}

void cli_parse_reset(void)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    optreset = 1;
#endif
    optind = 1;
}

/* Release what the settings still own. An option dictionary handed to
 * libvmaf was cleared from the settings at the hand-off (vmaf.cpp), so every
 * dictionary left here is one no libvmaf call took: the run stopped before
 * registering its feature or overloading its model. The frees cannot fail
 * (their argument is never NULL) and clear each pointer. */
void cli_free(CLISettings *const settings)
{
    for (unsigned i = 0; i < settings->model_cnt; i++) {
        CLIModelConfig &model = settings->model_config[i];
        for (unsigned j = 0; j < model.overload_cnt; j++)
            (void)vmaf_feature_dictionary_free(&model.feature_overload[j].opts_dict);
        free(model.buf);
        model.buf = nullptr;
    }
    for (unsigned i = 0; i < settings->feature_cnt; i++) {
        CLIFeatureConfig &feature = settings->feature_cfg[i];
        (void)vmaf_feature_dictionary_free(&feature.opts_dict);
        free(feature.buf);
        feature.buf = nullptr;
    }
}
