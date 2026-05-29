# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Enables `python -m vmaf` as a shortcut for the run_vmaf console script.
# Usage: python -m vmaf <fmt> <width> <height> <ref> <dis> [options]

import sys

from vmaf.script.run_vmaf import main

sys.exit(main())
