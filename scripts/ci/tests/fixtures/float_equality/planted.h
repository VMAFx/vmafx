/* Copyright 2026 Lusoris */
/* SPDX-License-Identifier: EUPL-1.2 */
/* Planted shape from issue 3303: both operand types are invisible to Semgrep. */
float vmaf_float_motion_scaler(unsigned bpc);

struct TableRow {
    unsigned bpc; // cppcheck-suppress unusedStructMember
    float expected; // cppcheck-suppress unusedStructMember
    const char *msg; // cppcheck-suppress unusedStructMember
};

static const struct TableRow table[] = {
    {10, 1.0f, "ten"},
};

int planted_check(void);
