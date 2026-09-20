#!/usr/bin/env python3
import sys
from pathlib import Path

from vmaf.core.quality_runner import QualityRunner
from vmaf.core.result_store import FileSystemResultStore
from vmaf.routine import run_remove_results_for_dataset
from vmaf.tools.misc import import_python_file

_COMPARISON_VALUE_3 = 3

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


def print_usage():
    quality_runner_types = ["VMAF", "PSNR", "SSIM", "MS_SSIM"]
    print("usage: " + Path(sys.argv[0]).name + " quality_type dataset_filepath\n")
    print("quality_type:\n\t" + "\n\t".join(quality_runner_types) + "\n")


def main():
    if len(sys.argv) < _COMPARISON_VALUE_3:
        print_usage()
        return 2

    try:
        quality_type = sys.argv[1]
        dataset_filepath = sys.argv[2]
    except ValueError:
        print_usage()
        return 2

    try:
        dataset = import_python_file(dataset_filepath)
    except Exception as e:
        print("Error: " + str(e))
        return 1

    try:
        runner_class = QualityRunner.find_subclass(quality_type)
    except Exception:
        # find_subclass raises on unknown quality_type; KeyboardInterrupt /
        # SystemExit must still propagate. (CodeQL py/catch-base-exception)
        print_usage()
        return 2

    result_store = FileSystemResultStore()

    run_remove_results_for_dataset(result_store, dataset, runner_class)

    return 0


if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
