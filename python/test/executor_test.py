import os
import tempfile
import threading
import time
import unittest
import warnings

from vmaf.core.asset import Asset
from vmaf.core.executor import Executor

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


class SerializationProbeExecutor(Executor):

    TYPE = "SerializationProbe"
    VERSION = "1.0"

    def _assert_assets(self):
        pass

    def _run_on_asset(self, asset):
        marker_path = os.path.join(self.optional_dict2["marker_dir"], asset)
        marker_fd = os.open(marker_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        try:
            time.sleep(0.05)
        finally:
            os.close(marker_fd)
            os.remove(marker_path)
        return asset

    def _generate_result(self, asset):
        raise NotImplementedError

    def _read_result(self, asset):
        raise NotImplementedError


class ExecutorTest(unittest.TestCase):

    def test_parallel_run_serializes_duplicate_assets_and_preserves_order(self):
        stop = threading.Event()
        thread = threading.Thread(target=stop.wait)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as marker_dir:
                executor = SerializationProbeExecutor(
                    ["same", "other", "same"],
                    None,
                    optional_dict2={"marker_dir": marker_dir},
                )
                with warnings.catch_warnings():
                    warnings.simplefilter("error")
                    executor.run(parallelize=True, processes=2)
        finally:
            stop.set()
            thread.join()

        self.assertEqual(executor.results, ["same", "other", "same"])

    def _check_default_and_reference_types(self):
        asset = Asset(
            dataset="test",
            content_id=0,
            asset_id=0,
            ref_path="",
            dis_path="",
            asset_dict={},
            workdir_root="my_workdir_root",
        )
        self.assertEqual(Executor._get_workfile_yuv_type(asset), "yuv420p")

        asset = Asset(
            dataset="test",
            content_id=0,
            asset_id=0,
            ref_path="",
            dis_path="",
            asset_dict={"ref_yuv_type": "notyuv", "dis_yuv_type": "notyuv"},
            workdir_root="my_workdir_root",
        )
        self.assertEqual(Executor._get_workfile_yuv_type(asset), "yuv420p")

        asset = Asset(
            dataset="test",
            content_id=0,
            asset_id=0,
            ref_path="",
            dis_path="",
            asset_dict={"ref_yuv_type": "yuv444p", "dis_yuv_type": "notyuv"},
            workdir_root="my_workdir_root",
        )
        self.assertEqual(Executor._get_workfile_yuv_type(asset), "yuv444p")

    def _check_mismatched_and_distorted_types(self):
        with self.assertRaises(AssertionError):
            asset = Asset(
                dataset="test",
                content_id=0,
                asset_id=0,
                ref_path="",
                dis_path="",
                asset_dict={"ref_yuv_type": "yuv444p", "dis_yuv_type": "yuv420p"},
                workdir_root="my_workdir_root",
            )
            self.assertEqual(Executor._get_workfile_yuv_type(asset), "yuv444p")

        asset = Asset(
            dataset="test",
            content_id=0,
            asset_id=0,
            ref_path="",
            dis_path="",
            asset_dict={"ref_yuv_type": "notyuv", "dis_yuv_type": "yuv422p"},
            workdir_root="my_workdir_root",
        )
        self.assertEqual(Executor._get_workfile_yuv_type(asset), "yuv422p")

        asset = Asset(
            dataset="test",
            content_id=0,
            asset_id=0,
            ref_path="",
            dis_path="",
            asset_dict={"ref_yuv_type": "yuv444p", "dis_yuv_type": "yuv444p"},
            workdir_root="my_workdir_root",
        )
        self.assertEqual(Executor._get_workfile_yuv_type(asset), "yuv444p")

    def _check_explicit_workfile_types(self):
        asset = Asset(
            dataset="test",
            content_id=0,
            asset_id=0,
            ref_path="",
            dis_path="",
            asset_dict={
                "ref_yuv_type": "yuv444p",
                "dis_yuv_type": "yuv444p",
                "workfile_yuv_type": "yuv420p10le",
            },
            workdir_root="my_workdir_root",
        )
        self.assertEqual(Executor._get_workfile_yuv_type(asset), "yuv420p10le")

        asset = Asset(
            dataset="test",
            content_id=0,
            asset_id=0,
            ref_path="",
            dis_path="",
            asset_dict={
                "ref_yuv_type": "yuv444p",
                "dis_yuv_type": "notyuv",
                "workfile_yuv_type": "yuv420p",
            },
            workdir_root="my_workdir_root",
        )
        self.assertEqual(Executor._get_workfile_yuv_type(asset), "yuv420p")

        asset = Asset(
            dataset="test",
            content_id=0,
            asset_id=0,
            ref_path="",
            dis_path="",
            asset_dict={
                "ref_yuv_type": "yuv444p",
                "dis_yuv_type": "notyuv",
                "workfile_yuv_type": "yuv444p",
            },
            workdir_root="my_workdir_root",
        )
        self.assertEqual(Executor._get_workfile_yuv_type(asset), "yuv444p")

    def test_get_workfile_yuv_type(self):
        self._check_default_and_reference_types()
        self._check_mismatched_and_distorted_types()
        self._check_explicit_workfile_types()


if __name__ == "__main__":
    unittest.main(verbosity=2)
