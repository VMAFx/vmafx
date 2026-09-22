// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

#include <stdlib.h>

// system() hands a runtime-built string to /bin/sh: dynamic execution, which HISS-08
// bans outright. No banned-call rule covers it.
void run_command(const char *command)
{
    system(command);
}
