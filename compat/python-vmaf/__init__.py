# SPDX-License-Identifier: BSD-2-Clause-Patent
import logging
import os
import subprocess

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"
__version__ = "1.0.0-rc.4"  # x-release-please-version

logging.basicConfig()
logger = logging.getLogger(os.path.splitext(os.path.basename(__file__))[0])
logger.setLevel("INFO")

try:
    from matplotlib import pyplot as plt  # noqa: F401  re-exported: `from vmaf import plt`
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
    plt = None  # type: ignore[assignment]

from . import config

# Path to folder containing this file
VMAF_PYTHON_ROOT = os.path.dirname(os.path.abspath(__file__))


# Assuming vmaf source checkout, path to top checked out folder
VMAF_ROOT = os.path.abspath(
    os.path.join(
        VMAF_PYTHON_ROOT,
        "..",
        "..",
    )
)


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
            subprocess.check_output(cmd, stderr=subprocess.STDOUT, **kwargs)
        except subprocess.CalledProcessError as e:
            raise AssertionError(
                f"Process returned {e.returncode}, cmd: {cmd}, kwargs: {kwargs}, msg: {str(e.output)}"
            )


def run_process(cmd, **kwargs):
    process_runner = ProcessRunner()
    process_runner.run(cmd, kwargs)
    return 0


def project_path(relative_path):
    path = os.path.join(VMAF_ROOT, relative_path)
    return path


def required(path):
    if not os.path.exists(path):
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
        assert False

    if ffmpeg_pix_fmt in ["yuv420p", "yuv422p", "yuv444p"]:
        bitdepth = 8
    elif ffmpeg_pix_fmt in ["yuv420p10le", "yuv422p10le", "yuv444p10le"]:
        bitdepth = 10
    elif ffmpeg_pix_fmt in ["yuv420p12le", "yuv422p12le", "yuv444p12le"]:
        bitdepth = 12
    elif ffmpeg_pix_fmt in ["yuv420p16le", "yuv422p16le", "yuv444p16le"]:
        bitdepth = 16
    else:
        assert False
    return pixel_format, bitdepth


class ExternalProgram(object):
    """
    External C programs relied upon by the python vmaf code
    These external programs should be compiled before vmaf is ran, as per instructions in README
    """

    try:
        from . import externals
    except ImportError:
        # The externals module is an optional local override; VmafExternalConfig
        # below resolves paths without it (environment overrides, then the
        # in-tree build).
        pass

    external_vmaf_feature = config.VmafExternalConfig.vmaf_path()
    external_vmafexec = config.VmafExternalConfig.vmafexec_path()

    build_dir = os.environ.get("VMAF_BUILD_DIR", os.path.join("core", "build"))
    vmaf_feature = (
        project_path(os.path.join(build_dir, "tools", "vmaf_feature"))
        if external_vmaf_feature is None
        else external_vmaf_feature
    )
    vmafexec = (
        project_path(os.path.join(build_dir, "tools", "vmaf"))
        if external_vmafexec is None
        else external_vmafexec
    )


def _multi_features_run_arguments(options):
    """``--backend``, ``--cpumask`` and ``--threads`` of one multi-feature run, in that order."""
    args = []

    backend = None
    if options is not None and "backend" in options:
        backend = options["backend"]
    if not backend:
        backend = os.environ.get("VMAF_FORCE_BACKEND") or os.environ.get("VMAF_BACKEND")
    if backend:
        args += ["--backend", str(backend)]

    if options is not None and "disable_avx" in options:
        assert isinstance(options["disable_avx"], bool)
        if options["disable_avx"] is True:
            # 0xFFFFFFFF disables all CPU ISA extensions (all mask bits set).
            # parse_unsigned() now rejects negative strings such as "-1"
            # (ADR-1088); pass the unsigned equivalent instead.
            args += ["--cpumask", "4294967295"]

    if options is not None and "n_threads" in options:
        assert isinstance(options["n_threads"], int) and options["n_threads"] >= 1
        args += ["--threads", str(options["n_threads"])]

    return args


