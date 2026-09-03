/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Implementation of the Windows MSVC getopt / getopt_long shim declared in
 *  core/tools/compat/win32/getopt.h. See the header for scope and known
 *  divergences from GNU/POSIX behaviour.
 *
 *  Algorithm sketch:
 *    - `optind` starts at 1 and advances through argv.
 *    - Short options: walk the optstring. Trailing ':' means
 *      required_argument, trailing '::' means optional_argument.
 *    - Long options: token is "--name" or "--name=val". Match against the
 *      longopts[] table; falls back to a prefix match only when unique.
 *    - Non-option arguments are permuted to the tail of argv so that on
 *      end-of-options (`-1`) the caller sees options first, then positional
 *      arguments in their original relative order.
 */

#ifdef _WIN32

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "getopt.h"

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

/* Scan cursor inside the current argv element when it is a cluster of short
 * options (e.g. "-abc"). Reset to NULL between argv elements. */
static const char *short_cursor = NULL;

static void report_unknown(const char *prog, int opt, int is_long, const char *name)
{
    if (!opterr)
        return;
    if (is_long) {
        (void)fprintf(stderr, "%s: unrecognized option '--%s'\n", prog ? prog : "getopt",
                      name ? name : "");
    } else {
        (void)fprintf(stderr, "%s: invalid option -- '%c'\n", prog ? prog : "getopt", opt);
    }
}

static void report_missing_arg(const char *prog, int opt, int is_long, const char *name)
{
    if (!opterr)
        return;
    if (is_long) {
        (void)fprintf(stderr, "%s: option '--%s' requires an argument\n", prog ? prog : "getopt",
                      name ? name : "");
    } else {
        (void)fprintf(stderr, "%s: option requires an argument -- '%c'\n", prog ? prog : "getopt",
                      opt);
    }
}

/* Move argv[src] to argv[dst], shifting the intermediate slots right.
 * Used to permute non-option args to the tail. `argv` is `char *const argv[]`
 * per POSIX, but in practice callers pass a writable array; we cast away
 * const for the shuffle — POSIX explicitly permits this in getopt. */
static void argv_shift(char **argv, int src, int dst)
{
    if (src == dst)
        return;
    char *save = argv[src];
    if (src > dst) {
        for (int i = src; i > dst; i--)
            argv[i] = argv[i - 1];
    } else {
        for (int i = src; i < dst; i++)
            argv[i] = argv[i + 1];
    }
    argv[dst] = save;
}

static int handle_short(int argc, char *const argv[], const char *optstring, char **argv_w)
{
    const char *prog = argv[0];
    char opt = *short_cursor++;
    optopt = (unsigned char)opt;

    const char *spec = strchr(optstring, opt);
    if (!spec || opt == ':') {
        report_unknown(prog, opt, 0, NULL);
        if (*short_cursor == '\0') {
            short_cursor = NULL;
            optind++;
        }
        return '?';
    }

    int has_arg = no_argument;
    if (spec[1] == ':') {
        has_arg = (spec[2] == ':') ? optional_argument : required_argument;
    }

    if (has_arg == no_argument) {
        if (*short_cursor == '\0') {
            short_cursor = NULL;
            optind++;
        }
        return (unsigned char)opt;
    }

    /* has_arg != no_argument: argument is the rest of the current argv
     * element if non-empty, else the next argv element. */
    if (*short_cursor != '\0') {
        optarg = (char *)short_cursor;
        short_cursor = NULL;
        optind++;
        return (unsigned char)opt;
    }

    short_cursor = NULL;
    optind++;
    if (has_arg == optional_argument) {
        optarg = NULL;
        return (unsigned char)opt;
    }
    /* required_argument */
    if (optind >= argc) {
        report_missing_arg(prog, opt, 0, NULL);
        return (optstring[0] == ':') ? ':' : '?';
    }
    optarg = argv_w[optind++];
    return (unsigned char)opt;
}

