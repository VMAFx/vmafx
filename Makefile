# Use bash for shell
SHELL := /bin/bash

# Path and environment setup
VENV := .venv
VIRTUAL_ENV_PATH := $(VENV)/bin

# Build tools configured in the virtual environment
PYTHON_INTERPRETER := python3
VENV_PIP := $(VIRTUAL_ENV_PATH)/pip
VENV_PYTHON := $(VIRTUAL_ENV_PATH)/python
MESON := $(VIRTUAL_ENV_PATH)/meson
MESON_SETUP := $(MESON) setup
NINJA := $(VIRTUAL_ENV_PATH)/ninja

# Lint and format tools resolve from the project venv first, then the system
# PATH. Without this, `make lint-py` / `make format-check` silently found no
# ruff / black / isort and reported success, so the local gate could pass while
# CI's identical checks failed.
export PATH := $(CURDIR)/$(VIRTUAL_ENV_PATH):$(PATH)

# require-tool,<binary>,<install hint>
# A gate that cannot run is a gate that cannot fail. Every lint / format tool is
# pinned and installable, so a missing one is a setup error to fix, not a step
# to skip.
define require-tool
@command -v $(1) >/dev/null || { \
   echo "error: $(1) not found — this gate cannot run without it."; \
   echo "       install: $(2)"; exit 1; }
endef

# Build types and options
BUILDTYPE_RELEASE := --buildtype release
BUILDTYPE_DEBUG := --buildtype debug
ENABLE_FLOAT := -Denable_float=true
ENABLE_NVCC :=	true
ENABLE_CUDA := -Denable_cuda=true -Denable_nvcc=$(ENABLE_NVCC)

# Directories
# Tree was renamed `libvmaf/` → `core/` on 2026-05-28 (ADR-0700,
# "VMAFX repo layout"). The variable name LIBVMAF_DIR stays for
# rebase compatibility; only the path it points to changes.
LIBVMAF_DIR := core
BUILD_DIR := $(LIBVMAF_DIR)/build
DEBUG_DIR := $(LIBVMAF_DIR)/debug

.PHONY: default all debug build install cythonize clean distclean cythonize-deps \
    go-build go-test rust-build rust-test setup-envtest setup-envtest-env

default: build

all: build debug install test cythonize

$(BUILD_DIR): $(MESON) $(NINJA)
	PATH="$(VENV)/bin:$$PATH" $(MESON_SETUP) $(BUILD_DIR) $(LIBVMAF_DIR) $(BUILDTYPE_RELEASE) $(ENABLE_FLOAT) $(ENABLE_CUDA)

$(DEBUG_DIR): $(MESON) $(NINJA)
	PATH="$(VENV)/bin:$$PATH" $(MESON_SETUP) $(DEBUG_DIR) $(LIBVMAF_DIR) $(BUILDTYPE_DEBUG) $(ENABLE_FLOAT) $(ENABLE_CUDA)

cythonize: cythonize-deps
	pushd python && ../$(VENV_PYTHON) setup.py build_ext --build-lib . && popd || exit 1

build: $(BUILD_DIR) $(NINJA)
	PATH="$(VENV)/bin:$$PATH" $(NINJA) -vC $(BUILD_DIR)

test: build $(NINJA)
	PATH="$(VENV)/bin:$$PATH" $(NINJA) -vC $(BUILD_DIR) test

debug: $(DEBUG_DIR) $(NINJA)
	PATH="$(VENV)/bin:$$PATH" $(NINJA) -vC $(DEBUG_DIR)

install: $(BUILD_DIR) $(NINJA)
	PATH="$(VENV)/bin:$$PATH" $(NINJA) -vC $(BUILD_DIR) install

clean:
	rm -rf $(BUILD_DIR) $(DEBUG_DIR)
	rm -f compat/python-vmaf/core/adm_dwt2_cy.c*

distclean: clean
	rm -rf $(VENV)

# Set up or rebuild virtual environment
$(VENV_PIP):
	@echo "Setting up the virtual environment..."
	$(PYTHON_INTERPRETER) -m venv $(VENV) || { echo "Failed to create virtual environment"; exit 1; }
	$(VENV_PIP) install --upgrade pip || { echo "Failed to upgrade pip"; exit 1; }
	@echo "Virtual environment setup complete."

$(MESON): $(VENV_PIP)
	$(VENV_PIP) install meson || { echo "Failed to install meson"; exit 1; }

