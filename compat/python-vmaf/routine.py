import os
from types import SimpleNamespace

import numpy as np
import pandas

from vmaf import plt
from vmaf.config import DisplayConfig, VmafConfig
from vmaf.core.asset import Asset
from vmaf.core.cross_validation import ModelCrossValidation
from vmaf.core.feature_assembler import FeatureAssembler
from vmaf.core.local_explainer import LocalExplainer
from vmaf.core.quality_runner import VmafQualityRunner, VmafQualityRunnerModelMixin
from vmaf.core.result_store import FileSystemResultStore
from vmaf.core.train_test_model import ClassifierMixin, RegressorMixin, TrainTestModel
from vmaf.tools.exceptions import CalibrationError
from vmaf.tools.misc import (
    _import_dataset_and_filter,
    close_logger,
    get_file_name_without_extension,
    get_stdout_logger,
    import_python_file,
    indices,
)

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


_DATASET_LEVEL_FIELDS = (
    "width",
    "height",
    "yuv_fmt",
    "quality_width",
    "quality_height",
    "resampling_type",
    "crop_cmd",
    "pad_cmd",
    "workfile_yuv_type",
    "duration_sec",
    "fps",
    "start_frame",
    "end_frame",
)


def _dataset_defaults(dataset):
    """Dataset-level overrides; each entry is None when the dataset omits it."""
    return SimpleNamespace(**{name: getattr(dataset, name, None) for name in _DATASET_LEVEL_FIELDS})


def _resolve_groundtruth(dis_video, groundtruth_key):
    """The groundtruth score: an explicit key wins, else dmos, then mos, then groundtruth."""
    if groundtruth_key is not None:
        return dis_video[groundtruth_key]
    for key in ("dmos", "mos", "groundtruth"):
        if key in dis_video:
            return dis_video[key]
    return None


def _override_or_dis(dataset_value, dis_video, key):
    """Dataset-level override, else the distorted video's own value, else None."""
    if dataset_value is not None:
        return dataset_value
    return dis_video[key] if key in dis_video else None


def _resolve_dimension(dataset_value, ref_video, dis_video, key):
    """Frame width/height: dataset override, else whichever side declares it.

    When both sides declare it they have to agree; that assert is the dataset's
    only guard against a reference/distorted geometry mismatch.
    """
    if dataset_value is not None:
        return dataset_value
    in_ref = key in ref_video
    in_dis = key in dis_video
    if in_ref and not in_dis:
        return ref_video[key]
    if in_dis and not in_ref:
        return dis_video[key]
    if in_ref and in_dis:
        assert ref_video[key] == dis_video[key]
        return ref_video[key]
    return None


def _resolve_cmd_pair(dataset_value, ref_video, dis_video, key):
    """(ref, dis) crop/pad command pair; a dataset-level command applies to both."""
    if dataset_value is not None:
        return dataset_value, dataset_value
    ref_cmd = ref_video[key] if key in ref_video else None
    dis_cmd = dis_video[key] if key in dis_video else None
    return ref_cmd, dis_cmd


def _resolve_asset_fields(defaults, ref_video, dis_video, groundtruth):
    """Every per-asset field with the dataset-level overrides already applied."""
    ref_yuv_fmt = defaults.yuv_fmt if defaults.yuv_fmt is not None else ref_video["yuv_fmt"]
    fields = SimpleNamespace(
        ref_yuv_fmt=ref_yuv_fmt,
        dis_yuv_fmt=dis_video["yuv_fmt"] if "yuv_fmt" in dis_video else ref_yuv_fmt,
        groundtruth=groundtruth,
        raw_groundtruth=dis_video["os"] if "os" in dis_video else None,
        groundtruth_std=dis_video["groundtruth_std"] if "groundtruth_std" in dis_video else None,
        rebuf_indices=dis_video["rebuf_indices"] if "rebuf_indices" in dis_video else None,
        width=_resolve_dimension(defaults.width, ref_video, dis_video, "width"),
        height=_resolve_dimension(defaults.height, ref_video, dis_video, "height"),
        quality_width=_override_or_dis(defaults.quality_width, dis_video, "quality_width"),
        quality_height=_override_or_dis(defaults.quality_height, dis_video, "quality_height"),
        resampling_type=_override_or_dis(defaults.resampling_type, dis_video, "resampling_type"),
        duration_sec=_override_or_dis(defaults.duration_sec, dis_video, "duration_sec"),
        fps=_override_or_dis(defaults.fps, dis_video, "fps"),
        start_frame=_override_or_dis(defaults.start_frame, dis_video, "start_frame"),
        end_frame=_override_or_dis(defaults.end_frame, dis_video, "end_frame"),
        workfile_yuv_type=defaults.workfile_yuv_type,
    )
    fields.ref_crop_cmd, fields.dis_crop_cmd = _resolve_cmd_pair(
        defaults.crop_cmd, ref_video, dis_video, "crop_cmd"
    )
    fields.ref_pad_cmd, fields.dis_pad_cmd = _resolve_cmd_pair(
        defaults.pad_cmd, ref_video, dis_video, "pad_cmd"
    )
    return fields


def _copy_present(target, sources):
    """Copy ``source[src_key]`` into ``target[dst_key]`` for every key a source has.

    The tuples are walked in order so the resulting key order matches what the
    original straight-line sequence of ``if key in video`` checks produced.
    """
    for source, dst_key, src_key in sources:
        if src_key in source:
            target[dst_key] = source[src_key]


