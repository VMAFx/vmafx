import multiprocessing
import os
import tempfile
import threading
import time
import unittest
import warnings
from types import SimpleNamespace

import vmaf.core.executor as executor_module
from vmaf.core.asset import Asset
from vmaf.core.executor import Executor, NorefExecutorMixin

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


class FailingFifoExecutor(Executor):

    TYPE = "FailingFifo"
    VERSION = "1.0"

    def _assert_assets(self):
        pass

    def _open_ref_workfile(self, asset, fifo_mode, open_sem=None):
        raise RuntimeError("reference workfile child failed before FIFO readiness")

    def _open_dis_workfile(self, asset, fifo_mode, open_sem=None):
        open_sem.release()

    def _open_ref_procfile(self, asset, fifo_mode, open_sem=None):
        raise RuntimeError("reference procfile child failed before FIFO readiness")

    def _open_dis_procfile(self, asset, fifo_mode, open_sem=None):
        open_sem.release()

    def _generate_result(self, asset):
        raise NotImplementedError

    def _read_result(self, asset):
        raise NotImplementedError


class FailingNorefFifoExecutor(NorefExecutorMixin, FailingFifoExecutor):

    TYPE = "FailingNorefFifo"

    def _open_dis_workfile(self, asset, fifo_mode, open_sem=None):
        raise RuntimeError("distorted workfile child failed before FIFO readiness")

    def _open_dis_procfile(self, asset, fifo_mode, open_sem=None):
        raise RuntimeError("distorted procfile child failed before FIFO readiness")


class NeverReadyFifoExecutor(FailingFifoExecutor):

    TYPE = "NeverReadyFifo"

    @staticmethod
    def _wait_without_readiness():
        threading.Event().wait(timeout=30)

    def _open_ref_workfile(self, asset, fifo_mode, open_sem=None):
        self._wait_without_readiness()

    def _open_dis_workfile(self, asset, fifo_mode, open_sem=None):
        self._wait_without_readiness()


def _fifo_asset():
    return SimpleNamespace(
        ref_workfile_path="ref-workfile",
        dis_workfile_path="dis-workfile",
        ref_procfile_path="ref-procfile",
        dis_procfile_path="dis-procfile",
    )


def _run_failing_fifo_helper(executor_class, method_name, sender):
    executor = executor_class([], None)
    try:
        getattr(executor, method_name)(_fifo_asset())
    except Exception as exc:
        sender.send((type(exc).__name__, str(exc)))
    else:
        sender.send((None, "FIFO helper returned despite a failed child"))
    finally:
        sender.close()


def _run_fifo_timeout_helper(sender):
    executor_module._FIFO_OPEN_TIMEOUT_SECONDS = 0.25
    executor = NeverReadyFifoExecutor([], None)
    try:
        executor._open_workfiles_in_fifo_mode(_fifo_asset())
    except Exception as exc:
        sender.send((type(exc).__name__, str(exc)))
    else:
        sender.send((None, "FIFO helper returned despite producers never becoming ready"))
    finally:
        sender.close()


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

    def test_fifo_helpers_surface_child_failure(self):
        context = multiprocessing.get_context("spawn")
        for executor_class, method_name, failed_stage in (
            (FailingFifoExecutor, "_open_workfiles_in_fifo_mode", "reference workfile"),
            (FailingFifoExecutor, "_open_procfiles_in_fifo_mode", "reference procfile"),
            (
                FailingNorefFifoExecutor,
                "_open_workfiles_in_fifo_mode",
                "distorted workfile",
            ),
            (
                FailingNorefFifoExecutor,
                "_open_procfiles_in_fifo_mode",
                "distorted procfile",
            ),
        ):
            with self.subTest(executor_class=executor_class.__name__, method_name=method_name):
                receiver, sender = context.Pipe(duplex=False)
                parent = context.Process(
                    target=_run_failing_fifo_helper,
                    args=(executor_class, method_name, sender),
                )
                parent.start()
                sender.close()
                try:
                    self.assertTrue(
                        receiver.poll(8),
                        f"{method_name} blocked after its child exited",
                    )
                    error_type, message = receiver.recv()
                    self.assertEqual(error_type, "RuntimeError")
                    self.assertIn(failed_stage, message)
                    self.assertIn("exit code 1", message)
                    self.assertIn("Child traceback (also emitted on stderr)", message)
                    self.assertIn("RuntimeError", message)
                    parent.join(5)
                    self.assertFalse(parent.is_alive(), "FIFO failure reporter did not exit")
                finally:
                    if parent.is_alive():
                        parent.terminate()
                    parent.join(5)
                    receiver.close()

    def test_fifo_helpers_bound_live_child_wait(self):
        context = multiprocessing.get_context("spawn")
        receiver, sender = context.Pipe(duplex=False)
        parent = context.Process(target=_run_fifo_timeout_helper, args=(sender,))
        parent.start()
        sender.close()
        try:
            self.assertTrue(receiver.poll(5), "FIFO helper exceeded its startup deadline")
            error_type, message = receiver.recv()
            self.assertEqual(error_type, "TimeoutError")
            self.assertIn("within 0.25 seconds", message)
            self.assertIn("reference workfile", message)
            self.assertIn("distorted workfile", message)
            parent.join(5)
            self.assertFalse(parent.is_alive(), "FIFO timeout reporter did not exit")
        finally:
            if parent.is_alive():
                parent.terminate()
            parent.join(5)
            receiver.close()

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