$(NINJA): $(VENV_PIP)
	$(VENV_PIP) install ninja || { echo "Failed to install ninja"; exit 1; }

# Provision the lint / format toolchain into the project venv. Versions are
# kept identical to .pre-commit-config.yaml so the local gate and the CI hooks
# cannot disagree about what counts as a violation.
RUFF_VERSION  := 0.16.5
BLACK_VERSION := 26.5.1
ISORT_VERSION := 6.0.1

.PHONY: lint-tools
lint-tools: $(VENV_PIP)
	$(VENV_PIP) install --quiet \
	    'ruff==$(RUFF_VERSION)' 'black==$(BLACK_VERSION)' 'isort==$(ISORT_VERSION)' mypy
	@echo "lint tools installed into $(VENV_PIP:%/pip=%)"
	@command -v shfmt >/dev/null || { \
	   echo "note: shfmt is not a Python package and was not installed."; \
	   echo "      get it with: go install mvdan.cc/sh/v3/cmd/shfmt@v3.13.1"; }
	@command -v shellcheck >/dev/null || \
	   echo "note: shellcheck not found — install it via your package manager."

cythonize-deps: $(VENV_PIP)
	$(VENV_PIP) install setuptools cython numpy || { echo "Failed to install dependencies"; exit 1; }

# ============================================================================
# Fork-specific targets (lusoris). The upstream targets above are preserved as-is.
# ============================================================================

.PHONY: lint lint-c lint-py lint-sh lint-md lint-go format format-check sec sbom \
        test-netflix-golden test-sanitizers test-fast install-hooks hooks-install help \
        coverage coverage-html coverage-check assertion-density pr-check

# Top-level lint — runs every analyzer we own. Uses the meson compile_commands.json.
lint: lint-c lint-py lint-sh lint-md lint-go docs-fragments-check
	@echo "=== all lints passed ==="

# Go security scan (gosec). Skips generated files by default; surfaces every
# G* finding outside the gen/ tree. Source of truth for the gate added by
# the gosec-findings-fix sweep — keep the touched-file rule honest.
lint-go:
	$(call require-tool,gosec,go install github.com/securego/gosec/v2/cmd/gosec@v2.29.0)
	@echo "--- gosec (exclude-generated) ---"
	@gosec -exclude-generated -quiet ./...

# Fragment-tree drift check (ADR-0221). Verifies CHANGELOG.md and
# docs/adr/README.md are in sync with their per-PR fragment trees.
docs-fragments-check:
	@echo "--- changelog.d/ vs CHANGELOG.md ---"
	@bash scripts/release/concat-changelog-fragments.sh --check
	@echo "--- docs/adr/_index_fragments/ vs docs/adr/README.md ---"
	@bash scripts/docs/concat-adr-index.sh --check

# Regenerate consolidated outputs from fragments (ADR-0221).
docs-fragments-write:
	@bash scripts/release/concat-changelog-fragments.sh --write
	@bash scripts/docs/concat-adr-index.sh --write

lint-c: $(BUILD_DIR)
	@command -v clang-tidy >/dev/null || { echo "clang-tidy not found; skipping"; exit 0; }
	@command -v cppcheck >/dev/null   || { echo "cppcheck not found; skipping"; exit 0; }
	@echo "--- clang-tidy ---"
	@FILES=$$(git ls-files 'core/src/**/*.c' 'core/src/**/*.cpp' 'core/tools/*.c' \
	         | grep -v '^subprojects/' \
	         | grep -v '^core/src/interop/pelorus_'); \
	 clang-tidy -p $(BUILD_DIR) --quiet $$FILES
	@echo "--- cppcheck ---"
	cppcheck --enable=all --inline-suppr \
	         --suppressions-list=.cppcheck-suppressions.txt \
	         --project=$(BUILD_DIR)/compile_commands.json \
	         --error-exitcode=1

lint-py:
	$(call require-tool,ruff,pip install ruff==0.15.17)
	ruff check python/ ai/ scripts/
	$(call require-tool,black,pip install black==26.5.1)
	black --check python/ ai/ scripts/
# mypy is advisory (leading `-`): it currently reports ~295 module-resolution
# errors ("duplicate module", "adding __init__.py somewhere") that stop it
# before it type-checks anything real. That is a mypy-configuration gap
# (needs --explicit-package-bases / a mypy_path), not type debt, and fixing it
# is tracked separately. Kept running so the output stays visible.
	@command -v mypy >/dev/null || { echo "note: mypy not installed, skipping advisory check"; exit 0; }
	-mypy ai/scripts/ ai/tests/ ai/train/ ai/lpips_export.py scripts/

