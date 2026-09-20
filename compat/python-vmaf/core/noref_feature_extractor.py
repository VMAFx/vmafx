import copy
from itertools import compress
from pathlib import Path
from typing import ClassVar

import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.ndimage import correlate1d
from scipy.special._ufuncs import gamma
from skimage.util import view_as_windows

from vmaf.core.executor import Executor, NorefExecutorMixin
from vmaf.tools.decorator import override

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"

from vmaf.core.feature_extractor import FeatureExtractor
from vmaf.tools.reader import YuvReader


class MomentNorefFeatureExtractor(NorefExecutorMixin, FeatureExtractor):

    # Resolve the static MRO conflict between NorefExecutorMixin._assert_an_asset
    # and FeatureExtractor._assert_an_asset (inherited from Executor) by binding
    # the mixin's classmethod explicitly at this class level. Runtime MRO already
    # picked NorefExecutorMixin's version; this just makes that decision visible
    # to static analysis. (CodeQL py/conflicting-attributes)
    _assert_an_asset = NorefExecutorMixin._assert_an_asset

    TYPE = "Moment_noref_feature"
    VERSION = "1.0"  # python only

    ATOM_FEATURES: ClassVar = [
        "1st",
        "2nd",
    ]  # order matters

    DERIVED_ATOM_FEATURES: ClassVar = [
        "var",
    ]

    def _generate_result(self, asset):
        # routine to generate feature scores in the log file.

        quality_w, quality_h = asset.quality_width_height
        with YuvReader(
            filepath=asset.dis_procfile_path,
            width=quality_w,
            height=quality_h,
            yuv_type=self._get_workfile_yuv_type(asset),
        ) as dis_yuv_reader:
            scores_mtx_list = []
            for dis_yuv in dis_yuv_reader:
                dis_y = dis_yuv[0]
                dis_y = dis_y.astype(np.double)
                firstm = dis_y.mean()
                secondm = dis_y.var() + firstm**2
                scores_mtx_list.append(np.hstack(([firstm], [secondm])))
            scores_mtx = np.vstack(scores_mtx_list)

        # write scores_mtx to log file
        log_file_path = self._get_log_file_path(asset)
        with Path(log_file_path).open("wb") as log_file:
            np.save(log_file, scores_mtx)

    def _get_feature_scores(self, asset):
        # routine to read the feature scores from the log file, and return
        # the scores in a dictionary format.

        log_file_path = self._get_log_file_path(asset)
        with Path(log_file_path).open("rb") as log_file:
            scores_mtx = np.load(log_file)

        _num_frm, num_features = scores_mtx.shape
        assert num_features == len(self.ATOM_FEATURES)

        feature_result = {}

        for idx, atom_feature in enumerate(self.ATOM_FEATURES):
            scores_key = self.get_scores_key(atom_feature)
            feature_result[scores_key] = list(scores_mtx[:, idx])

        return feature_result

    @classmethod
    @override(Executor)
    def _post_process_result(cls, result):

        result = super(MomentNorefFeatureExtractor, cls)._post_process_result(result)

        # calculate var from 1st, 2nd
        var_scores_key = cls.get_scores_key("var")
        first_scores_key = cls.get_scores_key("1st")
        second_scores_key = cls.get_scores_key("2nd")
        value = [
            m[1] - m[0] * m[0]
            for m in zip(
                result.result_dict[first_scores_key],
                result.result_dict[second_scores_key],
                strict=False,
            )
        ]
        result.result_dict[var_scores_key] = value

        # validate
        for feature in cls.DERIVED_ATOM_FEATURES:
            assert cls.get_scores_key(feature) in result.result_dict

        return result


