"""Restricted loader for the legacy VMAF model pickle format.

Pickle remains the on-disk compatibility format for historical Netflix models,
but the standard loader permits arbitrary global imports. This loader accepts
only the small set of reconstruction helpers and estimator classes present in
the shipped model corpus. New model types must be reviewed and added by exact
module/name pair rather than opening a module-prefix wildcard.
"""

import pickle
from importlib import import_module

__copyright__ = "Copyright 2026 Lusoris"
__license__ = "BSD+Patent"


class UnsafePickleError(pickle.UnpicklingError):
    """Raised when a pickle requests a global outside the reviewed allowlist."""


_ALLOWED_GLOBALS = frozenset(
    {
        ("builtins", "object"),
        ("builtins", "set"),
        ("copy_reg", "_reconstructor"),  # Python 2 model compatibility
        ("copyreg", "_reconstructor"),
        ("dill._dill", "_load_type"),
        ("dill.dill", "_load_type"),
        ("numpy", "dtype"),
        ("numpy", "ndarray"),
        ("numpy._core.multiarray", "_reconstruct"),
        ("numpy._core.multiarray", "scalar"),
        ("numpy._core.numeric", "_frombuffer"),
        ("numpy.core.multiarray", "_reconstruct"),
        ("numpy.core.multiarray", "scalar"),
        ("numpy.core.numeric", "_frombuffer"),
        ("sklearn.ensemble._forest", "RandomForestRegressor"),
        ("sklearn.ensemble.forest", "RandomForestRegressor"),
        ("sklearn.tree._classes", "DecisionTreeRegressor"),
        ("sklearn.tree._tree", "Tree"),
        ("sklearn.tree.tree", "DecisionTreeRegressor"),
    }
)

_GLOBAL_ALIASES = {
    ("dill.dill", "_load_type"): ("dill._dill", "_load_type"),
    ("sklearn.ensemble.forest", "RandomForestRegressor"): (
        "sklearn.ensemble",
        "RandomForestRegressor",
    ),
    ("sklearn.tree.tree", "DecisionTreeRegressor"): (
        "sklearn.tree",
        "DecisionTreeRegressor",
    ),
}


class _RestrictedModelUnpickler(pickle.Unpickler):
    def find_class(self, module, name):
        if (module, name) not in _ALLOWED_GLOBALS:
            raise UnsafePickleError(f"pickle global is not allowed: {module}.{name}")
        alias = _GLOBAL_ALIASES.get((module, name))
        if alias is not None:
            alias_module, alias_name = alias
            return getattr(import_module(alias_module), alias_name)
        return super().find_class(module, name)


def load_pickle(file_object, *, encoding="ASCII"):
    """Load a legacy model pickle through the exact-global allowlist."""

    return _RestrictedModelUnpickler(file_object, encoding=encoding).load()
