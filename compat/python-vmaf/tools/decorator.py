import os
from functools import partial

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"

import hashlib
import json
import sys
import warnings


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


def _write_json_cache_atomic(file_path, data):
    """Write JSON data to a temporary file and atomically replace the destination."""
    file_dir = os.path.dirname(file_path)
    if file_dir:
        os.makedirs(file_dir, exist_ok=True)
    temp_file = f"{file_path}.tmp.{os.getpid()}"
    with open(temp_file, "wt") as fh:
        json.dump(data, fh)
    os.replace(temp_file, file_path)


def persist(original_func):
    """
    Cache returned value of function in a function. Useful when calling functions
    recursively, especially in dynamic programming where lots of returned values
    can be reused.
    """

    cache = {}

    def new_func(*args):
        raw_key = (str(original_func.__name__) + str(args)).encode()
        # Primary lookup uses modern SHA-256 key (64 hex characters).
        # usedforsecurity=False indicates non-cryptographic memoization (FIPS compliance).
        h = hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()
        if h in cache:
            return cache[h]

        # Backward-compatible read-through migration: check legacy SHA-1 key.
        # Required for compatibility lookup of pre-existing cache entries.
        # nosemgrep: python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1
        h_legacy = hashlib.sha1(raw_key, usedforsecurity=False).hexdigest()
        if h_legacy in cache:
            val = cache[h_legacy]
            cache[h] = val
            return val

        # Absent from both: compute value and write SHA-256 key only. Never create new SHA-1 keys.
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
    Cache (or persist) returned value of function in a json file .
    """

    def decorator(original_func):

        if not os.path.exists(file_name):
            cache = {}
        else:
            try:
                with open(file_name, "rt") as fh:
                    cache = json.load(fh)
            except (IOError, ValueError):
                sys.exit(1)

        def new_func(*args):
            raw_key = (str(original_func.__name__) + str(args)).encode()
            # Primary lookup uses modern SHA-256 key (64 hex characters).
            # usedforsecurity=False indicates non-cryptographic memoization (FIPS compliance).
            h = hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()
            if h in cache:
                return cache[h]

            # Backward-compatible read-through migration: check legacy SHA-1 key in existing JSON cache.
            # If an existing entry was created under prior SHA-1 indexing, read it and promote
            # it to the SHA-256 key, persisting atomically so subsequent reads hit the primary key.
            # nosemgrep: python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1
            h_legacy = hashlib.sha1(raw_key, usedforsecurity=False).hexdigest()
            if h_legacy in cache:
                val = cache[h_legacy]
                cache[h] = val
                _write_json_cache_atomic(file_name, cache)
                return val

            # Absent from both: compute value and write SHA-256 key only. Never write new SHA-1 keys.
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

        def new_func(*args):
            raw_key = (str(original_func.__name__) + str(args)).encode()
            # Primary lookup uses modern SHA-256 filename (64 hex characters).
            # usedforsecurity=False indicates non-cryptographic memoization (FIPS compliance).
            h = hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()
            file_name = os.path.join(dir_name, h)

            if os.path.exists(file_name):
                with open(file_name, "rt") as fh:
                    return json.load(fh)

            # Backward-compatible read-through migration: check legacy SHA-1 filename.
            # If an existing cache file exists under the 40-hex SHA-1 name, load its value,
            # promote it to the SHA-256 file atomically, and return it without re-computing.
            # nosemgrep: python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1
            h_legacy = hashlib.sha1(raw_key, usedforsecurity=False).hexdigest()
            legacy_file = os.path.join(dir_name, h_legacy)
            if os.path.exists(legacy_file):
                with open(legacy_file, "rt") as fh:
                    res = json.load(fh)
                _write_json_cache_atomic(file_name, res)
                return res

            # Absent from both: compute value and write SHA-256 file only. Never create new SHA-1 files.
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
