// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

void f(int n)
{
    if (n) {
        goto done;
    }
done:
    return;
}