def _build_asset_dict(fields, ref_video, dis_video):
    """The Asset ``asset_dict``; fields the dataset does not set are left out."""
    asset_dict = {"ref_yuv_type": fields.ref_yuv_fmt, "dis_yuv_type": fields.dis_yuv_fmt}
    if fields.width is not None:
        if asset_dict["ref_yuv_type"] != "notyuv":
            asset_dict["ref_width"] = fields.width
        if asset_dict["dis_yuv_type"] != "notyuv":
            asset_dict["dis_width"] = fields.width
    if fields.height is not None:
        if asset_dict["ref_yuv_type"] != "notyuv":
            asset_dict["ref_height"] = fields.height
        if asset_dict["dis_yuv_type"] != "notyuv":
            asset_dict["dis_height"] = fields.height

    for asset_key, value in (
        ("groundtruth", fields.groundtruth),
        ("raw_groundtruth", fields.raw_groundtruth),
        ("groundtruth_std", fields.groundtruth_std),
        ("quality_width", fields.quality_width),
        ("quality_height", fields.quality_height),
        ("resampling_type", fields.resampling_type),
        ("ref_crop_cmd", fields.ref_crop_cmd),
        ("dis_crop_cmd", fields.dis_crop_cmd),
        ("ref_pad_cmd", fields.ref_pad_cmd),
        ("dis_pad_cmd", fields.dis_pad_cmd),
        ("duration_sec", fields.duration_sec),
        ("workfile_yuv_type", fields.workfile_yuv_type),
        ("rebuf_indices", fields.rebuf_indices),
        ("fps", fields.fps),
        ("start_frame", fields.start_frame),
        ("end_frame", fields.end_frame),
    ):
        if value is not None:
            asset_dict[asset_key] = value

    _copy_present(
        asset_dict,
        (
            (ref_video, "ref_start_frame", "ref_start_frame"),
            (dis_video, "dis_start_frame", "dis_start_frame"),
            (ref_video, "ref_end_frame", "ref_end_frame"),
            (dis_video, "dis_end_frame", "dis_end_frame"),
            (dis_video, "dis_enc_width", "enc_width"),
            (dis_video, "dis_enc_height", "enc_height"),
            (dis_video, "dis_enc_bitdepth", "enc_bitdepth"),
        ),
    )
    return asset_dict


def read_dataset(dataset, **kwargs):

    groundtruth_key = kwargs["groundtruth_key"] if "groundtruth_key" in kwargs else None
    skip_asset_with_none_groundtruth = (
        kwargs["skip_asset_with_none_groundtruth"]
        if "skip_asset_with_none_groundtruth" in kwargs
        else False
    )
    content_ids = kwargs["content_ids"] if "content_ids" in kwargs else None
    asset_ids = kwargs["asset_ids"] if "asset_ids" in kwargs else None
    workdir_root = kwargs["workdir_root"] if "workdir_root" in kwargs else VmafConfig.workdir_path()

    # asserts, can add more to the list...
    assert hasattr(dataset, "dataset_name")
    assert hasattr(dataset, "ref_videos")
    assert hasattr(dataset, "dis_videos")

    assert hasattr(dataset, "yuv_fmt") or all(
        ["yuv_fmt" in ref_video for ref_video in dataset.ref_videos]
    )

    data_set_name = dataset.dataset_name
    ref_videos = dataset.ref_videos
    dis_videos = dataset.dis_videos
    defaults = _dataset_defaults(dataset)

    ref_dict = {}  # dictionary of content_id -> path for ref videos
    for ref_video in ref_videos:
        ref_dict[ref_video["content_id"]] = ref_video

    assets = []
    for dis_video in dis_videos:
        if content_ids is not None and dis_video["content_id"] not in content_ids:
            continue

        if asset_ids is not None and dis_video["asset_id"] not in asset_ids:
            continue

        groundtruth = _resolve_groundtruth(dis_video, groundtruth_key)

        ref_video = ref_dict[dis_video["content_id"]]
        fields = _resolve_asset_fields(defaults, ref_video, dis_video, groundtruth)
        asset_dict = _build_asset_dict(fields, ref_video, dis_video)

        if groundtruth is None and skip_asset_with_none_groundtruth:
            pass
        else:
            asset = Asset(
                dataset=data_set_name,
                content_id=dis_video["content_id"],
                asset_id=dis_video["asset_id"],
                workdir_root=workdir_root,
                ref_path=ref_video["path"],
                dis_path=dis_video["path"],
                asset_dict=asset_dict,
            )
            assets.append(asset)

    return assets


def _run_one_side(test_dataset, runner_class, opts):
    """Score `test_dataset` with one of the two runners under comparison.

    Mirrors the positional call this comparison has always made into
    `run_test_on_dataset`: no axis, no model filepath.
    """
    return run_test_on_dataset(
        test_dataset,
        runner_class,
        None,
        opts.result_store,
        None,
        opts.parallelize,
        opts.fifo_mode,
        opts.aggregate_method,
        opts.model_type,
        **opts.runner_kwargs,
    )


def _correlation_stat(
    df: pandas.DataFrame,
    xcol: str,
    ycol: str,
) -> dict:
    """PLCC and SROCC of `ycol` against `xcol`."""
    plcc = df[ycol].corr(df[xcol], method="pearson")
    srocc = df[ycol].corr(df[xcol], method="spearman")
    return {"plcc": plcc, "srocc": srocc}


def _paired_prediction_frame(test_dataset, first_runner_class, second_runner_class, opts):
    """Run both runners and join their predictions to the shared groundtruth.

    One row per asset. Asserts the two runs line up asset-for-asset and agree
    on each asset's groundtruth before the pair is recorded.
    """
    first_test_assets, first_results = _run_one_side(test_dataset, first_runner_class, opts)
    second_test_assets, second_results = _run_one_side(test_dataset, second_runner_class, opts)

    # collect data to list of dictionaries
    ds = list()
    assert (
        len(first_test_assets)
        == len(second_test_assets)
        == len(first_results)
        == len(second_results)
    )
    for first_test_asset, first_result, second_test_asset, second_result in zip(
        first_test_assets, first_results, second_test_assets, second_results
    ):
        assert first_test_asset.groundtruth is not None
        assert second_test_asset.groundtruth is not None
        assert first_test_asset.groundtruth == second_test_asset.groundtruth
        d = {
            "groundtruth": first_test_asset.groundtruth,
            "first_prediction": first_result[first_runner_class.get_score_key()],
            "second_prediction": second_result[second_runner_class.get_score_key()],
        }
        ds.append(d)
    return pandas.DataFrame(ds)


