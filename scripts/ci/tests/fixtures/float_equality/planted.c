/* Copyright 2026 Lusoris */
/* SPDX-License-Identifier: EUPL-1.2 */
#include "planted.h"

int planted_check(void)
{
    return vmaf_float_motion_scaler(table[0].bpc) == table[0].expected;
}
