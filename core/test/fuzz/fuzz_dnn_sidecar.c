/*
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * libFuzzer harness for the tiny-AI sidecar JSON loader
 * (libvmaf/core/src/dnn/model_loader.c — `vmaf_dnn_sidecar_load`).
 *
 * Rationale: every shipped tiny-AI model under model/tiny/
 * pairs with a sibling `<basename>.json` "sidecar" that the
 * loader parses to drive feature scaling, output binding,
 * encoder vocabulary lookup, and quantisation-mode selection.
 * The parser is a hand-rolled
 * `strstr` / `strchr` / `snprintf("\"%s\"", key)` walker rather
 * than a structured JSON tokenizer — every `extract_string`,
 * `extract_string_array`, `extract_float_array`, and
 * `extract_int` call walks the document fresh from byte 0,
 * heap-allocates the value, and trusts the array-terminator
 * search to stay inside the document. Attacker-supplied
 * sidecars (e.g. via a model registry override or a side-loaded
 * tiny-model directory) reach this code directly. Classic
 * fuzz-amenable shape: per-key heap alloc * unbounded array
 * length * scrappy bounds checks.
 *
 * Strategy: `vmaf_dnn_sidecar_load` takes an .onnx path and
 * derives `<basename>.json` next to it. The harness creates a
 * per-process tempfile pair: `/tmp/vmaf-fuzz-sidecar-<pid>.onnx`
 * (never opened by the loader, but its name drives the path
 * derivation) and `/tmp/vmaf-fuzz-sidecar-<pid>.json` (the
 * fuzzer-controlled content). Each iteration rewrites the JSON
 * file from `data` and calls the public entry point.
 *
 * Listed as deferred target #4 ("fuzz_sidecar") in
 * docs/research/0083-libfuzzer-harness-expansion-target-survey.md.
 * Build is opt-in via `-Dfuzz=true`; CI nightly runs a
 * 60-second smoke per harness. See docs/development/fuzzing.md.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "dnn/model_loader.h"

/* Hard cap on input size: the loader itself rejects sidecars
 * larger than 1 MiB (`> (size_t)(1u << 20)`). Cap the fuzzer at
 * 128 KiB to keep iterations fast — past that the parser's
 * `strstr` linear-scan dominates wall time without adding
 * meaningful branch coverage. */
#define FUZZ_MAX_INPUT_BYTES (128u * 1024u)

/* Reuse the same on-disk tempfile pair across iterations.
 * Re-creating + unlinking per iteration would dominate wall
 * time at libFuzzer's 100k+ iterations/min throughput; the
 * loader's `fopen`+`fread` path is `O(1)` w.r.t. how the file
 * got there. The path uses the pid suffix so parallel
 * libFuzzer workers (`-jobs=N`) get distinct files.
 *
 * Initialised lazily on first call and never freed — the
 * harness process exits with the OS reaping the inode.
 * `INT_MAX` decimal width caps pid at 10 chars; the prefix +
 * suffix add up to under 64 bytes, comfortably below `PATH_MAX`. */
static char g_onnx_path[64];
static char g_json_path[64];
static int g_paths_inited = 0;

static int init_paths_once(void)
{
    if (g_paths_inited)
        return 0;
    const long pid = (long)getpid();
    int n_onnx = snprintf(g_onnx_path, sizeof(g_onnx_path), "/tmp/vmaf-fuzz-sidecar-%ld.onnx", pid);
    int n_json = snprintf(g_json_path, sizeof(g_json_path), "/tmp/vmaf-fuzz-sidecar-%ld.json", pid);
    if (n_onnx < 0 || (size_t)n_onnx >= sizeof(g_onnx_path) || n_json < 0 ||
        (size_t)n_json >= sizeof(g_json_path))
        return -1;
    g_paths_inited = 1;
    return 0;
}

/* AddressSanitizer override: keep heap-error detection on,
 * silence leak-only reports. The sidecar parser's error paths
 * try to free everything they allocated, but a fuzzer-induced
 * mid-array `-EINVAL` return walks back through `out[cnt++]`
 * slots with no cleanup — leaking the partially-built array
 * is a pre-existing fork-local issue; ASan leak reports here
 * would mask interesting overflow / use-after-free finds.
 * Environment-side `ASAN_OPTIONS` overrides this if a CI
 * operator wants leak detection on. The `__` prefix is
 * mandated by the AddressSanitizer weak-symbol ABI. */
/* NOLINTNEXTLINE(misc-use-internal-linkage,bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — ASan weak-symbol ABI */
const char *__asan_default_options(void);
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — ASan weak-symbol ABI */
const char *__asan_default_options(void)
{
    return "detect_leaks=0:allocator_may_return_null=1";
}

/* Rewrite the sidecar JSON tempfile with @p data, then call
 * the public loader. On the success path we free the parsed
 * sidecar's owned strings via `vmaf_dnn_sidecar_free`; on
 * failure the loader owns its own cleanup. */
static void drive_one(const uint8_t *data, size_t size)
{
    FILE *f = fopen(g_json_path, "wb");
    if (!f)
        return;
    if (fwrite(data, 1u, size, f) != size) {
        (void)fclose(f);
        return;
    }
    if (fclose(f) != 0)
        return;

    VmafModelSidecar sc;
    memset(&sc, 0, sizeof(sc));
    (void)vmaf_dnn_sidecar_load(g_onnx_path, &sc);
    vmaf_dnn_sidecar_free(&sc);
}

/* libFuzzer's contract requires `LLVMFuzzerTestOneInput` to have
 * external linkage; the runtime resolves it by name at link time
 * (`-fsanitize=fuzzer`). Cannot be static — the
 * `misc-use-internal-linkage` warning is load-bearing-wrong. */
/* NOLINTNEXTLINE(misc-use-internal-linkage) — libFuzzer entry-point ABI */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0u || size > FUZZ_MAX_INPUT_BYTES)
        return 0;
    if (init_paths_once() != 0)
        return 0;
    drive_one(data, size);
    return 0;
}
