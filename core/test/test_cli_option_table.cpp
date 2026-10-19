/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Every command-line spelling the hand-written option table of
 * core/tools/cli_parse.cpp accepted before the table was generated from the
 * option groups of core/api/vmafx.toml (RC4 WP8, ADR-1852) is still accepted,
 * with the same argument requirement and, for options with a short letter, the
 * same letter (HISS-14: spellings are only added, never removed).
 *
 * The list below is frozen: it is the `long_opts[]` table and the short option
 * string as they were on 027aebc56, before the change. The test includes the
 * generated table itself (the file cli_parse.cpp compiles), so a spelling the
 * definition drops, a switch that starts to take a value, or a short letter
 * that moves fails here, and so does a long spelling that no longer reaches
 * the short letter's handler.
 *
 * Failing first: removing `--tiny_model` from the definition fails
 * test_every_old_long_spelling_is_accepted; making `--no_cuda` take an
 * argument fails the has_arg comparison; dropping `cli_short = "c"` fails
 * test_every_old_short_option_is_accepted.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include "compat/win32/getopt.h"
#else
#include <getopt.h>
#endif

extern "C" {
#include "test.h"
}

namespace generated
{
#include "cli_options.gen.inc"
} // namespace generated

namespace
{

struct OldOption {
    const char *name;
    int has_arg;
    char short_letter; /* '\0': the long spelling had a long-only identifier */
};

/* The 59 long spellings of the hand table (027aebc56). */
constexpr OldOption old_options[] = {
    {.name = "reference", .has_arg = 1, .short_letter = 'r'},
    {.name = "distorted", .has_arg = 1, .short_letter = 'd'},
    {.name = "width", .has_arg = 1, .short_letter = 'w'},
    {.name = "height", .has_arg = 1, .short_letter = 'h'},
    {.name = "pixel_format", .has_arg = 1, .short_letter = 'p'},
    {.name = "bitdepth", .has_arg = 1, .short_letter = 'b'},
    {.name = "model", .has_arg = 1, .short_letter = 'm'},
    {.name = "output", .has_arg = 1, .short_letter = 'o'},
    {.name = "xml", .has_arg = 0, .short_letter = '\0'},
    {.name = "json", .has_arg = 0, .short_letter = '\0'},
    {.name = "csv", .has_arg = 0, .short_letter = '\0'},
    {.name = "sub", .has_arg = 0, .short_letter = '\0'},
    {.name = "help", .has_arg = 0, .short_letter = '\0'},
    {.name = "threads", .has_arg = 1, .short_letter = '\0'},
    {.name = "feature", .has_arg = 1, .short_letter = '\0'},
    {.name = "subsample", .has_arg = 1, .short_letter = '\0'},
    {.name = "cpumask", .has_arg = 1, .short_letter = '\0'},
    {.name = "gpumask", .has_arg = 1, .short_letter = '\0'},
    {.name = "aom_ctc", .has_arg = 1, .short_letter = '\0'},
    {.name = "nflx_ctc", .has_arg = 1, .short_letter = '\0'},
    {.name = "frame_cnt", .has_arg = 1, .short_letter = '\0'},
    {.name = "frame_skip_ref", .has_arg = 1, .short_letter = '\0'},
    {.name = "frame_skip_dist", .has_arg = 1, .short_letter = '\0'},
    {.name = "no_cuda", .has_arg = 0, .short_letter = '\0'},
    {.name = "no_sycl", .has_arg = 0, .short_letter = '\0'},
    {.name = "sycl_device", .has_arg = 1, .short_letter = '\0'},
    {.name = "no_hip", .has_arg = 0, .short_letter = '\0'},
    {.name = "hip_device", .has_arg = 1, .short_letter = '\0'},
    {.name = "no_metal", .has_arg = 0, .short_letter = '\0'},
    {.name = "metal_device", .has_arg = 1, .short_letter = '\0'},
    {.name = "backend", .has_arg = 1, .short_letter = '\0'},
    {.name = "precision", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny-model", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny_model", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny-device", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny_device", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny-threads", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny_threads", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny-fp16", .has_arg = 0, .short_letter = '\0'},
    {.name = "tiny_fp16", .has_arg = 0, .short_letter = '\0'},
    {.name = "tiny-model-verify", .has_arg = 0, .short_letter = '\0'},
    {.name = "tiny_model_verify", .has_arg = 0, .short_letter = '\0'},
    {.name = "tiny-codec", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny_codec", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny-preset", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny_preset", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny-crf", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny_crf", .has_arg = 1, .short_letter = '\0'},
    {.name = "no-reference", .has_arg = 0, .short_letter = '\0'},
    {.name = "no_reference", .has_arg = 0, .short_letter = '\0'},
    {.name = "tiny-resize", .has_arg = 1, .short_letter = '\0'},
    {.name = "tiny_resize", .has_arg = 1, .short_letter = '\0'},
    {.name = "dnn-ep", .has_arg = 1, .short_letter = '\0'},
    {.name = "dnn_ep", .has_arg = 1, .short_letter = '\0'},
    {.name = "no_prediction", .has_arg = 0, .short_letter = 'n'},
    {.name = "netflix-compat", .has_arg = 0, .short_letter = '\0'},
    {.name = "netflix_compat", .has_arg = 0, .short_letter = '\0'},
    {.name = "version", .has_arg = 0, .short_letter = 'v'},
    {.name = "quiet", .has_arg = 0, .short_letter = 'q'},
};

/* The hand table's short option string. */
constexpr char old_short_opts[] = "r:d:w:h:p:b:m:c:o:nvq";

constexpr std::size_t n_long = sizeof(generated::long_opts) / sizeof(generated::long_opts[0]);

const struct option *find_long(const char *name)
{
    for (const struct option &candidate : generated::long_opts) {
        if (candidate.name != nullptr && std::strcmp(candidate.name, name) == 0) {
            return &candidate;
        }
    }
    return nullptr;
}

/* The generated short string declares `letter`, with an argument iff
 * `has_arg`. */
bool short_declared(char letter, bool has_arg)
{
    const char *const at = std::strchr(generated::short_opts, letter);
    if (at == nullptr) {
        return false;
    }
    return (at[1] == ':') == has_arg;
}

mu_message_t test_every_old_long_spelling_is_accepted()
{
    for (const OldOption &old : old_options) {
        const struct option *const now = find_long(old.name);
        if (now == nullptr) {
            (void)fprintf(stderr, "--%s is no longer accepted\n", old.name);
            return "an old long spelling is missing from the generated table";
        }
        if (now->has_arg != old.has_arg) {
            (void)fprintf(stderr, "--%s: has_arg %d, was %d\n", old.name, now->has_arg,
                          old.has_arg);
            return "an old long spelling changed its argument requirement";
        }
        if (old.short_letter != '\0' && now->val != old.short_letter) {
            (void)fprintf(stderr, "--%s no longer reaches -%c\n", old.name, old.short_letter);
            return "an old long spelling no longer shares its short option";
        }
    }
    return nullptr;
}

mu_message_t test_every_old_short_option_is_accepted()
{
    for (std::size_t i = 0; old_short_opts[i] != '\0'; i++) {
        const char letter = old_short_opts[i];
        if (letter == ':') {
            continue;
        }
        const bool has_arg = old_short_opts[i + 1] == ':';
        if (!short_declared(letter, has_arg)) {
            (void)fprintf(stderr, "-%c is missing or changed its argument\n", letter);
            return "an old short option is missing from the generated short string";
        }
    }
    return nullptr;
}

mu_message_t test_long_spellings_are_unique()
{
    for (const struct option &entry : generated::long_opts) {
        if (entry.name != nullptr && find_long(entry.name) != &entry) {
            (void)fprintf(stderr, "--%s appears twice\n", entry.name);
            return "a long spelling appears twice in the generated table";
        }
    }
    mu_assert("the generated table ends with the getopt sentinel",
              generated::long_opts[n_long - 1].name == nullptr);
    return nullptr;
}

mu_message_t test_usage_lists_every_long_spelling()
{
    for (std::size_t i = 0; i + 1 < n_long; i++) {
        const char *const name = generated::long_opts[i].name;
        bool listed = false;
        for (const char *const line : generated::usage_lines) {
            const char *const at = std::strstr(line, name);
            listed = listed || (at != nullptr && at > line && at[-1] == '-');
        }
        if (!listed) {
            (void)fprintf(stderr, "--%s is not in the usage text\n", name);
            return "a long spelling is missing from the generated usage text";
        }
    }
    return nullptr;
}

} // namespace

mu_message_t run_tests()
{
    mu_run_test(test_every_old_long_spelling_is_accepted);
    mu_run_test(test_every_old_short_option_is_accepted);
    mu_run_test(test_long_spellings_are_unique);
    mu_run_test(test_usage_lists_every_long_spelling);
    return nullptr;
}
