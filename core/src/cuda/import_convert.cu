/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Planarisation of imported semi-planar frames on a CUDA device (RC4 WP3,
 * ADR-1929 item 6, ADR-2023). The kernels are shared with the HIP lane
 * (ADR-2092) and live in core/src/vmafx/import_convert_kernels.h.
 */

#include "vmafx/import_convert_kernels.h"
