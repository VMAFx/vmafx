import itertools
import multiprocessing
import os
import shlex
import shutil
import sys
import tempfile
import unittest
from concurrent.futures import ProcessPoolExecutor
from fnmatch import fnmatch
from importlib import import_module
from pathlib import Path
from time import (
    sleep,
    time,
)

import numpy as np

from vmaf import (
    run_process,
)
from vmaf.tools.scanf import FormatError, IncompleteCaptureError, sscanf

__all__ = ["run_process", "sleep"]

_COMPARISON_VALUE_2 = 2

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"

try:
    multiprocessing.set_start_method("fork")
except ValueError:  # If platform does not support, just ignore
    pass
except RuntimeError:  # If context has already being set, just ignore
    pass


def get_stdout_logger():
    logging = import_module("logging")

    logger = logging.getLogger()
    handler = logging.StreamHandler(stream=sys.stdout)
    logger.setLevel(logging.DEBUG)
    logger.addHandler(handler)
    return logger


def close_logger(logger):
    for handler in logger.handlers:
        handler.close()
        logger.removeHandler(handler)


def get_file_name_without_extension(path):
    """

    >>> get_file_name_without_extension('yuv/src01_hrc01.yuv')
    'src01_hrc01'
    >>> get_file_name_without_extension('yuv/src01_hrc01')
    'src01_hrc01'
    >>> get_file_name_without_extension('abc/xyz/src01_hrc01.yuv')
    'src01_hrc01'
    >>> get_file_name_without_extension('abc/xyz/src01_hrc01.sdr.yuv')
    'src01_hrc01.sdr'
    >>> get_file_name_without_extension('abc/xyz/src01_hrc01.sdr.dvi.yuv')
    'src01_hrc01.sdr.dvi'

    """
    return Path(path).stem


def get_file_name_with_extension(path):
    """

    >>> get_file_name_with_extension('yuv/src01_hrc01.yuv')
    'src01_hrc01.yuv'
    >>> get_file_name_with_extension('src01_hrc01.yuv')
    'src01_hrc01.yuv'
    >>> get_file_name_with_extension('abc/xyz/src01_hrc01.yuv')
    'src01_hrc01.yuv'

    """
    return Path(path).name


def get_file_name_extension(path):
    """
    >>> get_file_name_extension("file:///mnt/zli/test.txt")
    'txt'
    >>> get_file_name_extension("test.txt")
    'txt'
    >>> get_file_name_extension("abc")
    ''
    >>> get_file_name_extension("test.265")
    '265'
    """
    return Path(path).suffix[1:]


def get_dir_without_last_slash(path: str) -> str:
    """

    >>> get_dir_without_last_slash('abc/src01_hrc01.yuv')
    'abc'
    >>> get_dir_without_last_slash('src01_hrc01.yuv')
    ''
    >>> get_dir_without_last_slash('abc/xyz/src01_hrc01.yuv')
    'abc/xyz'
    >>> get_dir_without_last_slash('abc/xyz/')
    'abc/xyz'

    """
    path_text = os.fspath(path)
    path_object = Path(path_text)
    if path_text.endswith("/"):
        return str(path_object)
    parent = path_object.parent
    return "" if parent == Path() else str(parent)


def make_parent_dirs_if_nonexist(path):
    dst_dir = get_dir_without_last_slash(path)
    Path(dst_dir).mkdir(parents=True, exist_ok=True)


def delete_dir_if_exists(dir_to_delete):
    if Path(dir_to_delete).is_dir():
        Path(dir_to_delete).rmdir()


def get_normalized_string_from_dict(d):
    """Normalized string representation with sorted keys.

    >>> get_normalized_string_from_dict({"max_buffer_sec": 5.0, "bitrate_kbps": 45, })
    'bitrate_kbps_45_max_buffer_sec_5.0'
    """
    return "_".join(("{k}_{v}".format(k=k, v=d[k]) for k in sorted(d.keys())))


def get_hashable_value_tuple_from_dict(d):
    """Hashable tuple of values with sorted keys.

    >>> get_hashable_value_tuple_from_dict({"max_buffer_sec": 5.0, "bitrate_kbps": 45, })
    (45, 5.0)
    >>> get_hashable_value_tuple_from_dict({"max_buffer_sec": 5.0, "bitrate_kbps": 45, "resolutions": [(740, 480), (1920, 1080), ]})
    (45, 5.0, ((740, 480), (1920, 1080)))
    """
    return tuple((tuple(d[k]) if isinstance(d[k], list) else d[k] for k in sorted(d.keys())))