lint-sh:
	$(call require-tool,shellcheck,your package manager, e.g. pacman -S shellcheck)
	shellcheck $$(git ls-files '*.sh')

# Markdown lint (ADR-0866). Default scope is the touched-file delta vs
# origin/master so the ~6.2k pre-existing-warning tail (ADR-0864) doesn't
# gate innocent PRs. Override MDLINT_SCOPE=all to run against the full
# corpus (docs/**/*.md changelog.d/**/*.md README.md CLAUDE.md AGENTS.md).
#
# The hook reads .markdownlint.json from the repo root (PR #332's tuned
# config). markdownlint-cli2 is unsafe under --fix for 7 default rules
# (ADR-0864); this target never passes --fix.
MDLINT_SCOPE ?= changed

lint-md:
	@command -v npx >/dev/null || { echo "npx not found (install Node.js to enable lint-md); skipping"; exit 0; }
	@if [ "$(MDLINT_SCOPE)" = "all" ]; then \
	    echo "--- markdownlint-cli2 (all files) ---"; \
	    npx --yes markdownlint-cli2 \
	        'docs/**/*.md' \
	        'README.md' 'CLAUDE.md' 'AGENTS.md' \
	        '!docs/adr/README.md' '!docs/adr/_index_fragments/**'; \
	else \
	    echo "--- markdownlint-cli2 (changed vs origin/master) ---"; \
	    files=$$(git diff --name-only --diff-filter=d origin/master...HEAD -- '*.md' 2>/dev/null \
	             | grep -E '^(docs/|README\.md|CLAUDE\.md|AGENTS\.md)' \
	             | grep -vE '^(docs/adr/README\.md|CHANGELOG\.md|docs/adr/_index_fragments/|changelog\.d/)' || true); \
	    if [ -z "$$files" ]; then \
	        echo "no markdown changes vs origin/master — skipping"; \
	    else \
	        echo "$$files"; \
	        npx --yes markdownlint-cli2 $$files; \
	    fi; \
	fi

# Formatters — writes changes.
format:
	@command -v clang-format >/dev/null && \
	 clang-format -i $$(git ls-files '*.c' '*.h' '*.cpp' '*.hpp' '*.cu' '*.cuh' \
	                   | grep -v '^subprojects/' | grep -v '^core/test/data/' \
	                   | grep -v '^core/src/interop/pelorus_' \
	                   | grep -v '^core/include/libvmaf/pelorus/') || true
	@command -v black >/dev/null && black python/ ai/ scripts/ 2>/dev/null || true
	@command -v isort >/dev/null && isort python/ ai/ scripts/ 2>/dev/null || true
	@command -v shfmt >/dev/null && shfmt -w -i 2 -ci $$(git ls-files '*.sh') || true

# Formatters — check-only (CI gate, no writes).
format-check:
	$(call require-tool,clang-format,your package manager, e.g. pacman -S clang)
	clang-format --dry-run --Werror \
	   $$(git ls-files '*.c' '*.h' '*.cpp' '*.hpp' '*.cu' '*.cuh' \
	      | grep -v '^subprojects/' | grep -v '^core/test/data/' \
	      | grep -v '^core/src/interop/pelorus_' \
	      | grep -v '^core/include/libvmaf/pelorus/')
	$(call require-tool,black,pip install black==26.5.1)
	black --check python/ ai/ scripts/
	$(call require-tool,isort,pip install isort==6.0.1)
	isort --check-only python/ ai/ scripts/
	$(call require-tool,shfmt,go install mvdan.cc/sh/v3/cmd/shfmt@latest)
	shfmt -d -i 2 -ci $$(git ls-files '*.sh')

# Security scan (semgrep custom + CERT-C + CWE Top 25).
sec:
	@command -v semgrep >/dev/null || { echo "semgrep not installed — see .semgrep.yml"; exit 1; }
	semgrep scan --config=.semgrep.yml \
	             --config=p/cwe-top-25 \
	             --config=p/cert-c-strict \
	             --error

# SBOM generation (Software Bill of Materials, both SPDX and CycloneDX).
sbom:
	@command -v syft >/dev/null || { echo "syft not installed"; exit 1; }
	@mkdir -p build/sbom
	syft . -o spdx-json=build/sbom/sbom.spdx.json
	syft . -o cyclonedx-json=build/sbom/sbom.cdx.json
	@echo "SBOM: build/sbom/sbom.{spdx,cdx}.json"

