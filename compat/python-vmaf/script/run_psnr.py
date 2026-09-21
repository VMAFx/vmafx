#!/usr/bin/env python3

import os
import sys
from types import SimpleNamespace

import numpy as np

from vmaf.config import VmafConfig
from vmaf.core.asset import Asset
from vmaf.core.quality_runner import PsnrQualityRunner
from vmaf.tools.misc import get_cmd_option
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
        "usage: %s fmt width height ref_path dis_path [--out-fmt out_fmt]\n"
        % os.path.basename(sys.argv[0])
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


def _parse_cmd_args():
    """Parse and validate argv; returns None once a usage or error message is printed."""
    if len(sys.argv) < 6:
        print_usage()
        return None

    try:
        fmt = sys.argv[1]
        width = int(sys.argv[2])
        height = int(sys.argv[3])
        ref_path = sys.argv[4]
        dis_path = sys.argv[5]
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

    out_fmt = get_cmd_option(sys.argv, 6, len(sys.argv), "--out-fmt")
    if not (out_fmt is None or out_fmt == "xml" or out_fmt == "json" or out_fmt == "text"):
        print_usage()
        return None

    pool_method = get_cmd_option(sys.argv, 6, len(sys.argv), "--pool")
    if not (pool_method is None or pool_method in POOL_METHODS):
        print("--pool can only have option among {}".format(", ".join(POOL_METHODS)))
        return None

    return SimpleNamespace(
        fmt=fmt,
        width=width,
        height=height,
        ref_path=ref_path,
        dis_path=dis_path,
        out_fmt=out_fmt,
        pool_method=pool_method,
    )


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
        content_id=0,
        asset_id=0,
        workdir_root=VmafConfig.workdir_path(),
        ref_path=args.ref_path,
        dis_path=args.dis_path,
        asset_dict={"width": args.width, "height": args.height, "yuv_type": args.fmt},
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

    # pooling
    _apply_pool_method(result, args.pool_method)

    # output
    _print_result(result, args.out_fmt)

    return 0


if __name__ == "__main__":
    ret = main()
    sys.exit(ret)