def get_unique_str_from_recursive_dict(d):
    """String representation with sorted keys and values for recursive dict.

    >>> get_unique_str_from_recursive_dict({'a':1, 'b':2, 'c':{'x':'0', 'y':'1'}})
    '{"a": 1, "b": 2, "c": {"x": "0", "y": "1"}}'
    >>> get_unique_str_from_recursive_dict({'a':1, 'c':2, 'b':{'y':'1', 'x':'0', }})
    '{"a": 1, "b": {"x": "0", "y": "1"}, "c": 2}'
    """
    json = import_module("json")
    OrderedDict = import_module("collections").OrderedDict

    def to_ordered_dict_recursively(d):
        if isinstance(d, dict):
            return OrderedDict(
                (
                    (
                        to_ordered_dict_recursively(t[0]),
                        to_ordered_dict_recursively(t[1]),
                    )
                    for t in sorted(d.items())
                )
            )
        return d

    return json.dumps(to_ordered_dict_recursively(d))


def indices(a, func):
    """
    Get indices of elements in an array which satisfies func
    >>> indices([1, 2, 3, 4], lambda x: x>2)
    [2, 3]
    >>> indices([1, 2, 3, 4], lambda x: x==2.5)
    []
    >>> indices([1, 2, 3, 4], lambda x: 1 < x <= 3)
    [1, 2]
    >>> indices([1, 2, 3, 4], lambda x: x in [2, 4])
    [1, 3]
    >>> indices([1,2,3,1,2,3,1,2,3], lambda x: x > 2)
    [2, 5, 8]
    """
    return [i for (i, val) in enumerate(a) if func(val)]


def import_python_file(filepath: str, override: dict | None = None):
    """
    Import a python file as a module, allowing overriding some of the variables.
    Assumption: in the original python file, variables to be overridden get assigned once only, in a single line.
    """
    if override is None:
        filename = get_file_name_without_extension(filepath)
        # SourceFileLoader.load_module() was deprecated in Python 3.4 and
        # scheduled for removal in Python 3.15.  imp.load_source() was
        # removed in Python 3.12.  Use the modern importlib.util path that
        # works on all supported Python versions (3.8+).
        importlib_util = import_module("importlib.util")
        spec = importlib_util.spec_from_file_location(filename, filepath)
        ret = importlib_util.module_from_spec(spec)
        sys.modules[filename] = ret
        spec.loader.exec_module(ret)
        return ret
    override_ = override.copy()
    with tempfile.NamedTemporaryFile(delete=False, suffix=".py") as tmpfile:
        tmp_path = Path(tmpfile.name)
    with Path(filepath).open("r") as fin:
        with tmp_path.open("w") as fout:
            for source_line in fin:
                line = source_line
                if len(override_) > 0:
                    suffixes = []
                    for key in list(override_.keys()):
                        if key in line and "=" in line:
                            s = (
                                f"{key} = '{override_[key]}'"
                                if isinstance(override_[key], str)
                                else f"{key} = {override_[key]}"
                            )
                            suffixes.append(s)
                            del override_[key]
                    if len(suffixes) > 0:
                        line = "\n".join([line.strip(), *suffixes]) + "\n"
                fout.write(line)
            if len(override_) > 0:
                for key in override_:
                    s = (
                        f"{key} = '{override_[key]}'"
                        if isinstance(override_[key], str)
                        else f"{key} = {override_[key]}"
                    )
                    s += "\n"
                    fout.write(s)
    try:
        return import_python_file(str(tmp_path))
    finally:
        tmp_path.unlink()


def make_absolute_path(path: str, current_dir: str) -> str:
    """
    >>> make_absolute_path('abc/cde.fg', '/xyz/')
    '/xyz/abc/cde.fg'
    >>> make_absolute_path('/abc/cde.fg', '/xyz/')
    '/abc/cde.fg'
    """
    assert current_dir.endswith("/"), f"expect current_dir ends with '/', but is: {current_dir}"
    if path[0] == "/":
        return path
    return current_dir + path


def empty_object():
    return type("", (), {})()


