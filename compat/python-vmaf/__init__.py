import logging
import os
import subprocess
from importlib import import_module
from pathlib import Path

from . import config

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"
__version__ = "3.2.1"  # x-release-please-version

logging.basicConfig()
logger = logging.getLogger(Path(__file__).stem)
logger.setLevel("INFO")

try:
    from matplotlib import pyplot as plt
except (ImportError, RuntimeError):
    # ImportError: matplotlib (or one of its native deps) not installed.
    # RuntimeError: OSX system-Python framework error described below.
    # Narrowed from bare 'except' so KeyboardInterrupt / SystemExit
    # propagate. (CodeQL py/catch-base-exception)
    # TODO: importing matplotlib fails on OSX with system python, check what can be done there...
    # Error reported is:
    #   RuntimeError: Python is not installed as a framework.
    #   The Mac OS X backend will not be able to function correctly if Python is not installed as a framework.
    #   See the Python documentation for more information on installing Python as a framework on Mac OS X.
    #   Please either reinstall Python as a framework, or try one of the other backends.
    #   If you are using (Ana)Conda please install python.app and replace the use of 'python' with 'pythonw'.
    #   See 'Working with Matplotlib on OSX' in the Matplotlib FAQ for more information.
    plt = None

# Path to folder containing this file
VMAF_PYTHON_ROOT = str(Path(__file__).resolve().parent)


# Assuming vmaf source checkout, path to top checked out folder
VMAF_ROOT = str(Path(VMAF_PYTHON_ROOT).joinpath("..", "..").resolve())


class ProcessRunner(object):

    def run(self, cmd, kwargs):
        try:
            logger.info(cmd)
            # Force a deterministic C locale so subprocess error messages
            # captured into the AssertionError text are English. Without
            # this, run on a non-English host produces locale-translated
            # shell errors (e.g. "Kommando nicht gefunden" instead of
            # "command not found") and breaks test assertions that grep
            # the message for canonical English phrases.
            #
            # Unconditional overwrite is intentional: we must force
            # LC_ALL/LANG=C regardless of what the parent shell or the
            # caller's env= dict already contains. Start from the
            # caller-provided env (or os.environ if none was given), then
            # stamp the C-locale keys on top so they cannot be overridden.
            caller_env = kwargs.get("env")
            base_env = dict(caller_env) if caller_env is not None else dict(os.environ)
            base_env["LC_ALL"] = "C"
            base_env["LANG"] = "C"
            kwargs["env"] = base_env
            if isinstance(cmd, (str, bytes)):
                raise TypeError("commands must be an argument sequence, not shell text")
            argv = [os.fspath(arg) for arg in cmd]
            if not argv or any("\0" in arg for arg in argv):
                raise ValueError("command arguments must be non-empty and NUL-free")
            if kwargs.pop("shell", False):
                raise ValueError("shell execution is not supported")
            kwargs.setdefault("stderr", subprocess.STDOUT)
            if "stdout" in kwargs:
                run_checked = subprocess.run
                run_checked(argv, check=True, **kwargs)
            else:
                check_output = subprocess.check_output
                check_output(argv, **kwargs)
        except subprocess.CalledProcessError as e:
            raise AssertionError(
                f"Process returned {e.returncode}, cmd: {cmd}, kwargs: {kwargs}, msg: {e.output!s}"
            ) from e


def run_process(cmd, **kwargs):
    process_runner = ProcessRunner()
    process_runner.run(cmd, kwargs)
    return 0


def project_path(relative_path):
    return str(Path(VMAF_ROOT).joinpath(relative_path))


def required(path):
    if not Path(path).exists():
        raise AssertionError(f"{path} does not exist, did you build?")
    return path


