/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Unit tests for the persistent VkPipelineCache implementation
 *  (ADR-0445 / PR #865 profiling finding).
 *
 *  Tests:
 *    1. First invocation creates the cache file; size > 0.
 *    2. Second invocation reads the cache; no errors.
 *    3. Tampered header is silently rejected (no crash).
 *    4. LIBVMAF_VULKAN_PIPELINE_CACHE=0 opt-out skips read + write.
 *
 *  All GPU-using tests skip cleanly when no Vulkan compute device is
 *  present (vmaf_vulkan_device_count() returns <= 0).
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "libvmaf/libvmaf_vulkan.h"
#include "vulkan/vulkan_common.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Build a reproducible temp path for the pipeline cache that is
 * distinct from any real user cache and is cleaned up across test
 * runs. */
static int make_test_cache_dir(char *dir_out, size_t dir_size, char *path_out, size_t path_size)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || tmp[0] == '\0')
        tmp = "/tmp";
    int n = snprintf(dir_out, dir_size, "%s/vmaf-pipeline-cache-test", tmp);
    if (n <= 0 || (size_t)n >= dir_size)
        return -1;
    n = snprintf(path_out, path_size, "%s/libvmaf/vulkan-pipeline-cache.bin", dir_out);
    if (n <= 0 || (size_t)n >= path_size)
        return -1;
    return 0;
}

/* Delete a file without asserting (cleanup helper). */
static void unlink_silent(const char *p)
{
    (void)remove(p);
}

/* Read the byte length of a file, or 0 on error. */
static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return 0;
    }
    long sz = ftell(f);
    (void)fclose(f);
    return (sz > 0) ? sz : 0;
}

/* ------------------------------------------------------------------ */
/*  Test 1 — first run creates the cache file                          */
/* ------------------------------------------------------------------ */

