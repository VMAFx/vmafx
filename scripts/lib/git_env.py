# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Environment builders for scripts that run Git outside the caller's checkout.

Python twins of ``scripts/lib/drop-git-env.sh`` and
``scripts/lib/clean-git-env.sh`` (issue #3324): one implementation of the
GIT_* scrub instead of an inline copy per script. A git hook (pre-commit,
pre-push) exports ``GIT_DIR`` and ``GIT_INDEX_FILE``; with those set, ``git
init``, ``git clone``, ``git config``, ``git add`` and ``git commit`` act on
the CALLER's repository whatever the working directory is. ``drop()`` removes
every ``GIT_*`` variable and changes nothing else — the twin for a script that
clones or initialises a repository elsewhere. ``hermetic()`` adds what
``clean-git-env.sh`` exports: the system and user configuration are ignored,
the fixture identity comes from the environment (no fixture may run ``git
config user.*``), and automatic maintenance is off — a background ``git
maintenance run --auto --detach`` repacks a scratch repository and deletes its
loose objects while the fixture copies or reads them.

Both builders return a new mapping and never mutate their input; hand the
result to ``subprocess.run(env=...)`` (or through
``scripts.lib.safe_subprocess``), or apply it to the process environment
explicitly. ``scripts/ci/test_git_fixture_isolation.py`` refuses a new inline
``GIT_*`` filter outside this module, so a fix here reaches every script.
"""

from __future__ import annotations

import os
from collections.abc import Mapping, Sequence

FIXTURE_NAME = "Fixture"
FIXTURE_EMAIL = "fixture@example.invalid"


def drop(
    env: Mapping[str, str] | None = None, prefixes: Sequence[str] = ("GIT_",)
) -> dict[str, str]:
    """Return `env` (default the process environment) without the prefixed variables.

    `prefixes` extends the scrub to sibling namespaces (``GH_``, ``PRE_COMMIT_``,
    ``MAKE``) a caller also must not inherit; the default is exactly the shell
    twin's GIT_* set.
    """
    source = os.environ if env is None else env
    return {key: value for key, value in source.items() if not key.startswith(tuple(prefixes))}


def drop_except(
    env: Mapping[str, str] | None = None,
    prefixes: Sequence[str] = ("GIT_",),
    keep: Sequence[str] = (),
) -> dict[str, str]:
    """``drop()`` that re-admits the whitelisted sub-namespaces in ``keep``.

    For scripts that must keep a whitelist of settings while still scrubbing
    everything else in the namespace: a key survives when it carries none of
    ``prefixes`` or when it starts with one of ``keep``. The upstream-pr sweep,
    for one, keeps the fixture's ``GIT_CONFIG_*`` / ``GIT_AUTHOR_*`` settings
    but must not inherit a hook's ``GIT_DIR``. Keys are never read twice from
    ``env`` and the input mapping is never mutated, like ``drop()``.
    """
    source = os.environ if env is None else env
    prefixes_t = tuple(prefixes)
    keep_t = tuple(keep)
    return {
        key: value
        for key, value in source.items()
        if not key.startswith(prefixes_t) or key.startswith(keep_t)
    }


def hermetic(env: Mapping[str, str] | None = None) -> dict[str, str]:
    """``drop()`` plus the ``clean-git-env.sh`` fixture environment.

    The keys and values mirror ``scripts/lib/clean-git-env.sh`` export for
    export; ``scripts/lib/test_git_env.py`` parses that file and fails when the
    two drift apart. ``GIT_CONFIG_GLOBAL`` is the platform null device rather
    than the shell file's literal ``/dev/null``: git accepts both, and the
    Python side runs natively on Windows too.
    """
    return {
        **drop(env),
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_COUNT": "3",
        "GIT_CONFIG_KEY_0": "init.defaultBranch",
        "GIT_CONFIG_VALUE_0": "master",
        "GIT_CONFIG_KEY_1": "maintenance.auto",
        "GIT_CONFIG_VALUE_1": "false",
        "GIT_CONFIG_KEY_2": "gc.auto",
        "GIT_CONFIG_VALUE_2": "0",
        "GIT_AUTHOR_NAME": FIXTURE_NAME,
        "GIT_AUTHOR_EMAIL": FIXTURE_EMAIL,
        "GIT_COMMITTER_NAME": FIXTURE_NAME,
        "GIT_COMMITTER_EMAIL": FIXTURE_EMAIL,
    }
