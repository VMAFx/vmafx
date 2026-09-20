#!/usr/bin/env python3

from importlib import import_module
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import sys

import numpy as np

from vmaf.config import DisplayConfig
from vmaf.core.result_store import FileSystemResultStore
from vmaf.routine import print_matplotlib_warning, train_test_vmaf_on_dataset
from vmaf.tools.misc import cmd_option_exists, get_cmd_option, import_python_file
from vmaf.tools.stats import ListStats

_COMPARISON_VALUE_5 = 5

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
    print(
        "usage: "
        + Path(sys.argv[0]).name
        + " train_dataset_filepath feature_param_filepath model_param_filepath output_model_filepath "
        "[--subj-model subjective_model] [--cache-result] [--parallelize] [--save-plot plot_dir] "
        "[--processes processes]\n"
    )
    print("subjective_model:\n\t" + "\n\t".join(SUBJECTIVE_MODELS) + "\n")
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


def _import_training_inputs(dataset_path, feature_path, model_path):
    try:
        return (
            import_python_file(dataset_path),
            import_python_file(feature_path),
            import_python_file(model_path),
        )
    except Exception as error:
        print(f"Error: {error}")
        raise _CliError(1) from error


def _parse_arguments():
    if len(sys.argv) < _COMPARISON_VALUE_5:
        print_usage()
        raise _CliError(2)
    output_model_filepath = sys.argv[4]
    train_dataset, feature_param, model_param = _import_training_inputs(*sys.argv[1:4])
    cache_result = cmd_option_exists(sys.argv, 3, len(sys.argv), "--cache-result")
    parallelize = cmd_option_exists(sys.argv, 3, len(sys.argv), "--parallelize")
    processes = get_cmd_option(sys.argv, 3, len(sys.argv), "--processes")
    suppress_plot = cmd_option_exists(sys.argv, 3, len(sys.argv), "--suppress-plot")

    pool_method = get_cmd_option(sys.argv, 3, len(sys.argv), "--pool")
    if not (pool_method is None or pool_method in POOL_METHODS):
        print("--pool can only have option among {}".format(", ".join(POOL_METHODS)))
        raise _CliError(2)
    subj_model = get_cmd_option(sys.argv, 3, len(sys.argv), "--subj-model")
    save_plot_dir = get_cmd_option(sys.argv, 3, len(sys.argv), "--save-plot")
    return {
        "train_dataset": train_dataset,
        "feature_param": feature_param,
        "model_param": model_param,
        "output_model_filepath": output_model_filepath,
        "result_store": FileSystemResultStore() if cache_result else None,
        "parallelize": parallelize,
        "processes": _process_count(processes),
        "suppress_plot": suppress_plot,
        "aggregate_method": _aggregate_method(pool_method),
        "subj_model_class": _subjective_model_class(subj_model),
        "save_plot_dir": save_plot_dir,
    }


def _run_training(arguments, ax):
    return train_test_vmaf_on_dataset(
        train_dataset=arguments["train_dataset"],
        test_dataset=None,
        feature_param=arguments["feature_param"],
        model_param=arguments["model_param"],
        train_ax=ax,
        test_ax=None,
        result_store=arguments["result_store"],
        parallelize=arguments["parallelize"],
        logger=None,
        output_model_filepath=arguments["output_model_filepath"],
        aggregate_method=arguments["aggregate_method"],
        subj_model_class=arguments["subj_model_class"],
        processes=arguments["processes"],
    )


def _run_with_plot(arguments):
    if arguments["suppress_plot"]:
        return _run_training(arguments, None)
    try:
        plt = import_module("vmaf").plt
        _figure, ax = plt.subplots(figsize=(5, 5), nrows=1, ncols=1)
        result = _run_training(arguments, ax)
        bbox = {"facecolor": "white", "alpha": 0.5, "pad": 20}
        ax.annotate("Training Set", xy=(0.1, 0.85), xycoords="axes fraction", bbox=bbox)
        plt.tight_layout()
        if arguments["save_plot_dir"] is None:
            DisplayConfig.show()
        else:
            DisplayConfig.show(write_to_dir=arguments["save_plot_dir"])
        return result
    except ImportError:
        print_matplotlib_warning()
        return _run_training(arguments, None)
    except AssertionError:
        return _run_training(arguments, None)


def main():
    try:
        arguments = _parse_arguments()
    except _CliError as error:
        return error.exit_code
    _run_with_plot(arguments)
    return 0


if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
