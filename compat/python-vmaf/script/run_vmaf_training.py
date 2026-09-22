#!/usr/bin/env python3

import matplotlib

matplotlib.use("Agg")

import os
import sys
from types import SimpleNamespace

import numpy as np

from vmaf.config import DisplayConfig
from vmaf.core.result_store import FileSystemResultStore
from vmaf.routine import print_matplotlib_warning, train_test_vmaf_on_dataset
from vmaf.tools.misc import cmd_option_exists, get_cmd_option, import_python_file
from vmaf.tools.stats import ListStats

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
        + os.path.basename(sys.argv[0])
        + " train_dataset_filepath feature_param_filepath model_param_filepath output_model_filepath "
        "[--subj-model subjective_model] [--cache-result] [--parallelize] [--save-plot plot_dir] "
        "[--processes processes]\n"
    )
    print("subjective_model:\n\t" + "\n\t".join(SUBJECTIVE_MODELS) + "\n")
    print("processes: must be an integer >=1")


# Pool method -> score aggregator; "mean" (and an unset --pool) uses np.mean.
POOL_METHOD_AGGREGATORS = {
    "harmonic_mean": ListStats.harmonic_mean,
    "min": np.min,
    "median": np.median,
    "perc5": ListStats.perc5,
    "perc10": ListStats.perc10,
    "perc20": ListStats.perc20,
}


def _find_subjective_model_class(subj_model):
    """Resolve --subj-model, defaulting to MLE_CO_AP2."""
    from sureal.subjective_model import SubjectiveModel

    if subj_model is not None:
        return SubjectiveModel.find_subclass(subj_model)
    else:
        return SubjectiveModel.find_subclass("MLE_CO_AP2")


def _parse_processes(processes):
    """Validate --processes; None when it was not given."""
    if processes is None:
        return None
    try:
        processes = int(processes)
    except ValueError:
        print("Input error: processes must be an integer")
    assert processes >= 1
    return processes


def _parse_cmd_args():
    """Parse argv into an options namespace, or return the process exit code."""
    if len(sys.argv) < 5:
        print_usage()
        return 2

    try:
        train_dataset_filepath = sys.argv[1]
        feature_param_filepath = sys.argv[2]
        model_param_filepath = sys.argv[3]
        output_model_filepath = sys.argv[4]
    except ValueError:
        print_usage()
        return 2

    try:
        args = SimpleNamespace(
            train_dataset=import_python_file(train_dataset_filepath),
            feature_param=import_python_file(feature_param_filepath),
            model_param=import_python_file(model_param_filepath),
            output_model_filepath=output_model_filepath,
        )
    except Exception as e:
        print("Error: %s" % e)
        return 1

    args.cache_result = cmd_option_exists(sys.argv, 3, len(sys.argv), "--cache-result")
    args.parallelize = cmd_option_exists(sys.argv, 3, len(sys.argv), "--parallelize")
    args.processes = get_cmd_option(sys.argv, 3, len(sys.argv), "--processes")
    args.suppress_plot = cmd_option_exists(sys.argv, 3, len(sys.argv), "--suppress-plot")

    args.pool_method = get_cmd_option(sys.argv, 3, len(sys.argv), "--pool")
    if not (args.pool_method is None or args.pool_method in POOL_METHODS):
        print("--pool can only have option among {}".format(", ".join(POOL_METHODS)))
        return 2

    subj_model = get_cmd_option(sys.argv, 3, len(sys.argv), "--subj-model")
    try:
        args.subj_model_class = _find_subjective_model_class(subj_model)
    except Exception as e:
        print("Error: %s" % e)
        return 1

    args.save_plot_dir = get_cmd_option(sys.argv, 3, len(sys.argv), "--save-plot")

    return args


def _train(args, train_ax, result_store, aggregate_method, logger):
    """One train_test_vmaf_on_dataset call; train_ax is None when not plotting."""
    train_test_vmaf_on_dataset(
        train_dataset=args.train_dataset,
        test_dataset=None,
        feature_param=args.feature_param,
        model_param=args.model_param,
        train_ax=train_ax,
        test_ax=None,
        result_store=result_store,
        parallelize=args.parallelize,
        logger=logger,
        output_model_filepath=args.output_model_filepath,
        aggregate_method=aggregate_method,
        subj_model_class=args.subj_model_class,
        processes=args.processes,
    )


def _train_with_plot(args, result_store, aggregate_method, logger):
    """Train, plotting unless --suppress-plot is set or matplotlib is missing."""
    try:
        if args.suppress_plot:
            raise AssertionError

        from vmaf import plt

        fig, ax = plt.subplots(figsize=(5, 5), nrows=1, ncols=1)

        _train(args, ax, result_store, aggregate_method, logger)

        bbox = {"facecolor": "white", "alpha": 0.5, "pad": 20}
        ax.annotate("Training Set", xy=(0.1, 0.85), xycoords="axes fraction", bbox=bbox)

        # ax.set_xlim([-10, 110])
        # ax.set_ylim([-10, 110])

        plt.tight_layout()

        if args.save_plot_dir is None:
            DisplayConfig.show()
        else:
            DisplayConfig.show(write_to_dir=args.save_plot_dir)

    except ImportError:
        print_matplotlib_warning()
        _train(args, None, result_store, aggregate_method, logger)
    except AssertionError:
        _train(args, None, result_store, aggregate_method, logger)


def main():

    args = _parse_cmd_args()
    if isinstance(args, int):
        return args

    if args.cache_result:
        result_store = FileSystemResultStore()
    else:
        result_store = None

    args.processes = _parse_processes(args.processes)

    # pooling
    aggregate_method = POOL_METHOD_AGGREGATORS.get(args.pool_method, np.mean)

    logger = None

    _train_with_plot(args, result_store, aggregate_method, logger)

    return 0


if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
