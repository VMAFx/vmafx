import os
import subprocess

import numpy as np

from vmaf.config import VmafConfig, VmafExternalConfig
from vmaf.core.executor import Executor
from vmaf.core.feature_extractor import FeatureExtractor
from vmaf.tools.decorator import override
from vmaf.tools.misc import make_absolute_path
from vmaf.tools.stats import ListStats

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


def _run_matlab(matlab_bin: str, matlab_script: str, log_file_path: str | None = None) -> None:
    """Invoke MATLAB safely with a single -r script argument.

    Replaces the historical ``run_process(' '.join(cmd), shell=True)``
    pattern that joined user-derived asset paths into a shell string
    (CWE-78: shell injection). We build the argv list explicitly and
    bypass the shell entirely. If ``log_file_path`` is provided the
    process stdout is appended to it — matching the previous
    ``>> {log_file_path}`` redirect semantics without invoking a shell.

    Stderr is folded into stdout so the captured/redirected log
    matches the prior behaviour for MATLAB error diagnostics.
    """
    argv: list[str] = [
        matlab_bin,
        "-nodisplay",
        "-nosplash",
        "-nodesktop",
        "-r",
        matlab_script,
    ]
    # Force a deterministic C locale (mirrors ProcessRunner) so any
    # AssertionError text raised on a non-English host stays English.
    # Unconditional assignment: setdefault() would leave a host-set
    # LANG=de_DE.UTF-8 in place, defeating the intent.
    env = dict(os.environ)
    env["LC_ALL"] = "C"
    env["LANG"] = "C"
    try:
        if log_file_path is None:
            subprocess.check_output(argv, stderr=subprocess.STDOUT, env=env, shell=False)
        else:
            with open(log_file_path, "ab") as log_fh:
                subprocess.run(
                    argv,
                    stdout=log_fh,
                    stderr=subprocess.STDOUT,
                    env=env,
                    shell=False,
                    check=True,
                )
    except subprocess.CalledProcessError as e:
        raise AssertionError(f"Process returned {e.returncode}, cmd: {argv}, msg: {str(e.output)}")


class MatlabFeatureExtractor(FeatureExtractor):

    @classmethod
    @override(Executor)
    def _assert_class(cls):
        super(MatlabFeatureExtractor, cls)._assert_class()
        VmafExternalConfig.get_and_assert_matlab()