# Netflix CPU golden-data gate (D24) — the 3 test pairs that MUST pass.
# Runs the Python tests whose hardcoded CPU scores are the source of truth
# for VMAF numerical correctness.
test-netflix-golden: build
	@echo "=== Netflix CPU golden-data gate (D24) ==="
	PYTHONPATH=$(CURDIR)/python python3 -m pytest \
	    python/test/quality_runner_test.py \
	    python/test/feature_extractor_test.py \
	    python/test/vmafexec_test.py \
	    python/test/vmafexec_feature_extractor_test.py \
	    python/test/result_test.py \
	    -v -m "not slow" --tb=short

# Sanitizer build (ASan + UBSan) — used by CI and `/build-vmaf --sanitizers`.
test-sanitizers:
	@mkdir -p build-san
	meson setup build-san $(LIBVMAF_DIR) --buildtype=debug \
	    -Db_sanitize=address,undefined \
	    -Denable_cuda=false -Denable_sycl=false \
	    --reconfigure 2>/dev/null || \
	meson setup build-san $(LIBVMAF_DIR) --buildtype=debug \
	    -Db_sanitize=address,undefined \
	    -Denable_cuda=false -Denable_sycl=false
	ninja -C build-san
	meson test -C build-san --print-errorlogs

test-fast: build
	PATH="$(VENV)/bin:$$PATH" meson test -C $(BUILD_DIR) --suite=fast

# ============================================================================
# Coverage gate (docs/principles.md §3 — ≥70% overall, ≥85% security-critical)
# ============================================================================

COVERAGE_DIR := build-coverage
COVERAGE_MIN_OVERALL := 70
COVERAGE_MIN_CRITICAL := 85

# Build with gcov instrumentation, run tests, emit lcov report.
# Uses a dedicated build dir so normal `make build` isn't instrumented.
coverage:
	@command -v lcov >/dev/null || { echo "lcov not found — install lcov"; exit 1; }
	@command -v gcov >/dev/null || { echo "gcov not found — install gcc"; exit 1; }
	@mkdir -p $(COVERAGE_DIR)
	meson setup $(COVERAGE_DIR) $(LIBVMAF_DIR) --buildtype=debug -Db_coverage=true \
	    -Denable_cuda=false -Denable_sycl=false --reconfigure 2>/dev/null || \
	meson setup $(COVERAGE_DIR) $(LIBVMAF_DIR) --buildtype=debug -Db_coverage=true \
	    -Denable_cuda=false -Denable_sycl=false
	ninja -C $(COVERAGE_DIR)
	meson test -C $(COVERAGE_DIR) --print-errorlogs
	@echo "--- gathering coverage ---"
	lcov --capture --directory $(COVERAGE_DIR) --output-file $(COVERAGE_DIR)/coverage.info \
	     --ignore-errors mismatch,gcov,source --rc geninfo_unexecuted_blocks=1
	lcov --remove $(COVERAGE_DIR)/coverage.info \
	     '/usr/*' '*/subprojects/*' '*/test/*' '*/tests/*' \
	     --output-file $(COVERAGE_DIR)/coverage.filtered.info \
	     --ignore-errors unused
	lcov --list $(COVERAGE_DIR)/coverage.filtered.info | tee $(COVERAGE_DIR)/coverage.summary.txt

# Render HTML coverage report (open $(COVERAGE_DIR)/html/index.html).
coverage-html: coverage
	genhtml $(COVERAGE_DIR)/coverage.filtered.info \
	    --output-directory $(COVERAGE_DIR)/html \
	    --demangle-cpp --legend --title "libvmaf coverage"
	@echo "open $(COVERAGE_DIR)/html/index.html"

# Enforce the coverage thresholds from docs/principles.md §3.
# Overall: ≥70% line coverage. Security-critical (core/src/dnn/, opt.c,
# read_json_model.c): ≥85% line coverage.
coverage-check: coverage
	@scripts/ci/coverage-check.sh $(COVERAGE_DIR)/coverage.filtered.info \
	    $(COVERAGE_MIN_OVERALL) $(COVERAGE_MIN_CRITICAL)

# Power-of-10 rule 5 density check (≥2 asserts per function average across
# fork-added code). Warns on any non-trivial fork-added function with 0 asserts.
assertion-density:
	@scripts/ci/assertion-density.sh

