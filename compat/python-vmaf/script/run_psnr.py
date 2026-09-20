#!/usr/bin/env python3

import sys
from pathlib import Path

import numpy as np

from vmaf.config import VmafConfig
from vmaf.core.asset import Asset
from vmaf.core.quality_runner import PsnrQualityRunner
from vmaf.tools.misc import get_cmd_option
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
        "usage: %s fmt width height ref_path dis_path [--out-fmt out_fmt]\n"
        % Path(sys.argv[0]).name
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
        ref_path = sys.argv[4]
        dis_path = sys.argv[5]
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
    out_fmt = get_cmd_option(sys.argv, 6, len(sys.argv), "--out-fmt")
    if not (out_fmt is None or out_fmt in {"xml", "json", "text"}):
        _usage_error()
    pool_method = get_cmd_option(sys.argv, 6, len(sys.argv), "--pool")
    if not (pool_method is None or pool_method in POOL_METHODS):
        print("--pool can only have option among {}".format(", ".join(POOL_METHODS)))
        raise _CliError
    return fmt, width, height, ref_path, dis_path, out_fmt, pool_method


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


def main():
    try:
        fmt, width, height, ref_path, dis_path, out_fmt, pool_method = _parse_arguments()
    except _CliError:
        return 2
    asset = Asset(
        dataset="cmd",
        content_id=0,
        asset_id=0,
        workdir_root=VmafConfig.workdir_path(),
        ref_path=ref_path,
        dis_path=dis_path,
        asset_dict={"width": width, "height": height, "yuv_type": fmt},
    )
    assets = [asset]

    runner_class = PsnrQualityRunner

    runner = runner_class(
        assets,
        None,
        fifo_mode=True,
        delete_workdir=True,
        result_store=None,
        optional_dict=None,
        optional_dict2=None,
    )

    # run
    runner.run()
    result = runner.results[0]

    _set_pool_method(result, pool_method)
    _print_result(result, out_fmt)
    return 0


if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