class BrisqueNorefFeatureExtractor(NorefExecutorMixin, FeatureExtractor):

    # Static MRO disambiguation — see MomentNorefFeatureExtractor for rationale.
    # (CodeQL py/conflicting-attributes)
    _assert_an_asset = NorefExecutorMixin._assert_an_asset

    TYPE = "BRISQUE_noref_feature"

    # VERSION = "0.1"
    VERSION = "0.2"  # update PIL package to 3.2 to fix interpolation issue

    ATOM_FEATURES: ClassVar = [
        "alpha_m1",
        "sq_m1",
        "alpha_m2",
        "sq_m2",
        "alpha_m3",
        "sq_m3",
        "alpha11",
        "N11",
        "lsq11",
        "rsq11",
        "alpha12",
        "N12",
        "lsq12",
        "rsq12",
        "alpha13",
        "N13",
        "lsq13",
        "rsq13",
        "alpha14",
        "N14",
        "lsq14",
        "rsq14",
        "alpha21",
        "N21",
        "lsq21",
        "rsq21",
        "alpha22",
        "N22",
        "lsq22",
        "rsq22",
        "alpha23",
        "N23",
        "lsq23",
        "rsq23",
        "alpha24",
        "N24",
        "lsq24",
        "rsq24",
        "alpha31",
        "N31",
        "lsq31",
        "rsq31",
        "alpha32",
        "N32",
        "lsq32",
        "rsq32",
        "alpha33",
        "N33",
        "lsq33",
        "rsq33",
        "alpha34",
        "N34",
        "lsq34",
        "rsq34",
    ]  # order matters

    gamma_range = np.arange(0.2, 10, 0.001)
    a = gamma(2.0 / gamma_range) ** 2
    b = gamma(1.0 / gamma_range)
    c = gamma(3.0 / gamma_range)
    prec_gammas = a / (b * c)

    def _generate_result(self, asset):
        # routine to call the command-line executable and generate feature
        # scores in the log file.

        quality_w, quality_h = asset.quality_width_height
        with YuvReader(
            filepath=asset.dis_procfile_path,
            width=quality_w,
            height=quality_h,
            yuv_type=self._get_workfile_yuv_type(asset),
        ) as dis_yuv_reader:
            scores_mtx_list = []
            for dis_yuv in dis_yuv_reader:
                dis_y = dis_yuv[0]
                dis_y = dis_y.astype(np.double)
                fgroup1_dis, fgroup2_dis = self.mscn_extract(dis_y)
                scores_mtx_list.append(np.hstack((fgroup1_dis, fgroup2_dis)))
            scores_mtx = np.vstack(scores_mtx_list)

        # write scores_mtx to log file
        log_file_path = self._get_log_file_path(asset)
        with Path(log_file_path).open("wb") as log_file:
            np.save(log_file, scores_mtx)

    def _get_feature_scores(self, asset):
        # routine to read the feature scores from the log file, and return
        # the scores in a dictionary format.

        log_file_path = self._get_log_file_path(asset)
        with Path(log_file_path).open("rb") as log_file:
            scores_mtx = np.load(log_file)

        _num_frm, num_features = scores_mtx.shape
        assert num_features == len(self.ATOM_FEATURES)

        feature_result = {}

        for idx, atom_feature in enumerate(self.ATOM_FEATURES):
            scores_key = self.get_scores_key(atom_feature)
            feature_result[scores_key] = list(scores_mtx[:, idx])

        return feature_result

    @classmethod
    def _selected_aggd_features(cls, image):
        alpha, mean, _left, _right, left_square, right_square = cls.extract_aggd_features(image)
        return alpha, mean, left_square, right_square

    @classmethod
    def _paired_product_features(cls, image):
        return np.array(
            [
                value
                for paired_product in cls.paired_p(image)
                for value in cls._selected_aggd_features(paired_product)
            ]
        )

    @classmethod
    def mscn_extract(cls, img):
        height, width = np.shape(img)
        pyramid = [
            img,
            np.array(
                Image.fromarray(img).resize(
                    (int(width / 2.0), int(height / 2.0)), Image.Resampling.BILINEAR
                )
            ),
            np.array(
                Image.fromarray(img).resize(
                    (int(width / 4.0), int(height / 4.0)), Image.Resampling.BILINEAR
                )
            ),
        ]
        mscn_images = [cls.calc_image(image)[0] for image in pyramid]
        mscn_features = np.array(
            [value for image in mscn_images for value in cls.extract_ggd_features(image)]
        )
        paired_product_features = np.hstack(
            [cls._paired_product_features(image) for image in mscn_images]
        )
        return mscn_features, paired_product_features

    @staticmethod
    def gauss_window(lw, sigma):
        sd = float(sigma)
        lw = int(lw)
        weights = [0.0] * (2 * lw + 1)
        weights[lw] = 1.0
        curr_sum = 1.0
        sd *= sd
        for ii in range(1, lw + 1):
            tmp = np.exp(-0.5 * float(ii * ii) / sd)
            weights[lw + ii] = tmp
            weights[lw - ii] = tmp
            curr_sum += 2.0 * tmp
        for ii in range(2 * lw + 1):
            weights[ii] /= curr_sum
        return weights

    @classmethod
    def calc_image(cls, image, extend_mode="constant"):
        avg_window = cls.gauss_window(3, 7.0 / 6.0)
        w, h = np.shape(image)
        mu_image = np.zeros((w, h))
        var_image = np.zeros((w, h))
        image = np.array(image).astype("float")
        correlate1d(image, avg_window, 0, mu_image, mode=extend_mode)
        correlate1d(mu_image, avg_window, 1, mu_image, mode=extend_mode)
        correlate1d(image**2, avg_window, 0, var_image, mode=extend_mode)
        correlate1d(var_image, avg_window, 1, var_image, mode=extend_mode)
        var_image = np.sqrt(np.abs(var_image - mu_image**2))
        return (image - mu_image) / (var_image + 1), var_image, mu_image

    @staticmethod
    def paired_p(new_im):
        hr_shift = np.roll(new_im, 1, axis=1)
        hl_shift = np.roll(new_im, -1, axis=1)
        v_shift = np.roll(new_im, 1, axis=0)
        vr_shift = np.roll(hr_shift, 1, axis=0)
        vl_shift = np.roll(hl_shift, 1, axis=0)

        h_img = hr_shift * new_im
        v_img = v_shift * new_im
        d1_img = vr_shift * new_im
        D2_img = vl_shift * new_im

        return v_img, h_img, d1_img, D2_img

    @classmethod
    def extract_ggd_features(cls, imdata):
        nr_gam = 1.0 / cls.prec_gammas
        sigma_sq = np.average(imdata**2)
        E = np.average(np.abs(imdata))
        rho = sigma_sq / E**2
        pos = np.argmin(np.abs(nr_gam - rho))
        return cls.gamma_range[pos], np.sqrt(sigma_sq)

    @classmethod
    def extract_aggd_features(cls, imdata):
        imdata_cp = np.reshape(imdata.copy(), -1)
        imdata2 = imdata_cp * imdata_cp
        left_data = imdata2[imdata_cp < 0]
        right_data = imdata2[imdata_cp >= 0]
        left_mean_sqrt = 0
        right_mean_sqrt = 0
        if len(left_data) > 0:
            left_mean_sqrt = np.sqrt(np.average(left_data))
        if len(right_data) > 0:
            right_mean_sqrt = np.sqrt(np.average(right_data))

        gamma_hat = left_mean_sqrt / right_mean_sqrt
        # solve r-hat norm
        r_hat = (np.average(np.abs(imdata_cp)) ** 2) / (np.average(imdata2))
        rhat_norm = r_hat * (((gamma_hat**3 + 1) * (gamma_hat + 1)) / ((gamma_hat**2 + 1) ** 2))

        # solve alpha by guessing values that minimize ro
        pos = np.argmin(np.abs(cls.prec_gammas - rhat_norm))
        alpha = cls.gamma_range[pos]

        gam1 = gamma(1.0 / alpha)
        gam2 = gamma(2.0 / alpha)
        gam3 = gamma(3.0 / alpha)

        aggdratio = np.sqrt(gam1) / np.sqrt(gam3)
        bl = aggdratio * left_mean_sqrt
        br = aggdratio * right_mean_sqrt

        # mean parameter
        N = (br - bl) * (gam2 / gam1) * aggdratio
        return alpha, N, bl, br, left_mean_sqrt, right_mean_sqrt