def get_cmd_option(argv, begin, end, option):
    """

    >>> get_cmd_option(['a', 'b', 'c', '--xyz', '123'], 3, 5, '--xyz')
    '123'
    >>> get_cmd_option(['a', 'b', 'c', '--xyz', '123'], 0, 5, '--xyz')
    '123'
    >>> get_cmd_option(['a', 'b', 'c', '--xyz', '123'], 4, 5, '--xyz')
    >>> get_cmd_option(['a', 'b', 'c', '--xyz', '123'], 5, 5, '--xyz')
    >>> get_cmd_option(['a', 'b', 'c', '--xyz', '123'], 6, 5, '--xyz')
    >>> get_cmd_option(['a', 'b', 'c', '--xyz', '123'], 0, 5, 'a')
    'b'
    >>> get_cmd_option(['a', 'b', 'c', '--xyz', '123'], 0, 5, 'b')
    'c'

    """
    itr = None
    for itr in range(begin, end):
        if argv[itr] == option:
            break
    if itr is not None and itr != end and (itr + 1) != end:
        return argv[itr + 1]
    return None


def cmd_option_exists(argv, begin, end, option):
    """

    >>> cmd_option_exists(['a', 'b', 'c', 'd'], 2, 4, 'c')
    True
    >>> cmd_option_exists(['a', 'b', 'c', 'd'], 3, 4, 'c')
    False
    >>> cmd_option_exists(['a', 'b', 'c', 'd'], 3, 4, 'd')
    True
    >>> cmd_option_exists(['a', 'b', 'c', 'd'], 2, 4, 'a')
    False
    >>> cmd_option_exists(['a', 'b', 'c', 'd'], 2, 4, 'b')
    False

    """
    found = False
    for itr in range(begin, end):
        if argv[itr] == option:
            found = True
            break
    return found


def index_and_value_of_min(values):
    """

    >>> index_and_value_of_min([2, 0, 3])
    (1, 0)

    """
    return min(enumerate(values), key=lambda x: x[1])


_PARALLEL_MAP_STATE = {}


def _parallel_map_rt(idx):
    return_dict = _PARALLEL_MAP_STATE["return_dict"]
    func = _PARALLEL_MAP_STATE["func"]
    list_args = _PARALLEL_MAP_STATE["list_args"]
    return_dict[idx] = func(list_args[idx])


def parallel_map(func, list_args, processes=None):
    """
    Use multiprocessing.Pool to create a fast parallel map that doesn't pickle arguments
    Note: only works on Unix! fork() used to propagate unpickleable data
    """

    context = multiprocessing

    if getattr(multiprocessing, "get_context", None) is None:
        assert os.name == "posix", "parallel_map() requires fork() support, but not running on Unix"
    else:
        assert (
            "fork" in multiprocessing.get_all_start_methods()
        ), "parallel_map() requires fork() support"
        context = multiprocessing.get_context("fork")

    # create shared dictionary
    return_dict = context.Manager().dict()

    def pool_init():
        _PARALLEL_MAP_STATE.update(
            func=func,
            list_args=list_args,
            return_dict=return_dict,
        )

    with ProcessPoolExecutor(processes, mp_context=context, initializer=pool_init) as pool:
        # ProcessPoolExecutor prevents hanging on the slowest processes that get too much work - delegates one at a time
        for _ in pool.map(_parallel_map_rt, range(len(list_args))):
            pass

    return [return_dict[i] for i in range(len(list_args))]


def check_program_exist(program):
    """

    >>> check_program_exist("xxxafasd34df")
    False
    >>> check_program_exist("xxxafasd34df f899")
    False
    >>> check_program_exist("ls")
    True
    >>> check_program_exist("ls -all")
    True
    >>> check_program_exist("pwd")
    True

    """
    argv = shlex.split(program)
    return bool(argv and shutil.which(argv[0]) is not None)