class StrredFeatureExtractor(MatlabFeatureExtractor):
    TYPE = "STRRED_feature"

    # VERSION = '1.0'
    # VERSION = '1.1' # fix matlab code where width and height are mistakenly swapped
    # VERSION = '1.2' # fix minor frame and prev frame swap issue
    VERSION = "1.3"  # align ST-RRED with ST-RREDopt calculations

    ATOM_FEATURES = [
        "srred",
        "trred",
    ]

    DERIVED_ATOM_FEATURES = [
        "strred",
    ]

    MATLAB_WORKSPACE = VmafConfig.root_path("python", "vmaf", "matlab", "strred")

    @classmethod
    def _assert_an_asset(cls, asset):
        super(StrredFeatureExtractor, cls)._assert_an_asset(asset)
        assert (
            asset.ref_yuv_type == "yuv420p" and asset.dis_yuv_type == "yuv420p"
        ), "STRRED feature extractor only supports yuv420p for now."

    def _generate_result(self, asset):
        # routine to call the command-line executable and generate quality
        # scores in the log file.

        ref_procfile_path = asset.ref_procfile_path
        dis_procfile_path = asset.dis_procfile_path
        log_file_path = self._get_log_file_path(asset)

        current_dir = os.getcwd() + "/"

        ref_procfile_path = make_absolute_path(ref_procfile_path, current_dir)
        dis_procfile_path = make_absolute_path(dis_procfile_path, current_dir)
        log_file_path = make_absolute_path(log_file_path, current_dir)

        quality_width, quality_height = asset.quality_width_height

        matlab_bin = VmafExternalConfig.get_and_assert_matlab()
        matlab_script = "run_strred('{ref}', '{dis}', {h}, {w}); exit;".format(
            ref=ref_procfile_path,
            dis=dis_procfile_path,
            w=quality_width,
            h=quality_height,
        )
        if self.logger:
            self.logger.info(
                "%s -nodisplay -nosplash -nodesktop -r %r >> %s",
                matlab_bin,
                matlab_script,
                log_file_path,
            )

        os.chdir(self.MATLAB_WORKSPACE)
        # Argv-list invocation (no shell) — fixes CWE-78 shell injection
        # via asset paths. Log redirect is performed by `_run_matlab`
        # opening `log_file_path` in append mode.
        _run_matlab(matlab_bin, matlab_script, log_file_path=log_file_path)
        os.chdir(current_dir)

    @classmethod
    @override(Executor)
    def _post_process_result(cls, result):

        def _strred(srred_trred):
            srred, trred = srred_trred
            try:
                return srred * trred
            except TypeError:  # possible either srred or trred is None
                return None

        result = super(StrredFeatureExtractor, cls)._post_process_result(result)

        # calculate refvar and disvar from ref1st, ref2nd, dis1st, dis2nd
        srred_scores_key = cls.get_scores_key("srred")
        trred_scores_key = cls.get_scores_key("trred")
        strred_scores_key = cls.get_scores_key("strred")

        strred_all_same_scores_key = cls.get_scores_key("strred_all_same")

        srred_scores = result.result_dict[srred_scores_key]
        trred_scores = result.result_dict[trred_scores_key]

        assert len(srred_scores) == len(trred_scores)

        # === Way One: consistent with VMAF framework, which is to multiply S and T scores per frame, then average
        strred_scores = list(map(_strred, zip(srred_scores, trred_scores)))
        # === Way Two: authentic way of calculating STRRED score: average first, then multiply ===
        strred_all_same_scores = (
            ListStats.nonemean(srred_scores)
            * ListStats.nonemean(trred_scores)
            * np.ones(len(srred_scores))
        )

        result.result_dict[strred_all_same_scores_key] = strred_all_same_scores
        result.result_dict[strred_scores_key] = strred_scores

        # validate
        for feature in cls.DERIVED_ATOM_FEATURES:
            assert cls.get_scores_key(feature) in result.result_dict

        return result


class StrredOptFeatureExtractor(MatlabFeatureExtractor):
    TYPE = "STRREDOpt_feature"

    VERSION = "1.1"  # aligned ST-RREDopt computation, i.e. each current and previous frame for calculation and append to the ST-RREDopt of the first frame the result from the 2nd one

    ATOM_FEATURES = [
        "srred",
        "trred",
    ]

    DERIVED_ATOM_FEATURES = ["strred", "strred_all_same"]

    MATLAB_WORKSPACE = VmafConfig.root_path("python", "vmaf", "matlab", "strred")

    @classmethod
    def _assert_an_asset(cls, asset):
        super(StrredOptFeatureExtractor, cls)._assert_an_asset(asset)
        assert (
            asset.ref_yuv_type == "yuv420p" and asset.dis_yuv_type == "yuv420p"
        ), "STRRED opt feature extractor only supports yuv420p for now."

    def _generate_result(self, asset):
        # routine to call the command-line executable and generate quality
        # scores in the log file.

        ref_procfile_path = asset.ref_procfile_path
        dis_procfile_path = asset.dis_procfile_path
        log_file_path = self._get_log_file_path(asset)

        current_dir = os.getcwd() + "/"

        ref_procfile_path = make_absolute_path(ref_procfile_path, current_dir)
        dis_procfile_path = make_absolute_path(dis_procfile_path, current_dir)
        log_file_path = make_absolute_path(log_file_path, current_dir)

        quality_width, quality_height = asset.quality_width_height

        matlab_bin = VmafExternalConfig.get_and_assert_matlab()
        matlab_script = "run_strred_opt('{ref}', '{dis}', {w}, {h}); exit;".format(
            ref=ref_procfile_path,
            dis=dis_procfile_path,
            w=quality_width,
            h=quality_height,
        )

        if self.logger:
            self.logger.info(
                "%s -nodisplay -nosplash -nodesktop -r %r >> %s",
                matlab_bin,
                matlab_script,
                log_file_path,
            )

        os.chdir(self.MATLAB_WORKSPACE)
        # Argv-list invocation (no shell) — fixes CWE-78 shell injection.
        _run_matlab(matlab_bin, matlab_script, log_file_path=log_file_path)
        os.chdir(current_dir)

    @classmethod
    @override(Executor)
    def _post_process_result(cls, result):

        def _strred(srred_trred):
            srred, trred = srred_trred
            if srred is not None and trred is not None:
                return srred * trred
            elif srred is None:
                # Covers (None, None) -> trred is None -> returns None.
                return trred
            else:
                # srred is not None and trred is None.
                return srred
            # (CodeQL py/unreachable-statement: prior `else: return None`
            # was unreachable — the `srred is None` branch already covers
            # the (None, None) case.)

        result = super(StrredOptFeatureExtractor, cls)._post_process_result(result)

        srred_scores_key = cls.get_scores_key("srred")
        trred_scores_key = cls.get_scores_key("trred")
        strred_scores_key = cls.get_scores_key("strred")

        strred_all_same_scores_key = cls.get_scores_key("strred_all_same")

        srred_scores = result.result_dict[srred_scores_key]
        trred_scores = result.result_dict[trred_scores_key]

        assert len(srred_scores) == len(trred_scores)

        # === Way One: consistent with VMAF framework, which is to multiply S and T scores per frame, then average
        strred_scores = list(map(_strred, zip(srred_scores, trred_scores)))
        # === Way Two: authentic way of calculating STRRED score: average first, then multiply ===
        strred_all_same_scores = (
            ListStats.nonemean(srred_scores)
            * ListStats.nonemean(trred_scores)
            * np.ones(len(srred_scores))
        )

        result.result_dict[strred_all_same_scores_key] = strred_all_same_scores
        result.result_dict[strred_scores_key] = strred_scores

        # validate
        for feature in cls.DERIVED_ATOM_FEATURES:
            assert cls.get_scores_key(feature) in result.result_dict

        return result