class NiqeNorefFeatureExtractor(BrisqueNorefFeatureExtractor):

    TYPE = "NIQE_noref_feature"

    VERSION = "0.1"

    ATOM_FEATURES: ClassVar = [
        "alpha_m1",
        "blbr1",
        "alpha11",
        "N11",
        "lsq11",
        "rsq11",
        "alpha12",
        "N12",
        "lsq12",
        "rsq12",
        "alpha13",
        "N13",
        "lsq13",
        "rsq13",
        "alpha14",
        "N14",
        "lsq14",
        "rsq14",
        "alpha_m2",
        "blbr2",
        "alpha21",
        "N21",
        "lsq21",
        "rsq21",
        "alpha22",
        "N22",
        "lsq22",
        "rsq22",
        "alpha23",
        "N23",
        "lsq23",
        "rsq23",
        "alpha24",
        "N24",
        "lsq24",
        "rsq24",
    ]  # order matters

    DEFAULT_PATCH_SIZE = 96
    DEFAULT_VAR_THRESHOLD = 0.75

    @property
    def patch_size(self):
        if self.optional_dict and "patch_size" in self.optional_dict:
            return self.optional_dict["patch_size"]
        return self.DEFAULT_PATCH_SIZE

    @property
    def mode(self):
        if self.optional_dict and "mode" in self.optional_dict:
            mode = self.optional_dict["mode"]
            assert mode in {"train", "test"}
            return mode
        return "test"

    def _generate_result(self, asset):
        # routine to call the command-line executable and generate feature
        # scores in the log file.

        quality_w, quality_h = asset.quality_width_height
        with YuvReader(
            filepath=asset.dis_procfile_path,
            width=quality_w,
            height=quality_h,
            yuv_type=self._get_workfile_yuv_type(asset),
        ) as dis_yuv_reader:
            scores_mtx_list = []
            for dis_yuv in dis_yuv_reader:
                dis_y = dis_yuv[0]
                dis_y = dis_y.astype(np.double)
                list_features = self.mscn_extract_niqe(dis_y, self.patch_size, self.mode)
                scores_mtx_list += list_features
            scores_mtx = np.vstack(scores_mtx_list)

        # write scores_mtx to log file
        log_file_path = self._get_log_file_path(asset)
        with Path(log_file_path).open("wb") as log_file:
            np.save(log_file, scores_mtx)

    @classmethod
    def _niqe_scale_features(cls, patch):
        alpha, _mean, left, right, _left_square, _right_square = cls.extract_aggd_features(patch)
        return np.hstack(
            (
                np.array([alpha, (left + right) / 2.0]),
                cls._paired_product_features(patch),
            )
        )

    @classmethod
    def _filter_niqe_features(cls, features, variance, patch_size, mode):
        if mode == "test":
            return features
        if mode != "train":
            raise AssertionError()
        variance_field = view_as_windows(
            variance, (patch_size, patch_size), step=patch_size
        ).reshape(-1, patch_size, patch_size)
        average_variance = np.mean(np.mean(variance_field, axis=2), axis=1)
        average_variance /= np.max(average_variance)
        return list(compress(features, average_variance > cls.DEFAULT_VAR_THRESHOLD))

    @classmethod
    def mscn_extract_niqe(cls, img, patch_size, mode):
        height, width = img.shape
        half_image = np.array(
            Image.fromarray(img).resize(
                (int(width / 2.0), int(height / 2.0)), Image.Resampling.BICUBIC
            )
        )
        full_mscn, variance, _ = cls.calc_image(img, extend_mode="nearest")
        half_mscn, _, _ = cls.calc_image(half_image, extend_mode="nearest")
        full_mscn = full_mscn.astype(np.float32)
        half_mscn = half_mscn.astype(np.float32)
        features = []
        for row in range(0, height - patch_size + 1, patch_size):
            for column in range(0, width - patch_size + 1, patch_size):
                full_patch = full_mscn[row : row + patch_size, column : column + patch_size]
                half_patch = half_mscn[
                    row // 2 : (row + patch_size) // 2,
                    column // 2 : (column + patch_size) // 2,
                ]
                features.append(
                    np.hstack(
                        (
                            cls._niqe_scale_features(full_patch),
                            cls._niqe_scale_features(half_patch),
                        )
                    )
                )
        return cls._filter_niqe_features(features, variance, patch_size, mode)


