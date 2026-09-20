from contextlib import ExitStack, contextmanager
from pathlib import Path
from typing import ClassVar

import numpy as np

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


@contextmanager
def _open_binary_reader(filepath):
    with Path(filepath).open("rb") as file_handle:
        yield file_handle


class YuvReader(object):

    SUPPORTED_YUV_8BIT_TYPES: ClassVar = [
        "yuv420p",
        "yuv422p",
        "yuv444p",
        "gray",
    ]

    SUPPORTED_YUV_10BIT_LE_TYPES: ClassVar = [
        "yuv420p10le",
        "yuv422p10le",
        "yuv444p10le",
        "gray10le",
    ]

    SUPPORTED_YUV_12BIT_LE_TYPES: ClassVar = [
        "yuv420p12le",
        "yuv422p12le",
        "yuv444p12le",
        "gray12le",
    ]

    SUPPORTED_YUV_16BIT_LE_TYPES: ClassVar = [
        "yuv420p16le",
        "yuv422p16le",
        "yuv444p16le",
        "gray16le",
    ]

    # ex: for yuv420p, the width and height of U/V is 0.5x, 0.5x of Y
    UV_WIDTH_HEIGHT_MULTIPLIERS_DICT: ClassVar = {
        "yuv420p": (0.5, 0.5),
        "yuv422p": (0.5, 1.0),
        "yuv444p": (1.0, 1.0),
        "gray": (0.0, 0.0),
        "yuv420p10le": (0.5, 0.5),
        "yuv422p10le": (0.5, 1.0),
        "yuv444p10le": (1.0, 1.0),
        "gray10le": (0.0, 0.0),
        "yuv420p12le": (0.5, 0.5),
        "yuv422p12le": (0.5, 1.0),
        "yuv444p12le": (1.0, 1.0),
        "gray12le": (0.0, 0.0),
        "yuv420p16le": (0.5, 0.5),
        "yuv422p16le": (0.5, 1.0),
        "yuv444p16le": (1.0, 1.0),
        "gray16le": (0.0, 0.0),
    }

    def __init__(self, filepath, width, height, yuv_type):

        self.filepath = filepath
        self.width = width
        self.height = height
        self.yuv_type = yuv_type

        self._asserts()

        self._exit_stack = ExitStack()
        self.file = self._exit_stack.enter_context(_open_binary_reader(self.filepath))

    def close(self):
        self._exit_stack.close()

    # make YuvReader withable, e.g.:
    # with YuvReader(...) as yuv_reader:
    #     ...
    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    # make YuvReader iterable, e.g.:
    # for y, u, v in yuv_reader:
    #    ...
    def __iter__(self):
        return self

    def __next__(self):
        """next() is for python2 only, in python3 all you need to define is __next__(self)"""
        return self.next()

    @property
    def num_bytes(self):
        self._assert_file_exist()
        return Path(self.filepath).stat().st_size

    @property
    def num_frms(self):
        w_multiplier, h_multiplier = self._get_uv_width_height_multiplier()

        if self._is_10bitle() or self._is_12bitle() or self._is_16bitle():
            num_frms = (
                float(self.num_bytes)
                / self.width
                / self.height
                / (1.0 + w_multiplier * h_multiplier * 2)
                / 2
            )

        elif self._is_8bit():
            num_frms = (
                float(self.num_bytes)
                / self.width
                / self.height
                / (1.0 + w_multiplier * h_multiplier * 2)
            )

        else:
            raise AssertionError()

        assert num_frms.is_integer(), "Number of frames is not integer: {}".format(num_frms)

        return int(num_frms)

    def _get_uv_width_height_multiplier(self):
        self._assert_yuv_type()
        return self.UV_WIDTH_HEIGHT_MULTIPLIERS_DICT[self.yuv_type]

    def _assert_yuv_type(self):
        assert (
            self.yuv_type in self.SUPPORTED_YUV_8BIT_TYPES
            or self.yuv_type in self.SUPPORTED_YUV_10BIT_LE_TYPES
            or self.yuv_type in self.SUPPORTED_YUV_12BIT_LE_TYPES
            or self.yuv_type in self.SUPPORTED_YUV_16BIT_LE_TYPES
        ), "Unsupported YUV type: {}".format(self.yuv_type)

    def _assert_file_exist(self):
        assert Path(self.filepath).exists(), "File does not exist: {}".format(self.filepath)

    def _asserts(self):

        # assert YUV type
        self._assert_yuv_type()

        # assert file exists
        self._assert_file_exist()

        # assert file size: if consists of integer number of frames
        assert isinstance(self.num_frms, int)

    def _is_8bit(self):
        return self.yuv_type in self.SUPPORTED_YUV_8BIT_TYPES

    def _is_10bitle(self):
        return self.yuv_type in self.SUPPORTED_YUV_10BIT_LE_TYPES

    def _is_12bitle(self):
        return self.yuv_type in self.SUPPORTED_YUV_12BIT_LE_TYPES

    def _is_16bitle(self):
        return self.yuv_type in self.SUPPORTED_YUV_16BIT_LE_TYPES

    def convert_format(self, value, bit_depth):
        return value.astype(np.double) / (2.0**bit_depth - 1.0)

    def _pixel_type_word_and_depth(self):
        if self._is_8bit():
            return np.uint8, 1, 8
        if self._is_10bitle():
            return np.uint16, 2, 10
        if self._is_12bitle():
            return np.uint16, 2, 12
        if self._is_16bitle():
            return np.uint16, 2, 16
        raise AssertionError()

    def _read_chroma(self, uv_width, uv_height, word, pix_type):
        if uv_width == 0 and uv_height == 0:
            return None, None
        if uv_width <= 0 or uv_height <= 0:
            raise AssertionError(f"Unsupported uv_width and uv_height: {uv_width}, {uv_height}")
        plane_size = uv_width * uv_height * word
        u = np.frombuffer(self.file.read(plane_size), pix_type)
        if u.size == 0:
            raise StopIteration
        v = np.frombuffer(self.file.read(plane_size), pix_type)
        if v.size == 0:
            raise StopIteration
        return u, v

    def next(self, format="uint"):

        assert format in {"uint", "float"}

        y_width = self.width
        y_height = self.height
        uv_w_multiplier, uv_h_multiplier = self._get_uv_width_height_multiplier()
        uv_width = int(y_width * uv_w_multiplier)
        uv_height = int(y_height * uv_h_multiplier)

        pix_type, word, bit_depth = self._pixel_type_word_and_depth()

        y = np.frombuffer(self.file.read(y_width * y_height * word), pix_type)

        if y.size == 0:
            raise StopIteration

        u, v = self._read_chroma(uv_width, uv_height, word, pix_type)

        y = y.reshape(y_height, y_width)
        u = u.reshape(uv_height, uv_width) if u is not None else None
        v = v.reshape(uv_height, uv_width) if v is not None else None

        if format == "float":
            y = self.convert_format(y, bit_depth)
            u = self.convert_format(u, bit_depth) if u is not None else None
            v = self.convert_format(v, bit_depth) if v is not None else None
        return y, u, v
