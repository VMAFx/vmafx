"""Unified developer entry point for Python test orchestration.

This file lets a developer run any package's pytest suite via a single
command without remembering each package's venv recipe. CI workflows
(see ``.github/workflows/tests-and-quality-gates.yml``) keep their
existing per-package ``python3 -m venv ... && pip install ... && pytest``
invocations intact — nox does **not** replace them. It is a local
dev affordance, not a CI gate.

Usage::

    pip install nox
    nox -l                          # list every defined session
    nox -s ai                       # run the ai/ pytest suite
    nox -s mcp vmaf_tune            # run multiple suites in sequence
    nox -s python_harness           # run the legacy python/ tox harness
    nox -s all                      # every fork-local Python package
    nox -s lint                     # ruff + black + isort (check-only)

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

# Default to the interpreter nox itself runs under; do not pin a matrix
# here. The matrix lives in CI (tests-and-quality-gates.yml) where it
# can vary per package. Locally, developers run whatever ``python3``
# resolves to in their venv, matching the CI dev-container baseline.
nox.options.sessions = ["lint", "all"]
nox.options.reuse_existing_virtualenvs = True


# ---------------------------------------------------------------------------
# Per-package pytest sessions.
# ---------------------------------------------------------------------------


@nox.session(name="ai")
def ai_tests(session: nox.Session) -> None:
    """Run the ``ai/`` package pytest suite (tiny-AI training scripts)."""
    session.install("-e", "./ai")
    session.install("pytest")
    session.run("pytest", "ai/tests/", "-v", *session.posargs)


@nox.session(name="mcp")
def mcp_tests(session: nox.Session) -> None:
    """Run the ``mcp-server/vmaf-mcp/`` pytest suite."""
    session.install("-e", "./mcp-server/vmaf-mcp[dev]")
    session.run(
        "pytest",
        "mcp-server/vmaf-mcp/tests/",
        "-v",
        "--tb=short",
        *session.posargs,
    )


@nox.session(name="vmaf_tune")
def vmaf_tune_tests(session: nox.Session) -> None:
    """Run the ``tools/vmaf-tune/`` pytest suite."""
    session.install("-e", "./tools/vmaf-tune[dev]")
    session.run("pytest", "tools/vmaf-tune/tests/", "-v", *session.posargs)


@nox.session(name="dev_llm")
def dev_llm_tests(session: nox.Session) -> None:
    """Run the ``dev-llm/`` pytest suite (local-LLM helper)."""
    session.install("-e", "./dev-llm[dev]")
    session.run("pytest", "dev-llm/tests/", "-v", *session.posargs)


@nox.session(name="roi_score")
def roi_score_tests(session: nox.Session) -> None:
    """Run the ``tools/vmaf-roi-score/`` pytest suite."""
    session.install("-e", "./tools/vmaf-roi-score[dev]")
    session.run("pytest", "tools/vmaf-roi-score/tests/", "-v", *session.posargs)


@nox.session(name="ensemble_kit")
def ensemble_kit_tests(session: nox.Session) -> None:
    """Run the ``tools/ensemble-training-kit/`` pytest suite."""
    session.install("-e", "./tools/ensemble-training-kit[dev]")
    session.run(
        "pytest",
        "tools/ensemble-training-kit/tests/",
        "-v",
        *session.posargs,
    )


@nox.session(name="python_harness")
def python_harness_tests(session: nox.Session) -> None:
    """Delegate to the legacy ``python/tox.ini`` harness.

    The legacy harness drives the Netflix golden-data gate and needs the
    Cython build of ``compat/python-vmaf/core/adm_dwt2_cy`` plus the
    built ``core/build/tools/vmaf`` binary. Nox does not re-implement
    those build steps; it shells out to tox to keep one source of truth.
    """
    session.install("tox")
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


@nox.session(name="lint")
def lint(session: nox.Session) -> None:
    """Run ruff + black + isort in check-only mode across Python trees.

    Mirrors ``make lint-py`` for parity with the CI gate, but installs
    the linters into a controlled venv rather than relying on whatever
    happens to be on ``PATH``.
    """
    session.install("ruff", "black", "isort")
    targets = ["python/", "ai/", "scripts/"]
    session.run("ruff", "check", *targets)
    session.run("black", "--check", *targets)
    session.run("isort", "--check-only", *targets)
