/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The trace a conformance scenario writes (RC4 WP6): every return value and
 * every output of every call, doubles as %a, so two runs agree only when
 * every score is bit for bit equal. Pointers never enter a trace; whether a
 * pointer is NULL does.
 */

#ifndef VMAF_COMPAT_CONFORMANCE_TRACE_H
#define VMAF_COMPAT_CONFORMANCE_TRACE_H

#include <stddef.h>

#include "compat_conformance_gen.h"

#if defined(__GNUC__) || defined(__clang__)
#define TRACE_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define TRACE_FORMAT(fmt, args)
#endif

typedef struct Trace {
    char *text;
    size_t len;
    size_t cap;
    int failed; /* out of memory: the trace is incomplete */
} Trace;

/* Append one formatted line. */
void trace(Trace *t, const char *fmt, ...) TRACE_FORMAT(2, 3);
/* A string or "(null)". */
const char *trace_str(const char *s);
void trace_free(Trace *t);
/* The planted defect of this run (VMAF_COMPAT_PLANT), "" for none. The three
 * meson tests run at once, so each names its scratch files after it. */
const char *conformance_plant(void);

/* A scenario: calls through `api` only and writes what it saw to `t`. */
typedef void (*Scenario)(const VmafCompatApi *api, Trace *t);

/* Scenarios (test_compat_conformance_*.c). */
void scenario_lifecycle(const VmafCompatApi *api, Trace *t);
void scenario_dictionary(const VmafCompatApi *api, Trace *t);
void scenario_models(const VmafCompatApi *api, Trace *t);
void scenario_collections(const VmafCompatApi *api, Trace *t);
void scenario_scoring(const VmafCompatApi *api, Trace *t);
void scenario_preallocated(const VmafCompatApi *api, Trace *t);
void scenario_pictures(const VmafCompatApi *api, Trace *t);
void scenario_conversion(const VmafCompatApi *api, Trace *t);
void scenario_perceptual(const VmafCompatApi *api, Trace *t);
void scenario_tiny_ai(const VmafCompatApi *api, Trace *t);
void scenario_absent_backends(const VmafCompatApi *api, Trace *t);
void scenario_mcp(const VmafCompatApi *api, Trace *t);

#endif /* VMAF_COMPAT_CONFORMANCE_TRACE_H */