def check_scanf_match(string, template):
    """
    >>> check_scanf_match('frame00000000.icpf', 'frame%08d.icpf')
    True
    >>> check_scanf_match('frame00000003.icpf', 'frame%08d.icpf')
    True
    >>> check_scanf_match('frame0000001.icpf', 'frame%08d.icpf')
    True
    >>> check_scanf_match('frame00000001.icpff', 'frame%08d.icpf')
    True
    >>> check_scanf_match('gframe00000001.icpff', 'frame%08d.icpf')
    False
    >>> check_scanf_match('fyrame00000001.icpff', 'frame%08d.icpf')
    False
    >>> check_scanf_match('xx/yy/frame00000000.icpf', 'xx/yy/frame%08d.icpf')
    True
    >>> check_scanf_match('xx/yy//frame00000000.icpf', 'xx/yy/frame%08d.icpf')
    False
    >>> check_scanf_match('xx/yy/frame00000000.icpf', 'xx/yy//frame%08d.icpf')
    False
    >>> check_scanf_match("-1-2+3-4", "%02d%02d%02d%02d")
    True
    >>> check_scanf_match('frame00000240.icpf', 'frame%08d.icpf')
    True
    >>> check_scanf_match('/mnt/hgfs/ZLI-NFLX-10/USCJND/ref/1920x1080/videoSRC001_1920x1080_30.yuv.avi', '/mnt/hgfs/ZLI-NFLX-10/USCJND/ref/1920x1080/videoSRC001_1920x1080_*.yuv.avi')
    True
    """
    try:
        sscanf(string, template)
        return True
    except (FormatError, IncompleteCaptureError):
        pass

    if fnmatch(string, template):
        return True

    return False


def match_any_files(template):
    directory = Path(template).parent
    return any(check_scanf_match(str(filepath), template) for filepath in directory.iterdir())


def unroll_dict_of_lists(dict_of_lists):
    """Unfold a dictionary of lists into a list of dictionaries.

    >>> dict_of_lists = {'norm_type':['normalize'], 'n_estimators':[10, 50], 'random_state': [0]}
    >>> expected = [{'n_estimators': 10, 'norm_type': 'normalize', 'random_state': 0}, {'n_estimators': 50, 'norm_type': 'normalize', 'random_state': 0}]
    >>> unroll_dict_of_lists(dict_of_lists) == expected
    True

    """
    keys = sorted(dict_of_lists.keys())  # normalize order
    list_of_key_value_pairs = []
    for key in keys:
        values = dict_of_lists[key]
        key_value_pairs = []
        for value in values:
            key_value_pairs.append((key, value))
        list_of_key_value_pairs.append(key_value_pairs)

    list_of_key_value_pairs_rearranged = itertools.product(*list_of_key_value_pairs)

    list_of_dicts = []
    for key_value_pairs in list_of_key_value_pairs_rearranged:
        list_of_dicts.append(dict(key_value_pairs))

    return list_of_dicts


def neg_if_even(x):
    """
    >>> neg_if_even(2)
    -1
    >>> neg_if_even(1)
    1
    >>> neg_if_even(0)
    -1
    >>> neg_if_even(-1)
    1
    >>> neg_if_even(-2)
    -1

    """
    return 1 - (x % 2 == 0) * 2


def get_unique_sorted_list(values):
    """
    >>> get_unique_sorted_list([3, 4, 4, 1])
    [1, 3, 4]
    >>> get_unique_sorted_list([])
    []
    """
    return sorted(set(values))


class Timer(object):

    def __enter__(self):
        self.tstart = time()

    def __exit__(self, type, value, traceback):
        print("Elapsed: %s sec" % (time() - self.tstart))


def dedup_value_in_dict(d):
    """
    >>> dedup_value_in_dict({'a': 1, 'b': 1, 'c': 2}) == {'a': 1, 'c': 2}
    True
    """
    reversed_d = {}
    keys = sorted(d.keys())
    for key in keys:
        value = d[key]
        if value not in reversed_d:
            reversed_d[value] = key
    d_ = {}
    for value, key in reversed_d.items():
        d_[key] = value
    return d_


class MyTestCase(unittest.TestCase):
    def setUp(self):
        self.verificationErrors = []
        self.maxDiff = None

    def tearDown(self):
        unittest.TestCase.assertEqual(self, [], self.verificationErrors)

    def assertAlmostEqual(self, first, second, places=None, msg=None, delta=None):
        try:
            super().assertAlmostEqual(first, second, places, msg, delta)
        except AssertionError as e:
            self.verificationErrors.append(str(e))

    def assertEqual(self, first, second, msg=None):
        try:
            super().assertEqual(first, second, msg)
        except AssertionError as e:
            self.verificationErrors.append(str(e))

    def assertTrue(self, expr, msg=None):
        try:
            super().assertTrue(expr, msg)
        except AssertionError as e:
            self.verificationErrors.append(str(e))


