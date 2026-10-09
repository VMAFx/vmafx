/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VENDORED FROM VMAFx/pelorus@e2e4040311a443210927549c3a336f909c6473f3 — DO NOT EDIT.
 * Append-only ABI; single
 * source of truth is pelorus. Re-sync via scripts/sync-pelorus-interop.sh.
 * See docs/adr/1113-vendor-pelorus-interop-abi.md.
 *
 * Local edit vs the pelorus original: the intra-pelorus #include below is
 * rewritten from "pelorus/interop.h" to "libvmaf/pelorus/interop.h" so it resolves
 * under core/include/. Nothing else is changed.
 */

/*
 * qp_report_csv.c — the runnable closed-loop encoder-stat reader (ADR-0122).
 *
 * Parses the per-frame statistics x265 emits with `--csv --csv-log-level 2`
 * (the HEVC software reference encoder) and folds them into a PEL_SEC_QPREPORT
 * section. This is the one closed-loop readback surface that runs end-to-end on
 * a box without a working HW encoder: the QSV per-block path (ADR-0119) needs
 * Intel HW that is low-power-bugged here, so it stays code-complete-unvalidated;
 * x265's CSV gives real, honest, post-encode honored-QP / bits / PSNR numbers.
 *
 * libpelorus stays SDK-free (vmafx vendors this verbatim): pure stdio + string
 * parsing, no libx265 / oneVPL link. Banned-function policy (AGENTS.md §3):
 * no atoi/atof/strtok/strcpy/sprintf — strtol/strtod + a hand-rolled
 * comma-field splitter that never writes past the line buffer.
 *
 * The CSV path is UTF-8 on every platform (ADR-0149). On Windows the narrow CRT
 * decodes a path through the process ANSI code page, so the one open goes
 * through open_utf8(), which widens to UTF-16 for _wfsopen; the only extra
 * dependency is kernel32 (MultiByteToWideChar), which every Windows link has.
 */

/* NOLINTBEGIN(modernize-use-nullptr): this C translation unit is built as C23
 * by vmafx, where clang-tidy also proposes the `nullptr` keyword, but MSVC's C
 * mode has no `nullptr` (C2065); the Windows builds compile it with cl.exe.
 * The NULL macro stays. Same decision as vmafx ADR-1138
 * (docs/adr/1138-c-translation-units-keep-null.md in VMAFx/vmafx). */

#include "libvmaf/pelorus/interop.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <share.h>
#include <wchar.h>
#include <windows.h>
#endif

/* ASCII-only character classifiers. The standard <ctype.h> macros index a
 * locale table by the byte value, which clang-analyzer (rightly, CERT STR37-C)
 * treats as a tainted-index hazard when the byte comes from file input. The CSV
 * grammar is pure ASCII, so locale-independent comparisons are both safer (no
 * table index at all) and more correct than the locale-sensitive <ctype.h>. */
static int ascii_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static char ascii_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

/* x265 CSV lines are wide (one row carries ~90 columns); 4 KiB is comfortably
 * above the longest observed row and bounds the stack/heap footprint (Po10). */
#define PEL_CSV_LINE_MAX 4096u
/* The header names ~90 columns; we only ever need a handful of indices. */
#define PEL_CSV_MAX_FIELDS 256u

/* Column indices located by name from the header row; -1 => not present. */
typedef struct csv_cols {
    int type;
    int poc;
    int qp;
    int bits;
    int psnr_y;
    int psnr_u;
    int psnr_v;
} csv_cols;

/* Trim leading/trailing ASCII whitespace in place; returns the trimmed start.
 * Does not allocate; mutates s by writing a NUL at the new end. */