def _bootstrap_correlations(df, num_resample, seed_resample):
    """Resample `df` with replacement `num_resample` times, collecting both correlations.

    Returns `(plcc_first, plcc_second, srocc_first, srocc_second)`. `df.sample()`
    draws from the global numpy RNG, so the seed is set here, immediately before
    the loop, to keep the draw sequence reproducible for a given `seed_resample`.
    """
    np.random.seed(seed_resample)
    xs = list()
    ys = list()
    xs2 = list()
    ys2 = list()
    for _ in range(num_resample):
        dfb = df.sample(n=df.shape[0], replace=True)
        d_stat_first = _correlation_stat(dfb, "groundtruth", "first_prediction")
        d_stat_second = _correlation_stat(dfb, "groundtruth", "second_prediction")
        xs.append(d_stat_first["plcc"])
        ys.append(d_stat_second["plcc"])
        xs2.append(d_stat_first["srocc"])
        ys2.append(d_stat_second["srocc"])
    return xs, ys, xs2, ys2


def _plot_resampled_correlation(ax, xs, ys, label, first, second, ci95, title_gap):
    """Scatter one runner's resampled correlation against the other's, with the y=x line.

    `title_gap` is the separator between the runner pair and the CI in the title.
    The PLCC and SROCC titles have always differed here -- one space against two
    -- so the caller supplies it rather than this helper imposing one spelling.
    """
    ci95_x, ci95_y, ci95_diff = ci95
    ax.scatter(xs, ys, alpha=0.2, label=label)
    ax.plot([min(xs), max(xs)], [min(xs), max(xs)], "-r")
    ax.set_xlabel(f"{first.TYPE} 95%-CI: [{ci95_x[0]:.4f}, {ci95_x[1]:.4f}]")
    ax.set_ylabel(f"{second.TYPE} 95%-CI: [{ci95_y[0]:.4f}, {ci95_y[1]:.4f}]")
    ax.set_title(
        f"({second.TYPE} - {first.TYPE}){title_gap}95%-CI: [{ci95_diff[0]:.4f}, {ci95_diff[1]:.4f}]"
    )
    ax.grid()
    ax.legend()


def _summarize_metric(xs, ys, ax, label, first, second, title_gap):
    """95% percentile intervals for one metric, plotted onto `ax` when there is one.

    Returns `(ci95_first, ci95_second, ci95_diff)`.
    """
    ci95_x = [np.percentile(xs, 2.5), np.percentile(xs, 97.5)]
    ci95_y = [np.percentile(ys, 2.5), np.percentile(ys, 97.5)]
    diffs = np.array(ys) - np.array(xs)
    ci95_diff = [np.percentile(diffs, 2.5), np.percentile(diffs, 97.5)]
    ci95 = (ci95_x, ci95_y, ci95_diff)
    if ax is not None:
        _plot_resampled_correlation(ax, xs, ys, label, first, second, ci95, title_gap)
    return ci95


def compare_two_quality_runners_on_dataset(
    test_dataset,
    first_runner_class,
    second_runner_class,
    result_store,
    parallelize=True,
    fifo_mode=True,
    aggregate_method=np.mean,
    type="regressor",
    num_resample=1000,
    seed_resample=None,
    ax_plcc=None,
    ax_srocc=None,
    **kwargs,
):
    opts = SimpleNamespace(
        result_store=result_store,
        parallelize=parallelize,
        fifo_mode=fifo_mode,
        aggregate_method=aggregate_method,
        model_type=type,
        runner_kwargs=kwargs,
    )
    df = _paired_prediction_frame(test_dataset, first_runner_class, second_runner_class, opts)

    # bootstrapping
    xs, ys, xs2, ys2 = _bootstrap_correlations(df, num_resample, seed_resample)
    ci95_xs, ci95_ys, ci95_diffs = _summarize_metric(
        xs, ys, ax_plcc, "PLCC with resampling", first_runner_class, second_runner_class, " "
    )
    ci95_xs2, ci95_ys2, ci95_diffs2 = _summarize_metric(
        xs2, ys2, ax_srocc, "SROCC with resampling", first_runner_class, second_runner_class, "  "
    )

    return {
        "plcc": list(zip(xs, ys)),
        "srocc": list(zip(xs2, ys2)),
        "plcc_ci95_first": ci95_xs,
        "plcc_ci95_second": ci95_ys,
        "plcc_ci95_diff": ci95_diffs,
        "srocc_ci95_first": ci95_xs2,
        "srocc_ci95_second": ci95_ys2,
        "srocc_ci95_diff": ci95_diffs2,
    }