def _feature_argument(feature, options):
    """The value of one ``--feature`` argument: ``name`` or ``name=key=value:key=value``."""
    if options is None:
        return feature
    assert isinstance(options, dict)
    if feature in options and options[feature] is not None and len(options[feature]) > 0:
        assert isinstance(options[feature], dict)
        options_lst = []
        for k, v in options[feature].items():
            if isinstance(v, bool):
                v = str(v).lower()
            options_lst.append(f"{k}={v}")
        options_str = ":".join(options_lst)
        return "=".join([feature, options_str])
    return feature


def _vmafexec_base_command(
    exe, reference, distorted, width, height, pixel_format, bitdepth, output
):
    """The executable and the arguments every ``call_vmafexec`` run has."""
    return (
        "{exe} --reference {reference} --distorted {distorted} --width {width} --height {height} "
        "--pixel_format {pixel_format} --bitdepth {bitdepth} --output {output}".format(
            exe=exe,
            reference=reference,
            distorted=distorted,
            width=width,
            height=height,
            pixel_format=pixel_format,
            bitdepth=bitdepth,
            output=output,
        )
    )


def _vmafexec_feature_flags(
    float_psnr, psnr, float_ssim, ssim, float_ms_ssim, ms_ssim, float_moment
):
    """The ``--feature`` flags of ``call_vmafexec``, in the order the command has always had."""
    flags = ""
    if float_psnr:
        flags += " --feature float_psnr"
    if float_ssim:
        flags += " --feature float_ssim"
    if float_ms_ssim:
        flags += " --feature float_ms_ssim"
    if float_moment:
        flags += " --feature float_moment"

    if psnr:
        flags += " --feature psnr"
    if ssim:
        # flags += ' --feature ssim'
        assert False, "ssim (the daala integer ssim) is deprecated"
    if ms_ssim:
        flags += " --feature ms_ssim"
    return flags


_VMAFEXEC_COLOR_KEYS = {"range", "primaries", "trc", "matrix"}


def _vmafexec_color_flags(color_ref, color_dist):
    """``--color_<attribute>_ref`` / ``--color_<attribute>_dist`` of ``call_vmafexec``.

    Per-input source colorimetry, e.g. ``{'range': 'limited', 'primaries': 'bt2020',
    'trc': 'smpte2084', 'matrix': 'bt2020nc'}``. All four keys are required for an
    input that is given; ``None`` leaves that input unspecified.
    """
    flags = ""
    for suffix, color in (("ref", color_ref), ("dist", color_dist)):
        if color is None:
            continue
        assert (
            set(color) == _VMAFEXEC_COLOR_KEYS
        ), "color_{} needs exactly range, primaries, trc and matrix".format(suffix)
        for attribute, value in color.items():
            flags += " --color_{}_{} {}".format(attribute, suffix, value)
    return flags


def _vmafexec_model_overloads(
    vif_enhn_gain_limit, adm_enhn_gain_limit, motion_force_zero, enc_width, enc_height, enc_bitdepth
):
    """The ``:feature.option=value`` suffix that follows a ``--model`` argument.

    Pure: it is evaluated for every model, so each model gets the same suffix.
    """
    suffix = ""

    # FIXME: hacky - since we do not know which feature is the one used in the model,
    # we have to set the parameter for all three, at the expense of extra computation.

    if vif_enhn_gain_limit is not None:
        suffix += f":vif.vif_enhn_gain_limit={vif_enhn_gain_limit}:float_vif.vif_enhn_gain_limit={vif_enhn_gain_limit}"
    if adm_enhn_gain_limit is not None:
        suffix += f":adm.adm_enhn_gain_limit={adm_enhn_gain_limit}:float_adm.adm_enhn_gain_limit={adm_enhn_gain_limit}"
    if motion_force_zero:
        assert isinstance(motion_force_zero, bool)
        force_zero = str(motion_force_zero).lower()
        suffix += (
            f":motion.motion_force_zero={force_zero}:float_motion.motion_force_zero={force_zero}"
        )

    # CAMBI encode-resolution / encode-bitdepth overrides. These
    # flow through to the cambi feature so its feature-name key
    # snapshots encbd/ench/encw (e.g. for VMAF v1.0.16 models).
    # Matches Netflix upstream call_vmafexec.
    if enc_width is not None:
        suffix += f":cambi.enc_width={enc_width}"
    if enc_height is not None:
        suffix += f":cambi.enc_height={enc_height}"
    if enc_bitdepth is not None:
        suffix += f":cambi.enc_bitdepth={enc_bitdepth}"
    return suffix


