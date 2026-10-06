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
    {"reference", 1, 'r'},
    {"distorted", 1, 'd'},
    {"width", 1, 'w'},
    {"height", 1, 'h'},
    {"pixel_format", 1, 'p'},
    {"bitdepth", 1, 'b'},
    {"model", 1, 'm'},
    {"output", 1, 'o'},
    {"xml", 0, '\0'},
    {"json", 0, '\0'},
    {"csv", 0, '\0'},
    {"sub", 0, '\0'},
    {"help", 0, '\0'},
    {"threads", 1, '\0'},
    {"feature", 1, '\0'},
    {"subsample", 1, '\0'},
    {"cpumask", 1, '\0'},
    {"gpumask", 1, '\0'},
    {"aom_ctc", 1, '\0'},
    {"nflx_ctc", 1, '\0'},
    {"frame_cnt", 1, '\0'},
    {"frame_skip_ref", 1, '\0'},
    {"frame_skip_dist", 1, '\0'},
    {"no_cuda", 0, '\0'},
    {"no_sycl", 0, '\0'},
    {"sycl_device", 1, '\0'},
    {"no_hip", 0, '\0'},
    {"hip_device", 1, '\0'},
    {"no_metal", 0, '\0'},
    {"metal_device", 1, '\0'},
    {"backend", 1, '\0'},
    {"precision", 1, '\0'},
    {"tiny-model", 1, '\0'},
    {"tiny_model", 1, '\0'},
    {"tiny-device", 1, '\0'},
    {"tiny_device", 1, '\0'},
    {"tiny-threads", 1, '\0'},
    {"tiny_threads", 1, '\0'},
    {"tiny-fp16", 0, '\0'},
    {"tiny_fp16", 0, '\0'},
    {"tiny-model-verify", 0, '\0'},
    {"tiny_model_verify", 0, '\0'},
    {"tiny-codec", 1, '\0'},
    {"tiny_codec", 1, '\0'},
    {"tiny-preset", 1, '\0'},
    {"tiny_preset", 1, '\0'},
    {"tiny-crf", 1, '\0'},
    {"tiny_crf", 1, '\0'},
    {"no-reference", 0, '\0'},
    {"no_reference", 0, '\0'},
    {"tiny-resize", 1, '\0'},
    {"tiny_resize", 1, '\0'},
    {"dnn-ep", 1, '\0'},
    {"dnn_ep", 1, '\0'},
    {"no_prediction", 0, 'n'},
    {"netflix-compat", 0, '\0'},
    {"netflix_compat", 0, '\0'},
    {"version", 0, 'v'},
    {"quiet", 0, 'q'},
};

/* The hand table's short option string. */
constexpr char old_short_opts[] = "r:d:w:h:p:b:m:c:o:nvq";

constexpr std::size_t n_long = sizeof(generated::long_opts) / sizeof(generated::long_opts[0]);

const struct option *find_long(const char *name)
{
    for (std::size_t i = 0; i < n_long; i++) {
        const char *const candidate = generated::long_opts[i].name;
        if (candidate != nullptr && std::strcmp(candidate, name) == 0) {
            return &generated::long_opts[i];
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
    for (std::size_t i = 0; i < n_long; i++) {
        const char *const name = generated::long_opts[i].name;
        if (name != nullptr && find_long(name) != &generated::long_opts[i]) {
            (void)fprintf(stderr, "--%s appears twice\n", name);
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
