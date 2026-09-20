#!/usr/bin/env python3

from importlib import import_module
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import re
import sys

import numpy as np

from vmaf.config import DisplayConfig
from vmaf.core.quality_runner import BootstrapVmafQualityRunner, QualityRunner, VmafQualityRunner
from vmaf.core.result_store import FileSystemResultStore
from vmaf.routine import print_matplotlib_warning, run_test_on_dataset
from vmaf.tools.misc import cmd_option_exists, get_cmd_option, import_python_file
from vmaf.tools.stats import ListStats

import_module("vmaf.core.cambi_quality_runner")
import_module("vmaf.core.matlab_quality_runner")

_COMPARISON_VALUE_3 = 3

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"

POOL_METHODS = ["mean", "harmonic_mean", "min", "median", "perc5", "perc10", "perc20"]

SUBJECTIVE_MODELS = [
    "DMOS",
    "DMOS_MLE",
    "MLE",
    "MLE_CO_AP",
    "MLE_CO_AP2 (default)",
    "MOS",
    "SR_DMOS",
    "SR_MOS (i.e. ITU-R BT.500)",
    "BR_SR_MOS (i.e. ITU-T P.913)",
    "ZS_SR_DMOS",
    "ZS_SR_MOS",
    "...",
]


def print_usage():
    quality_runner_types = ["VMAF", "PSNR", "SSIM", "MS_SSIM", "..."]
    print(
        "usage: "
        + Path(sys.argv[0]).name
        + " quality_type test_dataset_filepath [--vmaf-model VMAF_model_path] "
        "[--vmaf-phone-model] [--subj-model subjective_model] [--cache-result] "
        "[--parallelize] [--print-result] [--save-plot plot_dir] [--plot-wh plot_wh] "
        "[--processes processes]\n"
    )
    print("quality_type:\n\t" + "\n\t".join(quality_runner_types) + "\n")
    print("subjective_model:\n\t" + "\n\t".join(SUBJECTIVE_MODELS) + "\n")
    print("plot_wh: plot width and height in inches, example: 5x5 (default)")
    print("processes: must be an integer >=1")


class _CliError(Exception):
    def __init__(self, exit_code):
        super().__init__()
        self.exit_code = exit_code


def _aggregate_method(pool_method):
    methods = {
        "harmonic_mean": ListStats.harmonic_mean,
        "min": np.min,
        "median": np.median,
        "perc5": ListStats.perc5,
        "perc10": ListStats.perc10,
        "perc20": ListStats.perc20,
    }
    return methods.get(pool_method, np.mean)


def _subjective_model_class(subj_model):
    try:
        subjective_model = import_module("sureal.subjective_model").SubjectiveModel
        return subjective_model.find_subclass(subj_model or "MLE_CO_AP2")
    except Exception as error:
        print(f"Error: {error}")
        raise _CliError(1) from error


def _plot_size(plot_wh):
    if plot_wh is None:
        return 5, 5
    match = re.fullmatch(r"([0-9]+)x([0-9]+)", plot_wh)
    if match is None:
        print("Error: plot_wh must be in the format of WxH, example: 5x5")
        raise _CliError(1)
    return int(match.group(1)), int(match.group(2))


def _quality_runner_class(quality_type):
    try:
        return QualityRunner.find_subclass(quality_type)
    except Exception as error:
        print(f"Error: {error}")
        raise _CliError(1) from error


def _process_count(raw_processes):
    if raw_processes is None:
        return None
    try:
        processes = int(raw_processes)
    except ValueError as error:
        print("Input error: processes must be an integer")
        raise _CliError(2) from error
    if processes < 1:
        print("Input error: processes must be at least 1")
        raise _CliError(2)
    return processes


def _import_dataset(filepath):
    try:
        return import_python_file(filepath)
    except Exception as error:
        print(f"Error: {error}")
        raise _CliError(1) from error


