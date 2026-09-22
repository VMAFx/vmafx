#!/usr/bin/env python3

import matplotlib

matplotlib.use("Agg")

import os
import sys
from types import SimpleNamespace

import numpy as np

from vmaf.config import DisplayConfig, VmafConfig
from vmaf.core.asset import Asset
from vmaf.core.quality_runner import VmafQualityRunner
from vmaf.tools.misc import cmd_option_exists, get_cmd_option, get_file_name_without_extension
from vmaf.tools.stats import ListStats

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"

FMTS = [
    "yuv420p",
    "yuv422p",
    "yuv444p",
    "yuv420p10le",
    "yuv422p10le",
    "yuv444p10le",
    "yuv420p12le",
    "yuv422p12le",
    "yuv444p12le",
    "yuv420p16le",
    "yuv422p16le",
    "yuv444p16le",
]
OUT_FMTS = ["text (default)", "xml", "json"]
POOL_METHODS = ["mean", "harmonic_mean", "min", "median", "perc5", "perc10", "perc20"]


def print_usage():
    print(
        "usage: "
        + os.path.basename(sys.argv[0])
        + " fmt width height ref_path dis_path [--model model_path] [--out-fmt out_fmt] "
        "[--phone-model] [--ci] [--save-plot plot_dir]\n"
    )
    print("fmt:\n\t" + "\n\t".join(FMTS) + "\n")
    print("out_fmt:\n\t" + "\n\t".join(OUT_FMTS) + "\n")


# Pool method -> score aggregator. "mean" (and an unset --pool) keeps the
# Result's own default aggregator, so it is deliberately absent from the table.
POOL_METHOD_AGGREGATORS = {
    "harmonic_mean": ListStats.harmonic_mean,
    "min": np.min,
    "median": np.median,
    "perc5": ListStats.perc5,
    "perc10": ListStats.perc10,
    "perc20": ListStats.perc20,
}


def _parse_positional_args():
    """The five positional arguments; returns None once usage has been printed."""
    if len(sys.argv) < 6:
        print_usage()
        return None

    try:
        fmt = sys.argv[1]
        width = int(sys.argv[2])
        height = int(sys.argv[3])
        ref_file = sys.argv[4]
        dis_file = sys.argv[5]
    except ValueError:
        print_usage()
        return None

    if width < 0 or height < 0:
        print(
            "width and height must be non-negative, but are {w} and {h}".format(w=width, h=height)
        )
        print_usage()
        return None

    if fmt not in FMTS:
        print_usage()
        return None

    return fmt, width, height, ref_file, dis_file


def _parse_cmd_args():
    """Parse and validate argv; returns None once a usage or error message is printed."""
    positional = _parse_positional_args()
    if positional is None:
        return None
    fmt, width, height, ref_file, dis_file = positional

    model_path = get_cmd_option(sys.argv, 6, len(sys.argv), "--model")

    out_fmt = get_cmd_option(sys.argv, 6, len(sys.argv), "--out-fmt")
    if not (out_fmt is None or out_fmt == "xml" or out_fmt == "json" or out_fmt == "text"):
        print_usage()
        return None

    pool_method = get_cmd_option(sys.argv, 6, len(sys.argv), "--pool")
    if not (pool_method is None or pool_method in POOL_METHODS):
        print("--pool can only have option among {}".format(", ".join(POOL_METHODS)))
        return None

    show_local_explanation = cmd_option_exists(sys.argv, 6, len(sys.argv), "--local-explain")

    phone_model = cmd_option_exists(sys.argv, 6, len(sys.argv), "--phone-model")

    enable_conf_interval = cmd_option_exists(sys.argv, 6, len(sys.argv), "--ci")

    save_plot_dir = get_cmd_option(sys.argv, 6, len(sys.argv), "--save-plot")

    if show_local_explanation and enable_conf_interval:
        print("cannot set both --local-explain and --ci flags")
        return None

    return SimpleNamespace(
        fmt=fmt,
        width=width,
        height=height,
        ref_file=ref_file,
        dis_file=dis_file,
        model_path=model_path,
        out_fmt=out_fmt,
        pool_method=pool_method,
        show_local_explanation=show_local_explanation,
        phone_model=phone_model,
        enable_conf_interval=enable_conf_interval,
        save_plot_dir=save_plot_dir,
    )


def _select_runner_class(show_local_explanation, enable_conf_interval):
    """Pick the runner the flags ask for; both imports stay lazy as before."""
    if show_local_explanation:
        from vmaf.core.quality_runner_extra import VmafQualityRunnerWithLocalExplainer

        return VmafQualityRunnerWithLocalExplainer
    if enable_conf_interval:
        from vmaf.core.quality_runner import BootstrapVmafQualityRunner

        return BootstrapVmafQualityRunner
    return VmafQualityRunner


def _build_optional_dict(model_path, phone_model):
    """Runner options for --model / --phone-model; None when neither was given."""
    if model_path is None:
        optional_dict = None
    else:
        optional_dict = {"model_filepath": model_path}

    if phone_model:
        if optional_dict is None:
            optional_dict = {}
        optional_dict["enable_transform_score"] = True

    return optional_dict


def _apply_pool_method(result, pool_method):
    """Override the score aggregator when --pool asked for something else."""
    aggregator = POOL_METHOD_AGGREGATORS.get(pool_method)
    if aggregator is not None:
        result.set_score_aggregate_method(aggregator)


def _print_result(result, out_fmt):
    """Print the result in the requested format; text is the default."""
    if out_fmt == "xml":
        print(result.to_xml())
    elif out_fmt == "json":
        print(result.to_json())
    else:  # None or 'text'
        print(str(result))


def main():
    args = _parse_cmd_args()
    if args is None:
        return 2

    asset = Asset(
        dataset="cmd",
        content_id=abs(hash(get_file_name_without_extension(args.ref_file))) % (10**16),
        asset_id=abs(hash(get_file_name_without_extension(args.ref_file))) % (10**16),
        workdir_root=VmafConfig.workdir_path(),
        ref_path=args.ref_file,
        dis_path=args.dis_file,
        asset_dict={"width": args.width, "height": args.height, "yuv_type": args.fmt},
    )
    assets = [asset]

    runner_class = _select_runner_class(args.show_local_explanation, args.enable_conf_interval)
    optional_dict = _build_optional_dict(args.model_path, args.phone_model)

    runner = runner_class(
        assets,
        None,
        fifo_mode=True,
        delete_workdir=True,
        result_store=None,
        optional_dict=optional_dict,
        optional_dict2=None,
    )

    # run
    runner.run()
    result = runner.results[0]

    # pooling
    _apply_pool_method(result, args.pool_method)

    # output
    _print_result(result, args.out_fmt)

    # local explanation
    if args.show_local_explanation:
        runner.show_local_explanations([result])

        if args.save_plot_dir is None:
            DisplayConfig.show()
        else:
            DisplayConfig.show(write_to_dir=args.save_plot_dir)

    return 0


if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