# LLVM IR diff harness (ADR-0918). On-demand only — NOT in CI by default
# (would add a clang re-compile per SIMD file to every build).
#
# Use `make ir-diff` after touching a SIMD file or bumping clang to catch
# compiler-induced bit-exactness regressions BEFORE the
# score-vs-snapshot tests do. `make ir-diff-update` re-seeds the
# snapshots; include the justification in the commit message (same
# discipline as /regen-snapshots for score JSONs).
#
# Environment:
#   IR_DIFF_CLANG=<path>     override clang binary
#   IR_DIFF_FILTER=<substr>  run only entries whose source matches
ir-diff:
	@bash scripts/perf/check-ir-diff.sh

ir-diff-update:
	@bash scripts/perf/check-ir-diff.sh update

# Install the pre-commit + pre-push git hooks.
#
# Default (framework) path — symlinks the framework-managed pre-commit
# hook from .pre-commit-config.yaml (including the
# `agent-worktree-drift-guard` local hook; ADR-0332) plus the commit-msg
# hook, then the fork's pre-push PR-body deliverables validator at
# scripts/git-hooks/pre-push (mirrors rule-enforcement.yml; ADR-0108).
#
# Native (opt-in) path — set VMAFX_NATIVE_HOOKS=1 to install the bash
# pre-commit at scripts/githooks/pre-commit.sh instead of the framework
# hook. The native path skips the per-hook venv-wrap cost (~3 s/hook)
# and typically completes in ~0.4 s on a small commit. CI is unaffected.
# See docs/development/pre-commit-hooks.md and ADR-0924.
#
# Usage:
#   make install-hooks                          # framework (default)
#   VMAFX_NATIVE_HOOKS=1 make install-hooks     # native bash
#
# Idempotent: re-running replaces stale symlinks. Existing non-symlink
# pre-push or pre-commit hooks are preserved with a `.local-backup`
# suffix so a contributor's hand-rolled hook is never silently
# overwritten.
#
# `hooks-install` retained as a legacy alias for `install-hooks`.
install-hooks:
	@scripts/githooks/install.sh

hooks-install: install-hooks

# pr-check — local equivalent of the rule-enforcement.yml deliverables gate.
# Runs scripts/ci/deliverables-check.sh against an existing PR's body
# (PR=<num>) or against a local body file (BODY=<path>). Exits non-zero
# if the six-deliverable checklist or any ticked file reference is
# inconsistent with the diff vs origin/master.
#
# Usage:
#   make pr-check PR=260
#   make pr-check BODY=pr-body.md
pr-check:
	@if [ -n "$(PR)" ]; then \
	    echo "--- pr-check: fetching PR $(PR) body via gh ---"; \
	    PR_BODY="$$(gh pr view $(PR) --json body -q .body)" \
	        bash scripts/ci/deliverables-check.sh; \
	elif [ -n "$(BODY)" ]; then \
	    echo "--- pr-check: reading body from $(BODY) ---"; \
	    PR_BODY="$$(cat "$(BODY)")" \
	        bash scripts/ci/deliverables-check.sh; \
	else \
	    echo "Usage: make pr-check PR=<number>  OR  make pr-check BODY=<file>" >&2; \
	    exit 2; \
	fi

# ── Go workspace (ADR-0702) ─────────────────────────────────────────────────
#
# go-build: compile all Go packages in the workspace (no output binary in the
#           foundation PR; cmd/ binaries are added by per-sweep PRs).
# go-test:  run `go test ./...` (covers pkg/version and future packages).
#
# Both targets require Go ≥ 1.23 on PATH. If `go` is absent they fail with an
# actionable message rather than a confusing "command not found".

go-build:
	@command -v go >/dev/null || { echo "go not found — install Go ≥ 1.23 (https://go.dev/dl/)"; exit 1; }
	go build ./...

go-test:
	@command -v go >/dev/null || { echo "go not found — install Go ≥ 1.23 (https://go.dev/dl/)"; exit 1; }
	go test ./...

# setup-envtest: install the kubebuilder envtest control-plane binaries
#                (etcd + kube-apiserver + kubectl) and print the export line
#                needed to run `cmd/vmafx-operator/internal/controller` tests.
#
# The vmafx-operator suite needs an embedded etcd + API server to start before
# BeforeSuite can run; without KUBEBUILDER_ASSETS pointing at the asset dir,
# envtest.Environment.Start() panics with a nil-pointer deref (PRs #330 / #341 /
# #362 all tripped this). This target installs the sigs.k8s.io/controller-runtime
# setup-envtest binary into GOBIN, downloads the v1.31 control-plane bundle, and
# prints the eval-friendly export line. Re-runs are idempotent.
#
# Usage:
#   make setup-envtest                              # install + download
#   eval $$(make -s setup-envtest-env)              # export KUBEBUILDER_ASSETS
#   go test ./cmd/vmafx-operator/internal/controller/...
#
# The CI workflow (.github/workflows/go-ci.yml) runs this target before
# `go test ./...` so the operator suite executes for real instead of skipping.