def run_test_on_dataset(
    test_dataset,
    runner_class,
    ax,
    result_store,
    model_filepath,
    parallelize=True,
    fifo_mode=True,
    aggregate_method=np.mean,
    type="regressor",
    allow_uncalibrated=False,
    **kwargs,
):

    test_assets = read_dataset(test_dataset, **kwargs)
    test_raw_assets = None
    try:
        for test_asset in test_assets:
            assert test_asset.groundtruth is not None
    except AssertionError:
        # no groundtruth, try to do subjective modeling
        from sureal.dataset_reader import RawDatasetReader
        from sureal.subjective_model import DmosModel

        subj_model_class = (
            kwargs["subj_model_class"]
            if "subj_model_class" in kwargs and kwargs["subj_model_class"] is not None
            else DmosModel
        )
        dataset_reader_class = (
            kwargs["dataset_reader_class"] if "dataset_reader_class" in kwargs else RawDatasetReader
        )
        subjective_model = subj_model_class(dataset_reader_class(test_dataset))
        subjective_model.run_modeling(**kwargs)
        test_dataset_aggregate = subjective_model.to_aggregated_dataset(**kwargs)
        test_raw_assets = test_assets
        test_assets = read_dataset(test_dataset_aggregate, **kwargs)

    optional_dict = kwargs["optional_dict"] if "optional_dict" in kwargs else None

    if model_filepath is not None:
        if not optional_dict:
            optional_dict = {}
        optional_dict["model_filepath"] = model_filepath
        if "model_720_filepath" in kwargs and kwargs["model_720_filepath"] is not None:
            optional_dict["720model_filepath"] = kwargs["model_720_filepath"]
        if "model_480_filepath" in kwargs and kwargs["model_480_filepath"] is not None:
            optional_dict["480model_filepath"] = kwargs["model_480_filepath"]
        if "model_2160_filepath" in kwargs and kwargs["model_2160_filepath"] is not None:
            optional_dict["2160model_filepath"] = kwargs["model_2160_filepath"]

    if "enable_transform_score" in kwargs and kwargs["enable_transform_score"] is not None:
        if not optional_dict:
            optional_dict = {}
        optional_dict["enable_transform_score"] = kwargs["enable_transform_score"]

    if "disable_clip_score" in kwargs and kwargs["disable_clip_score"] is not None:
        if not optional_dict:
            optional_dict = {}
        optional_dict["disable_clip_score"] = kwargs["disable_clip_score"]

    if "subsample" in kwargs and kwargs["subsample"] is not None:
        if not optional_dict:
            optional_dict = {}
        optional_dict["subsample"] = kwargs["subsample"]

    if "additional_optional_dict" in kwargs and kwargs["additional_optional_dict"] is not None:
        assert isinstance(kwargs["additional_optional_dict"], dict)
        if not optional_dict:
            optional_dict = {}
        optional_dict.update(kwargs["additional_optional_dict"])

    if "processes" in kwargs and kwargs["processes"] is not None:
        assert isinstance(kwargs["processes"], int)
        processes = kwargs["processes"]
    else:
        processes = None
    if processes is not None:
        assert parallelize is True, "if processes is not None, parallelize must be True"

    # run
    runner = runner_class(
        test_assets,
        None,
        fifo_mode=fifo_mode,
        delete_workdir=True,
        result_store=result_store,
        optional_dict=optional_dict,
        optional_dict2=None,
    )
    runner.run(parallelize=parallelize, processes=processes)
    results = runner.results

    for result in results:
        result.set_score_aggregate_method(aggregate_method)

    try:
        model_type = runner.get_train_test_model_class()
    except (AttributeError, NotImplementedError):
        # AttributeError: runner subclass does not expose
        # get_train_test_model_class. NotImplementedError: subclass marks the
        # method as abstract. Narrowed from bare 'except' so KeyboardInterrupt /
        # SystemExit propagate. (CodeQL py/catch-base-exception)
        if type == "regressor":
            model_type = RegressorMixin
        elif type == "classifier":
            model_type = ClassifierMixin
        else:
            assert False

    split_test_indices_for_perf_ci = (
        kwargs["split_test_indices_for_perf_ci"]
        if "split_test_indices_for_perf_ci" in kwargs
        else False
    )

    # plot
    groundtruths = list(map(lambda asset: asset.groundtruth, test_assets))
    predictions = list(map(lambda result: result[runner_class.get_score_key()], results))
    raw_grountruths = (
        None
        if test_raw_assets is None
        else list(map(lambda asset: asset.raw_groundtruth, test_raw_assets))
    )
    groundtruths_std = (
        None if test_assets is None else list(map(lambda asset: asset.groundtruth_std, test_assets))
    )
    try:
        stats_kwargs = {
            "ys_label_raw": raw_grountruths,
            "ys_label_stddev": groundtruths_std,
            "split_test_indices_for_perf_ci": split_test_indices_for_perf_ci,
        }
        num_models = 1
        has_bootstrap_predictions = all(
            hasattr(runner_class, key_getter)
            for key_getter in (
                "get_bagging_score_key",
                "get_stddev_score_key",
                "get_ci95_low_score_key",
                "get_ci95_high_score_key",
                "get_all_models_score_key",
            )
        )
        if has_bootstrap_predictions:
            predictions_bagging = list(
                map(lambda result: result[runner_class.get_bagging_score_key()], results)
            )
            predictions_stddev = list(
                map(lambda result: result[runner_class.get_stddev_score_key()], results)
            )
            predictions_ci95_low = list(
                map(lambda result: result[runner_class.get_ci95_low_score_key()], results)
            )
            predictions_ci95_high = list(
                map(lambda result: result[runner_class.get_ci95_high_score_key()], results)
            )
            predictions_all_models = list(
                map(lambda result: result[runner_class.get_all_models_score_key()], results)
            )

            # need to revert the list of lists, so that the outer list has the predictions for each model separately
            predictions_all_models = np.array(predictions_all_models).T.tolist()
            num_models = np.shape(predictions_all_models)[0]
            stats_kwargs.update(
                {
                    "ys_label_pred_bagging": predictions_bagging,
                    "ys_label_pred_stddev": predictions_stddev,
                    "ys_label_pred_ci95_low": predictions_ci95_low,
                    "ys_label_pred_ci95_high": predictions_ci95_high,
                    "ys_label_pred_all_models": predictions_all_models,
                }
            )

        # ClassifierMixin.get_stats only accepts positional (ys_label,
        # ys_label_pred); the regressor-specific kwargs below are
        # invalid for it (CodeQL py/call/wrong-named-argument). Branch
        # explicitly so the classifier path doesn't unconditionally raise
        # TypeError into the broad fallback handler.
        if model_type is ClassifierMixin:
            stats = model_type.get_stats(groundtruths, predictions)
        else:
            stats = model_type.get_stats(groundtruths, predictions, **stats_kwargs)
    except Exception as exc:
        if not allow_uncalibrated:
            raise CalibrationError(
                "Stats calculation failed and allow_uncalibrated=False. "
                "Pass allow_uncalibrated=True to fall back to default "
                "(uncalibrated) normalisation stats. "
                "Original error: {}".format(exc)
            ) from exc
        print(
            "Warning: stats calculation failed, falling back to default "
            "(uncalibrated) normalisation stats. "
            "Pass allow_uncalibrated=True to suppress this check. "
            "Original error: {}".format(exc)
        )
        # Fallback stats: same classifier/regressor split as above.
        if model_type is ClassifierMixin:
            stats = model_type.get_stats(groundtruths, predictions)
        else:
            stats = model_type.get_stats(
                groundtruths,
                predictions,
                ys_label_raw=raw_grountruths,
                ys_label_stddev=groundtruths_std,
                split_test_indices_for_perf_ci=split_test_indices_for_perf_ci,
            )

    print("Stats on testing data: {}".format(model_type.format_stats_for_print(stats)))

    # printing stats if multiple models are present
    if (
        "SRCC_across_model_distribution" in stats
        and "PCC_across_model_distribution" in stats
        and "RMSE_across_model_distribution" in stats
    ):
        print(
            "Stats on testing data (across multiple models, using all test indices): {}".format(
                model_type.format_across_model_stats_for_print(
                    model_type.extract_across_model_stats(stats)
                )
            )
        )

    if split_test_indices_for_perf_ci:
        print(
            "Stats on testing data (single model, multiple test sets): {}".format(
                model_type.format_stats_across_test_splits_for_print(
                    model_type.extract_across_test_splits_stats(stats)
                )
            )
        )

    if ax is not None:
        content_ids = list(map(lambda asset: asset.content_id, test_assets))

        if "point_label" in kwargs and kwargs["point_label"] is not None:
            if kwargs["point_label"] == "asset_id":
                point_labels = list(map(lambda asset: asset.asset_id, test_assets))
            elif kwargs["point_label"] == "dis_path":
                point_labels = list(
                    map(lambda asset: get_file_name_without_extension(asset.dis_path), test_assets)
                )
            else:
                raise AssertionError("Unknown point_label {}".format(kwargs["point_label"]))
        else:
            point_labels = None

        model_type.plot_scatter(
            ax, stats, content_ids=content_ids, point_labels=point_labels, **kwargs
        )
        ax.set_xlabel("True Score")
        ax.set_ylabel("Predicted Score")
        ax.grid()
        ax.set_title(
            "{runner}{num_models}\n{stats}".format(
                runner=runner_class.TYPE,
                stats=model_type.format_stats_for_plot(stats),
                num_models=", {} models".format(num_models) if num_models > 1 else "",
            )
        )

    return test_assets, results