class SpEEDMatlabFeatureExtractor(MatlabFeatureExtractor):
    TYPE = "SpEED_Matlab_feature"

    VERSION = "0.1"

    scale_list = [2, 3, 4]
    ATOM_FEATURES = []
    DERIVED_ATOM_FEATURES = []
    for scale_now in scale_list:
        ATOM_FEATURES.append("sspeed_" + str(scale_now))
        ATOM_FEATURES.append("tspeed_" + str(scale_now))
        DERIVED_ATOM_FEATURES.append("speed_" + str(scale_now))

    MATLAB_WORKSPACE = VmafConfig.root_path("python", "vmaf", "matlab", "SpEED")

    def _generate_result(self, asset):

        # routine to call the command-line executable and generate quality
        # scores in the log file.
        ref_procfile_path = asset.ref_procfile_path
        dis_procfile_path = asset.dis_procfile_path
        log_file_path = self._get_log_file_path(asset)
        current_dir = os.getcwd() + "/"
        ref_procfile_path = make_absolute_path(ref_procfile_path, current_dir)
        dis_procfile_path = make_absolute_path(dis_procfile_path, current_dir)
        log_file_path = make_absolute_path(log_file_path, current_dir)
        quality_width, quality_height = asset.quality_width_height
        matlab_bin = VmafExternalConfig.get_and_assert_matlab()
        matlab_script = (
            "run_speed('{ref}', '{dis}', {w}, {h}, {bands}, '{yuv_type}'); exit;".format(
                ref=ref_procfile_path,
                dis=dis_procfile_path,
                w=quality_width,
                h=quality_height,
                bands=self.scale_list,
                yuv_type=self._get_workfile_yuv_type(asset),
            )
        )
        if self.logger:
            self.logger.info(
                "%s -nodisplay -nosplash -nodesktop -r %r >> %s",
                matlab_bin,
                matlab_script,
                log_file_path,
            )
        os.chdir(self.MATLAB_WORKSPACE)
        # Argv-list invocation (no shell) — fixes CWE-78 shell injection.
        _run_matlab(matlab_bin, matlab_script, log_file_path=log_file_path)
        os.chdir(current_dir)

    @classmethod
    @override(Executor)
    def _post_process_result(cls, result):

        def _speed(sspeed_tspeed):
            sspeed, tspeed = sspeed_tspeed
            if sspeed is not None and tspeed is not None:
                return sspeed * tspeed
            elif sspeed is None:
                # Covers (None, None) -> tspeed is None -> returns None.
                return tspeed
            else:
                # sspeed is not None and tspeed is None.
                return sspeed
            # (CodeQL py/unreachable-statement: prior `else: return None`
            # was unreachable — the `sspeed is None` branch already covers
            # the (None, None) case.)

        result = super(SpEEDMatlabFeatureExtractor, cls)._post_process_result(result)
        for scale_now in cls.scale_list:
            sspeed_scale_now_scores_key = cls.get_scores_key("sspeed_" + str(scale_now))
            tspeed_scale_now_scores_key = cls.get_scores_key("tspeed_" + str(scale_now))
            speed_scale_now_scores_key = cls.get_scores_key("speed_" + str(scale_now))
            sspeed_scale_now_scores = result.result_dict[sspeed_scale_now_scores_key]
            tspeed_scale_now_scores = result.result_dict[tspeed_scale_now_scores_key]
            assert len(sspeed_scale_now_scores) == len(tspeed_scale_now_scores)
            # consistent with VMAF framework, which is to multiply S and T scores per frame, then average
            speed_scale_now_scores = list(
                map(_speed, zip(sspeed_scale_now_scores, tspeed_scale_now_scores))
            )
            result.result_dict[speed_scale_now_scores_key] = speed_scale_now_scores

        # validate
        for feature in cls.DERIVED_ATOM_FEATURES:
            assert cls.get_scores_key(feature) in result.result_dict
        return result


