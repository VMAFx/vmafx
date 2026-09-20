import sys
import tempfile
import unittest
from pathlib import Path

from vmaf import ExternalProgram, run_process
from vmaf.config import VmafConfig
from vmaf.tools.misc import MyTestCase

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


class RunProcessTest(MyTestCase):

    def test_run_process(self):
        ret = run_process([sys.executable, "-c", "print('hello')"])
        self.assertEqual(ret, 0)

    def test_run_process_false_cmd(self):
        with self.assertRaises(AssertionError) as e:
            run_process(
                [
                    sys.executable,
                    "-c",
                    "import sys; print('not found', file=sys.stderr); sys.exit(127)",
                ]
            )
        self.assertTrue("Process returned 127" in e.exception.args[0])
        self.assertTrue("not found" in e.exception.args[0])


class CommandLineTest(MyTestCase):

    @staticmethod
    def _python_script(script_name, *args):
        return [
            sys.executable,
            VmafConfig.root_path("python", "vmaf", "script", script_name),
            *map(str, args),
        ]

    @staticmethod
    def _default_yuv_args():
        return [
            "yuv420p",
            "576",
            "324",
            VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324.yuv"),
            VmafConfig.test_resource_path("yuv", "src01_hrc01_576x324.yuv"),
        ]

    def setUp(self):
        super().setUp()
        self.dataset_filename = VmafConfig.test_resource_path("example_dataset.py")
        self.raw_dataset_filename = VmafConfig.test_resource_path("example_raw_dataset.py")
        self.out_model_filepath = VmafConfig.workdir_path("tmp.json")
        self.param_filename = VmafConfig.test_resource_path("vmaf_v4.py")
        self.batch_filename = VmafConfig.workdir_path("test_batch_input")

    def tearDown(self):
        for path in (
            Path(self.out_model_filepath),
            Path(f"{self.out_model_filepath}.model"),
            Path(self.batch_filename),
        ):
            if path.exists():
                path.unlink()
        super().tearDown()

    def test_run_testing_vmaf(self):
        ret = run_process(
            self._python_script(
                "run_testing.py",
                "VMAF",
                self.dataset_filename,
                "--parallelize",
                "--suppress-plot",
            )
        )
        self.assertEqual(ret, 0)

    def test_run_testing_vmaf_raw_dataset(self):
        ret = run_process(
            self._python_script(
                "run_testing.py",
                "VMAF",
                self.raw_dataset_filename,
                "--parallelize",
                "--suppress-plot",
            )
        )
        self.assertEqual(ret, 0)

    def test_run_testing_psnr(self):
        ret = run_process(
            self._python_script(
                "run_testing.py",
                "PSNR",
                self.dataset_filename,
                "--parallelize",
                "--suppress-plot",
            )
        )
        self.assertEqual(ret, 0)

    def test_run_testing_proccesses0(self):
        with self.assertRaises(AssertionError):
            run_process(
                self._python_script(
                    "run_testing.py",
                    "PSNR",
                    self.dataset_filename,
                    "--parallelize",
                    "--suppress-plot",
                    "--processes",
                    "0",
                )
            )

    def test_run_testing_proccesses2_without_parallelize(self):
        with self.assertRaises(AssertionError):
            run_process(
                self._python_script(
                    "run_testing.py",
                    "PSNR",
                    self.dataset_filename,
                    "--suppress-plot",
                    "--processes",
                    "2",
                )
            )

    def test_run_vmaf_training(self):
        ret = run_process(
            self._python_script(
                "run_vmaf_training.py",
                self.dataset_filename,
                self.param_filename,
                self.param_filename,
                self.out_model_filepath,
                "--parallelize",
                "--suppress-plot",
            )
        )
        self.assertEqual(ret, 0)

    def test_run_vmaf_training_processes0(self):
        with self.assertRaises(AssertionError):
            run_process(
                self._python_script(
                    "run_vmaf_training.py",
                    self.dataset_filename,
                    self.param_filename,
                    self.param_filename,
                    self.out_model_filepath,
                    "--parallelize",
                    "--suppress-plot",
                    "--processes",
                    "0",
                )
            )

    def test_run_vmaf_training_processes2_without_parallelize(self):
        with self.assertRaises(AssertionError):
            run_process(
                self._python_script(
                    "run_vmaf_training.py",
                    self.dataset_filename,
                    self.param_filename,
                    self.param_filename,
                    self.out_model_filepath,
                    "--suppress-plot",
                    "--processes",
                    "2",
                )
            )

    def test_run_vmaf_training_raw_dataset(self):
        ret = run_process(
            self._python_script(
                "run_vmaf_training.py",
                self.raw_dataset_filename,
                self.param_filename,
                self.param_filename,
                self.out_model_filepath,
                "--parallelize",
                "--suppress-plot",
            )
        )
        self.assertEqual(ret, 0)

    def test_run_vmaf(self):
        ret = run_process(self._python_script("run_vmaf.py", *self._default_yuv_args()))
        self.assertEqual(ret, 0)

    def test_run_vmaf_ci(self):
        ret = run_process(self._python_script("run_vmaf.py", *self._default_yuv_args(), "--ci"))
        self.assertEqual(ret, 0)

    def test_run_vmaf_both_local_explain_and_ci(self):
        with self.assertRaisesRegex(AssertionError, r"Process returned 2"):
            run_process(
                self._python_script(
                    "run_vmaf.py",
                    *self._default_yuv_args(),
                    "--local-explain",
                    "--ci",
                )
            )

    def test_run_psnr(self):
        ret = run_process(self._python_script("run_psnr.py", *self._default_yuv_args()))
        self.assertEqual(ret, 0)

    def test_run_cleaning_cache_psnr(self):
        ret = run_process(
            self._python_script(
                "run_testing.py",
                "PSNR",
                self.dataset_filename,
                "--parallelize",
                "--cache-result",
                "--suppress-plot",
            )
        )
        self.assertEqual(ret, 0)

        ret = run_process(
            self._python_script("run_cleaning_cache.py", "PSNR", self.dataset_filename)
        )
        self.assertEqual(ret, 0)


