/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * libFuzzer harness for the libvmaf JSON model parser
 * (libvmaf/core/src/read_json_model.c) — exercised through the
 * public buffer-mode entry points
 * `vmaf_read_json_model_from_buffer` and
 * `vmaf_read_json_model_collection_from_buffer`.
 *
 * Rationale: VMAF SVM models ship as `.json` files
 * (`model/vmaf_v0.6.1.json` etc.) that the CLI loads via
 * `--model path=<file>`. The parser is the pure-C `pdjson`
 * incremental tokenizer driving custom object walkers in
 * `read_json_model.c`; the walker grows feature / knot arrays
 * (`grow_count`, `ensure_feature_capacity`,
 * `ensure_knot_capacity`) using attacker-controllable
 * `feature_names` / `score_transform.knots` lengths, calls
 * `realloc` on every doubling step, and embeds an opaque
 * libsvm-formatted string ("svm_type nu_svr\nkernel_type rbf
 * ...") that gets handed to libsvm's own `svm_load_model`
 * parser. All three are classic fuzz-amenable shapes
 * (untrusted-length growth, repeated reallocation, embedded
 * mini-parser).
 *
 * Strategy: feed the fuzzer bytes verbatim into
 * `vmaf_read_json_model_from_buffer` (and, when bytes are
 * long enough, also into the collection variant which adds
 * a second outer-object layer). AddressSanitizer + UBSan
 * flag any heap or arithmetic crash. The library destroys
 * the parsed model on success so cleanup is straightforward;
 * on error the parser is responsible for releasing every
 * partial allocation it made before returning.
 *
 * Listed as deferred target #3 ("fuzz_model_load") in
 * docs/research/0083-libfuzzer-harness-expansion-target-survey.md.
 * Build is opt-in via `-Dfuzz=true`; CI nightly runs a
 * 60-second smoke per harness. See docs/development/fuzzing.md.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/model.h"
#include "read_json_model.h"

/* Hard cap on input size: shipped models top out around 23 KiB
 * (`model/vmaf_4k_v0.6.1neg.json`). 64 KiB of headroom lets the
 * fuzzer splice a few oversized libsvm SV blocks without paying
 * for memcpy time past the point of diminishing branch-coverage
 * returns. */
#define FUZZ_MAX_INPUT_BYTES 65536u

/* AddressSanitizer override: keep heap-use-after-free / overflow
 * detection ON (the entire point of the harness) and enable leak
 * detection. The JSON parser's single-model error path calls
 * `vmaf_model_destroy` on partial state, so all per-model
 * allocations (feature[], knots.list, name, opts_dict) are freed
 * before returning an error. The harness fixes the collection
 * variant's partial-parse oversight: it now frees `model_c` and
 * `collection` even on non-zero return from
 * `vmaf_read_json_model_collection_from_buffer`, eliminating the
 * residual leak that forced `detect_leaks=0` previously.
 * Environment-side `ASAN_OPTIONS` overrides this if a CI operator
 * needs to suppress leaks temporarily. The `__` prefix is
 * mandated by the AddressSanitizer weak-symbol ABI (compiler-rt). */
/* NOLINTNEXTLINE(misc-use-internal-linkage,bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — ASan weak-symbol ABI (ADR-0141 / ADR-0278) */
const char *__asan_default_options(void);
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — ASan weak-symbol ABI (ADR-0141 / ADR-0278) */
const char *__asan_default_options(void)
{
    return "detect_leaks=1:allocator_may_return_null=1";
}

/* libFuzzer's contract requires `LLVMFuzzerTestOneInput` to have
 * external linkage; the runtime resolves it by name at link time
 * (`-fsanitize=fuzzer`). Cannot be static — the
 * `misc-use-internal-linkage` warning is load-bearing-wrong. */
/* NOLINTNEXTLINE(misc-use-internal-linkage) — libFuzzer entry-point ABI (ADR-0141 / ADR-0278) */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0u || size > FUZZ_MAX_INPUT_BYTES)
        return 0;

    /* Drive both buffer-entry-point variants. The collection
     * variant adds an additional outer-object layer (`{"0":
     * <model>, "1": <model>, ...}`) so it exercises a strict
     * superset of the parser branches on any given input.
     *
     * The parser's contract: on success `*model` is heap-owned
     * and must be released with `vmaf_model_destroy`; on
     * non-zero return `*model` is left untouched (we initialise
     * it to NULL so an accidentally-leaked write would be
     * caught by ASan on the dangling free below). */
    VmafModel *model = NULL;
    VmafModelConfig cfg = {0};
    const int rc = vmaf_read_json_model_from_buffer(&model, &cfg, (const char *)data, (int)size);
    if (rc == 0 && model != NULL)
        vmaf_model_destroy(model);

    VmafModel *model_c = NULL;
    VmafModelCollection *collection = NULL;
    VmafModelConfig cfg_c = {0};
    (void)vmaf_read_json_model_collection_from_buffer(&model_c, &collection, &cfg_c,
                                                      (const char *)data, (int)size);
    /* Always clean up regardless of return code: a partial parse can
     * populate *model_c (key "0" succeeded) or *collection (keys "1"+
     * succeeded) before the parse of a later key fails. Freeing on
     * error prevents the residual leak that previously forced
     * detect_leaks=0 in __asan_default_options. */
    if (model_c != NULL)
        vmaf_model_destroy(model_c);
    if (collection != NULL)
        vmaf_model_collection_destroy(collection);

    return 0;
}