ENVTEST_K8S_VERSION ?= 1.31

setup-envtest:
	@command -v go >/dev/null || { echo "go not found — install Go ≥ 1.23 (https://go.dev/dl/)"; exit 1; }
	@command -v setup-envtest >/dev/null || \
	    go install sigs.k8s.io/controller-runtime/tools/setup-envtest@latest
	@setup-envtest use $(ENVTEST_K8S_VERSION) -p path >/dev/null
	@echo "envtest assets installed; export with:"
	@echo "  export KUBEBUILDER_ASSETS=\$$(setup-envtest use $(ENVTEST_K8S_VERSION) -p path)"

# setup-envtest-env: print the export line on stdout (machine-readable form).
# Use as `eval $(make -s setup-envtest-env)` to wire KUBEBUILDER_ASSETS into the
# current shell.
setup-envtest-env:
	@command -v setup-envtest >/dev/null || { \
	    echo 'setup-envtest not installed — run `make setup-envtest` first' >&2; \
	    exit 1; \
	}
	@printf 'export KUBEBUILDER_ASSETS=%s\n' "$$(setup-envtest use $(ENVTEST_K8S_VERSION) -p path)"

# ── Rust workspace (ADR-0702) ────────────────────────────────────────────────
#
# rust-build: cargo check --all (no members yet; validates the workspace manifest).
# rust-test:  cargo test --all (no tests until vmafx-sys crate is added).
#
# Both targets require a stable Rust toolchain on PATH.

rust-build:
	@command -v cargo >/dev/null || { echo "cargo not found — install Rust via https://rustup.rs/"; exit 1; }
	cargo check --all

rust-test:
	@command -v cargo >/dev/null || { echo "cargo not found — install Rust via https://rustup.rs/"; exit 1; }
	cargo test --all

help:
	@echo "Fork-specific targets:"
	@echo "  make lint             — clang-tidy + cppcheck + ruff + shellcheck + markdownlint"
	@echo "  make lint-md          — markdownlint-cli2 on changed *.md (MDLINT_SCOPE=all for full tree, ADR-0866)"
	@echo "  make format           — clang-format + black + isort + shfmt (writes)"
	@echo "  make format-check     — same, no writes (CI gate)"
	@echo "  make sec              — semgrep (CERT-C + CWE + fork rules)"
	@echo "  make sbom             — SPDX + CycloneDX SBOMs via syft"
	@echo "  make pr-check         — ADR-0108 deliverables gate (PR=<num> or BODY=<file>)"
	@echo "  make test-netflix-golden — D24 gate: 3 Netflix CPU test pairs"
	@echo "  make test-sanitizers  — ASan + UBSan build + run"
	@echo "  make test-fast        — meson --suite=fast (pre-push gate)"
	@echo "  make coverage         — gcov/lcov line+branch coverage report"
	@echo "  make coverage-html    — render HTML coverage report"
	@echo "  make coverage-check   — enforce ≥70% overall / ≥85% critical"
	@echo "  make assertion-density — Power-of-10 rule 5 density check"
	@echo "  make lint-tools       — install ruff/black/isort/mypy into .venv at the pinned versions"
	@echo "  make install-hooks    — wire up pre-commit + pre-push git hooks"
	@echo "                          (set VMAFX_NATIVE_HOOKS=1 for native bash; ADR-0924)"
	@echo "  make hooks-install    — legacy alias for install-hooks"
	@echo ""
	@echo "  make go-build         — go build ./... (Go workspace, ADR-0702)"
	@echo "  make go-test          — go test ./... (Go workspace, ADR-0702)"
	@echo "  make rust-build       — cargo check --all (Rust workspace, ADR-0702)"
	@echo "  make rust-test        — cargo test --all (Rust workspace, ADR-0702)"
	@echo "  make setup-envtest    — install kubebuilder envtest binaries for vmafx-operator suite"
	@echo ""
	@echo "Upstream targets: build, test, debug, install, clean, distclean, cythonize"
