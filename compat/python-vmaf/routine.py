from importlib import import_module
from pathlib import Path

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
    close_logger,
    get_file_name_without_extension,
    get_stdout_logger,
    import_python_file,
    indices,
)

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


_DATASET_OPTION_NAMES = (
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


def _dataset_options(dataset):
    return {name: getattr(dataset, name, None) for name in _DATASET_OPTION_NAMES}


def _groundtruth(dis_video, groundtruth_key):
    if groundtruth_key is not None:
        return dis_video[groundtruth_key]
    for key in ("dmos", "mos", "groundtruth"):
        if key in dis_video:
            return dis_video[key]
    return None


def _shared_dimension(default, ref_video, dis_video, name):
    if default is not None:
        return default
    ref_value = ref_video.get(name)
    dis_value = dis_video.get(name)
    if ref_value is not None and dis_value is not None:
        assert ref_value == dis_value
        return ref_value
    return ref_value if ref_value is not None else dis_value


def _dataset_or_video_option(dataset_options, video, name):
    value = dataset_options[name]
    return value if value is not None else video.get(name)


def _optional_asset_values(dataset_options, ref_video, dis_video, groundtruth):
    crop_cmd = dataset_options["crop_cmd"]
    pad_cmd = dataset_options["pad_cmd"]
    return {
        "groundtruth": groundtruth,
        "raw_groundtruth": dis_video.get("os"),
        "groundtruth_std": dis_video.get("groundtruth_std"),
        "quality_width": _dataset_or_video_option(dataset_options, dis_video, "quality_width"),
        "quality_height": _dataset_or_video_option(dataset_options, dis_video, "quality_height"),
        "resampling_type": _dataset_or_video_option(dataset_options, dis_video, "resampling_type"),
        "ref_crop_cmd": crop_cmd if crop_cmd is not None else ref_video.get("crop_cmd"),
        "dis_crop_cmd": crop_cmd if crop_cmd is not None else dis_video.get("crop_cmd"),
        "ref_pad_cmd": pad_cmd if pad_cmd is not None else ref_video.get("pad_cmd"),
        "dis_pad_cmd": pad_cmd if pad_cmd is not None else dis_video.get("pad_cmd"),
        "duration_sec": _dataset_or_video_option(dataset_options, dis_video, "duration_sec"),
        "workfile_yuv_type": dataset_options["workfile_yuv_type"],
        "rebuf_indices": dis_video.get("rebuf_indices"),
        "fps": _dataset_or_video_option(dataset_options, dis_video, "fps"),
        "start_frame": _dataset_or_video_option(dataset_options, dis_video, "start_frame"),
        "end_frame": _dataset_or_video_option(dataset_options, dis_video, "end_frame"),
        "ref_start_frame": ref_video.get("ref_start_frame"),
        "dis_start_frame": dis_video.get("dis_start_frame"),
        "ref_end_frame": ref_video.get("ref_end_frame"),
        "dis_end_frame": dis_video.get("dis_end_frame"),
        "dis_enc_width": dis_video.get("enc_width"),
        "dis_enc_height": dis_video.get("enc_height"),
        "dis_enc_bitdepth": dis_video.get("enc_bitdepth"),
    }


def _asset_dict(dataset_options, ref_video, dis_video, groundtruth):
    ref_yuv_type = dataset_options["yuv_fmt"]
    if ref_yuv_type is None:
        ref_yuv_type = ref_video["yuv_fmt"]
    dis_yuv_type = dis_video.get("yuv_fmt", ref_yuv_type)
    width = _shared_dimension(dataset_options["width"], ref_video, dis_video, "width")
    height = _shared_dimension(dataset_options["height"], ref_video, dis_video, "height")
    asset_dict = {"ref_yuv_type": ref_yuv_type, "dis_yuv_type": dis_yuv_type}
    if width is not None and ref_yuv_type != "notyuv":
        asset_dict["ref_width"] = width
    if width is not None and dis_yuv_type != "notyuv":
        asset_dict["dis_width"] = width
    if height is not None and ref_yuv_type != "notyuv":
        asset_dict["ref_height"] = height
    if height is not None and dis_yuv_type != "notyuv":
        asset_dict["dis_height"] = height

    values = _optional_asset_values(dataset_options, ref_video, dis_video, groundtruth)
    asset_dict.update({key: value for key, value in values.items() if value is not None})
    return asset_dict


def read_dataset(dataset, **kwargs):
    groundtruth_key = kwargs.get("groundtruth_key")
    skip_missing = kwargs.get("skip_asset_with_none_groundtruth", False)
    content_ids = kwargs.get("content_ids")
    asset_ids = kwargs.get("asset_ids")
    workdir_root = kwargs.get("workdir_root", VmafConfig.workdir_path())
    assert hasattr(dataset, "dataset_name")
    assert hasattr(dataset, "ref_videos")
    assert hasattr(dataset, "dis_videos")
    assert hasattr(dataset, "yuv_fmt") or all(
        "yuv_fmt" in ref_video for ref_video in dataset.ref_videos
    )
    ref_videos = {video["content_id"]: video for video in dataset.ref_videos}
    options = _dataset_options(dataset)
    assets = []
    for dis_video in dataset.dis_videos:
        if content_ids is not None and dis_video["content_id"] not in content_ids:
            continue
        if asset_ids is not None and dis_video["asset_id"] not in asset_ids:
            continue
        groundtruth = _groundtruth(dis_video, groundtruth_key)
        ref_video = ref_videos[dis_video["content_id"]]
        asset_dict = _asset_dict(options, ref_video, dis_video, groundtruth)
        if groundtruth is None and skip_missing:
            continue
        assets.append(
            Asset(
                dataset=dataset.dataset_name,
                content_id=dis_video["content_id"],
                asset_id=dis_video["asset_id"],
                workdir_root=workdir_root,
                ref_path=ref_video["path"],
                dis_path=dis_video["path"],
                asset_dict=asset_dict,
            )
        )
    return assets


def _correlation_stats(dataframe, prediction_column):
    groundtruth = dataframe["groundtruth"]
    prediction = dataframe[prediction_column]
    return prediction.corr(groundtruth, method="pearson"), prediction.corr(
        groundtruth, method="spearman"
    )


def _bootstrap_correlations(dataframe, num_resample, seed_resample):
    np.random.seed(seed_resample)
    plcc_first = []
    plcc_second = []
    srocc_first = []
    srocc_second = []
    for _ in range(num_resample):
        sample = dataframe.sample(n=dataframe.shape[0], replace=True)
        first_plcc, first_srocc = _correlation_stats(sample, "first_prediction")
        second_plcc, second_srocc = _correlation_stats(sample, "second_prediction")
        plcc_first.append(first_plcc)
        plcc_second.append(second_plcc)
        srocc_first.append(first_srocc)
        srocc_second.append(second_srocc)
    return plcc_first, plcc_second, srocc_first, srocc_second


def _ci95(values):
    return [np.percentile(values, 2.5), np.percentile(values, 97.5)]


def _plot_correlation_resampling(
    ax, first_values, second_values, first_runner_class, second_runner_class, label
):
    first_ci = _ci95(first_values)
    second_ci = _ci95(second_values)
    diff_ci = _ci95(np.array(second_values) - np.array(first_values))
    if ax is not None:
        ax.scatter(first_values, second_values, alpha=0.2, label=f"{label} with resampling")
        ax.plot(
            [min(first_values), max(first_values)],
            [min(first_values), max(first_values)],
            "-r",
        )
        ax.set_xlabel(f"{first_runner_class.TYPE} 95%-CI: [{first_ci[0]:.4f}, {first_ci[1]:.4f}]")
        ax.set_ylabel(
            f"{second_runner_class.TYPE} 95%-CI: [{second_ci[0]:.4f}, {second_ci[1]:.4f}]"
        )
        ax.set_title(
            f"({second_runner_class.TYPE} - {first_runner_class.TYPE}) 95%-CI: "
            f"[{diff_ci[0]:.4f}, {diff_ci[1]:.4f}]"
        )
        ax.grid()
        ax.legend()
    return first_ci, second_ci, diff_ci


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
    first_test_assets, first_results = run_test_on_dataset(
        test_dataset,
        first_runner_class,
        None,
        result_store,
        None,
        parallelize,
        fifo_mode,
        aggregate_method,
        type,
        **kwargs,
    )

    second_test_assets, second_results = run_test_on_dataset(
        test_dataset,
        second_runner_class,
        None,
        result_store,
        None,
        parallelize,
        fifo_mode,
        aggregate_method,
        type,
        **kwargs,
    )

    # collect data to list of dictionaries
    ds = []
    assert (
        len(first_test_assets)
        == len(second_test_assets)
        == len(first_results)
        == len(second_results)
    )
    for first_test_asset, first_result, second_test_asset, second_result in zip(
        first_test_assets, first_results, second_test_assets, second_results, strict=False
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
    df = pandas.DataFrame(ds)

    xs, ys, xs2, ys2 = _bootstrap_correlations(df, num_resample, seed_resample)
    ci95_xs, ci95_ys, ci95_diffs = _plot_correlation_resampling(
        ax_plcc, xs, ys, first_runner_class, second_runner_class, "PLCC"
    )
    ci95_xs2, ci95_ys2, ci95_diffs2 = _plot_correlation_resampling(
        ax_srocc, xs2, ys2, first_runner_class, second_runner_class, "SROCC"
    )

    return {
        "plcc": list(zip(xs, ys, strict=False)),
        "srocc": list(zip(xs2, ys2, strict=False)),
        "plcc_ci95_first": ci95_xs,
        "plcc_ci95_second": ci95_ys,
        "plcc_ci95_diff": ci95_diffs,
        "srocc_ci95_first": ci95_xs2,
        "srocc_ci95_second": ci95_ys2,
        "srocc_ci95_diff": ci95_diffs2,
    }


def _read_dataset_with_subjective_model(dataset, kwargs):
    assets = read_dataset(dataset, **kwargs)
    if all(asset.groundtruth is not None for asset in assets):
        return assets, None
    raw_dataset_reader = import_module("sureal.dataset_reader").RawDatasetReader
    dmos_model = import_module("sureal.subjective_model").DmosModel
    subjective_model_class = kwargs.get("subj_model_class") or dmos_model
    dataset_reader_class = kwargs.get("dataset_reader_class", raw_dataset_reader)
    subjective_model = subjective_model_class(dataset_reader_class(dataset))
    subjective_model.run_modeling(**kwargs)
    aggregate_dataset = subjective_model.to_aggregated_dataset(**kwargs)
    return read_dataset(aggregate_dataset, **kwargs), assets


def _test_runner_options(model_filepath, kwargs):
    options = kwargs.get("optional_dict")
    updates = {}
    if model_filepath is not None:
        updates["model_filepath"] = model_filepath
        model_paths = {
            "model_720_filepath": "720model_filepath",
            "model_480_filepath": "480model_filepath",
            "model_2160_filepath": "2160model_filepath",
        }
        updates.update(
            {
                option_name: kwargs[argument_name]
                for argument_name, option_name in model_paths.items()
                if kwargs.get(argument_name) is not None
            }
        )
    for name in ("enable_transform_score", "disable_clip_score", "subsample"):
        if kwargs.get(name) is not None:
            updates[name] = kwargs[name]
    additional = kwargs.get("additional_optional_dict")
    if additional is not None:
        assert isinstance(additional, dict)
        updates.update(additional)
    if updates:
        options = options or {}
        options.update(updates)
    return options


def _runner_model_type(runner, model_kind):
    try:
        return runner.get_train_test_model_class()
    except (AttributeError, NotImplementedError):
        if model_kind == "regressor":
            return RegressorMixin
        if model_kind == "classifier":
            return ClassifierMixin
        raise AssertionError() from None


def _bootstrap_prediction_stats(runner_class, results):
    key_getters = (
        "get_bagging_score_key",
        "get_stddev_score_key",
        "get_ci95_low_score_key",
        "get_ci95_high_score_key",
        "get_all_models_score_key",
    )
    if not all(hasattr(runner_class, key_getter) for key_getter in key_getters):
        return {}, 1
    all_models = [result[runner_class.get_all_models_score_key()] for result in results]
    all_models = np.array(all_models).T.tolist()
    return {
        "ys_label_pred_bagging": [
            result[runner_class.get_bagging_score_key()] for result in results
        ],
        "ys_label_pred_stddev": [result[runner_class.get_stddev_score_key()] for result in results],
        "ys_label_pred_ci95_low": [
            result[runner_class.get_ci95_low_score_key()] for result in results
        ],
        "ys_label_pred_ci95_high": [
            result[runner_class.get_ci95_high_score_key()] for result in results
        ],
        "ys_label_pred_all_models": all_models,
    }, np.shape(all_models)[0]


def _model_stats(model_type, labels, predictions, stats_kwargs):
    if model_type is ClassifierMixin:
        return model_type.get_stats(labels, predictions)
    return model_type.get_stats(labels, predictions, **stats_kwargs)


def _calculate_test_stats(
    model_type,
    runner_class,
    results,
    groundtruths,
    predictions,
    raw_groundtruths,
    groundtruths_std,
    split_test_indices_for_perf_ci,
    allow_uncalibrated,
):
    stats_kwargs = {
        "ys_label_raw": raw_groundtruths,
        "ys_label_stddev": groundtruths_std,
        "split_test_indices_for_perf_ci": split_test_indices_for_perf_ci,
    }
    bootstrap_stats, num_models = _bootstrap_prediction_stats(runner_class, results)
    stats_kwargs.update(bootstrap_stats)
    try:
        return _model_stats(model_type, groundtruths, predictions, stats_kwargs), num_models
    except Exception as exc:
        if not allow_uncalibrated:
            raise CalibrationError(
                "Stats calculation failed and allow_uncalibrated=False. "
                "Pass allow_uncalibrated=True to fall back to default "
                "(uncalibrated) normalisation stats. "
                f"Original error: {exc}"
            ) from exc
        print(
            "Warning: stats calculation failed, falling back to default "
            "(uncalibrated) normalisation stats. "
            "Pass allow_uncalibrated=True to suppress this check. "
            f"Original error: {exc}"
        )
        fallback_kwargs = {
            key: stats_kwargs[key]
            for key in (
                "ys_label_raw",
                "ys_label_stddev",
                "split_test_indices_for_perf_ci",
            )
        }
        return _model_stats(model_type, groundtruths, predictions, fallback_kwargs), num_models


def _print_test_stats(model_type, stats, split_test_indices_for_perf_ci):
    print("Stats on testing data: {}".format(model_type.format_stats_for_print(stats)))
    distribution_keys = (
        "SRCC_across_model_distribution",
        "PCC_across_model_distribution",
        "RMSE_across_model_distribution",
    )
    if all(key in stats for key in distribution_keys):
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


def _point_labels(test_assets, point_label):
    if point_label is None:
        return None
    if point_label == "asset_id":
        return [asset.asset_id for asset in test_assets]
    if point_label == "dis_path":
        return [get_file_name_without_extension(asset.dis_path) for asset in test_assets]
    raise AssertionError(f"Unknown point_label {point_label}")


def _plot_test_stats(ax, model_type, runner_class, test_assets, stats, num_models, kwargs):
    if ax is None:
        return
    content_ids = [asset.content_id for asset in test_assets]
    point_labels = _point_labels(test_assets, kwargs.get("point_label"))
    model_type.plot_scatter(ax, stats, content_ids=content_ids, point_labels=point_labels, **kwargs)
    ax.set_xlabel("True Score")
    ax.set_ylabel("Predicted Score")
    ax.grid()
    ax.set_title(
        "{runner}{num_models}\n{stats}".format(
            runner=runner_class.TYPE,
            stats=model_type.format_stats_for_plot(stats),
            num_models=f", {num_models} models" if num_models > 1 else "",
        )
    )


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
    test_assets, test_raw_assets = _read_dataset_with_subjective_model(test_dataset, kwargs)
    optional_dict = _test_runner_options(model_filepath, kwargs)
    processes = kwargs.get("processes")
    if processes is not None:
        assert isinstance(processes, int)
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

    model_type = _runner_model_type(runner, type)
    split_test_indices_for_perf_ci = kwargs.get("split_test_indices_for_perf_ci", False)

    # plot
    groundtruths = [asset.groundtruth for asset in test_assets]
    predictions = [result[runner_class.get_score_key()] for result in results]
    raw_grountruths = (
        None if test_raw_assets is None else [asset.raw_groundtruth for asset in test_raw_assets]
    )
    groundtruths_std = (
        None if test_assets is None else [asset.groundtruth_std for asset in test_assets]
    )
    stats, num_models = _calculate_test_stats(
        model_type,
        runner_class,
        results,
        groundtruths,
        predictions,
        raw_grountruths,
        groundtruths_std,
        split_test_indices_for_perf_ci,
        allow_uncalibrated,
    )
    _print_test_stats(model_type, stats, split_test_indices_for_perf_ci)
    _plot_test_stats(ax, model_type, runner_class, test_assets, stats, num_models, kwargs)

    return test_assets, results


def print_matplotlib_warning():
    print(
        "Warning: cannot import matplotlib, no picture displayed. "
        "If you are on Mac OS and have installed matplotlib, you "
        "possibly need to run: \nsudo pip uninstall python-dateutil \n"
        "sudo pip install python-dateutil==2.2 \n"
        "Refer to: https://stackoverflow.com/questions/27630114/matplotlib-issue-on-os-x-importerror-cannot-import-name-thread"
    )


def _assemble_features(
    assets,
    feature_dict,
    feature_option_dict,
    logger,
    fifo_mode,
    result_store,
    parallelize,
    aggregate_method,
    processes=None,
):
    assembler = FeatureAssembler(
        feature_dict=feature_dict,
        feature_option_dict=feature_option_dict,
        assets=assets,
        logger=logger,
        fifo_mode=fifo_mode,
        delete_workdir=True,
        result_store=result_store,
        optional_dict=None,
        optional_dict2=None,
        parallelize=parallelize,
        processes=processes,
    )
    assembler.run()
    for result in assembler.results:
        result.set_score_aggregate_method(aggregate_method)
    return assembler, assembler.results


def _raw_groundtruths(raw_assets):
    if raw_assets is None:
        return None
    return [asset.raw_groundtruth for asset in raw_assets]


def _log_model_stats(prefix, formatter, stats, logger):
    message = f"Stats on {prefix} data: {formatter(stats)}"
    if logger:
        logger.info(message)
    else:
        print(message)


def _save_trained_model(model, output_model_filepath):
    if output_model_filepath is None:
        return
    suffix = Path(output_model_filepath).suffix
    supported_formats = [".pkl", ".json"]
    VmafQualityRunnerModelMixin._assert_extension_format(supported_formats, suffix)
    if suffix == ".pkl":
        model.to_file(output_model_filepath, format="pkl")
        return
    if suffix == ".json":
        model.to_file(output_model_filepath, format="json", combined=True)
        return
    raise AssertionError()


def _plot_model_stats(ax, dataset, assets, model, model_class, stats):
    if ax is None:
        return
    model_class.plot_scatter(ax, stats, content_ids=[asset.content_id for asset in assets])
    ax.set_xlabel("True Score")
    ax.set_ylabel("Predicted Score")
    ax.grid()
    ax.set_title(
        "Dataset: {dataset}, Model: {model}\n{stats}".format(
            dataset=dataset.dataset_name,
            model=model.model_id,
            stats=model_class.format_stats_for_plot(stats),
        )
    )


def _test_trained_model(
    test_dataset,
    model,
    model_class,
    feature_dict,
    feature_option_dict,
    test_ax,
    result_store,
    logger,
    fifo_mode,
    parallelize,
    aggregate_method,
    kwargs,
):
    if test_dataset is None:
        return None, None, None
    test_assets, test_raw_assets = _read_dataset_with_subjective_model(test_dataset, kwargs)
    test_assembler, test_features = _assemble_features(
        test_assets,
        feature_dict,
        feature_option_dict,
        logger,
        fifo_mode,
        result_store,
        parallelize,
        aggregate_method,
    )
    test_xs = model_class.get_xs_from_results(test_features)
    test_ys = model_class.get_ys_from_results(test_features)
    test_predictions = VmafQualityRunner.predict_with_model(model, test_xs, **kwargs)["ys_pred"]
    test_stats = model.get_stats(
        test_ys["label"],
        test_predictions,
        ys_label_raw=_raw_groundtruths(test_raw_assets),
    )
    _log_model_stats("testing", model_class.format_stats_for_print, test_stats, logger)
    _plot_model_stats(test_ax, test_dataset, test_assets, model, model_class, test_stats)
    return test_assembler, test_assets, test_stats


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
    train_assets, train_raw_assets = _read_dataset_with_subjective_model(train_dataset, kwargs)
    parallelize = kwargs.get("parallelize", True)
    assert isinstance(parallelize, bool)
    processes = kwargs.get("processes")
    if processes is not None:
        assert isinstance(processes, int) and processes > 0
        assert parallelize is True, "if processes is not None, parallelize must be True"
    assert hasattr(feature_param, "feature_dict")
    feature_dict = feature_param.feature_dict
    feature_option_dict = (
        feature_param.feature_optional_dict
        if hasattr(feature_param, "feature_optional_dict")
        else None
    )

    train_fassembler, train_features = _assemble_features(
        train_assets,
        feature_dict,
        feature_option_dict,
        logger,
        fifo_mode,
        result_store,
        parallelize,
        aggregate_method,
        processes,
    )
    model_type = model_param.model_type
    model_param_dict = model_param.model_param_dict

    model_class = TrainTestModel.find_subclass(model_type)

    train_xys = model_class.get_xys_from_results(train_features)
    train_xs = model_class.get_xs_from_results(train_features)
    train_ys = model_class.get_ys_from_results(train_features)

    model = model_class(model_param_dict, logger)

    model.train(train_xys, feature_option_dict=feature_option_dict, **kwargs)

    model.append_info("feature_dict", feature_param.feature_dict)
    if "score_clip" in model_param_dict:
        VmafQualityRunner.set_clip_score(model, model_param_dict["score_clip"])
    if "score_transform" in model_param_dict:
        VmafQualityRunner.set_transform_score(model, model_param_dict["score_transform"])

    train_ys_pred = VmafQualityRunner.predict_with_model(model, train_xs, **kwargs)["ys_pred"]

    train_stats = model.get_stats(
        train_ys["label"],
        train_ys_pred,
        ys_label_raw=_raw_groundtruths(train_raw_assets),
    )
    _log_model_stats("training", model.format_stats_for_print, train_stats, logger)
    _save_trained_model(model, output_model_filepath)
    _plot_model_stats(train_ax, train_dataset, train_assets, model, model_class, train_stats)
    test_fassembler, test_assets, test_stats = _test_trained_model(
        test_dataset,
        model,
        model_class,
        feature_dict,
        feature_option_dict,
        test_ax,
        result_store,
        logger,
        fifo_mode,
        parallelize,
        aggregate_method,
        kwargs,
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
    content_ids = [asset.content_id for asset in assets]
    kfold = []
    for curr_content_group in contentid_groups:
        curr_indices = indices(content_ids, lambda x, group=curr_content_group: x in group)
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

    parallelize = kwargs.get("parallelize", True)
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
    _fig, axs = plt.subplots(figsize=(5 * ncols, 5 * nrows), nrows=nrows, ncols=ncols)

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
    result_store_dir=None,
):

    if result_store_dir is None:
        result_store_dir = VmafConfig.file_result_store_path()
    logger = get_stdout_logger()
    result_store = FileSystemResultStore(result_store_dir)
    dataset = import_python_file(dataset_filepath)
    param = import_python_file(param_filepath)

    _fig, ax = plt.subplots(figsize=(5, 5), nrows=1, ncols=1)

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
    result_store_dir=None,
):

    if result_store_dir is None:
        result_store_dir = VmafConfig.file_result_store_path()

    def print_assets(test_assets):
        print(
            "\n".join(
                (
                    "Asset {i}: {name}".format(
                        i=tasset[0], name=get_file_name_without_extension(tasset[1].dis_path)
                    )
                    for tasset in enumerate(test_assets)
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
        DmosModel = import_module("sureal.subjective_model").DmosModel

        subj_model_class = kwargs.get("subj_model_class", DmosModel)
        content_ids = kwargs.get("content_ids")
        asset_ids = kwargs.get("asset_ids")
        subjective_model = subj_model_class.from_dataset_file(
            raw_dataset_filepath, content_ids=content_ids, asset_ids=asset_ids
        )
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
            with Path(output_dataset_filepath).open("r") as f:
                body = f.read()
        except OSError:
            return
        if "np.float64(" in body or "np.int64(" in body:
            if not body.lstrip().startswith("import numpy"):
                with Path(output_dataset_filepath).open("w") as f:
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

    train_output_dataset_filepath = str(
        Path(workspace_path).joinpath("dataset", "train_dataset.py")
    )
    generate_dataset_from_raw(
        raw_dataset_filepath=train_dataset_raw_filepath,
        output_dataset_filepath=train_output_dataset_filepath,
        quality_width=train_quality_width,
        quality_height=train_quality_height,
        transform_final=train_transform_final,
        **kwargs,
    )

    test_output_dataset_filepath = (
        str(Path(workspace_path).joinpath("dataset", "test_dataset.py"))
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
