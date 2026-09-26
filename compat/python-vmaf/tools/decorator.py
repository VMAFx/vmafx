import os
from functools import partial

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"

import contextlib
import hashlib
import json
import sys
import tempfile
import threading
import warnings
from typing import Any

_fcntl: Any
try:
    import fcntl as _fcntl
except ImportError:
    _fcntl = None

_msvcrt: Any
try:
    import msvcrt as _msvcrt
except ImportError:
    _msvcrt = None


def deprecated(func):
    """
    Mark a function as deprecated.
    It will result in a warning being emitted when the function is used.
    """

    def new_func(*args, **kwargs):
        warnings.simplefilter("always", DeprecationWarning)  # turn off filter
        warnings.warn(
            "Call to deprecated function {}.".format(func.__name__),
            category=DeprecationWarning,
            stacklevel=2,
        )
        warnings.simplefilter("default", DeprecationWarning)  # reset filter
        return func(*args, **kwargs)

    new_func.__name__ = func.__name__
    new_func.__doc__ = func.__doc__
    new_func.__dict__.update(func.__dict__)
    return new_func


_process_locks_guard = threading.Lock()
_process_path_locks: dict[str, threading.RLock] = {}
_thread_local = threading.local()


def _lock_file_descriptor(fd: int) -> None:
    """Acquire the platform-native exclusive lock for one lock-file byte."""
    if _fcntl is not None:
        _fcntl.flock(fd, _fcntl.LOCK_EX)
        return
    if _msvcrt is not None:
        os.lseek(fd, 0, os.SEEK_SET)
        _msvcrt.locking(fd, _msvcrt.LK_LOCK, 1)
        return
    raise RuntimeError("cross-process cache locking is unsupported on this platform")


def _unlock_file_descriptor(fd: int) -> None:
    """Release the platform-native lock acquired by ``_lock_file_descriptor``."""
    if _fcntl is not None:
        _fcntl.flock(fd, _fcntl.LOCK_UN)
    elif _msvcrt is not None:
        os.lseek(fd, 0, os.SEEK_SET)
        _msvcrt.locking(fd, _msvcrt.LK_UNLCK, 1)


@contextlib.contextmanager
def _file_lock(lock_path: str):
    """Re-entrant cross-process and cross-thread file lock."""
    with _process_locks_guard:
        abs_path = os.path.abspath(lock_path)
        if abs_path not in _process_path_locks:
            _process_path_locks[abs_path] = threading.RLock()
        thread_lock = _process_path_locks[abs_path]

    with thread_lock:
        if not hasattr(_thread_local, "locks"):
            _thread_local.locks = {}

        held = _thread_local.locks.get(abs_path)
        if held is not None:
            held["count"] += 1
            try:
                yield
            finally:
                held["count"] -= 1
            return

        lock_dir = os.path.dirname(abs_path) or "."
        os.makedirs(lock_dir, exist_ok=True)
        flags = os.O_CREAT | os.O_RDWR | getattr(os, "O_BINARY", 0)
        fd = os.open(abs_path, flags, 0o600)
        try:
            _lock_file_descriptor(fd)
            _thread_local.locks[abs_path] = {"fd": fd, "count": 1}
            try:
                yield
            finally:
                _thread_local.locks.pop(abs_path, None)
                with contextlib.suppress(OSError):
                    _unlock_file_descriptor(fd)
        finally:
            with contextlib.suppress(OSError):
                os.close(fd)


def _write_json_cache_atomic(file_path, data):
    """Write JSON data to a unique temporary file in the same directory and atomically replace destination.

    Provides atomic write guarantees for same-process threads and cross-process callers.
    """
    file_dir = os.path.dirname(file_path) or "."
    os.makedirs(file_dir, exist_ok=True)
    base_name = os.path.basename(file_path)
    fd, temp_file = tempfile.mkstemp(
        dir=file_dir,
        prefix=f".{base_name}.",
        suffix=".tmp",
    )
    try:
        with open(fd, "wt", encoding="utf-8") as fh:
            json.dump(data, fh)
        os.replace(temp_file, file_path)
    except Exception:
        with contextlib.suppress(OSError):
            os.close(fd)
        with contextlib.suppress(OSError):
            os.unlink(temp_file)
        raise


