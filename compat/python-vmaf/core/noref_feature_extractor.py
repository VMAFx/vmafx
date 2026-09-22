import copy
from itertools import compress

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

    ATOM_FEATURES = [
        "1st",
        "2nd",
    ]  # order matters

    DERIVED_ATOM_FEATURES = [
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
            i = 0
            for dis_yuv in dis_yuv_reader:
                dis_y = dis_yuv[0]
                dis_y = dis_y.astype(np.double)
                firstm = dis_y.mean()
                secondm = dis_y.var() + firstm**2
                scores_mtx_list.append(np.hstack(([firstm], [secondm])))
                i += 1
            scores_mtx = np.vstack(scores_mtx_list)

        # write scores_mtx to log file
        log_file_path = self._get_log_file_path(asset)
        with open(log_file_path, "wb") as log_file:
            np.save(log_file, scores_mtx)

    def _get_feature_scores(self, asset):
        # routine to read the feature scores from the log file, and return
        # the scores in a dictionary format.

        log_file_path = self._get_log_file_path(asset)
        with open(log_file_path, "rb") as log_file:
            scores_mtx = np.load(log_file)

        num_frm, num_features = scores_mtx.shape
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
        value = list(
            map(
                lambda m: m[1] - m[0] * m[0],
                zip(result.result_dict[first_scores_key], result.result_dict[second_scores_key]),
            )
        )
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

    ATOM_FEATURES = [
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
        with open(log_file_path, "wb") as log_file:
            np.save(log_file, scores_mtx)

    def _get_feature_scores(self, asset):
        # routine to read the feature scores from the log file, and return
        # the scores in a dictionary format.

        log_file_path = self._get_log_file_path(asset)
        with open(log_file_path, "rb") as log_file:
            scores_mtx = np.load(log_file)

        num_frm, num_features = scores_mtx.shape
        assert num_features == len(self.ATOM_FEATURES)

        feature_result = {}

        for idx, atom_feature in enumerate(self.ATOM_FEATURES):
            scores_key = self.get_scores_key(atom_feature)
            feature_result[scores_key] = list(scores_mtx[:, idx])

        return feature_result

    @classmethod
    def _bilinear_downscale(cls, img, factor):
        """Bilinear resize of *img* to 1/*factor* of its width and height."""
        return np.array(
            Image.fromarray(img).resize(
                (int(np.shape(img)[1] / factor), int(np.shape(img)[0] / factor)),
                Image.Resampling.BILINEAR,
            )
        )

    @classmethod
    def _paired_aggd_features(cls, m_image):
        """AGGD (alpha, N, lsq, rsq) of the V, H, D1 and D2 paired products.

        paired_p() yields the four orientations in that fixed order, so the
        returned flat list keeps the historical 4-per-orientation layout.
        """
        features = []
        for pps in cls.paired_p(m_image):
            alpha, N, bl, br, lsq, rsq = cls.extract_aggd_features(pps)
            features.extend([alpha, N, lsq, rsq])
        return features

    @classmethod
    def mscn_extract(cls, img):
        img2 = cls._bilinear_downscale(img, 2.0)
        img3 = cls._bilinear_downscale(img, 4.0)

        m_image, _, _ = cls.calc_image(img)
        m_image2, _, _ = cls.calc_image(img2)
        m_image3, _, _ = cls.calc_image(img3)

        alpha_m1, sq_m1 = cls.extract_ggd_features(m_image)
        alpha_m2, sq_m2 = cls.extract_ggd_features(m_image2)
        alpha_m3, sq_m3 = cls.extract_ggd_features(m_image3)

        mscn_features = np.array(
            [
                alpha_m1,
                sq_m1,  # 0, 1
                alpha_m2,
                sq_m2,  # 0, 1
                alpha_m3,
                sq_m3,  # 0, 1
            ]
        )

        # 12 groups of (alpha, N, lsq, rsq): three scales x (V, H, D1, D2).
        pp_features = np.array(
            cls._paired_aggd_features(m_image)
            + cls._paired_aggd_features(m_image2)
            + cls._paired_aggd_features(m_image3)
        )

        return mscn_features, pp_features

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
        imdata_cp = imdata.copy()
        imdata_cp.shape = (len(imdata_cp.flat),)
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

    ATOM_FEATURES = [
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
        else:
            return self.DEFAULT_PATCH_SIZE

    @property
    def mode(self):
        if self.optional_dict and "mode" in self.optional_dict:
            mode = self.optional_dict["mode"]
            assert mode == "train" or mode == "test"
            return mode
        else:
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
        with open(log_file_path, "wb") as log_file:
            np.save(log_file, scores_mtx)

    @classmethod
    def _niqe_patch_features(cls, m_patch):
        """One NIQE level vector: the patch's own AGGD pair then its paired products.

        Layout is (alpha, (bl + br) / 2) followed by 4 x (alpha, N, lsq, rsq)
        for the V, H, D1 and D2 orientations — 18 values in total.
        """
        alpha_m, N, bl, br, lsq, rsq = cls.extract_aggd_features(m_patch)
        return np.array([alpha_m, (bl + br) / 2.0] + cls._paired_aggd_features(m_patch))

    @classmethod
    def mscn_extract_niqe(cls, img, patch_size, mode):
        h, w = img.shape

        img2 = np.array(
            Image.fromarray(img).resize((int(w / 2.0), int(h / 2.0)), Image.Resampling.BICUBIC)
        )

        m_image1, img_var, _ = cls.calc_image(img, extend_mode="nearest")
        m_image1 = m_image1.astype(np.float32)

        m_image2, _, _ = cls.calc_image(img2, extend_mode="nearest")
        m_image2 = m_image2.astype(np.float32)

        block_w, block_h, shift_w, shift_h = patch_size, patch_size, patch_size, patch_size

        list_features = []

        for j in range(0, h - block_h + 1, shift_h):
            for i in range(0, w - block_w + 1, shift_w):
                m_patch1 = m_image1[j : j + block_h, i : i + block_w]
                m_patch2 = m_image2[j // 2 : (j + block_h) // 2, i // 2 : (i + block_w) // 2]

                lvl1_features = cls._niqe_patch_features(m_patch1)
                lvl2_features = cls._niqe_patch_features(m_patch2)

                list_features.append(np.hstack((lvl1_features, lvl2_features)))

        if mode == "train":
            variancefield = view_as_windows(img_var, (patch_size, patch_size), step=patch_size)
            variancefield = variancefield.reshape(-1, patch_size, patch_size)
            avg_variance = np.mean(np.mean(variancefield, axis=2), axis=1)
            avg_variance /= np.max(avg_variance)
            list_features = list(compress(list_features, avg_variance > cls.DEFAULT_VAR_THRESHOLD))
        elif mode == "test":
            pass
        else:
            assert False

        return list_features


class SiTiNorefFeatureExtractor(NorefExecutorMixin, FeatureExtractor):

    # Static MRO disambiguation — see MomentNorefFeatureExtractor for rationale.
    # (CodeQL py/conflicting-attributes)
    _assert_an_asset = NorefExecutorMixin._assert_an_asset

    TYPE = "SITI_noref_feature"
    VERSION = "1.0"

    ATOM_FEATURES = ["si", "ti"]  # order matters

    @staticmethod
    def sobel_filt(img):

        dx = ndimage.sobel(img, 1)  # horizontal derivative
        dy = ndimage.sobel(img, 0)  # vertical derivative
        mag = np.hypot(dx, dy)  # magnitude

        return mag

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
            i = 0
            for dis_yuv in dis_yuv_reader:
                dis_y = dis_yuv[0].astype("int32")
                mag = self.sobel_filt(dis_y)
                si = np.std(mag)
                if i == 0:
                    ti = 0
                else:
                    ti = np.std(dis_y - dis_y_prev)  # noqa: F821  (bound in previous iteration)
                dis_y_prev = copy.deepcopy(dis_y)
                scores_mtx_list.append(np.hstack(([si], [ti])))
                i += 1
            scores_mtx = np.vstack(scores_mtx_list)

            # write scores_mtx to log file
        log_file_path = self._get_log_file_path(asset)
        with open(log_file_path, "wb") as log_file:
            np.save(log_file, scores_mtx)

    def _get_feature_scores(self, asset):
        # routine to read the feature scores from the log file, and return
        # the scores in a dictionary format.

        log_file_path = self._get_log_file_path(asset)
        with open(log_file_path, "rb") as log_file:
            scores_mtx = np.load(log_file)

        num_frm, num_features = scores_mtx.shape
        assert num_features == len(self.ATOM_FEATURES)

        feature_result = {}

        for idx, atom_feature in enumerate(self.ATOM_FEATURES):
            scores_key = self.get_scores_key(atom_feature)
            feature_result[scores_key] = list(scores_mtx[:, idx])

        return feature_result