def _vmafexec_model_flags(no_prediction, models, overload_args):
    """``--no_prediction``, or one ``--model`` argument per model with its overload suffix.

    The suffix is built once per model, after that model's argument, so its
    assertions fire where they always did (not at all without a model).
    """
    if no_prediction:
        return " --no_prediction"
    assert models is not None
    assert isinstance(models, list)
    flags = ""
    for model in models:
        flags += " --model {}".format(model)
        flags += _vmafexec_model_overloads(*overload_args)
    return flags


def _vmafexec_run_flags(subsample, n_threads, disable_avx, backend):
    """``--subsample``, ``--threads``, ``--cpumask`` and ``--backend`` of ``call_vmafexec``."""
    flags = ""

    assert isinstance(subsample, int) and subsample >= 1
    if subsample != 1:
        flags += " --subsample {}".format(subsample)

    assert isinstance(n_threads, int) and n_threads >= 1
    if n_threads != 1:
        flags += " --threads {}".format(n_threads)

    if disable_avx:
        # 0xFFFFFFFF disables all CPU ISA extensions (all mask bits set).
        # parse_unsigned() rejects negative strings (ADR-1088).
        flags += " --cpumask 4294967295"

    if backend is None:
        backend = os.environ.get("VMAF_FORCE_BACKEND") or os.environ.get("VMAF_BACKEND")
    if backend:
        flags += f" --backend {backend}"
    return flags


class ExternalProgramCaller(object):
    """
    Caller of ExternalProgram.
    """

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

        cmd += _multi_features_run_arguments(options)

        for feature in features:
            cmd += ["--feature", _feature_argument(feature, options)]

        if logger:
            logger.info(" ".join(cmd))
        run_process(" ".join(cmd), shell=True)

    @staticmethod
    def call_vifdiff_feature(yuv_type, ref_path, dis_path, w, h, log_file_path, logger=None):

        # APPEND (>>) result (since _prepare_generate_log_file method has already created the file
        # and written something in advance).
        vifdiff_feature_cmd = (
            "{vmaf} vifdiff {yuv_type} {ref_path} {dis_path} {w} {h} >> {log_file_path}".format(
                vmaf=required(ExternalProgram.vmaf_feature),
                yuv_type=yuv_type,
                ref_path=ref_path,
                dis_path=dis_path,
                w=w,
                h=h,
                log_file_path=log_file_path,
            )
        )
        if logger:
            logger.info(vifdiff_feature_cmd)
        run_process(vifdiff_feature_cmd, shell=True)

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
        color_ref=None,
        color_dist=None,
    ):
        if exe is None:
            exe = required(ExternalProgram.vmafexec)

        vmafexec_cmd = _vmafexec_base_command(
            exe, reference, distorted, width, height, pixel_format, bitdepth, output
        )
        vmafexec_cmd += _vmafexec_color_flags(color_ref, color_dist)
        vmafexec_cmd += _vmafexec_feature_flags(
            float_psnr, psnr, float_ssim, ssim, float_ms_ssim, ms_ssim, float_moment
        )
        vmafexec_cmd += _vmafexec_model_flags(
            no_prediction,
            models,
            (
                vif_enhn_gain_limit,
                adm_enhn_gain_limit,
                motion_force_zero,
                enc_width,
                enc_height,
                enc_bitdepth,
            ),
        )
        vmafexec_cmd += _vmafexec_run_flags(subsample, n_threads, disable_avx, backend)

        if logger:
            logger.info(vmafexec_cmd)

        run_process(vmafexec_cmd, shell=True)


def model_path(*components):
    return os.path.join(VMAF_PYTHON_ROOT, "model", *components)
