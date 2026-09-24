# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Unified developer entry point for Python test orchestration.

This file lets a developer run any package's pytest suite via a single
command without remembering each package's venv recipe. CI workflows
(see ``.github/workflows/tests-and-quality-gates.yml``) keep their
existing per-package ``python3 -m venv ... && pip install ... && pytest``
invocations intact — nox does **not** replace them. It is a local
dev affordance, not a CI gate.

Usage::

    python3 -m pip install --require-hashes -r requirements/locks/nox.txt
    nox -l                          # list every defined session
    nox -s ai                       # run the ai/ pytest suite
    nox -s mcp vmaf_tune            # run multiple suites in sequence
    nox -s python_harness           # run the legacy python/ tox harness
    nox -s all                      # every fork-local Python package
    nox -s lint                     # ruff + black (check-only)

Each per-package session installs the package into a throw-away venv
with its ``[dev]`` extras (where defined) and invokes ``pytest`` against
its ``tests/`` directory. The ``python_harness`` session delegates to
the existing ``python/tox.ini`` because that suite needs a Cython build
step and the CPU vmaf binary that ``setup.py build_ext`` produces — nox
should not duplicate that setup.

Decision record: see ADR-0914 (unified Python test orchestrator).
"""

from __future__ import annotations

import nox

# Default to the interpreter nox itself runs under. Sessions whose package
# metadata excludes that interpreter pin the supported CI version instead;
# Nox can obtain that standalone interpreter when it is not installed locally.
nox.options.sessions = ["lint", "all"]
nox.options.reuse_existing_virtualenvs = True


# ---------------------------------------------------------------------------
# Per-package pytest sessions.
# ---------------------------------------------------------------------------


@nox.session(name="ai", python="3.14")
def ai_tests(session: nox.Session) -> None:
    """Run the ``ai/`` package pytest suite (tiny-AI training scripts)."""
    session.install("--require-hashes", "-r", "requirements/locks/package-build.txt")
    session.install(
        "--no-build-isolation", "--require-hashes", "-r", "ai/requirements-dev-lock.txt"
    )
    session.install("--no-deps", "--no-build-isolation", "-e", "./ai")
    session.run("pytest", "ai/tests/", "-v", *session.posargs)


@nox.session(name="mcp", python="3.14")
def mcp_tests(session: nox.Session) -> None:
    """Run the ``mcp-server/vmaf-mcp/`` pytest suite."""
    session.install("--require-hashes", "-r", "requirements/locks/package-build.txt")
    session.install(
        "--no-build-isolation",
        "--require-hashes",
        "-r",
        "mcp-server/vmaf-mcp/requirements-dev-lock.txt",
    )
    session.install("--no-deps", "--no-build-isolation", "-e", "./mcp-server/vmaf-mcp")
    session.run(
        "pytest",
        "mcp-server/vmaf-mcp/tests/",
        "-v",
        "--tb=short",
        *session.posargs,
    )


@nox.session(name="vmaf_tune", python="3.14")
def vmaf_tune_tests(session: nox.Session) -> None:
    """Run the ``tools/vmaf-tune/`` pytest suite."""
    session.install(
        "--no-build-isolation",
        "--require-hashes",
        "-r",
        "tools/vmaf-tune/requirements-dev-lock.txt",
    )
    session.install("--no-deps", "--no-build-isolation", "-e", "./tools/vmaf-tune")
    session.run("pytest", "tools/vmaf-tune/tests/", "-v", *session.posargs)


@nox.session(name="dev_llm", python="3.14")
def dev_llm_tests(session: nox.Session) -> None:
    """Run the ``dev-llm/`` pytest suite (local-LLM helper)."""
    session.install(
        "--no-build-isolation",
        "--require-hashes",
        "-r",
        "dev-llm/requirements-dev-lock.txt",
    )
    session.install("--no-deps", "--no-build-isolation", "-e", "./dev-llm")
    session.run("pytest", "dev-llm/tests/", "-v", *session.posargs)


@nox.session(name="roi_score", python="3.12")
def roi_score_tests(session: nox.Session) -> None:
    """Run the ``tools/vmaf-roi-score/`` pytest suite."""
    session.install(
        "--no-build-isolation",
        "--require-hashes",
        "-r",
        "tools/vmaf-roi-score/requirements-dev-lock.txt",
    )
    session.install("--no-deps", "--no-build-isolation", "-e", "./tools/vmaf-roi-score")
    session.run("pytest", "tools/vmaf-roi-score/tests/", "-v", *session.posargs)


@nox.session(name="ensemble_kit", python="3.12")
def ensemble_kit_tests(session: nox.Session) -> None:
    """Run the ``tools/ensemble-training-kit/`` pytest suite."""
    session.install(
        "--no-build-isolation",
        "--require-hashes",
        "-r",
        "tools/ensemble-training-kit/requirements-dev-lock.txt",
    )
    session.install("--no-deps", "--no-build-isolation", "-e", "./tools/ensemble-training-kit")
    session.run(
        "pytest",
        "tools/ensemble-training-kit/tests/",
        "-v",
        *session.posargs,
    )


@nox.session(name="python_harness", python="3.14")
def python_harness_tests(session: nox.Session) -> None:
    """Delegate to the legacy ``python/tox.ini`` harness.

    The legacy harness drives the Netflix golden-data gate and needs the
    Cython build of ``compat/python-vmaf/core/adm_dwt2_cy`` plus the
    built ``core/build/tools/vmaf`` binary. Nox does not re-implement
    those build steps; it shells out to tox to keep one source of truth.
    """
    session.install("--require-hashes", "-r", "requirements/locks/tox.txt")
    session.run("tox", "-c", "python", *session.posargs)


# ---------------------------------------------------------------------------
# Meta-sessions.
# ---------------------------------------------------------------------------


@nox.session(name="all")
def all_tests(session: nox.Session) -> None:
    """Run every per-package pytest suite in sequence.

    Skips ``python_harness`` because that session needs the C build
    artifacts and is exercised separately by ``make test-netflix-golden``.
    """
    for name in ("ai", "mcp", "vmaf_tune", "dev_llm", "roi_score", "ensemble_kit"):
        session.notify(name)


@nox.session(name="lint", python="3.12")
def lint(session: nox.Session) -> None:
    """Run ruff + black in check-only mode across Python trees (ADR-1126: no isort).

    Mirrors ``make lint-py`` for parity with the CI gate, but installs
    the linters into a controlled venv rather than relying on whatever
    happens to be on ``PATH``.
    """
    session.install("--require-hashes", "-r", "requirements/locks/dev-linters.txt")
    targets = ["python/", "ai/", "scripts/"]
    session.run("ruff", "check", *targets)
    session.run("black", "--check", *targets)