class QualityRunnerTestMixin(object):

    @staticmethod
    def _run_each_no_assert(
        runner_class, asset, optional_dict, optional_dict2=None, result_store=None, **more
    ):
        runner = runner_class(
            [asset],
            None,
            fifo_mode=False,
            delete_workdir=True,
            result_store=result_store,
            optional_dict=optional_dict,
            optional_dict2=optional_dict2,
        )
        runner.run(parallelize=False)
        return runner.results[0]

    def run_each(
        self,
        score,
        runner_class,
        asset,
        optional_dict,
        optional_dict2=None,
        result_store=None,
        places=5,
        **more,
    ):
        result = self._run_each_no_assert(
            runner_class, asset, optional_dict, optional_dict2, result_store, **more
        )
        self.assertAlmostEqual(result[runner_class.get_score_key()], score, places=places)

    def plot_frame_scores(self, ax, *args, **kwargs):
        result = self._run_each_no_assert(*args, **kwargs)
        runner_class, _asset, _optional_dict = args
        avg_score = result[runner_class.get_score_key()]
        label = [f"avg. {runner_class.TYPE}: {avg_score:.3f}"]
        if "label" in kwargs:
            label += [kwargs["label"]]
        label = ", ".join(label)
        ax.set_xlabel("Frame Number")
        ax.set_ylabel("Score")
        ax.plot(result[runner_class.get_scores_key()], label=label)


def find_linear_function_parameters(p1, p2):
    """
    Find parameters of a linear function connecting first_point and second_point

    >>> try:
    ...     find_linear_function_parameters((1, 1), (0, 0))
    ... except AssertionError as exc:
    ...     print(str(exc).splitlines()[0])
    first_point coordinates need to be smaller or equal to second_point coordinates
    >>> try:
    ...     find_linear_function_parameters((0, 1), (0, 0))
    ... except AssertionError as exc:
    ...     print(str(exc).splitlines()[0])
    first_point coordinates need to be smaller or equal to second_point coordinates
    >>> try:
    ...     find_linear_function_parameters((1, 0), (0, 0))
    ... except AssertionError as exc:
    ...     print(str(exc).splitlines()[0])
    first_point coordinates need to be smaller or equal to second_point coordinates
    >>> try:
    ...     find_linear_function_parameters((50.0, 30.0), (50.0, 100.0))
    ... except AssertionError as exc:
    ...     print(str(exc).splitlines()[0])
    first_point and second_point cannot lie on a horizontal or vertical line
    >>> try:
    ...     find_linear_function_parameters((50.0, 30.0), (100.0, 30.0))
    ... except AssertionError as exc:
    ...     print(str(exc).splitlines()[0])
    first_point and second_point cannot lie on a horizontal or vertical line
    >>> find_linear_function_parameters((50.0, 20.0), (110.0, 110.0))
    (1.5, -55.0)
    >>> a, b = find_linear_function_parameters((50.0, 30.0), (110.0, 110.0))
    >>> np.testing.assert_almost_equal(a, 1.333333333333333)
    >>> np.testing.assert_almost_equal(b, -36.666666666666664)
    >>> find_linear_function_parameters((50.0, 30.0), (50.0, 30.0))
    (1, 0)
    >>> find_linear_function_parameters((10.0, 10.0), (50.0, 110.0))
    (2.5, -15.0)

    """
    assert len(p1) == _COMPARISON_VALUE_2, "first_point needs to have exactly 2 coordinates"
    assert len(p2) == _COMPARISON_VALUE_2, "second_point needs to have exactly 2 coordinates"
    assert (
        p1[0] <= p2[0] and p1[1] <= p2[1]
    ), "first_point coordinates need to be smaller or equal to second_point coordinates"

    if p2[0] - p1[0] == 0 or p2[1] - p1[1] == 0:
        assert p1 == p2, "first_point and second_point cannot lie on a horizontal or vertical line"
        alpha = 1  # both points are the same
        beta = 0
    else:
        alpha = (p2[1] - p1[1]) / (p2[0] - p1[0])
        beta = p1[1] - (p1[0] * alpha)

    return alpha, beta