def print_matplotlib_warning():
    print(
        "Warning: cannot import matplotlib, no picture displayed. "
        "If you are on Mac OS and have installed matplotlib, you "
        "possibly need to run: \nsudo pip uninstall python-dateutil \n"
        "sudo pip install python-dateutil==2.2 \n"
        "Refer to: https://stackoverflow.com/questions/27630114/matplotlib-issue-on-os-x-importerror-cannot-import-name-thread"
    )


def train_test_vmaf_on_dataset(
    train_dataset,
    test_dataset,
    feature_param,
    model_param,
    train_ax,
    test_ax,
    result_store,
    logger=None,
    fifo_mode=True,
    output_model_filepath=None,
    aggregate_method=np.mean,
    **kwargs,
):

    train_assets = read_dataset(train_dataset, **kwargs)
    train_raw_assets = None
    try:
        for train_asset in train_assets:
            assert train_asset.groundtruth is not None
    except AssertionError:
        # no groundtruth, try to do subjective modeling
        from sureal.dataset_reader import RawDatasetReader
        from sureal.subjective_model import DmosModel

        subj_model_class = (
            kwargs["subj_model_class"]
            if "subj_model_class" in kwargs and kwargs["subj_model_class"] is not None
            else DmosModel
        )
        dataset_reader_class = (
            kwargs["dataset_reader_class"] if "dataset_reader_class" in kwargs else RawDatasetReader
        )
        subjective_model = subj_model_class(dataset_reader_class(train_dataset))
        subjective_model.run_modeling(**kwargs)
        train_dataset_aggregate = subjective_model.to_aggregated_dataset(**kwargs)
        train_raw_assets = train_assets
        train_assets = read_dataset(train_dataset_aggregate, **kwargs)

    parallelize = kwargs["parallelize"] if "parallelize" in kwargs else True
    isinstance(parallelize, bool)

    processes = kwargs["processes"] if "processes" in kwargs else None
    if processes is not None:
        assert isinstance(processes, int) and processes > 0
    if processes is not None:
        assert parallelize is True, "if processes is not None, parallelize must be True"

    assert hasattr(feature_param, "feature_dict")
    feature_dict = feature_param.feature_dict
    feature_option_dict = (
        feature_param.feature_optional_dict
        if hasattr(feature_param, "feature_optional_dict")
        else None
    )

    train_fassembler = FeatureAssembler(
        feature_dict=feature_dict,
        feature_option_dict=feature_option_dict,
        assets=train_assets,
        logger=logger,
        fifo_mode=fifo_mode,
        delete_workdir=True,
        result_store=result_store,
        optional_dict=None,  # WARNING: feature param not passed
        optional_dict2=None,
        parallelize=parallelize,
        processes=processes,
    )
    train_fassembler.run()
    train_features = train_fassembler.results

    for result in train_features:
        result.set_score_aggregate_method(aggregate_method)

    model_type = model_param.model_type
    model_param_dict = model_param.model_param_dict

    model_class = TrainTestModel.find_subclass(model_type)

    train_xys = model_class.get_xys_from_results(train_features)
    train_xs = model_class.get_xs_from_results(train_features)
    train_ys = model_class.get_ys_from_results(train_features)

    model = model_class(model_param_dict, logger)

    model.train(train_xys, feature_option_dict=feature_option_dict, **kwargs)

    # append additional information to model before saving, so that
    # VmafQualityRunner can read and process
    model.append_info(
        "feature_dict", feature_param.feature_dict
    )  # need feature_dict so that VmafQualityRunner knows how to call FeatureAssembler
    if "score_clip" in model_param_dict:
        VmafQualityRunner.set_clip_score(model, model_param_dict["score_clip"])
    if "score_transform" in model_param_dict:
        VmafQualityRunner.set_transform_score(model, model_param_dict["score_transform"])

    train_ys_pred = VmafQualityRunner.predict_with_model(model, train_xs, **kwargs)["ys_pred"]

    raw_groundtruths = (
        None
        if train_raw_assets is None
        else list(map(lambda asset: asset.raw_groundtruth, train_raw_assets))
    )

    train_stats = model.get_stats(train_ys["label"], train_ys_pred, ys_label_raw=raw_groundtruths)

    log = "Stats on training data: {}".format(model.format_stats_for_print(train_stats))
    if logger:
        logger.info(log)
    else:
        print(log)

    # save model
    if output_model_filepath is not None:
        format = os.path.splitext(output_model_filepath)[1]
        supported_formats = [".pkl", ".json"]
        VmafQualityRunnerModelMixin._assert_extension_format(supported_formats, format)
        if ".pkl" in format:
            model.to_file(output_model_filepath, format="pkl")
        elif ".json" in format:
            model.to_file(output_model_filepath, format="json", combined=True)
        else:
            assert False

    if train_ax is not None:
        train_content_ids = list(map(lambda asset: asset.content_id, train_assets))
        model_class.plot_scatter(train_ax, train_stats, content_ids=train_content_ids)

        train_ax.set_xlabel("True Score")
        train_ax.set_ylabel("Predicted Score")
        train_ax.grid()
        train_ax.set_title(
            "Dataset: {dataset}, Model: {model}\n{stats}".format(
                dataset=train_dataset.dataset_name,
                model=model.model_id,
                stats=model_class.format_stats_for_plot(train_stats),
            )
        )

    # === test model on test dataset ===

    if test_dataset is None:
        test_assets = None
        test_stats = None
        test_fassembler = None
    else:
        test_assets = read_dataset(test_dataset, **kwargs)
        test_raw_assets = None
        try:
            for test_asset in test_assets:
                assert test_asset.groundtruth is not None
        except AssertionError:
            # no groundtruth, try to do subjective modeling
            from sureal.dataset_reader import RawDatasetReader
            from sureal.subjective_model import DmosModel

            subj_model_class = (
                kwargs["subj_model_class"]
                if "subj_model_class" in kwargs and kwargs["subj_model_class"] is not None
                else DmosModel
            )
            dataset_reader_class = (
                kwargs["dataset_reader_class"]
                if "dataset_reader_class" in kwargs
                else RawDatasetReader
            )
            subjective_model = subj_model_class(dataset_reader_class(test_dataset))
            subjective_model.run_modeling(**kwargs)
            test_dataset_aggregate = subjective_model.to_aggregated_dataset(**kwargs)
            test_raw_assets = test_assets
            test_assets = read_dataset(test_dataset_aggregate, **kwargs)

        test_fassembler = FeatureAssembler(
            feature_dict=feature_dict,
            feature_option_dict=feature_option_dict,
            assets=test_assets,
            logger=logger,
            fifo_mode=fifo_mode,
            delete_workdir=True,
            result_store=result_store,
            optional_dict=None,  # WARNING: feature param not passed
            optional_dict2=None,
            parallelize=parallelize,
        )
        test_fassembler.run()
        test_features = test_fassembler.results

        for result in test_features:
            result.set_score_aggregate_method(aggregate_method)

        test_xs = model_class.get_xs_from_results(test_features)
        test_ys = model_class.get_ys_from_results(test_features)

        test_ys_pred = VmafQualityRunner.predict_with_model(model, test_xs, **kwargs)["ys_pred"]

        raw_groundtruths = (
            None
            if test_raw_assets is None
            else list(map(lambda asset: asset.raw_groundtruth, test_raw_assets))
        )

        test_stats = model.get_stats(test_ys["label"], test_ys_pred, ys_label_raw=raw_groundtruths)

        log = "Stats on testing data: {}".format(model_class.format_stats_for_print(test_stats))
        if logger:
            logger.info(log)
        else:
            print(log)

        if test_ax is not None:
            test_content_ids = list(map(lambda asset: asset.content_id, test_assets))
            model_class.plot_scatter(test_ax, test_stats, content_ids=test_content_ids)
            test_ax.set_xlabel("True Score")
            test_ax.set_ylabel("Predicted Score")
            test_ax.grid()
            test_ax.set_title(
                "Dataset: {dataset}, Model: {model}\n{stats}".format(
                    dataset=test_dataset.dataset_name,
                    model=model.model_id,
                    stats=model_class.format_stats_for_plot(test_stats),
                )
            )

    return (
        train_fassembler,
        train_assets,
        train_stats,
        test_fassembler,
        test_assets,
        test_stats,
        model,
    )


