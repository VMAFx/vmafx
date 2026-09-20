import numpy as np

from vmaf.core.executor import Executor
from vmaf.core.h5py_mixin import H5pyMixin
from vmaf.core.result import RawResult
from vmaf.tools.decorator import override
from vmaf.tools.reader import YuvReader

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


class RawExtractor(Executor):

    def _assert_args(self):
        super(RawExtractor, self)._assert_args()

        assert self.result_store is None, "{} won't use result store.".format(
            self.__class__.__name__
        )


class AssetExtractor(RawExtractor):
    """
    AssetExtractor directly reads input assets and generate list of RawResult
    results that have the assets retrievable by result['asset']. The main
    purpose of this dummy extractor is to keep the interface uniform, when used
    by any pixel-domain TrainTestModel, such as a neural net.
    """

    TYPE = "Asset"
    VERSION = "1.0"

    @classmethod
    @override(Executor)
    def _assert_an_asset(cls, asset):
        # override Executor._assert_an_asset bypassing ffmpeg check
        pass

    @override(Executor)
    def _open_ref_workfile(self, asset, fifo_mode, open_sem=None):
        if open_sem is not None:
            open_sem.release()

    @override(Executor)
    def _open_dis_workfile(self, asset, fifo_mode, open_sem=None):
        if open_sem is not None:
            open_sem.release()

    def _generate_result(self, asset):
        # do nothing
        pass

    def _read_result(self, asset):
        result = {"asset": asset}
        executor_id = self.executor_id
        return RawResult(asset, executor_id, result)


class DisYUVRawVideoExtractor(H5pyMixin, RawExtractor):
    """
    DisYUVRawVideoExtractor reads the distorted video Y, U, V channel into a
    h5py file
    """

    TYPE = "DisYUVRawVideo"
    VERSION = "1.0"

    @property
    def channels(self):
        if self.optional_dict is None or "channels" not in self.optional_dict:
            return "yuv"
        channels = self.optional_dict["channels"]
        assert isinstance(channels, str)
        channels = set(channels.lower())
        assert channels.issubset(set("yuv"))
        return "".join(channels)

    @override(Executor)
    def run(self, **kwargs):
        parallelize = kwargs.get("parallelize", False)

        assert parallelize is False, "DisYUVRawVideoExtractor cannot parallelize."

        super(DisYUVRawVideoExtractor, self).run(**kwargs)

    def _assert_args(self):
        super(DisYUVRawVideoExtractor, self)._assert_args()
        self.assert_h5py_file()

    def _write_channel(self, asset, channel, frames):
        dataset = self.h5py_file.create_dataset(
            f"{asset}_{channel}",
            (len(frames), frames[0].shape[0], frames[0].shape[1]),
            dtype="float",
        )
        dataset.dims[0].label = "frame"
        dataset.dims[1].label = "height"
        dataset.dims[2].label = "width"
        for index, frame in enumerate(frames):
            dataset[index] = frame

    @override(Executor)
    def _open_ref_workfile(self, asset, fifo_mode, open_sem=None):
        # do nothing
        if open_sem is not None:
            open_sem.release()

    def _generate_result(self, asset):
        quality_w, quality_h = asset.quality_width_height

        # count number of frames
        dis_ys = []
        dis_us = []
        dis_vs = []
        with YuvReader(
            filepath=asset.dis_procfile_path,
            width=quality_w,
            height=quality_h,
            yuv_type=self._get_workfile_yuv_type(asset),
        ) as dis_yuv_reader:
            for dis_yuv in dis_yuv_reader:
                dis_y, dis_u, dis_v = dis_yuv
                dis_y, dis_u, dis_v = (
                    dis_y.astype(np.double),
                    dis_u.astype(np.double),
                    dis_v.astype(np.double),
                )

                dis_ys.append(dis_y)
                dis_us.append(dis_u)
                dis_vs.append(dis_v)

        if "y" in self.channels.lower():
            self._write_channel(asset, "y", dis_ys)
        if "u" in self.channels.lower():
            self._write_channel(asset, "u", dis_us)
        if "v" in self.channels.lower():
            self._write_channel(asset, "v", dis_vs)

    def _read_result(self, asset):
        result = {}
        if "y" in self.channels.lower():
            result["dis_y"] = self.h5py_file[str(asset) + "_y"]
        if "u" in self.channels.lower():
            result["dis_u"] = self.h5py_file[str(asset) + "_u"]
        if "v" in self.channels.lower():
            result["dis_v"] = self.h5py_file[str(asset) + "_v"]

        executor_id = self.executor_id
        return RawResult(asset, executor_id, result)
