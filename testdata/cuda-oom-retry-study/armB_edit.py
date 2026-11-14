# -*- coding: utf-8 -*-
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Apply the Arm B edit to test_cuda_oom_pictures_released.c (probe-removed arm).

Local experiment edit, not committed. Run AFTER the Arm A campaign finishes.
The target source path is a required argument and must point at
core/test/test_cuda_oom_pictures_released.c inside the build worktree.
Reversible with:
    git -C <build-worktree> checkout -- core/test/test_cuda_oom_pictures_released.c
"""

import argparse
import io
import os
import sys

EXPECTED_SUFFIX = os.path.join("core", "test", "test_cuda_oom_pictures_released.c")

OLD = """    hog_release(state, hog);
    const int probe_back = probe_alloc(state, PROBE_BYTES);
    (void)fprintf(stderr, "[hog %zu MiB, luma probe under hog %d, first rc %d, memory back %d] ",
                  hog->bytes >> 20, probe_hogged, oom_err, probe_back);
    mu_assert("a frame was scored with the device's memory taken", oom_err != 0);
    (void)fprintf(stderr, "[first frame with no memory left: %d] ", oom_err);

    /* The pair of the failed call is back in the pool, and the ring that could
     * not be built is built now. */
    mu_assert("allocation probe failed", probe_back >= 0);
    mu_assert("device memory not yet returned after the hog was freed", probe_back == 1);
    return NULL;"""

NEW = """    hog_release(state, hog);
    /* #3277 probe-perturbation A/B, arm B: no in-process probe between the
     * hog release and the retry (local experiment, not for commit). */
    (void)fprintf(stderr, "[hog %zu MiB, luma probe under hog %d, first rc %d, memory back n/a] ",
                  hog->bytes >> 20, probe_hogged, oom_err);
    mu_assert("a frame was scored with the device's memory taken", oom_err != 0);
    (void)fprintf(stderr, "[first frame with no memory left: %d] ", oom_err);
    return NULL;"""


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Apply the arm B probe-removed edit to the CUDA OOM test source."
    )
    parser.add_argument(
        "source",
        help="path to core/test/test_cuda_oom_pictures_released.c " "in the build worktree",
    )
    args = parser.parse_args(argv)
    source = os.path.abspath(args.source)
    if not source.endswith(EXPECTED_SUFFIX):
        parser.error("source must point at %s, got %s" % (EXPECTED_SUFFIX, args.source))
    args.source = source
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    with io.open(args.source, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    if OLD not in text:
        print("PATTERN-NOT-FOUND (already applied or upstream changed)")
        return 1
    text = text.replace(OLD, NEW, 1)
    with io.open(args.source, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("ARM-B-EDIT-APPLIED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