static char *trim(char *s)
{
    char *end;

    while (*s != '\0' && ascii_is_space(*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    end = s + strlen(s) - 1;
    while (end > s && ascii_is_space(*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

/* Split line (mutated: commas overwritten by NUL) into trimmed field pointers.
 * Writes up to max_fields into fields[]; returns the field count.
 *
 * Loop bound (P10 r2): `line` is always a fgets-filled, NUL-terminated buffer of
 * <= PEL_CSV_LINE_MAX bytes, so the walk hits '\0' within PEL_CSV_LINE_MAX steps, the
 * explicit scalar bound of the loop; the `n >= max_fields` break bounds the field count
 * independently. */
static size_t split_fields(char *line, char **fields, size_t max_fields)
{
    size_t n = 0;
    char *cur = line;
    char *p = line;

    if (max_fields == 0) {
        return 0;
    }
    for (size_t step = 0; step <= (size_t)PEL_CSV_LINE_MAX; step++) {
        if (*p == ',' || *p == '\0') {
            int last = (*p == '\0');
            *p = '\0';
            if (n < max_fields) {
                fields[n] = trim(cur);
                n++;
            }
            if (last || n >= max_fields) {
                break;
            }
            cur = p + 1;
        }
        p++;
    }
    return n;
}

/* Case-insensitive whole-name match (header names are already trimmed). */
static int name_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (ascii_upper(*a) != ascii_upper(*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Locate the columns we consume by header name. x265's exact column set varies
 * with build flags, so we match by name, not fixed offset. */
static void locate_columns(char **hdr, size_t nh, csv_cols *c)
{
    size_t i;

    c->type = c->poc = c->qp = c->bits = -1;
    c->psnr_y = c->psnr_u = c->psnr_v = -1;
    for (i = 0; i < nh; i++) {
        const char *h = hdr[i];
        if (name_eq(h, "Type")) {
            c->type = (int)i;
        } else if (name_eq(h, "POC")) {
            c->poc = (int)i;
        } else if (name_eq(h, "QP")) {
            c->qp = (int)i;
        } else if (name_eq(h, "Bits")) {
            c->bits = (int)i;
        } else if (name_eq(h, "Y PSNR")) {
            c->psnr_y = (int)i;
        } else if (name_eq(h, "U PSNR")) {
            c->psnr_u = (int)i;
        } else if (name_eq(h, "V PSNR")) {
            c->psnr_v = (int)i;
        }
    }
}

/* Parse a float field; returns 0.0f on empty / unparseable (e.g. "-") or on a
 * non-finite value (a malformed CSV must never inject inf/NaN into the means). */
static float field_f(char **f, size_t nf, int idx)
{
    char *endp = NULL;
    double v;

    if (idx < 0 || (size_t)idx >= nf || f[idx][0] == '\0') {
        return 0.0f;
    }
    v = strtod(f[idx], &endp);
    if (endp == f[idx] || !isfinite(v)) {
        return 0.0f;
    }
    return (float)v;
}

/* Parse a signed long field; returns 0 on empty / unparseable. */
static long field_l(char **f, size_t nf, int idx)
{
    char *endp = NULL;
    long v;

    if (idx < 0 || (size_t)idx >= nf || f[idx][0] == '\0') {
        return 0;
    }
    errno = 0;
    v = strtol(f[idx], &endp, 10);
    if (endp == f[idx] || errno == ERANGE) {
        return 0; /* unparseable or out of range (CERT ERR30-C/INT30-C) */
    }
    return v;
}

/* Parse a non-negative count field into uint64_t; returns 0 on empty /
 * unparseable / negative. x265 bit counts are non-negative; a malformed
 * negative text must not wrap into a huge unsigned weight (CERT INT30-C). */
static uint64_t field_u64(char **f, size_t nf, int idx)
{
    char *endp = NULL;
    unsigned long long v;
    const char *s;

    if (idx < 0 || (size_t)idx >= nf || f[idx][0] == '\0') {
        return 0;
    }
    s = f[idx];
    if (s[0] == '-') {
        return 0; /* reject negative; strtoull would otherwise wrap it */
    }
    errno = 0;
    v = strtoull(s, &endp, 10);
    if (endp == s || errno == ERANGE) {
        return 0; /* unparseable or overflow — never inject a huge weight */
    }
    return (uint64_t)v;
}

/* True if a Type field names a coded picture row (I/P/B-SLICE...). The trailing
 * summary lines x265 appends ("Total frames...", aggregate rows) do not. */
static int is_frame_type(const char *t)
{
    char c0;

    if (t == NULL || t[0] == '\0') {
        return 0;
    }
    c0 = ascii_upper(t[0]);
    /* x265 writes "I-SLICE" / "P-SLICE" / "B-SLICE" / "b-SLICE"; the second
     * char is '-' for a coded picture, distinguishing it from word labels. */
    return (c0 == 'I' || c0 == 'P' || c0 == 'B') && t[1] == '-';
}

/* True if a split data row is a coded-frame row to admit. x265 appends a blank
 * line then a "Summary" block; both have a missing/empty/non-frame Type column,
 * so a positive Type test (not a negative reject) drops them. With no Type
 * column at all, require a non-empty QP field instead (same exclusion). */
static int row_is_frame(char **fields, size_t nf, const csv_cols *c)
{
    if (c->type >= 0) {
        return (size_t)c->type < nf && is_frame_type(fields[c->type]);
    }
    return c->qp >= 0 && (size_t)c->qp < nf && fields[c->qp][0] != '\0';
}

/* Fill one parsed frame row from the split fields. */
static void row_to_frame(char **fields, size_t nf, const csv_cols *c, PelorusX265Frame *fr)
{
    const char *t = (c->type >= 0 && (size_t)c->type < nf) ? fields[c->type] : "";

    memset(fr, 0, sizeof(*fr));
    fr->poc = (int32_t)field_l(fields, nf, c->poc);
    fr->qp = field_f(fields, nf, c->qp);
    fr->bits = field_u64(fields, nf, c->bits);
    fr->psnr_y = field_f(fields, nf, c->psnr_y);
    fr->psnr_u = field_f(fields, nf, c->psnr_u);
    fr->psnr_v = field_f(fields, nf, c->psnr_v);
    /* '?' on the no-Type-column fallback path so a consumer never sees a NUL
     * slice_type misread as a real type (the row was admitted on its QP field). */
    fr->slice_type = (t[0] != '\0') ? ascii_upper(t[0]) : '?';
}

#ifdef _WIN32
/* Every Windows path, even a \\?\ extended-length one, is bounded by the
 * 32767-code-unit UNICODE_STRING limit; +1 for the terminator. */
#define PEL_WPATH_UNITS_MAX 32768
/* One UTF-16 code unit never needs more than 3 UTF-8 bytes (an astral scalar is
 * 4 bytes for 2 units), so a longer byte string cannot name a Windows file. */
#define PEL_UTF8_PATH_BYTES_MAX ((size_t)3u * ((size_t)PEL_WPATH_UNITS_MAX - 1u))
/* Longest CRT mode string accepted, terminator included ("r, ccs=UTF-16LE"). */
#define PEL_WMODE_MAX 16u

/* Widen an ASCII CRT mode string ("r", "rb", ...) into wmode[PEL_WMODE_MAX].
 * Mode strings are ASCII by definition; anything else is a caller bug. */
static pel_result widen_mode(const char *mode, wchar_t *wmode)
{
    size_t i;

    for (i = 0; i < PEL_WMODE_MAX; i++) {
        unsigned char c = (unsigned char)mode[i];

        if (c > 0x7Fu) {
            break;
        }
        wmode[i] = (wchar_t)c;
        if (c == 0u) {
            return PEL_OK;
        }
    }
    errno = EINVAL; /* non-ASCII, or no terminator within PEL_WMODE_MAX */
    return PEL_ERR_INVALID;
}

/* strlen(s), or PEL_UTF8_PATH_BYTES_MAX + 1 when s is longer: the scan never
 * reads past that many bytes (P10 r2), and the result always fits an int. */
static size_t bounded_path_len(const char *s)
{
    size_t n = 0;

    while (n <= PEL_UTF8_PATH_BYTES_MAX && s[n] != '\0') {
        n++;
    }
    return n;
}

/* Widen a NUL-terminated UTF-8 path into a heap UTF-16 copy in *out, which the
 * caller frees. MB_ERR_INVALID_CHARS makes the decode strict: overlong forms,
 * encoded surrogates, truncated or stray sequences and values above U+10FFFF
 * fail instead of decaying to U+FFFD, so an ill-formed path can never alias a
 * different, existing file. Returns PEL_OK; PEL_ERR_ABSENT + ENAMETOOLONG (too
 * long to be any Windows path; checked first, before decoding);
 * PEL_ERR_INVALID + EILSEQ (ill-formed UTF-8); or PEL_ERR_NOMEM. */
static pel_result utf8_to_wide(const char *path, wchar_t **out)
{
    const size_t len = bounded_path_len(path);
    int units;
    wchar_t *wpath;

    *out = NULL;
    if (len > PEL_UTF8_PATH_BYTES_MAX) {
        errno = ENAMETOOLONG;
        return PEL_ERR_ABSENT;
    }
    /* len + 1 counts the terminator, so the UTF-16 copy is NUL-terminated too;
     * len <= PEL_UTF8_PATH_BYTES_MAX keeps the int conversion exact. */
    units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)len + 1, NULL, 0);
    if (units <= 0) {
        errno = EILSEQ;
        return PEL_ERR_INVALID;
    }
    if (units > (int)PEL_WPATH_UNITS_MAX) {
        errno = ENAMETOOLONG;
        return PEL_ERR_ABSENT;
    }
    wpath = malloc((size_t)units * sizeof(*wpath));
    if (wpath == NULL) {
        errno = ENOMEM;
        return PEL_ERR_NOMEM;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)len + 1, wpath, units) !=
        units) {
        free(wpath);
        errno = EILSEQ;
        return PEL_ERR_INVALID;
    }
    *out = wpath;
    return PEL_OK;
}
#endif

/* Open a caller-supplied UTF-8 path (ADR-0149); *out stays NULL on failure.
 *
 * POSIX: a literal fopen(path, mode). The kernel takes a path as bytes, so this
 * is byte-for-byte the pre-ADR-0149 behaviour (a non-UTF-8 byte name still
 * opens). Windows: the narrow CRT would decode the bytes through the ANSI code
 * page, so the path is widened strictly and opened with _wfsopen (_SH_DENYNO: the
 * sharing _wfopen gives, without its C4996 deprecation). No \\?\ prefix
 * is added: a caller that needs an extended-length path passes one.
 *
 * Every failure of the open itself maps to PEL_ERR_ABSENT (the pre-ADR-0149
 * contract); errno is preserved across the cleanup for a host that logs it. */
static pel_result open_utf8(const char *path, const char *mode, FILE **out)
{
#ifdef _WIN32
    wchar_t wmode[PEL_WMODE_MAX];
    wchar_t *wpath = NULL;
    pel_result rc;
    int open_errno;

    *out = NULL;
    rc = widen_mode(mode, wmode);
    if (rc == PEL_OK) {
        rc = utf8_to_wide(path, &wpath);
    }
    if (rc != PEL_OK) {
        return rc; /* nothing was allocated: utf8_to_wide frees on its failures */
    }
    *out = _wfsopen(wpath, wmode, _SH_DENYNO);
    open_errno = errno;
    free(wpath); /* the one allocation, released before any return below */
    errno = open_errno;
#else
    *out = fopen(path, mode);
#endif
    return (*out != NULL) ? PEL_OK : PEL_ERR_ABSENT;
}

/* Read the header and the frame rows from fp into out_frames. Sets *truncated when more frame
 * rows exist than cap, *have_header once a usable header was seen, and returns PEL_ERR_ABSENT
 * for a header without the QP and Bits columns. */
static pel_result x265_csv_read_rows(FILE *fp, PelorusX265Frame *out_frames, size_t cap,
                                     size_t *count, int *truncated, int *have_header)
{
    char line[PEL_CSV_LINE_MAX]; /* bounded, fixed (Po10): no heap, no VLA  */
    char *fields[PEL_CSV_MAX_FIELDS];
    /* Every index starts "absent" (-1): a row is then never admitted before a header has set the
     * real indices, and the compiler sees a defined value on every path. */
    csv_cols cols = {-1, -1, -1, -1, -1, -1, -1};

    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        size_t nf = split_fields(line, fields, PEL_CSV_MAX_FIELDS);

        if (!*have_header) {
            locate_columns(fields, nf, &cols);
            /* QP + Bits are the minimum we require to call this an x265 CSV. */
            if (cols.qp < 0 || cols.bits < 0) {
                return PEL_ERR_ABSENT;
            }
            *have_header = 1;
            continue;
        }

        if (nf == 0 || !row_is_frame(fields, nf, &cols)) {
            continue;
        }
        if (*count >= cap) {
            *truncated = 1;
            break;
        }
        row_to_frame(fields, nf, &cols, &out_frames[*count]);
        (*count)++;
    }
    return PEL_OK;
}

pel_result pel_x265_csv_parse(const char *path, PelorusX265Frame *out_frames, size_t cap,
                              size_t *out_count)
{
    FILE *fp;
    size_t count = 0;
    pel_result rc;
    int truncated = 0;
    int have_header = 0;

    if (path == NULL || out_frames == NULL || out_count == NULL || cap == 0) {
        return PEL_ERR_INVALID;
    }
    *out_count = 0;

    rc = open_utf8(path, "r", &fp); /* UTF-8 path on every platform (ADR-0149) */
    if (rc != PEL_OK) {
        return rc;
    }

    rc = x265_csv_read_rows(fp, out_frames, cap, &count, &truncated, &have_header);

    /* Distinguish EOF from a mid-file read error (CERT FIO35-C): fgets returns
     * NULL for both, so without this a truncated read would report PEL_OK. */
    if (ferror(fp) && rc == PEL_OK) {
        rc = PEL_ERR_TRUNCATED;
    }
    (void)fclose(fp);

    /* Empty file: no header was ever seen and rc is still PEL_OK -> ABSENT. A
     * bad-header break already set rc = PEL_ERR_ABSENT, so the `rc == PEL_OK`
     * guard makes this a no-op on that path (not a double-assign hazard). */
    if (!have_header && rc == PEL_OK) {
        rc = PEL_ERR_ABSENT;
    }
    *out_count = count;
    if (rc != PEL_OK) {
        return rc;
    }
    return truncated ? PEL_ERR_RANGE : PEL_OK;
}

/* honored_fraction: sign-agreement between the requested per-frame delta-QP (vs
 * the requested GOP mean) and the achieved per-frame delta-QP (vs the achieved
 * GOP mean, passed in). Both-near-zero counts as agreement (a flat request met
 * with a flat response). The frame-granular analogue of the QSV per-cell path. */
static float x265_honored_fraction(const PelorusX265Frame *frames, size_t nb,
                                   const float *requested_qp, double ach_mean)
{
    const double eps = 0.25; /* sub-QP-step: |delta| below this is "no move" */
    double req_sum = 0.0;
    double req_mean;
    size_t i;
    size_t agree = 0;

    for (i = 0; i < nb; i++) {
        req_sum += (double)requested_qp[i];
    }
    req_mean = req_sum / (double)nb;

    for (i = 0; i < nb; i++) {
        double dreq = (double)requested_qp[i] - req_mean;
        double dach = (double)frames[i].qp - ach_mean;
        int req_flat = fabs(dreq) < eps;
        int ach_flat = fabs(dach) < eps;
        int both_flat = req_flat && ach_flat;
        int same_sign = !req_flat && !ach_flat && (dreq > 0.0) == (dach > 0.0);

        if (both_flat || same_sign) {
            agree++;
        }
    }
    return (float)((double)agree / (double)nb);
}

pel_result pel_qp_report_from_x265_frames(const PelorusX265Frame *frames, size_t nb,
                                          const float *requested_qp,
                                          PelorusQpReportSection *out_section)
{
    double qp_bit_sum = 0.0; /* sum(qp_i * bits_i)                            */
    double psnr_y_sum = 0.0; /* bit-weighted PSNR accumulators                */
    double psnr_u_sum = 0.0;
    double psnr_v_sum = 0.0;
    double ach_sum = 0.0; /* for the achieved-QP GOP mean                  */
    uint64_t total_bits = 0;
    size_t i;

    if (frames == NULL || out_section == NULL || nb == 0) {
        return PEL_ERR_INVALID;
    }

    memset(out_section, 0, sizeof(*out_section));
    out_section->report_source = (uint8_t)PEL_QPSRC_NONE; /* x265 = SW reference */
    out_section->qp_valid = 0;                            /* frame-granular only */
    out_section->honored_fraction = 0.0f;

    for (i = 0; i < nb; i++) {
        double w = (double)frames[i].bits;

        /* Reject an impossible GOP rather than wrap total_bits (CERT INT30-C);
         * total_bits is the divisor for every weighted mean below. */
        if (frames[i].bits > UINT64_MAX - total_bits) {
            return PEL_ERR_RANGE;
        }
        total_bits += frames[i].bits;
        qp_bit_sum += (double)frames[i].qp * w;
        psnr_y_sum += (double)frames[i].psnr_y * w;
        psnr_u_sum += (double)frames[i].psnr_u * w;
        psnr_v_sum += (double)frames[i].psnr_v * w;
        ach_sum += (double)frames[i].qp;
    }

    out_section->total_bits = total_bits;
    if (total_bits > 0) {
        out_section->avg_qp = (float)(qp_bit_sum / (double)total_bits);
        out_section->psnr_y = (float)(psnr_y_sum / (double)total_bits);
        out_section->psnr_u = (float)(psnr_u_sum / (double)total_bits);
        out_section->psnr_v = (float)(psnr_v_sum / (double)total_bits);
    } else {
        /* Degenerate zero-bit CSV: fall back to an unweighted QP mean. */
        out_section->avg_qp = (float)(ach_sum / (double)nb);
    }

    if (requested_qp != NULL) {
        out_section->honored_fraction =
            x265_honored_fraction(frames, nb, requested_qp, ach_sum / (double)nb);
    }

    return PEL_OK;
}

/* NOLINTEND(modernize-use-nullptr) */
