// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

int fact(int n)
{
    if (n <= 1) {
        return 1;
    }
    return n * fact(n - 1);
}
