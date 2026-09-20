import json
import re
from collections import OrderedDict
from collections.abc import Callable
from importlib import import_module

import numpy as np

from vmaf.core.asset import Asset
from vmaf.tools.misc import get_file_name_with_extension

_COMPARISON_VALUE_2 = 2

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


class BasicResult(object):
    """
    Has some basic functions, but don't need asset or executor_id. To be used by
    FeatureAssembler, which is not an Executor.
    """

    def __init__(self, asset, result_dict):
        self.asset = asset
        self.result_dict = result_dict
        self.score_aggregate_method = np.mean

    def set_score_aggregate_method(self, score_aggregate_method: Callable | None):
        if score_aggregate_method is not None:
            self.score_aggregate_method = score_aggregate_method
        else:
            self.score_aggregate_method = np.mean

    # make access dictionary-like, i.e. can do: result['vif_score']
    def __getitem__(self, key):
        return self.get_result(key)

    def get_result(self, key):
        try:
            return self.result_dict[key]
        except KeyError as e:
            return self._try_get_aggregate_score(key, e)

    @staticmethod
    def get_scores_key_from_score_key(score_key):
        # e.g. 'VMAF_scores'
        return score_key + "s"

    def _try_get_aggregate_score(self, key, error):
        """
        Get aggregate score from list of scores. Must follow the convention
        that if the aggregate score uses key '*_score', then there must be
        a corresponding list of scores that uses key '*_scores'. For example,
        if the key is 'VMAF_score', there must exist a corresponding key
        'VMAF_scores'.
        The list of scores (when present) should also follow the convention
        that IF multiple models are present (e.g. in bootstrapping),
        the first dimension will be the model and the
        second dimension the video frames. This means that a 2D result array
        should be created ONLY when multiple models are present.
        """
        if re.search(r"_score$", key):
            scores_key = self.get_scores_key_from_score_key(key)
            if scores_key in self.result_dict:
                scores = self.result_dict[scores_key]
                # if scores are in a list, wrap in an array to be consistent
                if type(scores) is list:
                    scores = np.asarray(scores)
                # dimension assertion: scores should be either 1-D (one prediction per frame) or 2-D (single/multiple predictions per frame)
                assert (
                    scores.ndim <= _COMPARISON_VALUE_2
                ), "Per frame score aggregation is not well-defined; scores cannot be saved in a N-D array with N > 2."

                # check if there are more than one models (first dimension)
                if scores.ndim == _COMPARISON_VALUE_2:
                    # check that there are > 1 models present
                    assert (
                        scores.shape[0] > 1
                    ), "# models is <=1, but a 2D result array (models x frames) was used."
                    # check that there are >= 1 frames predicted
                    assert scores.shape[1] >= 1, "# predicted frames is < 1."
                    # apply score aggregation on each individual model
                    # a scores "piece" corresponds to a single model's predictions over all frames
                    return [self.score_aggregate_method(scores_piece) for scores_piece in scores]
                # just one prediction per frame
                return self.score_aggregate_method(scores)
        raise KeyError(error)

    def get_ordered_list_scores_key(self):
        # e.g. ['VMAF_scores', 'VMAF_vif_scores']
        # filter out scores that are > 1D (e.g. when having multiple models in bootstrapping)
        list_scores_key = filter(
            lambda key: re.search(r"_scores$", key) and np.asarray(self.result_dict[key]).ndim == 1,
            self.result_dict.keys(),
        )
        return sorted(list_scores_key)

    def get_ordered_list_multimodel_scores_key(self):
        # e.g. ['BOOTSTRAP_VMAF_all_models_scores']
        # select only scores that are > 1D (e.g. when having multiple models in bootstrapping)
        list_scores_key = filter(
            lambda key: re.search(r"_scores$", key) and np.asarray(self.result_dict[key]).ndim > 1,
            self.result_dict.keys(),
        )
        return sorted(list_scores_key)

    def get_ordered_list_score_key(self):
        # e.g. ['VMAF_score', 'VMAF_vif_score']
        list_scores_key = self.get_ordered_list_scores_key()
        return [scores_key[:-1] for scores_key in list_scores_key]

    def get_ordered_list_multimodel_score_key(self):
        # e.g. ['BOOTSTRAP_VMAF_all_models_score']
        list_scores_key = self.get_ordered_list_multimodel_scores_key()
        return [scores_key[:-1] for scores_key in list_scores_key]

    def _get_scores_str(self, unit_name="Frame"):
        list_scores_key = self.get_ordered_list_scores_key()
        list_score_key = self.get_ordered_list_score_key()
        list_scores = [self.result_dict[key] for key in list_scores_key]
        str_perframe = "\n".join(
            [
                "{unit} {num}: ".format(unit=unit_name, num=tframe_scores[0])
                + (
                    ", ".join(
                        [
                            "{score_key}:{score:.6f}".format(score_key=tscore[0], score=tscore[1])
                            for tscore in zip(list_score_key, tframe_scores[1], strict=False)
                        ]
                    )
                )
                for tframe_scores in enumerate(zip(*list_scores, strict=False))
            ]
        )
        str_perframe += "\n"
        return str_perframe

    def _get_aggregate_score_str(self):
        list_score_key = self.get_ordered_list_score_key()
        return "Aggregate ({}): ".format(self.score_aggregate_method.__name__) + (
            ", ".join(
                [
                    "{score_key}:{score:.6f}".format(score_key=tscore[0], score=tscore[1])
                    for tscore in zip(
                        list_score_key,
                        [self[score_key] for score_key in list_score_key],
                        strict=False,
                    )
                ]
            )
        )

    @staticmethod
    def scores_key_wildcard_match(result_dict, scores_key, excluded_scores_keys=None):
        """Find the exact or shortest unambiguous matching score key."""

        # first look for exact match
        result_keys_sorted = [
            key
            for key in sorted(result_dict.keys())
            if (excluded_scores_keys is not None and key not in excluded_scores_keys)
            or excluded_scores_keys is None
        ]
        for result_key in result_keys_sorted:
            if result_key == scores_key:
                return result_key

        # then look for wildcard match
        matched_result_keys = []
        for result_key in result_keys_sorted:
            if result_key.startswith(scores_key[: -len("_scores")]):
                matched_result_keys.append(result_key)
        if len(matched_result_keys) == 0:
            raise KeyError(f"no key matches {scores_key}")
        if len(matched_result_keys) == 1:
            return matched_result_keys[0]
        # look for shortest match
        strlens = [len(s) for s in matched_result_keys]
        argmin_strlen = np.concatenate(np.where(strlens == np.min(strlens))).tolist()
        if len(argmin_strlen) == 1:
            return matched_result_keys[argmin_strlen[0]]
        raise KeyError(f"more than one keys matches {scores_key}: {matched_result_keys}")