class VmafexecCommandLineTest(MyTestCase):

    RC_SUCCESS = 0

    def setUp(self) -> None:
        super().setUp()
        self.temp_dir = tempfile.TemporaryDirectory()
        self.output_file_path = Path(self.temp_dir.name) / "vmaf.xml"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()
        super().tearDown()

    def _command(self, *extra_args):
        return [
            ExternalProgram.vmafexec,
            "--reference",
            VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324.yuv"),
            "--distorted",
            VmafConfig.test_resource_path("yuv", "src01_hrc01_576x324.yuv"),
            "--width",
            "576",
            "--height",
            "324",
            "--pixel_format",
            "420",
            "--bitdepth",
            "8",
            "--xml",
            "--feature",
            "psnr",
            "--model",
            f"path={VmafConfig.model_path('other_models', 'vmaf_v0.6.0.json')}",
            "--quiet",
            "--output",
            self.output_file_path,
            *extra_args,
        ]

    def test_run_vmafexec(self):
        ret = run_process(self._command())
        self.assertEqual(ret, self.RC_SUCCESS)
        with self.output_file_path.open(encoding="utf-8") as fo:
            fc = fo.read()
            self.assertTrue(
                '<metric name="psnr_y" min="29.640688" max="34.760779" mean="30.755064" harmonic_mean="30.727905" />'
                in fc
            )

    def test_run_vmafexec_with_frame_skipping(self):
        ret = run_process(self._command("--frame_skip_ref", "2", "--frame_skip_dist", "2"))
        self.assertEqual(ret, self.RC_SUCCESS)
        with self.output_file_path.open(encoding="utf-8") as fo:
            fc = fo.read()
            self.assertTrue(
                '<metric name="psnr_y" min="29.640688" max="33.788213" mean="30.643458" harmonic_mean="30.626214" />'
                in fc
            )

    def test_run_vmafexec_with_frame_skipping_unequal(self):
        ret = run_process(self._command("--frame_skip_ref", "2", "--frame_skip_dist", "5"))
        self.assertEqual(ret, self.RC_SUCCESS)
        with self.output_file_path.open(encoding="utf-8") as fo:
            fc = fo.read()
            self.assertTrue(
                '<metric name="psnr_y" min="19.019327" max="21.084954" mean="20.269606" harmonic_mean="20.258113" />'
                in fc
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