class STMADFeatureExtractor(MatlabFeatureExtractor):
    TYPE = "STMAD_feature"

    VERSION = "0.1"

    ATOM_FEATURES = ["smad", "tmad", "stmad"]

    DERIVED_ATOM_FEATURES = ["smad_all_same", "tmad_all_same", "stmad_all_same"]

    MATLAB_WORKSPACE = VmafConfig.root_path("python", "vmaf", "matlab", "STMAD_2011_MatlabCode")

    # compile necessary functions; need to use mex from within matlab
    def _custom_init(self):

        def run_stmad_mex(matlab_bin: str, mex_script: str) -> None:
            current_dir = os.getcwd() + "/"
            os.chdir(self.MATLAB_WORKSPACE)
            # Argv-list invocation (no shell) — fixes CWE-78 shell injection.
            _run_matlab(matlab_bin, mex_script)
            os.chdir(current_dir)

        matlab_bin = VmafExternalConfig.get_and_assert_matlab()
        run_stmad_mex(matlab_bin, "mex ical_std.c; exit;")
        run_stmad_mex(matlab_bin, "mex ical_stat.c; exit;")

    @classmethod
    def _assert_an_asset(cls, asset):
        super(STMADFeatureExtractor, cls)._assert_an_asset(asset)
        assert (
            asset.ref_yuv_type == "yuv420p" and asset.dis_yuv_type == "yuv420p"
        ), "STMAD feature extractor only supports yuv420p for now."

    def _generate_result(self, asset):
        # routine to call the command-line executable and generate quality
        # scores in the log file.

        ref_procfile_path = asset.ref_procfile_path
        dis_procfile_path = asset.dis_procfile_path
        log_file_path = self._get_log_file_path(asset)

        current_dir = os.getcwd() + "/"

        ref_procfile_path = make_absolute_path(ref_procfile_path, current_dir)
        dis_procfile_path = make_absolute_path(dis_procfile_path, current_dir)
        log_file_path = make_absolute_path(log_file_path, current_dir)

        quality_width, quality_height = asset.quality_width_height

        matlab_bin = VmafExternalConfig.get_and_assert_matlab()
        matlab_script = "run_stmad('{ref}', '{dis}', {w}, {h}); exit;".format(
            ref=ref_procfile_path,
            dis=dis_procfile_path,
            w=quality_width,
            h=quality_height,
        )

        if self.logger:
            self.logger.info(
                "%s -nodisplay -nosplash -nodesktop -r %r >> %s",
                matlab_bin,
                matlab_script,
                log_file_path,
            )

        os.chdir(self.MATLAB_WORKSPACE)
        # Argv-list invocation (no shell) — fixes CWE-78 shell injection.
        _run_matlab(matlab_bin, matlab_script, log_file_path=log_file_path)
        os.chdir(current_dir)

    @classmethod
    @override(Executor)
    def _post_process_result(cls, result):

        result = super(STMADFeatureExtractor, cls)._post_process_result(result)

        smad_scores_key = cls.get_scores_key("smad")
        tmad_scores_key = cls.get_scores_key("tmad")
        stmad_scores_key = cls.get_scores_key("stmad")

        smad_all_same_scores_key = cls.get_scores_key("smad_all_same")
        tmad_all_same_scores_key = cls.get_scores_key("tmad_all_same")
        stmad_all_same_scores_key = cls.get_scores_key("stmad_all_same")

        # remember that smad, tmad, stmad are only one value for the whole video so
        # stmad_all_same_scores is identical to stmad_scores

        smad_scores = result.result_dict[smad_scores_key]
        tmad_scores = result.result_dict[tmad_scores_key]
        stmad_scores = result.result_dict[stmad_scores_key]

        result.result_dict[smad_scores_key] = smad_scores
        result.result_dict[tmad_scores_key] = tmad_scores
        result.result_dict[stmad_scores_key] = stmad_scores

        result.result_dict[smad_all_same_scores_key] = smad_scores
        result.result_dict[tmad_all_same_scores_key] = tmad_scores
        result.result_dict[stmad_all_same_scores_key] = stmad_scores

        # validate
        for feature in cls.DERIVED_ATOM_FEATURES:
            assert cls.get_scores_key(feature) in result.result_dict

        return result