def construct_kfold_list(assets, contentid_groups):
    # construct cross validation kfold input list
    content_ids = list(map(lambda asset: asset.content_id, assets))
    kfold = []
    for curr_content_group in contentid_groups:
        curr_indices = indices(content_ids, lambda x: x in curr_content_group)
        kfold.append(curr_indices)
    return kfold


def cv_on_dataset(
    dataset,
    feature_param,
    model_param,
    ax,
    result_store,
    contentid_groups,
    logger=None,
    aggregate_method=np.mean,
):

    assets = read_dataset(dataset)
    kfold = construct_kfold_list(assets, contentid_groups)

    # Mirror VmafQualityRunner's contract: prefer the per-extractor
    # options dict declared on the feature param (`feature_optional_dict`)
    # so cv_on_dataset and explain_model_on_dataset feed identical
    # options to FeatureAssembler as a real prediction run would. T7-32.
    feature_option_dict = (
        feature_param.feature_optional_dict
        if hasattr(feature_param, "feature_optional_dict")
        else None
    )

    fassembler = FeatureAssembler(
        feature_dict=feature_param.feature_dict,
        feature_option_dict=feature_option_dict,
        assets=assets,
        logger=logger,
        delete_workdir=True,
        result_store=result_store,
        optional_dict=None,  # WARNING: feature param not passed
        optional_dict2=None,
        parallelize=True,
        fifo_mode=True,
        # parallelize=False, fifo_mode=False, # VQM
    )
    fassembler.run()
    results = fassembler.results

    for result in results:
        result.set_score_aggregate_method(aggregate_method)

    model_class = TrainTestModel.find_subclass(model_param.model_type)
    # run nested kfold cv for each combintation
    cv_output = ModelCrossValidation.run_kfold_cross_validation(
        model_class,
        model_param.model_param_dict,
        results,
        kfold,
        logger=logger,
    )

    print("Feature parameters: {}".format(feature_param.feature_dict))
    print("Model type: {}".format(model_param.model_type))
    print("Model parameters: {}".format(model_param.model_param_dict))
    print("Stats: {}".format(model_class.format_stats_for_print(cv_output["aggr_stats"])))

    if ax is not None:
        model_class.plot_scatter(ax, cv_output["aggr_stats"], content_ids=cv_output["contentids"])
        ax.set_xlabel("True Score")
        ax.set_ylabel("Predicted Score")
        ax.grid()
        ax.set_title(
            "Dataset: {dataset}, Model: {model},\n{stats}".format(
                dataset=dataset.dataset_name,
                model=model_param.model_type,
                stats=model_class.format_stats_for_plot(cv_output["aggr_stats"]),
            )
        )

    return assets, cv_output