def _parse_arguments():
    if len(sys.argv) < _COMPARISON_VALUE_3:
        print_usage()
        raise _CliError(2)
    quality_type = sys.argv[1]
    test_dataset_filepath = sys.argv[2]
    vmaf_model_path = get_cmd_option(sys.argv, 3, len(sys.argv), "--vmaf-model")
    cache_result = cmd_option_exists(sys.argv, 3, len(sys.argv), "--cache-result")
    parallelize = cmd_option_exists(sys.argv, 3, len(sys.argv), "--parallelize")
    processes = get_cmd_option(sys.argv, 3, len(sys.argv), "--processes")
    print_result = cmd_option_exists(sys.argv, 3, len(sys.argv), "--print-result")
    suppress_plot = cmd_option_exists(sys.argv, 3, len(sys.argv), "--suppress-plot")
    vmaf_phone_model = cmd_option_exists(sys.argv, 3, len(sys.argv), "--vmaf-phone-model")

    pool_method = get_cmd_option(sys.argv, 3, len(sys.argv), "--pool")
    if not (pool_method is None or pool_method in POOL_METHODS):
        print("--pool can only have option among {}".format(", ".join(POOL_METHODS)))
        raise _CliError(2)
    subj_model = get_cmd_option(sys.argv, 3, len(sys.argv), "--subj-model")
    subj_model_class = _subjective_model_class(subj_model)
    save_plot_dir = get_cmd_option(sys.argv, 3, len(sys.argv), "--save-plot")
    plot_wh = _plot_size(get_cmd_option(sys.argv, 3, len(sys.argv), "--plot-wh"))
    runner_class = _quality_runner_class(quality_type)
    if vmaf_model_path is not None and runner_class not in (
        VmafQualityRunner,
        BootstrapVmafQualityRunner,
    ):
        print("Input error: only quality_type of VMAF accepts --vmaf-model.")
        print_usage()
        raise _CliError(2)
    if vmaf_phone_model and runner_class not in (VmafQualityRunner, BootstrapVmafQualityRunner):
        print("Input error: only quality_type of VMAF accepts --vmaf-phone-model.")
        print_usage()
        raise _CliError(2)
    return {
        "test_dataset": _import_dataset(test_dataset_filepath),
        "runner_class": runner_class,
        "vmaf_model_path": vmaf_model_path,
        "result_store": FileSystemResultStore() if cache_result else None,
        "parallelize": parallelize,
        "processes": _process_count(processes),
        "print_result": print_result,
        "suppress_plot": suppress_plot,
        "aggregate_method": _aggregate_method(pool_method),
        "subj_model_class": subj_model_class,
        "enable_transform_score": True if vmaf_phone_model else None,
        "save_plot_dir": save_plot_dir,
        "plot_wh": plot_wh,
    }


def _run_test(arguments, ax):
    return run_test_on_dataset(
        arguments["test_dataset"],
        arguments["runner_class"],
        ax,
        arguments["result_store"],
        arguments["vmaf_model_path"],
        parallelize=arguments["parallelize"],
        aggregate_method=arguments["aggregate_method"],
        subj_model_class=arguments["subj_model_class"],
        enable_transform_score=arguments["enable_transform_score"],
        processes=arguments["processes"],
    )


def _run_with_plot(arguments):
    if arguments["suppress_plot"]:
        return _run_test(arguments, None)
    try:
        plt = import_module("vmaf").plt
        _figure, ax = plt.subplots(figsize=arguments["plot_wh"], nrows=1, ncols=1)
        assets, results = _run_test(arguments, ax)
        bbox = {"facecolor": "white", "alpha": 0.5, "pad": 20}
        ax.annotate("Testing Set", xy=(0.1, 0.85), xycoords="axes fraction", bbox=bbox)
        plt.tight_layout()
        if arguments["save_plot_dir"] is None:
            DisplayConfig.show()
        else:
            DisplayConfig.show(write_to_dir=arguments["save_plot_dir"])
        return assets, results
    except ImportError:
        print_matplotlib_warning()
        return _run_test(arguments, None)
    except AssertionError:
        return _run_test(arguments, None)


def main():
    try:
        arguments = _parse_arguments()
    except _CliError as error:
        return error.exit_code
    _assets, results = _run_with_plot(arguments)
    if arguments["print_result"]:
        for result in results:
            print(result)
            print("")
    return 0


if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