def convert_pixel_format_ffmpeg2vmafexec(ffmpeg_pix_fmt):
    """
    Convert FFmpeg-style pixel format (pix_fmt) to vmaf style.

    :param ffmpeg_pix_fmt: FFmpeg-style pixel format, for example: yuv420p, yuv420p10le
    :return: (pixel_format: str, bitdepth: int), for example: (420, 8), (420, 10)
    """
    assert ffmpeg_pix_fmt in [
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

    if ffmpeg_pix_fmt in ["yuv420p", "yuv420p10le", "yuv420p12le", "yuv420p16le"]:
        pixel_format = "420"
    elif ffmpeg_pix_fmt in ["yuv422p", "yuv422p10le", "yuv422p12le", "yuv422p16le"]:
        pixel_format = "422"
    elif ffmpeg_pix_fmt in ["yuv444p", "yuv444p10le", "yuv444p12le", "yuv444p16le"]:
        pixel_format = "444"
    else:
        raise AssertionError()

    if ffmpeg_pix_fmt in ["yuv420p", "yuv422p", "yuv444p"]:
        bitdepth = 8
    elif ffmpeg_pix_fmt in ["yuv420p10le", "yuv422p10le", "yuv444p10le"]:
        bitdepth = 10
    elif ffmpeg_pix_fmt in ["yuv420p12le", "yuv422p12le", "yuv444p12le"]:
        bitdepth = 12
    elif ffmpeg_pix_fmt in ["yuv420p16le", "yuv422p16le", "yuv444p16le"]:
        bitdepth = 16
    else:
        raise AssertionError()
    return pixel_format, bitdepth


class ExternalProgram(object):
    """
    External C programs relied upon by the python vmaf code
    These external programs should be compiled before vmaf is ran, as per instructions in README
    """

    try:
        externals = import_module(".externals", package=__package__)

        external_vmaf_feature = config.VmafExternalConfig.vmaf_path()
        external_vmafexec = config.VmafExternalConfig.vmafexec_path()
    except ImportError:
        external_vmaf_feature = None
        external_vmafexec = None

    vmaf_feature = (
        project_path(str(Path("core").joinpath("build", "tools", "vmaf_feature")))
        if external_vmaf_feature is None
        else external_vmaf_feature
    )
    vmafexec = (
        project_path(str(Path("core").joinpath("build", "tools", "vmaf")))
        if external_vmafexec is None
        else external_vmafexec
    )


class ExternalProgramCaller(object):
    """
    Caller of ExternalProgram.
    """

    @staticmethod
    def _append_multi_feature_runtime_options(cmd, options):
        options = options or {}
        backend = options.get("backend")
        if not backend:
            backend = os.environ.get("VMAF_FORCE_BACKEND") or os.environ.get("VMAF_BACKEND")
        if backend:
            cmd += ["--backend", str(backend)]

        if "disable_avx" in options:
            assert isinstance(options["disable_avx"], bool)
            if options["disable_avx"]:
                cmd += ["--cpumask", "4294967295"]

        if "n_threads" in options:
            assert isinstance(options["n_threads"], int) and options["n_threads"] >= 1
            cmd += ["--threads", str(options["n_threads"])]

    @staticmethod
    def _multi_feature_argument(feature, options):
        if not options or not options.get(feature):
            return feature
        feature_options = options[feature]
        assert isinstance(feature_options, dict)
        option_strings = []
        for key, value in feature_options.items():
            option_value = str(value).lower() if isinstance(value, bool) else value
            option_strings.append(f"{key}={option_value}")
        return f"{feature}={':'.join(option_strings)}"

    @staticmethod
    def _prediction_arguments(
        no_prediction,
        models,
        vif_enhn_gain_limit,
        adm_enhn_gain_limit,
        motion_force_zero,
        enc_width,
        enc_height,
        enc_bitdepth,
    ):
        if no_prediction:
            return ["--no_prediction"]
        assert isinstance(models, list)
        arguments = []
        for model in models:
            model_spec = model
            if vif_enhn_gain_limit is not None:
                model_spec += f":vif.vif_enhn_gain_limit={vif_enhn_gain_limit}:float_vif.vif_enhn_gain_limit={vif_enhn_gain_limit}"
            if adm_enhn_gain_limit is not None:
                model_spec += f":adm.adm_enhn_gain_limit={adm_enhn_gain_limit}:float_adm.adm_enhn_gain_limit={adm_enhn_gain_limit}"
            if motion_force_zero:
                assert isinstance(motion_force_zero, bool)
                value = str(motion_force_zero).lower()
                model_spec += (
                    f":motion.motion_force_zero={value}:float_motion.motion_force_zero={value}"
                )
            for key, value in (
                ("enc_width", enc_width),
                ("enc_height", enc_height),
                ("enc_bitdepth", enc_bitdepth),
            ):
                if value is not None:
                    model_spec += f":cambi.{key}={value}"
            arguments += ["--model", model_spec]
        return arguments

    @staticmethod
    def _runtime_arguments(subsample, n_threads, disable_avx, backend):
        assert isinstance(subsample, int) and subsample >= 1
        assert isinstance(n_threads, int) and n_threads >= 1
        arguments = []
        if subsample != 1:
            arguments += ["--subsample", str(subsample)]
        if n_threads != 1:
            arguments += ["--threads", str(n_threads)]
        if disable_avx:
            arguments += ["--cpumask", "4294967295"]
        backend = backend or os.environ.get("VMAF_FORCE_BACKEND") or os.environ.get("VMAF_BACKEND")
        if backend:
            arguments += ["--backend", str(backend)]
        return arguments

    @staticmethod
    def call_vmafexec_single_feature(
        feature, yuv_type, ref_path, dis_path, w, h, log_file_path, logger=None, options=None
    ):
        options2 = {feature: options.copy() if options is not None else None}
        if options2[feature] is not None and "disable_avx" in options2[feature]:
            options2["disable_avx"] = options2[feature]["disable_avx"]
            del options2[feature]["disable_avx"]
        if options2[feature] is not None and "n_threads" in options2[feature]:
            options2["n_threads"] = options2[feature]["n_threads"]
            del options2[feature]["n_threads"]
        if options2[feature] is not None and "_open_workfile_method" in options2[feature]:
            options2["_open_workfile_method"] = options2[feature]["_open_workfile_method"]
            del options2[feature]["_open_workfile_method"]
        if options2[feature] is not None and "_close_workfile_method" in options2[feature]:
            options2["_close_workfile_method"] = options2[feature]["_close_workfile_method"]
            del options2[feature]["_close_workfile_method"]
        if options2[feature] is not None and "backend" in options2[feature]:
            options2["backend"] = options2[feature]["backend"]
            del options2[feature]["backend"]
        return ExternalProgramCaller.call_vmafexec_multi_features(
            [feature],
            yuv_type,
            ref_path,
            dis_path,
            w,
            h,
            log_file_path,
            logger=logger,
            options=options2,
        )

    @staticmethod
    def call_vmafexec_multi_features(
        features, yuv_type, ref_path, dis_path, w, h, log_file_path, logger=None, options=None
    ):

        # ./core/build/tools/vmaf
        # --reference python/test/resource/yuv/src01_hrc00_576x324.yuv
        # --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv
        # --width 576 --height 324 --pixel_format 420 --bitdepth 8
        # --output /dev/stdout --xml --no_prediction --feature float_motion --feature integer_motion

        pixel_format, bitdepth = convert_pixel_format_ffmpeg2vmafexec(yuv_type)

        cmd = [
            required(ExternalProgram.vmafexec),
            "--reference",
            ref_path,
            "--distorted",
            dis_path,
            "--width",
            str(w),
            "--height",
            str(h),
            "--pixel_format",
            pixel_format,
            "--bitdepth",
            str(bitdepth),
            "--output",
            log_file_path,
            "--xml",
            "--no_prediction",
        ]

        ExternalProgramCaller._append_multi_feature_runtime_options(cmd, options)

        for feature in features:
            cmd += ["--feature", ExternalProgramCaller._multi_feature_argument(feature, options)]

        if logger:
            logger.info(" ".join(cmd))
        run_process(cmd)

    @staticmethod
    def call_vifdiff_feature(yuv_type, ref_path, dis_path, w, h, log_file_path, logger=None):

        # APPEND (>>) result (since _prepare_generate_log_file method has already created the file
        # and written something in advance).
        vifdiff_feature_cmd = [
            required(ExternalProgram.vmaf_feature),
            "vifdiff",
            yuv_type,
            ref_path,
            dis_path,
            str(w),
            str(h),
        ]
        if logger:
            logger.info(" ".join(vifdiff_feature_cmd))
        with Path(log_file_path).open("ab") as log_file:
            run_process(vifdiff_feature_cmd, stdout=log_file)

    @staticmethod
    def call_vmafexec(
        reference,
        distorted,
        width,
        height,
        pixel_format,
        bitdepth,
        float_psnr,
        psnr,
        float_ssim,
        ssim,
        float_ms_ssim,
        ms_ssim,
        float_moment,
        no_prediction,
        models,
        subsample,
        n_threads,
        disable_avx,
        output,
        exe,
        logger,
        vif_enhn_gain_limit=None,
        adm_enhn_gain_limit=None,
        motion_force_zero=False,
        enc_width=None,
        enc_height=None,
        enc_bitdepth=None,
        backend=None,
    ):

        if exe is None:
            exe = required(ExternalProgram.vmafexec)

        vmafexec_cmd = [
            exe,
            "--reference",
            reference,
            "--distorted",
            distorted,
            "--width",
            str(width),
            "--height",
            str(height),
            "--pixel_format",
            pixel_format,
            "--bitdepth",
            str(bitdepth),
            "--output",
            output,
        ]
        for feature, enabled in (
            ("float_psnr", float_psnr),
            ("float_ssim", float_ssim),
            ("float_ms_ssim", float_ms_ssim),
            ("float_moment", float_moment),
            ("psnr", psnr),
            ("ms_ssim", ms_ssim),
        ):
            if enabled:
                vmafexec_cmd += ["--feature", feature]
        if ssim:
            raise AssertionError("ssim (the daala integer ssim) is deprecated")
        vmafexec_cmd += ExternalProgramCaller._prediction_arguments(
            no_prediction,
            models,
            vif_enhn_gain_limit,
            adm_enhn_gain_limit,
            motion_force_zero,
            enc_width,
            enc_height,
            enc_bitdepth,
        )
        vmafexec_cmd += ExternalProgramCaller._runtime_arguments(
            subsample, n_threads, disable_avx, backend
        )

        if logger:
            logger.info(" ".join(vmafexec_cmd))

        run_process(vmafexec_cmd)


def model_path(*components):
    return str(Path(VMAF_PYTHON_ROOT).joinpath("model", *components))
