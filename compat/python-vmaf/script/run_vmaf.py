#!/usr/bin/env python3

from importlib import import_module
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import sys

import numpy as np

from vmaf.config import DisplayConfig, VmafConfig
from vmaf.core.asset import Asset
from vmaf.core.quality_runner import VmafQualityRunner
from vmaf.tools.misc import cmd_option_exists, get_cmd_option, get_file_name_without_extension
from vmaf.tools.stats import ListStats

_COMPARISON_VALUE_6 = 6

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
        + Path(sys.argv[0]).name
        + " fmt width height ref_path dis_path [--model model_path] [--out-fmt out_fmt] "
        "[--phone-model] [--ci] [--save-plot plot_dir]\n"
    )
    print("fmt:\n\t" + "\n\t".join(FMTS) + "\n")
    print("out_fmt:\n\t" + "\n\t".join(OUT_FMTS) + "\n")


class _CliError(Exception):
    pass


def _usage_error():
    print_usage()
    raise _CliError


def _parse_arguments():
    if len(sys.argv) < _COMPARISON_VALUE_6:
        _usage_error()
    try:
        fmt = sys.argv[1]
        width = int(sys.argv[2])
        height = int(sys.argv[3])
        ref_file = sys.argv[4]
        dis_file = sys.argv[5]
    except ValueError as error:
        print_usage()
        raise _CliError from error
    if width < 0 or height < 0:
        print(
            "width and height must be non-negative, but are {w} and {h}".format(w=width, h=height)
        )
        _usage_error()
    if fmt not in FMTS:
        _usage_error()
    model_path = get_cmd_option(sys.argv, 6, len(sys.argv), "--model")
    out_fmt = get_cmd_option(sys.argv, 6, len(sys.argv), "--out-fmt")
    if not (out_fmt is None or out_fmt in {"xml", "json", "text"}):
        _usage_error()
    pool_method = get_cmd_option(sys.argv, 6, len(sys.argv), "--pool")
    if not (pool_method is None or pool_method in POOL_METHODS):
        print("--pool can only have option among {}".format(", ".join(POOL_METHODS)))
        raise _CliError
    show_local_explanation = cmd_option_exists(sys.argv, 6, len(sys.argv), "--local-explain")
    phone_model = cmd_option_exists(sys.argv, 6, len(sys.argv), "--phone-model")
    enable_conf_interval = cmd_option_exists(sys.argv, 6, len(sys.argv), "--ci")
    save_plot_dir = get_cmd_option(sys.argv, 6, len(sys.argv), "--save-plot")
    if show_local_explanation and enable_conf_interval:
        print("cannot set both --local-explain and --ci flags")
        raise _CliError
    return {
        "fmt": fmt,
        "width": width,
        "height": height,
        "ref_file": ref_file,
        "dis_file": dis_file,
        "model_path": model_path,
        "out_fmt": out_fmt,
        "pool_method": pool_method,
        "show_local_explanation": show_local_explanation,
        "phone_model": phone_model,
        "enable_conf_interval": enable_conf_interval,
        "save_plot_dir": save_plot_dir,
    }


def _runner_class(show_local_explanation, enable_conf_interval):
    if show_local_explanation:
        return import_module("vmaf.core.quality_runner_extra").VmafQualityRunnerWithLocalExplainer
    if enable_conf_interval:
        return import_module("vmaf.core.quality_runner").BootstrapVmafQualityRunner
    return VmafQualityRunner


def _set_pool_method(result, pool_method):
    methods = {
        "harmonic_mean": ListStats.harmonic_mean,
        "min": np.min,
        "median": np.median,
        "perc5": ListStats.perc5,
        "perc10": ListStats.perc10,
        "perc20": ListStats.perc20,
    }
    if pool_method in methods:
        result.set_score_aggregate_method(methods[pool_method])


def _print_result(result, out_fmt):
    outputs = {"xml": result.to_xml, "json": result.to_json}
    print(outputs[out_fmt]() if out_fmt in outputs else str(result))


def _show_local_explanation(runner, result, save_plot_dir):
    runner.show_local_explanations([result])
    if save_plot_dir is None:
        DisplayConfig.show()
    else:
        DisplayConfig.show(write_to_dir=save_plot_dir)


def main():
    try:
        arguments = _parse_arguments()
    except _CliError:
        return 2
    asset = Asset(
        dataset="cmd",
        content_id=abs(hash(get_file_name_without_extension(arguments["ref_file"]))) % (10**16),
        asset_id=abs(hash(get_file_name_without_extension(arguments["ref_file"]))) % (10**16),
        workdir_root=VmafConfig.workdir_path(),
        ref_path=arguments["ref_file"],
        dis_path=arguments["dis_file"],
        asset_dict={
            "width": arguments["width"],
            "height": arguments["height"],
            "yuv_type": arguments["fmt"],
        },
    )
    assets = [asset]
    runner_class = _runner_class(
        arguments["show_local_explanation"], arguments["enable_conf_interval"]
    )
    optional_dict = (
        None if arguments["model_path"] is None else {"model_filepath": arguments["model_path"]}
    )
    if arguments["phone_model"]:
        if optional_dict is None:
            optional_dict = {}
        optional_dict["enable_transform_score"] = True
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

    _set_pool_method(result, arguments["pool_method"])
    _print_result(result, arguments["out_fmt"])
    if arguments["show_local_explanation"]:
        _show_local_explanation(runner, result, arguments["save_plot_dir"])
    return 0


if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