def piecewise_linear_mapping(x, knots):
    """Map an array through monotonic knots, extrapolating the end slopes."""
    assert len(knots) > 1
    n_seg = len(knots) - 1

    y = np.zeros(np.shape(x))

    # construct the function
    for idx in range(n_seg):
        assert isinstance(knots[idx], list) and isinstance(
            knots[idx + 1], list
        ), "knots needs to be list of lists"
        assert (
            len(knots[idx]) == len(knots[idx + 1]) == _COMPARISON_VALUE_2
        ), "Each point needs to have two coordinates [x, y]"
        assert knots[idx][0] < knots[idx + 1][0] and knots[idx][1] <= knots[idx + 1][1], (
            "The x-coordinate of each point need to be greater that the x-coordinate of the previous point, "
            "the y-coordinate needs to be greater or equal."
        )

        cond0 = knots[idx][0] <= x
        cond1 = x <= knots[idx + 1][0]

        if knots[idx][1] == knots[idx + 1][1]:  # the segment is horizontal
            y[cond0 & cond1] = knots[idx][1]

            if idx == 0:
                # for points below the defined range
                y[x < knots[idx][0]] = knots[idx][1]
            if idx == n_seg - 1:
                # for points above the defined range
                y[x > knots[idx + 1][0]] = knots[idx][1]

        else:
            slope, offset = find_linear_function_parameters(
                tuple(knots[idx]), tuple(knots[idx + 1])
            )

            y[cond0 & cond1] = slope * x[cond0 & cond1] + offset

            if idx == 0:
                # for points below the defined range
                y[x < knots[idx][0]] = slope * x[x < knots[idx][0]] + offset
            if idx == n_seg - 1:
                # for points above the defined range
                y[x > knots[idx + 1][0]] = slope * x[x > knots[idx + 1][0]] + offset

    return y


def round_up_to_odd(f):
    """
    >>> round_up_to_odd(32.6)
    33
    >>> round_up_to_odd(33.1)
    35
    """
    return int(np.ceil(f) // 2 * 2 + 1)


class NoPrint(object):

    def __enter__(self):
        self._original_stdout = sys.stdout
        sys.stdout = Path(os.devnull).open("w")

    def __exit__(self, exc_type, exc_val, exc_tb):
        sys.stdout.close()
        sys.stdout = self._original_stdout


def linear_func(x, a, b):
    return a * x + b


def linear_fit(x, y):
    """
    >>> fit = linear_fit([0, 1], [0, 1])
    >>> tuple(map(float, fit[0]))
    (1.0, 0.0)
    """
    assert isinstance(x, (list, tuple, np.ndarray)), "x must be a list, tuple, or a numpy array"
    assert len(x) == np.size(x) and len(x) > 0, "x must be one-dimensional with non-zero length"
    assert isinstance(y, (list, tuple, np.ndarray)), "y must be a list or a numpy array"
    assert len(y) == np.size(y) and len(y) > 0, "y must be one-dimensional with non-zero length"
    assert len(x) == len(y), "x must be the same length as y"

    scipy = import_module("scipy")
    import_module("scipy.optimize")

    return scipy.optimize.curve_fit(linear_func, x, y, [1.0, 0.0])


def map_yuv_type_to_bitdepth(yuv_type):
    """
    >>> map_yuv_type_to_bitdepth('yuv420p')
    8
    >>> map_yuv_type_to_bitdepth('yuv422p')
    8
    >>> map_yuv_type_to_bitdepth('yuv444p')
    8
    >>> map_yuv_type_to_bitdepth('yuv420p10le')
    10
    >>> map_yuv_type_to_bitdepth('yuv422p10le')
    10
    >>> map_yuv_type_to_bitdepth('yuv444p10le')
    10
    >>> map_yuv_type_to_bitdepth('yuv420p12le')
    12
    >>> map_yuv_type_to_bitdepth('yuv422p12le')
    12
    >>> map_yuv_type_to_bitdepth('yuv444p12le')
    12
    >>> map_yuv_type_to_bitdepth('yuv420p16le')
    16
    >>> map_yuv_type_to_bitdepth('yuv422p16le')
    16
    >>> map_yuv_type_to_bitdepth('yuv444p16le')
    16
    >>> map_yuv_type_to_bitdepth('notyuv') is None
    True
    """
    if yuv_type in ["yuv420p", "yuv422p", "yuv444p"]:
        return 8
    if yuv_type in ["yuv420p10le", "yuv422p10le", "yuv444p10le"]:
        return 10
    if yuv_type in ["yuv420p12le", "yuv422p12le", "yuv444p12le"]:
        return 12
    if yuv_type in ["yuv420p16le", "yuv422p16le", "yuv444p16le"]:
        return 16
    return None


if __name__ == "__main__":
    import doctest

    doctest.testmod()