static char *test_pipeline_cache_first_run_creates_file(void)
{
    if (vmaf_vulkan_device_count() <= 0)
        return NULL; /* no GPU — skip */

    char cache_dir[512], cache_path[600];
    if (make_test_cache_dir(cache_dir, sizeof(cache_dir), cache_path, sizeof(cache_path)) != 0)
        return NULL; /* path too long — skip */

    /* Point the implementation at our throwaway directory. */
    if (setenv("XDG_CACHE_HOME", cache_dir, /*overwrite=*/1) != 0)
        return NULL;

    /* Remove any leftover from a prior run. */
    unlink_silent(cache_path);

    /* Create a context; init() should create the cache from scratch and
     * destroy() should serialise it to disk. */
    VmafVulkanContext *ctx = NULL;
    int rc = vmaf_vulkan_context_new(&ctx, -1);
    mu_assert("context_new must succeed", rc == 0);
    mu_assert("context must be non-NULL", ctx != NULL);
    vmaf_vulkan_context_destroy(ctx);

    /* The file must now exist and be non-empty. */
    long sz = file_size(cache_path);
    mu_assert("pipeline cache file must exist after first run", sz > 0);

    unlink_silent(cache_path);
    (void)unsetenv("XDG_CACHE_HOME");
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Test 2 — second run reads the cache without errors                 */
/* ------------------------------------------------------------------ */

static char *test_pipeline_cache_warm_run_no_error(void)
{
    if (vmaf_vulkan_device_count() <= 0)
        return NULL;

    char cache_dir[512], cache_path[600];
    if (make_test_cache_dir(cache_dir, sizeof(cache_dir), cache_path, sizeof(cache_path)) != 0)
        return NULL;

    if (setenv("XDG_CACHE_HOME", cache_dir, 1) != 0)
        return NULL;

    unlink_silent(cache_path);

    /* First run — populate the cache. */
    {
        VmafVulkanContext *ctx = NULL;
        mu_assert("first run context_new OK", vmaf_vulkan_context_new(&ctx, -1) == 0);
        vmaf_vulkan_context_destroy(ctx);
    }

    long sz_first = file_size(cache_path);
    mu_assert("cache file written after first run", sz_first > 0);

    /* Second run — must read the cache without crashing or returning error. */
    {
        VmafVulkanContext *ctx = NULL;
        int rc = vmaf_vulkan_context_new(&ctx, -1);
        mu_assert("second run context_new OK", rc == 0);
        mu_assert("second run context non-NULL", ctx != NULL);
        vmaf_vulkan_context_destroy(ctx);
    }

    /* The file should still exist (overwritten) and remain non-empty. */
    mu_assert("cache file still exists after second run", file_size(cache_path) > 0);

    unlink_silent(cache_path);
    (void)unsetenv("XDG_CACHE_HOME");
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Test 3 — tampered header is silently rejected, no crash            */
/* ------------------------------------------------------------------ */

static char *test_pipeline_cache_tampered_header_rejected(void)
{
    if (vmaf_vulkan_device_count() <= 0)
        return NULL;

    char cache_dir[512], cache_path[600];
    if (make_test_cache_dir(cache_dir, sizeof(cache_dir), cache_path, sizeof(cache_path)) != 0)
        return NULL;

    if (setenv("XDG_CACHE_HOME", cache_dir, 1) != 0)
        return NULL;

    /* Write a file with a plausible-but-bogus 32-byte header:
     * headerSize=32, headerVersion=1, vendorID=0xDEAD, deviceID=0xBEEF,
     * UUID=all-zeros.  The vendorID will not match any real device so
     * the loader must reject it silently. */
    {
        /* Ensure directory exists: create <dir> then <dir>/libvmaf. */
        if (mkdir(cache_dir, 0700) != 0 && errno != EEXIST) {
            (void)unsetenv("XDG_CACHE_HOME");
            return NULL; /* skip */
        }
        char libvmaf_subdir[512];
        (void)snprintf(libvmaf_subdir, sizeof(libvmaf_subdir), "%s/libvmaf", cache_dir);
        if (mkdir(libvmaf_subdir, 0700) != 0 && errno != EEXIST) {
            (void)unsetenv("XDG_CACHE_HOME");
            return NULL; /* skip */
        }

        FILE *f = fopen(cache_path, "wb");
        if (!f) {
            (void)unsetenv("XDG_CACHE_HOME");
            return NULL; /* skip */
        }

        uint32_t hdr[8] = {
            32u,                    /* headerSize */
            1u,                     /* headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE */
            0xDEADBEEFu,            /* vendorID — intentionally wrong */
            0xCAFEBABEu,            /* deviceID — intentionally wrong */
            0u,          0u, 0u, 0u /* UUID bytes (zero-padded) */
        };
        (void)fwrite(hdr, sizeof(uint32_t), 8, f);
        (void)fclose(f);
    }

    /* Creating a context on top of the tampered file must succeed — the
     * implementation discards the bad blob and creates an empty cache. */
    VmafVulkanContext *ctx = NULL;
    int rc = vmaf_vulkan_context_new(&ctx, -1);
    mu_assert("context_new with tampered cache must not crash or fail", rc == 0);
    mu_assert("context non-NULL despite tampered cache", ctx != NULL);
    vmaf_vulkan_context_destroy(ctx);

    unlink_silent(cache_path);
    (void)unsetenv("XDG_CACHE_HOME");
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Test 4 — env-var opt-out skips read + write                        */
/* ------------------------------------------------------------------ */

static char *test_pipeline_cache_opt_out_skips_io(void)
{
    if (vmaf_vulkan_device_count() <= 0)
        return NULL;

    char cache_dir[512], cache_path[600];
    if (make_test_cache_dir(cache_dir, sizeof(cache_dir), cache_path, sizeof(cache_path)) != 0)
        return NULL;

    if (setenv("XDG_CACHE_HOME", cache_dir, 1) != 0)
        return NULL;
    /* Activate the opt-out. */
    if (setenv("LIBVMAF_VULKAN_PIPELINE_CACHE", "0", 1) != 0) {
        (void)unsetenv("XDG_CACHE_HOME");
        return NULL;
    }

    unlink_silent(cache_path);

    VmafVulkanContext *ctx = NULL;
    int rc = vmaf_vulkan_context_new(&ctx, -1);
    mu_assert("context_new with opt-out must succeed", rc == 0);
    mu_assert("context non-NULL with opt-out", ctx != NULL);
    vmaf_vulkan_context_destroy(ctx);

    /* With the opt-out active, no cache file should have been written. */
    mu_assert("no cache file written when opt-out is set", file_size(cache_path) == 0);

    (void)unsetenv("LIBVMAF_VULKAN_PIPELINE_CACHE");
    (void)unsetenv("XDG_CACHE_HOME");
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Runner                                                              */
/* ------------------------------------------------------------------ */

char *run_tests(void)
{
    mu_run_test(test_pipeline_cache_first_run_creates_file);
    mu_run_test(test_pipeline_cache_warm_run_no_error);
    mu_run_test(test_pipeline_cache_tampered_header_rejected);
    mu_run_test(test_pipeline_cache_opt_out_skips_io);
    return NULL;
}