def run_remove_results_for_dataset(result_store, dataset, executor_class):
    assets = read_dataset(dataset)
    executor = executor_class(assets=assets, logger=None, result_store=result_store)
    executor.remove_results()


def run_vmaf_cv(
    train_dataset_filepath,
    test_dataset_filepath,
    param_filepath,
    output_model_filepath=None,
    **kwargs,
):

    result_store_dir = (
        kwargs["result_store_dir"]
        if "result_store_dir" in kwargs
        else VmafConfig.file_result_store_path()
    )

    parallelize = kwargs["parallelize"] if "parallelize" in kwargs else True
    isinstance(parallelize, bool)

    logger = get_stdout_logger()
    result_store = FileSystemResultStore(result_store_dir)

    train_dataset = import_python_file(train_dataset_filepath)
    test_dataset = (
        import_python_file(test_dataset_filepath) if test_dataset_filepath is not None else None
    )

    param = import_python_file(param_filepath)

    # === plot scatter ===

    nrows = 1
    ncols = 2
    fig, axs = plt.subplots(figsize=(5 * ncols, 5 * nrows), nrows=nrows, ncols=ncols)

    train_test_vmaf_on_dataset(
        train_dataset,
        test_dataset,
        param,
        param,
        axs[0],
        axs[1],
        result_store,
        logger=None,
        output_model_filepath=output_model_filepath,
        **kwargs,
    )

    if "xlim" in kwargs:
        axs[0].set_xlim(kwargs["xlim"])
        axs[1].set_xlim(kwargs["xlim"])

    if "ylim" in kwargs:
        axs[0].set_ylim(kwargs["ylim"])
        axs[1].set_ylim(kwargs["ylim"])

    bbox = {"facecolor": "white", "alpha": 1, "pad": 20}
    axs[0].annotate("Training Set", xy=(0.1, 0.85), xycoords="axes fraction", bbox=bbox)
    axs[1].annotate("Testing Set", xy=(0.1, 0.85), xycoords="axes fraction", bbox=bbox)

    plt.tight_layout()

    # === clean up ===
    close_logger(logger)


def run_vmaf_kfold_cv(
    dataset_filepath,
    contentid_groups,
    param_filepath,
    aggregate_method,
    result_store_dir=VmafConfig.file_result_store_path(),
):

    logger = get_stdout_logger()
    result_store = FileSystemResultStore(result_store_dir)
    dataset = import_python_file(dataset_filepath)
    param = import_python_file(param_filepath)

    fig, ax = plt.subplots(figsize=(5, 5), nrows=1, ncols=1)

    cv_on_dataset(
        dataset, param, param, ax, result_store, contentid_groups, logger, aggregate_method
    )

    ax.set_xlim([0, 120])
    ax.set_ylim([0, 120])
    plt.tight_layout()

    # === clean up ===
    close_logger(logger)


