#!/usr/bin/env python3

import matplotlib

matplotlib.use("Agg")

import os
import re
import sys
from types import SimpleNamespace

import numpy as np

from vmaf.config import DisplayConfig
from vmaf.core.cambi_quality_runner import (  # noqa: F401  registration side-effect
    CambiQualityRunner,
)
from vmaf.core.matlab_quality_runner import (  # noqa: F401  registration side-effect
    SpEEDMatlabQualityRunner,
    STMADQualityRunner,
    StrredOptQualityRunner,
    StrredQualityRunner,
)
from vmaf.core.quality_runner import BootstrapVmafQualityRunner, QualityRunner, VmafQualityRunner
from vmaf.core.result_store import FileSystemResultStore
from vmaf.routine import print_matplotlib_warning, run_test_on_dataset
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
    quality_runner_types = ["VMAF", "PSNR", "SSIM", "MS_SSIM", "..."]
    print(
        "usage: "
        + os.path.basename(sys.argv[0])
        + " quality_type test_dataset_filepath [--vmaf-model VMAF_model_path] "
        "[--vmaf-phone-model] [--subj-model subjective_model] [--cache-result] "
        "[--parallelize] [--print-result] [--save-plot plot_dir] [--plot-wh plot_wh] "
        "[--processes processes]\n"
    )
    print("quality_type:\n\t" + "\n\t".join(quality_runner_types) + "\n")
    print("subjective_model:\n\t" + "\n\t".join(SUBJECTIVE_MODELS) + "\n")
    print("plot_wh: plot width and height in inches, example: 5x5 (default)")
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


def _parse_plot_wh(plot_wh):
    """Parse a WxH plot size; None when --plot-wh was not given."""
    if plot_wh is None:
        return None
    mo = re.match(r"([0-9]+)x([0-9]+)", plot_wh)
    assert mo is not None
    w = mo.group(1)
    h = mo.group(2)
    w = int(w)
    h = int(h)
    return (w, h)


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
    if len(sys.argv) < 3:
        print_usage()
        return 2

    try:
        quality_type = sys.argv[1]
        test_dataset_filepath = sys.argv[2]
    except ValueError:
        print_usage()
        return 2

    args = SimpleNamespace(
        quality_type=quality_type,
        test_dataset_filepath=test_dataset_filepath,
        vmaf_model_path=get_cmd_option(sys.argv, 3, len(sys.argv), "--vmaf-model"),
        cache_result=cmd_option_exists(sys.argv, 3, len(sys.argv), "--cache-result"),
        parallelize=cmd_option_exists(sys.argv, 3, len(sys.argv), "--parallelize"),
        processes=get_cmd_option(sys.argv, 3, len(sys.argv), "--processes"),
        print_result=cmd_option_exists(sys.argv, 3, len(sys.argv), "--print-result"),
        suppress_plot=cmd_option_exists(sys.argv, 3, len(sys.argv), "--suppress-plot"),
        vmaf_phone_model=cmd_option_exists(sys.argv, 3, len(sys.argv), "--vmaf-phone-model"),
        save_plot_dir=get_cmd_option(sys.argv, 3, len(sys.argv), "--save-plot"),
    )

    args.pool_method = get_cmd_option(sys.argv, 3, len(sys.argv), "--pool")
    if not (args.pool_method is None or args.pool_method in POOL_METHODS):
        print("--pool can only have option among {}".format(", ".join(POOL_METHODS)))
        return 2

    subj_model = get_cmd_option(sys.argv, 3, len(sys.argv), "--subj-model")
    try:
        args.subj_model_class = _find_subjective_model_class(subj_model)
    except Exception as e:
        print("Error: " + str(e))
        return 1

    try:
        args.plot_wh = _parse_plot_wh(get_cmd_option(sys.argv, 3, len(sys.argv), "--plot-wh"))
    except Exception as e:
        print("Error: plot_wh must be in the format of WxH, example: 5x5")
        return 1

    return args


def _resolve_runner_class(args):
    """Find the QualityRunner subclass, or return the process exit code."""
    try:
        runner_class = QualityRunner.find_subclass(args.quality_type)
    except Exception as e:
        print("Error: " + str(e))
        return 1

    vmaf_runner_classes = (VmafQualityRunner, BootstrapVmafQualityRunner)

    if args.vmaf_model_path is not None and runner_class not in vmaf_runner_classes:
        print("Input error: only quality_type of VMAF accepts --vmaf-model.")
        print_usage()
        return 2

    if args.vmaf_phone_model and runner_class not in vmaf_runner_classes:
        print("Input error: only quality_type of VMAF accepts --vmaf-phone-model.")
        print_usage()
        return 2

    return runner_class


def _run_test(test_dataset, runner_class, ax, args, result_store, aggregate_method):
    """One run_test_on_dataset call with the flag-derived keyword arguments."""
    return run_test_on_dataset(
        test_dataset,
        runner_class,
        ax,
        result_store,
        args.vmaf_model_path,
        parallelize=args.parallelize,
        aggregate_method=aggregate_method,
        subj_model_class=args.subj_model_class,
        enable_transform_score=True if args.vmaf_phone_model else None,
        processes=args.processes,
    )


def _run_dataset(test_dataset, runner_class, args, result_store, aggregate_method):
    """Run the test set, plotting unless --suppress-plot or matplotlib is missing."""
    try:
        if args.suppress_plot:
            raise AssertionError

        from vmaf import plt

        plot_wh = args.plot_wh if args.plot_wh is not None else (5, 5)
        fig, ax = plt.subplots(figsize=plot_wh, nrows=1, ncols=1)

        assets, results = _run_test(
            test_dataset, runner_class, ax, args, result_store, aggregate_method
        )

        bbox = {"facecolor": "white", "alpha": 0.5, "pad": 20}
        ax.annotate("Testing Set", xy=(0.1, 0.85), xycoords="axes fraction", bbox=bbox)

        # ax.set_xlim([-10, 110])
        # ax.set_ylim([-10, 110])

        plt.tight_layout()

        if args.save_plot_dir is None:
            DisplayConfig.show()
        else:
            DisplayConfig.show(write_to_dir=args.save_plot_dir)

    except ImportError:
        print_matplotlib_warning()
        assets, results = _run_test(
            test_dataset, runner_class, None, args, result_store, aggregate_method
        )
    except AssertionError:
        assets, results = _run_test(
            test_dataset, runner_class, None, args, result_store, aggregate_method
        )

    return assets, results


def main():
    args = _parse_cmd_args()
    if isinstance(args, int):
        return args

    runner_class = _resolve_runner_class(args)
    if isinstance(runner_class, int):
        return runner_class

    args.processes = _parse_processes(args.processes)

    try:
        test_dataset = import_python_file(args.test_dataset_filepath)
    except Exception as e:
        print("Error: " + str(e))
        return 1

    if args.cache_result:
        result_store = FileSystemResultStore()
    else:
        result_store = None

    # pooling
    aggregate_method = POOL_METHOD_AGGREGATORS.get(args.pool_method, np.mean)

    assets, results = _run_dataset(test_dataset, runner_class, args, result_store, aggregate_method)

    if args.print_result:
        for result in results:
            print(result)
            print("")

    return 0


if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
