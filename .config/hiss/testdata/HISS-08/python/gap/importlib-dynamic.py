# SPDX-License-Identifier: EUPL-1.2
import importlib


# importlib.import_module imports and executes a module named by a runtime string.
def load(name):
    return importlib.import_module(name)