def explain_model_on_dataset(
    model,
    test_assets_selected_indexs,
    test_dataset_filepath,
    result_store_dir=VmafConfig.file_result_store_path(),
):

    def print_assets(test_assets):
        print(
            "\n".join(
                map(
                    lambda tasset: "Asset {i}: {name}".format(
                        i=tasset[0], name=get_file_name_without_extension(tasset[1].dis_path)
                    ),
                    enumerate(test_assets),
                )
            )
        )

    test_dataset = import_python_file(test_dataset_filepath)
    test_assets = read_dataset(test_dataset)
    print_assets(test_assets)
    print("Assets selected for local explanation: {}".format(test_assets_selected_indexs))
    result_store = FileSystemResultStore(result_store_dir)
    test_assets = [test_assets[i] for i in test_assets_selected_indexs]
    # VmafQualityRunner reads `feature_opts_dicts` from the model's
    # serialised `model_dict` to pin per-extractor options at predict
    # time. Pass it through here too so explain_model_on_dataset's
    # FeatureAssembler matches the production prediction contract. T7-32.
    test_fassembler = FeatureAssembler(
        feature_dict=model.model_dict["feature_dict"],
        feature_option_dict=model.model_dict.get("feature_opts_dicts", None),
        assets=test_assets,
        logger=None,
        fifo_mode=True,
        delete_workdir=True,
        result_store=result_store,
        optional_dict=None,  # WARNING: feature param not passed
        optional_dict2=None,
        parallelize=True,
    )
    test_fassembler.run()
    test_feature_results = test_fassembler.results
    test_xs = model.get_xs_from_results(test_feature_results)
    test_ys = model.get_ys_from_results(test_feature_results)
    test_ys_pred = model.predict(test_xs)["ys_label_pred"]
    explainer = LocalExplainer(neighbor_samples=1000)
    test_exps = explainer.explain(model, test_xs)

    explainer.print_explanations(test_exps, assets=test_assets, ys=test_ys, ys_pred=test_ys_pred)
    explainer.plot_explanations(test_exps, assets=test_assets, ys=test_ys, ys_pred=test_ys_pred)
    DisplayConfig.show()


def generate_dataset_from_raw(raw_dataset_filepath, output_dataset_filepath, **kwargs):
    if raw_dataset_filepath:
        from sureal.dataset_reader import RawDatasetReader
        from sureal.subjective_model import DmosModel

        subj_model_class = kwargs["subj_model_class"] if "subj_model_class" in kwargs else DmosModel
        content_ids = kwargs["content_ids"] if "content_ids" in kwargs else None
        asset_ids = kwargs["asset_ids"] if "asset_ids" in kwargs else None
        dataset_reader_class = (
            kwargs["dataset_reader_class"] if "dataset_reader_class" in kwargs else RawDatasetReader
        )
        # Inlined `subj_model_class.from_dataset_file(...)`: sureal imports the
        # dataset with the `SourceFileLoader.load_module()` API that Python 3.15
        # removes, which is a fatal DeprecationWarning under ADR-1278. Import it
        # with the harness's own importlib-based loader and construct the reader
        # here — the same two steps `from_dataset_file()` performs. The reader
        # class follows the same `dataset_reader_class` kwarg the other
        # subjective-modeling call sites in this module already honour.
        dataset = _import_dataset_and_filter(
            raw_dataset_filepath, content_ids=content_ids, asset_ids=asset_ids
        )
        subjective_model = subj_model_class(dataset_reader_class(dataset))
        subjective_model.run_modeling(**kwargs)
        subjective_model.to_aggregated_dataset_file(output_dataset_filepath, **kwargs)

        # sureal's `write_out_dataset` serialises numpy scalars via repr().
        # numpy 2.x changed scalar __repr__ from "<value>" to
        # "np.float64(<value>)", which makes the generated Python dataset
        # file unimportable (NameError: name 'np' is not defined) unless
        # `np` is in scope at import time. Prepend the import so the
        # canonical `import_python_file(path)` consumer can load the
        # generated file without locale-/version-dependent surprises.
        # See ADR-0494.
        try:
            with open(output_dataset_filepath, "r") as f:
                body = f.read()
        except OSError:
            return
        if "np.float64(" in body or "np.int64(" in body:
            if not body.lstrip().startswith("import numpy"):
                with open(output_dataset_filepath, "w") as f:
                    f.write("import numpy as np  # injected by routine.generate_dataset_from_raw\n")
                    f.write(body)


def run_vmaf_cv_from_raw(
    train_dataset_raw_filepath,
    test_dataset_raw_filepath,
    param_filepath,
    output_model_filepath,
    **kwargs,
):
    if "train_quality_wh" in kwargs and kwargs["train_quality_wh"] is not None:
        train_quality_width, train_quality_height = kwargs["train_quality_wh"]
    else:
        train_quality_width = None
        train_quality_height = None

    if "test_quality_wh" in kwargs and kwargs["test_quality_wh"] is not None:
        test_quality_width, test_quality_height = kwargs["test_quality_wh"]
    else:
        test_quality_width = None
        test_quality_height = None

    if "train_transform_final" in kwargs and kwargs["train_transform_final"] is not None:
        train_transform_final = kwargs["train_transform_final"]
    else:
        train_transform_final = None

    if "test_transform_final" in kwargs and kwargs["test_transform_final"] is not None:
        test_transform_final = kwargs["test_transform_final"]
    else:
        test_transform_final = None

    workspace_path = (
        kwargs["workspace_path"] if "workspace_path" in kwargs else VmafConfig.workspace_path()
    )

    train_output_dataset_filepath = os.path.join(workspace_path, "dataset", "train_dataset.py")
    generate_dataset_from_raw(
        raw_dataset_filepath=train_dataset_raw_filepath,
        output_dataset_filepath=train_output_dataset_filepath,
        quality_width=train_quality_width,
        quality_height=train_quality_height,
        transform_final=train_transform_final,
        **kwargs,
    )

    test_output_dataset_filepath = (
        os.path.join(workspace_path, "dataset", "test_dataset.py")
        if test_dataset_raw_filepath is not None
        else None
    )
    generate_dataset_from_raw(
        raw_dataset_filepath=test_dataset_raw_filepath,
        output_dataset_filepath=test_output_dataset_filepath,
        quality_width=test_quality_width,
        quality_height=test_quality_height,
        transform_final=test_transform_final,
        **kwargs,
    )

    run_vmaf_cv(
        train_dataset_filepath=train_output_dataset_filepath,
        test_dataset_filepath=test_output_dataset_filepath,
        param_filepath=param_filepath,
        output_model_filepath=output_model_filepath,
        **kwargs,
    )
