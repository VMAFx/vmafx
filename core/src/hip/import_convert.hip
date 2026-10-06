/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Planarisation of imported semi-planar frames on a HIP device (RC4 WP3,
 * ADR-1929 item 6, ADR-2092): NV12 / P010 / P016 chroma de-interleaved and
 * the P010 luma shifted, the kernels the CUDA lane runs (ADR-2023), from
 * core/src/vmafx/import_convert_kernels.h. Loaded by hip/import_device.c
 * (`import_convert_hsaco`). Built with `hip_strict_fp_args` like every HIP
 * kernel; the kernels do integer work only.
 */

#include <hip/hip_runtime.h>

#include "vmafx/import_convert_kernels.h"