static int handle_long(int argc, char *const argv[], const char *optstring,
                       const struct option *longopts, int *longindex, char **argv_w)
{
    const char *prog = argv[0];
    const char *arg = argv[optind] + 2u; /* skip leading "--" */
    const char *eq = strchr(arg, '=');
    size_t nlen = eq ? (size_t)(eq - arg) : strlen(arg);

    /* Exact match first; fall through to a unique-prefix match. */
    int match = -1;
    int ambig = 0;
    for (int i = 0; longopts[i].name; i++) {
        if (strncmp(longopts[i].name, arg, nlen) == 0) {
            if (strlen(longopts[i].name) == nlen) {
                match = i;
                ambig = 0;
                break;
            }
            if (match < 0)
                match = i;
            else
                ambig = 1;
        }
    }

    if (match < 0 || ambig) {
        if (opterr) {
            (void)fprintf(stderr, "%s: %s option '--%.*s'\n", prog ? prog : "getopt",
                          ambig ? "ambiguous" : "unrecognized", (int)nlen, arg);
        }
        optopt = 0;
        optind++;
        return '?';
    }

    const struct option *lo = &longopts[match];
    if (longindex)
        *longindex = match;

    if (lo->has_arg == no_argument) {
        if (eq) {
            if (opterr) {
                (void)fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                              prog ? prog : "getopt", lo->name);
            }
            optind++;
            optopt = lo->val;
            return '?';
        }
        optind++;
    } else if (eq) {
        optarg = (char *)(eq + 1);
        optind++;
    } else if (lo->has_arg == optional_argument) {
        optarg = NULL;
        optind++;
    } else { /* required_argument */
        optind++;
        if (optind >= argc) {
            report_missing_arg(prog, 0, 1, lo->name);
            optopt = lo->val;
            return (optstring && optstring[0] == ':') ? ':' : '?';
        }
        optarg = argv_w[optind++];
    }

    if (lo->flag) {
        *lo->flag = lo->val;
        return 0;
    }
    return lo->val;
}

static int getopt_internal(int argc, char *const argv[], const char *optstring,
                           const struct option *longopts, int *longindex)
{
    char **argv_w = (char **)(void *)argv; /* see comment on argv_shift */
    optarg = NULL;

    /* Resume scanning inside a short-option cluster from the prior call. */
    if (short_cursor && *short_cursor != '\0') {
        return handle_short(argc, argv, optstring, argv_w);
    }
    short_cursor = NULL;

    while (optind < argc) {
        const char *cur = argv[optind];
        /* Non-option: permute to the tail and continue. */
        if (cur[0] != '-' || cur[1] == '\0') {
            int non_opt = optind;
            /* Find the next option (or end). */
            int next_opt = non_opt + 1;
            while (next_opt < argc && (argv[next_opt][0] != '-' || argv[next_opt][1] == '\0')) {
                next_opt++;
            }
            if (next_opt >= argc)
                return -1; /* only non-options remain */
            /* Shift the non-option run past the options we've already
             * consumed by moving non_opt..next_opt-1 to the tail. */
            int run_len = next_opt - non_opt;
            for (int k = 0; k < run_len; k++) {
                argv_shift(argv_w, non_opt, argc - 1);
            }
            /* After the shift, optind still points at what is now the next
             * option token — don't advance optind. */
            continue;
        }
        /* "--" — explicit end of options; skip it and stop. */
        if (cur[1] == '-' && cur[2] == '\0') {
            optind++;
            return -1;
        }
        /* "--name" long option. */
        if (cur[1] == '-') {
            if (!longopts)
                return -1;
            return handle_long(argc, argv, optstring, longopts, longindex, argv_w);
        }
        /* Short option cluster "-abc". */
        short_cursor = cur + 1;
        return handle_short(argc, argv, optstring, argv_w);
    }
    return -1;
}

int getopt(int argc, char *const argv[], const char *optstring)
{
    return getopt_internal(argc, argv, optstring, NULL, NULL);
}

int getopt_long(int argc, char *const argv[], const char *optstring, const struct option *longopts,
                int *longindex)
{
    return getopt_internal(argc, argv, optstring, longopts, longindex);
}

#endif /* _WIN32 */