class SiTiNorefFeatureExtractor(NorefExecutorMixin, FeatureExtractor):

    # Static MRO disambiguation — see MomentNorefFeatureExtractor for rationale.
    # (CodeQL py/conflicting-attributes)
    _assert_an_asset = NorefExecutorMixin._assert_an_asset

    TYPE = "SITI_noref_feature"
    VERSION = "1.0"

    ATOM_FEATURES: ClassVar = ["si", "ti"]  # order matters

    @staticmethod
    def sobel_filt(img):

        dx = ndimage.sobel(img, 1)  # horizontal derivative
        dy = ndimage.sobel(img, 0)  # vertical derivative
        return np.hypot(dx, dy)  # magnitude

    def _generate_result(self, asset):
        # routine to generate feature scores in the log file.

        quality_w, quality_h = asset.quality_width_height
        yuv_type = self._get_workfile_yuv_type(asset)
        assert (
            yuv_type in YuvReader.SUPPORTED_YUV_8BIT_TYPES
        ), "{} only work with 8 bit for now.".format(self.__class__.__name__)
        with YuvReader(
            filepath=asset.dis_procfile_path, width=quality_w, height=quality_h, yuv_type=yuv_type
        ) as dis_yuv_reader:
            scores_mtx_list = []
            dis_y_prev = None
            for dis_yuv in dis_yuv_reader:
                dis_y = dis_yuv[0].astype("int32")
                mag = self.sobel_filt(dis_y)
                si = np.std(mag)
                ti = 0 if dis_y_prev is None else np.std(dis_y - dis_y_prev)
                dis_y_prev = copy.deepcopy(dis_y)
                scores_mtx_list.append(np.hstack(([si], [ti])))
            scores_mtx = np.vstack(scores_mtx_list)

            # write scores_mtx to log file
        log_file_path = self._get_log_file_path(asset)
        with Path(log_file_path).open("wb") as log_file:
            np.save(log_file, scores_mtx)

    def _get_feature_scores(self, asset):
        # routine to read the feature scores from the log file, and return
        # the scores in a dictionary format.

        log_file_path = self._get_log_file_path(asset)
        with Path(log_file_path).open("rb") as log_file:
            scores_mtx = np.load(log_file)

        _num_frm, num_features = scores_mtx.shape
        assert num_features == len(self.ATOM_FEATURES)

        feature_result = {}

        for idx, atom_feature in enumerate(self.ATOM_FEATURES):
            scores_key = self.get_scores_key(atom_feature)
            feature_result[scores_key] = list(scores_mtx[:, idx])

        return feature_result