def persist(original_func):
    """
    Cache returned value of function in a function. Useful when calling functions
    recursively, especially in dynamic programming where lots of returned values
    can be reused.
    """

    cache = {}
    lock = threading.RLock()

    def new_func(*args):
        raw_key = (str(original_func.__name__) + str(args)).encode()
        # SHA-256 used as a collision-resistant memoization cache key.
        # usedforsecurity=False explicitly marks this non-security use for restricted/FIPS builds.
        h = hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()
        with lock:
            if h in cache:
                return cache[h]
            val = original_func(*args)
            cache[h] = val
            return val

    return new_func


def dummy(func):
    """Dummy decorator."""
    return func


class memoized(object):
    """Decorator. Caches a function's return value each time it is called.
    If called later with the same arguments, the cached value is returned
    (not reevaluated).

    memoized is similar to persist, but if applied to
    class methods, persist will cache on a per-class basis, while memoized
    will cache on a per-object basis.

    Taken from: https://wiki.python.org/moin/PythonDecoratorLibrary#Memoize
    """

    def __init__(self, func):
        self.func = func
        self.cache = {}

    def __call__(self, *args):
        try:
            if args in self.cache:
                return self.cache[args]
        except TypeError:
            # args contains an unhashable element (e.g. a list); skip the cache
            # and call through directly on every invocation.
            return self.func(*args)
        value = self.func(*args)
        self.cache[args] = value
        return value

    def __repr__(self):
        """Return the function's docstring, or an empty string if none."""
        return self.func.__doc__ or ""

    def __get__(self, obj, objtype):
        """Support instance methods."""
        return partial(self.__call__, obj)


def persist_to_file(file_name):
    """
    Cache (or persist) returned value of function in a json file.
    Serializes same-process concurrent read/modify/write operations
    and guarantees cross-process atomic cache updates via file locking.
    """

    def decorator(original_func):
        lock = threading.RLock()
        lock_file = f"{file_name}.lock"

        cache = {}
        if os.path.exists(file_name):
            try:
                with open(file_name, "rt", encoding="utf-8") as fh:
                    cache = json.load(fh)
            except (IOError, ValueError):
                sys.exit(1)

        def new_func(*args):
            raw_key = (str(original_func.__name__) + str(args)).encode()
            # SHA-256 used as a collision-resistant memoization cache key.
            # usedforsecurity=False explicitly marks this non-security use for restricted/FIPS builds.
            h = hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()

            with lock:
                if h in cache:
                    return cache[h]

            with lock, _file_lock(lock_file):
                disk_cache = {}
                if os.path.exists(file_name):
                    try:
                        with open(file_name, "rt", encoding="utf-8") as fh:
                            disk_cache = json.load(fh)
                    except (IOError, ValueError):
                        disk_cache = {}
                cache.update(disk_cache)

                if h in cache:
                    return cache[h]

                val = original_func(*args)
                cache[h] = val
                _write_json_cache_atomic(file_name, cache)
                return val

        return new_func

    return decorator


def persist_to_dir(dir_name):
    """
    Cache (or persist) returned value of function in a directory of files.
    """

    def decorator(original_func):
        lock = threading.RLock()

        def new_func(*args):
            raw_key = (str(original_func.__name__) + str(args)).encode()
            # SHA-256 used as a collision-resistant memoization cache filename.
            # usedforsecurity=False explicitly marks this non-security use for restricted/FIPS builds.
            h = hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()
            file_name = os.path.join(dir_name, h)

            if os.path.exists(file_name):
                try:
                    with open(file_name, "rt", encoding="utf-8") as fh:
                        return json.load(fh)
                except (IOError, ValueError):
                    pass

            with lock:
                if os.path.exists(file_name):
                    try:
                        with open(file_name, "rt", encoding="utf-8") as fh:
                            return json.load(fh)
                    except (IOError, ValueError):
                        pass

                res = original_func(*args)
                _write_json_cache_atomic(file_name, res)
                return res

        return new_func

    return decorator


def override(interface_class):
    def overrider(method):
        assert method.__name__ in dir(
            interface_class
        ), f"{method.__name__} does not override any method in {interface_class.__name__}"
        return method

    return overrider


class change_repr(object):
    def __init__(self, functor):
        self.functor = functor
        #  lets copy some key attributes from the original function
        self.__name__ = functor.__name__
        self.__doc__ = functor.__doc__

    def __call__(self, *args, **kwargs):
        return self.functor(*args, **kwargs)

    def __repr__(self):
        return self.functor.__name__