class iCIDFeatureExtractor(MatlabFeatureExtractor):
    TYPE = "ICID_feature"

    VERSION = "1.0"

    ATOM_FEATURES = ["icid"]
    # DERIVED_ATOM_FEATURES = ['icid_all_same']

    MATLAB_WORKSPACE = VmafConfig.root_path("python", "vmaf", "matlab", "cid_icid")

    @classmethod
    def _assert_an_asset(cls, asset):
        super(iCIDFeatureExtractor, cls)._assert_an_asset(asset)
        assert asset.ref_yuv_type == asset.dis_yuv_type

    def _generate_result(self, asset):
        # routine to call the command-line executable and generate quality
        # scores in the log file.

        ref_workfile_path = asset.ref_workfile_path
        dis_workfile_path = asset.dis_workfile_path
        log_file_path = self._get_log_file_path(asset)

        current_dir = os.getcwd() + "/"

        ref_workfile_path = make_absolute_path(ref_workfile_path, current_dir)
        dis_workfile_path = make_absolute_path(dis_workfile_path, current_dir)
        log_file_path = make_absolute_path(log_file_path, current_dir)

        quality_width, quality_height = asset.quality_width_height

        matlab_bin = VmafExternalConfig.get_and_assert_matlab()
        matlab_script = "run_icid('{ref}', '{dis}', {h}, {w}, '{yuvtype}'); exit;".format(
            ref=ref_workfile_path,
            dis=dis_workfile_path,
            w=quality_width,
            h=quality_height,
            yuvtype=asset.ref_yuv_type,
        )
        if self.logger:
            self.logger.info(
                "%s -nodisplay -nosplash -nodesktop -r %r >> %s",
                matlab_bin,
                matlab_script,
                log_file_path,
            )

        os.chdir(self.MATLAB_WORKSPACE)
        # Argv-list invocation (no shell) — fixes CWE-78 shell injection.
        _run_matlab(matlab_bin, matlab_script, log_file_path=log_file_path)
        os.chdir(current_dir)

    @classmethod
    def _post_process_result(cls, result):
        # override Executor._post_process_result

        result = super(iCIDFeatureExtractor, cls)._post_process_result(result)

        # icid_scores_key = cls.get_scores_key('icid')
        # icid_all_same_scores_key = cls.get_scores_key('icid_all_same')
        # icid_scores = result.result_dict[icid_scores_key]
        # result.result_dict[icid_scores_key] = icid_scores
        # result.result_dict[icid_all_same_scores_key] = icid_scores
        #
        # # validate
        # for feature in cls.DERIVED_ATOM_FEATURES:
        #     assert cls.get_scores_key(feature) in result.result_dict

        return result