class Result(BasicResult):
    """
    Dictionary-like object that stores read-only result generated on an Asset
    by a Executor.
    """

    DATAFRAME_COLUMNS = (
        "dataset",
        "content_id",
        "asset_id",
        "ref_name",
        "dis_name",
        "asset",
        "executor_id",
        "scores_key",
        "scores",  # one score per unit - frame, chunk or else
    )
    __hash__ = None

    def __init__(self, asset, executor_id, result_dict):
        super(Result, self).__init__(asset, result_dict)
        self.executor_id = executor_id

    def __eq__(self, other):
        if self.asset != other.asset:
            return False
        if self.executor_id != other.executor_id:
            return False
        list_scores_key = self.get_ordered_list_scores_key()
        for scores_key in list_scores_key:
            if self.result_dict[scores_key] != other.result_dict[scores_key]:
                return False
        return True

    def __ne__(self, other):
        return not self.__eq__(other)

    # TODO: add __repr__, __hash__

    def __str__(self):
        return self.to_string()

    @staticmethod
    def get_unique_from_dataframe(df, scores_key, column):
        """
        Convenience method to access dataframe. Do two things 1) make assertion
        that only one row correspond to the scores_key, 2) retrive a column
        from that row
        :param df:
        :param scores_key:
        :param column:
        :return:
        """
        _df = df.loc[df["scores_key"] == scores_key]
        assert len(_df) == 1
        return _df.iloc[0][column]

    def to_string(self):
        """Example:
        Asset: {"asset_dict": {"height": 1080, "width": 1920}, "asset_id": 0, "content_id": 0, "dataset": "test",
            "dis_path": ".../vmaf/python/test/resource/yuv/checkerboard_1920_1080_10_3_1_0.yuv",
            "ref_path": ".../vmaf/python/test/resource/yuv/checkerboard_1920_1080_10_3_0_0.yuv",
            "workdir": ".../vmaf/workspace/workdir/d26050af-bd92-46a7-8519-7482306aa7fe"}
        Executor: SSIM_V1.0
        Result:
        Frame 0: SSIM_feature_ssim_c_score:0.997404, SSIM_feature_ssim_l_score:0.965512, SSIM_feature_ssim_s_score:0.935803, SSIM_score:0.901161
        Frame 1: SSIM_feature_ssim_c_score:0.997404, SSIM_feature_ssim_l_score:0.965512, SSIM_feature_ssim_s_score:0.935803, SSIM_score:0.901160
        Frame 2: SSIM_feature_ssim_c_score:0.997404, SSIM_feature_ssim_l_score:0.965514, SSIM_feature_ssim_s_score:0.935804, SSIM_score:0.901163
        Aggregate: SSIM_feature_ssim_c_score:0.997404, SSIM_feature_ssim_l_score:0.965513, SSIM_feature_ssim_s_score:0.935803, SSIM_score:0.901161
        """
        s = ""
        s += "Asset: {}\n".format(self.asset.to_full_repr())  # unlike repr(asset), print workdir
        s += "Executor: {}\n".format(self.executor_id)
        s += "Result:\n"
        s += self._get_scores_str()
        s += self._get_aggregate_score_str()
        return s

    def _output_score_data(self):
        score_keys = self.get_ordered_list_score_key()
        scores = [self.result_dict[key] for key in self.get_ordered_list_scores_key()]
        aggregates = [self[key] for key in score_keys]
        multimodel_score_keys = self.get_ordered_list_multimodel_score_key()
        multimodel_scores = [
            self.result_dict[key].T.tolist()
            for key in self.get_ordered_list_multimodel_scores_key()
        ]
        score_keys += multimodel_score_keys
        scores += multimodel_scores
        aggregates += [self[key] for key in multimodel_score_keys]
        return score_keys, scores, aggregates

    @staticmethod
    def _prettify_xml(element):
        minidom = import_module("xml.dom.minidom")
        element_tree = import_module("xml.etree.ElementTree")
        rough_string = element_tree.tostring(element, "utf-8")
        return minidom.parseString(rough_string).toprettyxml(indent="  ")

    def to_xml(self):
        """Return frame and aggregate scores as pretty-printed XML."""
        element_tree = import_module("xml.etree.ElementTree")
        score_keys, scores, aggregates = self._output_score_data()
        top = element_tree.Element("result")
        top.set("executorId", self.executor_id)
        asset = element_tree.SubElement(top, "asset")
        asset.set("identifier", str(self.asset))
        frames = element_tree.SubElement(top, "frames")
        for frame_number, frame_scores in enumerate(zip(*scores, strict=False)):
            frame = element_tree.SubElement(frames, "frame")
            frame.set("frameNum", str(frame_number))
            for score_key, score in zip(score_keys, frame_scores, strict=False):
                frame.set(score_key, str(score))
        aggregate = element_tree.SubElement(top, "aggregate")
        aggregate.set("method", self.score_aggregate_method.__name__)
        for score_key, score in zip(score_keys, aggregates, strict=False):
            aggregate.set(score_key, str(score))
        return self._prettify_xml(top)

    @classmethod
    def from_xml(cls, xml_string):

        ElementTree = import_module("xml.etree.ElementTree")

        top = ElementTree.fromstring(xml_string)
        asset = top.find("asset").get("identifier")
        executor_id = top.get("executorId")

        result_dict = {}

        frames = top.find("frames")

        pred_result_key = None

        for _frame_ind, frame in enumerate(frames):
            for score_key, score_value in frame.attrib.items():
                if "score" in score_key:
                    # same convention of 'score' and 'scores' as in _try_get_aggregate_score
                    if "feature" in score_key:
                        result_dict.setdefault(
                            Result.get_scores_key_from_score_key(score_key), []
                        ).append(np.float64(score_value))
                    else:
                        pred_result_key = Result.get_scores_key_from_score_key(score_key)
                        result_dict.setdefault(pred_result_key, []).append(np.float64(score_value))

        result_dict[pred_result_key] = np.array(result_dict[pred_result_key])

        return Result(asset, executor_id, result_dict)

    @classmethod
    def from_json(cls, json_string):

        json_data = json.loads(json_string)
        executor_id = json_data["executorId"]
        asset = json_data["asset"]["identifier"]

        result_dict = {}

        frames = json_data["frames"]

        pred_result_key = None

        for _frame_ind, frame in enumerate(frames):
            for score_key, score_value in frame.items():
                if "score" in score_key:
                    # same convention of 'score' and 'scores' as in _try_get_aggregate_score
                    if "feature" in score_key:
                        result_dict.setdefault(
                            Result.get_scores_key_from_score_key(score_key), []
                        ).append(np.float64(score_value))
                    else:
                        pred_result_key = Result.get_scores_key_from_score_key(score_key)
                        result_dict.setdefault(pred_result_key, []).append(np.float64(score_value))

        result_dict[pred_result_key] = np.array(result_dict[pred_result_key])

        return Result(asset, executor_id, result_dict)

    @classmethod
    def combine_result(cls, results):

        assert len(results) > 0, "Results list is empty."
        executor_ids = [result.executor_id for result in results]
        assert len(set(executor_ids)) == 1, "Executor ids do not match."
        executor_id = executor_ids[0]
        combined_result_dict = OrderedDict()

        sorted_scores_keys = sorted(results[0].result_dict.keys(), reverse=True)

        # initialize dictionary
        for scores_key in sorted_scores_keys:
            combined_result_dict[scores_key] = []

        for result in results:
            result_dict = result.result_dict
            # assert if the keys in result_dict match
            assert (
                sorted(result_dict.keys(), reverse=True) == sorted_scores_keys
            ), "Score keys do not match."
            for scores_key in sorted_scores_keys:
                combined_result_dict[scores_key] += list(result_dict[scores_key])

        pred_result_key = next(key for key in sorted_scores_keys if "feature" not in key)
        combined_result_dict[pred_result_key] = np.array(combined_result_dict[pred_result_key])

        return Result("combined_asset", executor_id, combined_result_dict)

    def to_dict(self):
        """Return frame and aggregate scores as a JSON-compatible mapping."""
        score_keys, scores, aggregates = self._output_score_data()
        top = OrderedDict()
        top["executorId"] = self.executor_id
        top["asset"] = {"identifier": str(self.asset)}
        top["frames"] = []
        for frame_number, frame_scores in enumerate(zip(*scores, strict=False)):
            frame = OrderedDict(frameNum=frame_number)
            for score_key, score in zip(score_keys, frame_scores, strict=False):
                frame[score_key] = score
            top["frames"].append(frame)
        top["aggregate"] = OrderedDict()
        for score_key, score in zip(score_keys, aggregates, strict=False):
            top["aggregate"][score_key] = score
        top["aggregate"]["method"] = self.score_aggregate_method.__name__
        return top

    def to_json(self):
        """
        :return str: JSON representation
        """
        return json.dumps(self.to_dict(), sort_keys=False, indent=4)

    def to_dataframe(self):
        """Export one row per score series using ``DATAFRAME_COLUMNS``."""
        pd = import_module("pandas")

        asset = self.asset
        executor_id = self.executor_id
        list_scores_key = self.get_ordered_list_scores_key()
        list_scores = [self.result_dict[key] for key in list_scores_key]

        rows = []
        for scores_key, scores in zip(list_scores_key, list_scores, strict=False):
            row = [
                asset.dataset,
                asset.content_id,
                asset.asset_id,
                get_file_name_with_extension(asset.ref_path),
                get_file_name_with_extension(asset.dis_path),
                repr(asset),
                executor_id,
                scores_key,
                scores,
            ]
            rows.append(row)

        # zip rows into a dict, and wrap with df
        return pd.DataFrame(
            dict(zip(self.DATAFRAME_COLUMNS, zip(*rows, strict=False), strict=False))
        )

    @classmethod
    def from_dataframe(cls, df, AssetClass=Asset):

        # first, make sure the df conform to the format for a single asset
        cls._assert_asset_dataframe(df)

        asset_repr = df.iloc[0]["asset"]
        asset = AssetClass.from_repr(asset_repr)

        executor_id = df.iloc[0]["executor_id"]

        result_dict = {}
        for _, row in df.iterrows():
            result_dict[row["scores_key"]] = row["scores"]

        return Result(asset, executor_id, result_dict)

    @classmethod
    def _assert_asset_dataframe(cls, df):
        """
        Make sure that the input dataframe conforms w.r.t. an asset
        :param df:
        :return:
        """
        # check columns
        for col in list(df.columns.values):
            assert col in cls.DATAFRAME_COLUMNS

        # all rows should have the same dataset, content_id, asset_id, ref_name,
        # dis_name, asset and executor_id
        assert len(set(df["dataset"].tolist())) == 1
        assert len(set(df["content_id"].tolist())) == 1
        assert len(set(df["asset_id"].tolist())) == 1
        assert len(set(df["ref_name"].tolist())) == 1
        assert len(set(df["dis_name"].tolist())) == 1
        assert len(set(df["asset"].tolist())) == 1
        assert len(set(df["executor_id"].tolist())) == 1

        # each scores key must have one single row
        assert len(df) == len(set(df["scores_key"].tolist()))

        # all scores should have equal length
        assert len(set(map(len, df["scores"].tolist()))) == 1


class RawResult(object):
    """
    Other than sharing the name 'Result' and have same initialization interface,
    RawResult is very different from vmaf.core.result.Result class -- it won't be
    stored using ResultStore, neither it has aggregation method like
    result['vmaf_score'] (which calls _try_get_aggregate_score()).
    """

    def __init__(self, asset, executor_id, result_dict):
        # same interface as Result
        self.asset = asset
        self.executor_id = executor_id
        self.result_dict = result_dict

    # make access dictionary-like, i.e. can do: result['pixels'], result['asset']
    def __getitem__(self, key):
        return self.get_result(key)

    def get_result(self, key):
        # different from BasicResult.get_result, it won't try to get
        # aggregate score
        return self.result_dict[key]

    def get_ordered_results(self):
        return sorted(self.result_dict.keys())


if __name__ == "__main__":
    import doctest

    doctest.testmod()
