# Use bash for shell
SHELL := /bin/bash

# Path and environment setup
VENV := .venv
VIRTUAL_ENV_PATH := $(VENV)/bin
VIRTUAL_ENV_ABS := $(abspath $(VIRTUAL_ENV_PATH))

# Build tools configured in the virtual environment
PYTHON_INTERPRETER := python3
VENV_PIP := $(VENV)/bin/pip
VENV_PYTHON := $(VIRTUAL_ENV_PATH)/python
MESON := $(VENV)/bin/meson
MESON_EXEC := $(abspath $(MESON))
MESON_SETUP := "$(MESON_EXEC)" setup
NINJA := $(VENV)/bin/ninja
NINJA_EXEC := $(abspath $(NINJA))

# Lint and format tools resolve from the project venv first, then the system
# PATH. Without this, `make lint-py` / `make format-check` silently found no
# ruff / black and reported success, so the local gate could pass while
# CI's identical checks failed.
export PATH := $(VIRTUAL_ENV_ABS):$(PATH)

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
GOLDEN_BUILD_DIR ?= $(LIBVMAF_DIR)/build-golden
# The aarch64 golden gate on an x86 host (ADR-1461): `gcc` or `clang`, each in
# its own build directory, cross-built and run through qemu-user.
GOLDEN_ARM64_CC ?= gcc
GOLDEN_ARM64_CROSS_FILE ?= build-aux/aarch64-linux-gnu$(if $(filter clang,$(GOLDEN_ARM64_CC)),-clang).ini
GOLDEN_ARM64_BUILD_DIR ?= $(LIBVMAF_DIR)/build-golden-arm64-$(GOLDEN_ARM64_CC)

.PHONY: default all debug build install cythonize clean distclean cythonize-deps \
    node-bpf go-build go-test go-fix go-fix-check go-ort-runner rust-build rust-test setup-envtest setup-envtest-env \
    build-golden build-golden-arm64 test-affected

default: build

all: build debug install test cythonize

$(BUILD_DIR): $(MESON) $(NINJA)
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" $(MESON_SETUP) $(BUILD_DIR) $(LIBVMAF_DIR) $(BUILDTYPE_RELEASE) $(ENABLE_FLOAT) $(ENABLE_CUDA)

$(DEBUG_DIR): $(MESON) $(NINJA)
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" $(MESON_SETUP) $(DEBUG_DIR) $(LIBVMAF_DIR) $(BUILDTYPE_DEBUG) $(ENABLE_FLOAT) $(ENABLE_CUDA)

build-golden: $(MESON) $(NINJA)
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" bash scripts/ci/setup-golden-build.sh $(GOLDEN_BUILD_DIR) $(LIBVMAF_DIR) $(NINJA)

# The same profile cross-built for aarch64; the preflight names what an x86
# host lacks (cross compiler, sysroot, binfmt handler) before anything is
# configured.
build-golden-arm64: $(MESON) $(NINJA)
	bash scripts/ci/golden-arm64-preflight.sh "$(GOLDEN_ARM64_CC)" "$(GOLDEN_ARM64_CROSS_FILE)" "$(QEMU_LD_PREFIX)"
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" GOLDEN_CROSS_FILE="$(GOLDEN_ARM64_CROSS_FILE)" \
	    bash scripts/ci/setup-golden-build.sh $(GOLDEN_ARM64_BUILD_DIR) $(LIBVMAF_DIR) $(NINJA)

cythonize: cythonize-deps
	pushd python && "$(VENV_PYTHON)" setup.py build_ext --build-lib . && popd || exit 1

build: $(BUILD_DIR) $(NINJA)
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" $(NINJA) -vC $(BUILD_DIR)

test: build $(NINJA)
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" "$(VENV_PYTHON)" scripts/ci/run_meson_test.py \
	    --meson-executable "$(MESON_EXEC)" -- -C $(BUILD_DIR) \
	    --no-rebuild --print-errorlogs

debug: $(DEBUG_DIR) $(NINJA)
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" $(NINJA) -vC $(DEBUG_DIR)

install: $(BUILD_DIR) $(NINJA)
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" $(NINJA) -vC $(BUILD_DIR) install

clean:
	rm -rf $(BUILD_DIR) $(DEBUG_DIR) $(GOLDEN_BUILD_DIR) $(LIBVMAF_DIR)/build-golden-arm64-gcc $(LIBVMAF_DIR)/build-golden-arm64-clang
	rm -f compat/python-vmaf/core/adm_dwt2_cy.c*

distclean: clean
	rm -rf $(VENV)

# Set up or rebuild virtual environment
$(VENV_PIP):
	@echo "Setting up the virtual environment..."
	$(PYTHON_INTERPRETER) -m venv $(VENV) || { echo "Failed to create virtual environment"; exit 1; }
	$(VENV_PIP) install --require-hashes -r requirements/locks/build.txt || { echo "Failed to bootstrap virtual environment"; exit 1; }
	@echo "Virtual environment setup complete."

$(MESON): $(VENV_PIP)
	$(VENV_PIP) install --require-hashes -r requirements/locks/build.txt || { echo "Failed to install meson"; exit 1; }

$(NINJA): $(VENV_PIP)
	$(VENV_PIP) install --require-hashes -r requirements/locks/build.txt || { echo "Failed to install ninja"; exit 1; }

# Provision the lint / format toolchain into the project venv. Versions are
# kept identical to .pre-commit-config.yaml so the local gate and the CI hooks
# cannot disagree about what counts as a violation.
RUFF_VERSION  := 0.16.10
BLACK_VERSION := 26.10.0

.PHONY: lint-tools
lint-tools: $(VENV_PIP)
	$(VENV_PIP) install --quiet --require-hashes -r requirements/locks/dev-linters.txt
	@echo "lint tools installed into $(VENV_PIP:%/pip=%)"
	@command -v shfmt >/dev/null || { \
	   echo "note: shfmt is not a Python package and was not installed."; \
	   echo "      get it with: go install mvdan.cc/sh/v3/cmd/shfmt@v3.13.1"; }
	@command -v shellcheck >/dev/null || \
	   echo "note: shellcheck not found — install it via your package manager."

# Cythonize build dependencies are hash-locked in requirements/locks/cythonize.txt.
cythonize-deps: $(VENV_PIP)
	$(VENV_PIP) install --require-hashes -r requirements/locks/cythonize.txt || { echo "Failed to install dependencies"; exit 1; }

# ============================================================================
# Fork-specific targets (lusoris). The upstream targets above are preserved as-is.
# ============================================================================

.PHONY: lint lint-c lint-py lint-sh lint-md lint-go lint-actions tidy-ratchet tidy-ratchet-write \
	tidy-ratchet-build tidy-lane tidy-lane-write \
	base-images-sync cuda-pin-sync python-deps-sync \
	python-locks-check python-locks-write \
	preflight \
	format format-check sec sbom \
        test-netflix-golden test-netflix-golden-arm64 test-sanitizers test-fast install-hooks hooks-install help \
        upstream-parity upstream-parity-full \
        coverage coverage-html coverage-check assertion-density pr-check ffmpeg-input-contract \
        silent-revert-check

# Top-level lint — runs every analyzer we own. Uses the meson compile_commands.json.
lint: lint-c lint-py lint-sh lint-md lint-go lint-actions docs-fragments-check lint-reuse python-locks-check
	@echo "=== all lints passed ==="

# REUSE 3.3 compliance check (BUG-003). Ensures 100% license and copyright coverage.
.PHONY: lint-reuse
lint-reuse:
	@echo "--- REUSE 3.3 compliance check ---"
	@if command -v reuse >/dev/null 2>&1; then \
	    reuse lint; \
	else \
	    echo "ERROR: reuse executable not found on PATH. Install via 'pip install reuse==6.2.0'." >&2; \
	    exit 1; \
	fi

python-locks-check:
	@python3 scripts/ci/check_python_dependency_locks.py check

python-locks-write:
	@python3 scripts/ci/check_python_dependency_locks.py write

# Go security scan (gosec). Skips generated files by default; surfaces every
# G* finding outside the gen/ tree. Source of truth for the gate added by
# the gosec-findings-fix sweep — keep the touched-file rule honest.
lint-go:
	$(call require-tool,gosec,go install github.com/securego/gosec/v2/cmd/gosec@v2.29.0)
	@echo "--- gosec (exclude-generated) ---"
	@gosec -exclude-generated -quiet ./...

# GitHub Actions lint. actionlint validates every workflow under
# .github/workflows/ against .github/actionlint.yaml; actionlint cannot read a
# composite action, so scripts/ci/check_composite_actions.py checks those.
lint-actions:
	$(call require-tool,actionlint,go install github.com/rhysd/actionlint/cmd/actionlint@v1.7.12)
	@echo "--- actionlint (.github/workflows) ---"
	@actionlint
	@echo "--- composite actions (.github/actions): structure + shellcheck ---"
	@python3 scripts/ci/check_composite_actions.py

# Fragment-tree drift check (ADR-0221). Verifies CHANGELOG.md and
# docs/adr/README.md are in sync with fragments, ADR tag pages match sources,
# the exact-twin table matches scripts/ci/exact_twins.d/ (ADR-1428), and every
# AGENTS.md next to an AGENTS.d/ matches its topic pages (ADR-1454), the
# documentation charts match their specs, data and renders, and every vendored
# docs asset matches the hashes in its vendor.json (ADR-1508).
docs-fragments-check:
	@echo "--- changelog.d/ vs CHANGELOG.md ---"
	@bash scripts/release/concat-changelog-fragments.sh --check
	@echo "--- docs/adr/_index_fragments/ vs docs/adr/README.md ---"
	@bash scripts/docs/concat-adr-index.sh --check
	@bash scripts/docs/generate-adr-by-tag.sh --check
	@python3 scripts/docs/generate-record-titles.py --check
	@echo "--- scripts/ci/exact_twins.d/ vs docs/development/cross-backend-exact-twins.md ---"
	@python3 scripts/docs/generate-exact-twins.py --check
	@echo "--- scripts/ci/exact_twins.d/ vs core/src/vmafx/exactness_gen.c (ADR-2073) ---"
	@python3 scripts/codegen/vmafx_exactness.py --check
	@echo "--- scripts/ci/upstream_parity.d/ vs docs/development/upstream-parity-allowlist.md ---"
	@python3 scripts/docs/generate-upstream-parity-allowlist.py --check
	@echo "--- */AGENTS.d/ vs */AGENTS.md ---"
	@python3 scripts/docs/agents_index.py --check
	@echo "--- docs/hardware-reports/ (tester reports: schema + integrity) ---"
	@python3 scripts/ci/check-hardware-reports.py
	@echo "--- docs/charts/ vs their renders and data (ADR-1508) ---"
	@python3 scripts/docs/generate-charts.py --check
	@echo "--- docs/**/vendor.json (vendored fonts and scripts vs their hashes) ---"
	@python3 scripts/docs/check_vendored_assets.py

# Regenerate consolidated outputs from fragments (ADR-0221).
docs-fragments-write:
	@bash scripts/release/concat-changelog-fragments.sh --write
	@bash scripts/docs/concat-adr-index.sh --write
	@bash scripts/docs/generate-adr-by-tag.sh --write
	@python3 scripts/docs/generate-record-titles.py --write
	@python3 scripts/docs/generate-exact-twins.py --write
	@python3 scripts/codegen/vmafx_exactness.py --write
	@python3 scripts/docs/generate-upstream-parity-allowlist.py --write
	@python3 scripts/docs/agents_index.py --write
	@python3 scripts/docs/generate-hardware-reports.py --write
	@python3 scripts/docs/generate-charts.py --write

# Analyze only this Meson profile, retaining all configured command variants.
# Backend-specific clang-tidy options can be supplied with repeated
# --clang-tidy-arg=... operands in LINT_CONFIGURED_ARGS.
LINT_JOBS ?= 4
LINT_CONFIGURED_ARGS ?=
lint-c: $(BUILD_DIR) $(MESON) $(NINJA)
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" $(MESON_SETUP) --reconfigure "$(BUILD_DIR)" "$(LIBVMAF_DIR)"
	$(MAKE) build
	$(PYTHON_INTERPRETER) scripts/ci/write-compile-commands.py \
	    --build-dir "$(BUILD_DIR)" --ninja "$(NINJA)"
	$(PYTHON_INTERPRETER) scripts/ci/lint-configured.py --build-dir "$(BUILD_DIR)" \
	    --jobs "$(LINT_JOBS)" $(LINT_CONFIGURED_ARGS)

# ADR-1142 — whole-tree clang-tidy debt ratchet. LANE=cpu|clang|cuda|sycl|hip|arm64
# (default cpu). `tidy-ratchet` measures a build dir that is configured for the
# lane (TIDY_RATCHET_BUILD_DIR, default $(BUILD_DIR)) and compares it with
# scripts/ci/tidy-baseline-$(LANE).json; `tidy-ratchet-write` rewrites that
# baseline — commit it in the same PR; never hand-edit a baseline.
#
# Where a lane is measured (ADR-1471): the five baselines are defined against
# the Ubuntu 26.04 dev container, not against the machine you sit at. `make tidy-lane LANE=<lane>` copies this checkout into a
# throwaway container of the dev image, configures the lane there with its real
# toolchain (`tidy-ratchet-build` below) and runs this target inside it;
# `make tidy-lane-write` brings the rewritten baseline back. A run on the host
# is a quick look, not a measurement: its numbers are not the baseline's.
#
# `tidy-ratchet-build` configures and builds TIDY_RATCHET_BUILD_DIR from the
# lane's own TIDY_RATCHET_COMPILERS_<lane> and TIDY_RATCHET_SETUP_<lane>. Every
# lane sets -Db_lto=false: the project default carries b_lto_threads=4
# (ADR-1172), which meson renders as GCC's -flto=4. clang-tidy parses these
# compile commands with clang, which rejects it ("unsupported argument '4' to
# option '-flto='"), so every translation unit is reported as a compile
# failure.
# The build dir may live inside or outside the repository: tidy-ratchet.py
# skips everything under --build-dir (the generated *_hsaco.c / *.json.c
# translation units and headers), so both measure the same checked-in sources.
# The arm64 lane is the only cross lane: nothing on an x86 host compiles
# core/src/feature/arm64/ or the ARCH_AARCH64 bodies in core/test/, so the
# cpu lane's compile database has no entry for them and they were unmeasured
# (ADR-1283). Its build dir is configured with the in-tree cross file, by
# `tidy-ratchet-build LANE=arm64` in the container or by hand for a look —
#
#   meson setup build-arm64 core --cross-file build-aux/aarch64-linux-gnu.ini \
#       -Denable_cuda=false -Denable_sycl=false -Db_lto=false
#   make tidy-ratchet LANE=arm64 TIDY_RATCHET_BUILD_DIR=build-arm64
# — which gives aarch64-linux-gnu-gcc compile commands. clang-tidy needs the
# same target and sysroot to parse them: without --target it reads the NEON
# and SVE2 intrinsics against the host's x86 headers, and without --sysroot
# it resolves libc against the host's. Both are the cross package's defaults
# (Arch `aarch64-linux-gnu-glibc`, Debian/Ubuntu `libc6-dev-arm64-cross`);
# override AARCH64_SYSROOT for a sysroot installed anywhere else.
AARCH64_TARGET ?= aarch64-linux-gnu
AARCH64_SYSROOT ?= /usr/aarch64-linux-gnu
LANE ?= cpu
TIDY_RATCHET_BUILD_DIR ?= $(BUILD_DIR)
TIDY_RATCHET_JOBS ?= 8
# clang-tidy for every lane; the sycl wrapper reads the same variable. The dev
# container's PATH resolves plain `clang-tidy` to ROCm's LLVM, so the container
# entry point sets this to /usr/bin/clang-tidy-22.
CLANG_TIDY_BIN ?= clang-tidy
export CLANG_TIDY_BIN

# What each lane configures (ADR-1471): one definition, used by
# `tidy-ratchet-build` in the container and repeated for cpu by the hosted
# `Tidy Ratchet` job (scripts/ci/tests/test_tidy_lane_container.py keeps the two
# the same). cpu is the hosted configuration: gcc-15, no GPU backend and no
# ONNX Runtime (the hosted runner has none, the container does). The GPU lanes
# turn their compiler on (nvcc, hipcc, icpx) so the device bodies are parsed,
# not the -ENOSYS stubs, and enable ONNX Runtime so the DNN bodies are too. sycl
# compiles SPIR-V only (no ahead-of-time targets): the device list changes
# backend arguments that the lint database drops anyway. arm64 cross-compiles
# with the distribution's aarch64 gcc; the second cross file names Ubuntu's
# `qemu-aarch64` where the first names `qemu-aarch64-static`.
TIDY_RATCHET_COMPILERS_cpu := CC=gcc-15 CXX=g++-15
TIDY_RATCHET_COMPILERS_clang := CC=clang-22 CXX=clang++-22
TIDY_RATCHET_COMPILERS_cuda := CC=gcc-15 CXX=g++-15
TIDY_RATCHET_COMPILERS_hip := CC=gcc-15 CXX=g++-15
TIDY_RATCHET_COMPILERS_sycl := CC=icx CXX=icpx
TIDY_RATCHET_COMPILERS_arm64 :=
# The metal lane is the one lane with no container: Apple's SDK exists on a
# macOS host only. The `Tidy Metal` workflow (tidy-metal.yml) repeats these
# two lines and passes the --select list of the Metal host sources.
TIDY_RATCHET_COMPILERS_metal := CC=clang CXX=clang++
TIDY_RATCHET_SETUP_metal := -Denable_metal=enabled -Denable_cuda=false -Denable_sycl=false \
	-Denable_dnn=disabled -Db_lto=false
TIDY_RATCHET_SETUP_cpu := -Denable_cuda=false -Denable_sycl=false \
	-Denable_dnn=disabled -Denable_mcp=true -Denable_mcp_sse=enabled \
	-Denable_mcp_uds=true -Denable_mcp_stdio=true -Db_lto=false
TIDY_RATCHET_SETUP_clang := -Denable_cuda=false -Denable_sycl=false \
	-Denable_dnn=disabled -Denable_mcp=true -Denable_mcp_sse=enabled \
	-Denable_mcp_uds=true -Denable_mcp_stdio=true -Dfuzz=true -Db_lto=false
TIDY_RATCHET_SETUP_cuda := -Denable_cuda=true -Denable_nvcc=true \
	-Denable_sycl=false -Denable_hip=false -Denable_dnn=enabled -Db_lto=false
TIDY_RATCHET_SETUP_hip := -Denable_hip=true -Denable_hipcc=true \
	-Denable_cuda=false -Denable_sycl=false -Denable_dnn=enabled -Db_lto=false
TIDY_RATCHET_SETUP_sycl := -Denable_sycl=true -Dsycl_icpx_aot_targets= \
	-Denable_cuda=false -Denable_hip=false -Denable_dnn=enabled -Db_lto=false
TIDY_RATCHET_SETUP_arm64 := --cross-file build-aux/aarch64-linux-gnu.ini \
	--cross-file build-aux/aarch64-linux-gnu-qemu-user.ini \
	-Denable_cuda=false -Denable_sycl=false -Denable_dnn=disabled -Db_lto=false
TIDY_RATCHET_EXTRA_cpu :=
TIDY_RATCHET_EXTRA_clang := --select core/test/fuzz/ --select core/src/read_json_model.c
TIDY_RATCHET_EXTRA_cuda := --extra-arg=--cuda-host-only --extra-arg=-nocudalib
TIDY_RATCHET_EXTRA_hip := --clang-tidy $(CURDIR)/scripts/ci/clang-tidy-hip.sh \
	--extra-arg=-D__HIP_PLATFORM_AMD__=1 --extra-arg=-I/opt/rocm/include
TIDY_RATCHET_EXTRA_sycl := --clang-tidy $(CURDIR)/scripts/ci/clang-tidy-sycl.sh
TIDY_RATCHET_EXTRA_arm64 := --extra-arg=--target=$(AARCH64_TARGET) \
	--extra-arg=--sysroot=$(AARCH64_SYSROOT)

# nvcc, hipcc and icpx compile through meson custom targets, which leaves their
# translation units out of compile_commands.json: write-compile-commands.py
# exports only the native c/cpp_COMPILER rules, so without this second pass the
# cuda and hip lanes measure the host files only and the sycl lane measures zero
# SYCL feature TUs, recording an empty backend in its baseline.
TIDY_RATCHET_COMPDB_cpu :=
TIDY_RATCHET_COMPDB_clang :=
TIDY_RATCHET_COMPDB_cuda := $(PYTHON_INTERPRETER) scripts/ci/gen-gpu-compile-commands.py \
	"$(TIDY_RATCHET_BUILD_DIR)"
TIDY_RATCHET_COMPDB_hip := $(PYTHON_INTERPRETER) scripts/ci/gen-gpu-compile-commands.py \
	"$(TIDY_RATCHET_BUILD_DIR)"
TIDY_RATCHET_COMPDB_sycl := $(PYTHON_INTERPRETER) scripts/ci/gen-sycl-compile-commands.py \
	"$(TIDY_RATCHET_BUILD_DIR)"
TIDY_RATCHET_COMPDB_arm64 :=

# Configure and build a lane's build dir: compile_commands.json plus the
# generated headers the translation units include.
tidy-ratchet-build: $(MESON) $(NINJA)
	$(TIDY_RATCHET_COMPILERS_$(LANE)) PATH="$(VIRTUAL_ENV_ABS):$$PATH" $(MESON_SETUP) \
	    "$(TIDY_RATCHET_BUILD_DIR)" "$(LIBVMAF_DIR)" $(TIDY_RATCHET_SETUP_$(LANE))
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" "$(NINJA_EXEC)" -C "$(TIDY_RATCHET_BUILD_DIR)" \
	    -j "$(TIDY_RATCHET_JOBS)"

# Measure LANE (cpu, cuda, hip, sycl, arm64 or all) in a throwaway dev container;
# TIDY_LANE_ARGS passes --image / --jobs / --out to scripts/dev/tidy-lane.sh.
tidy-lane:
	scripts/dev/tidy-lane.sh $(TIDY_LANE_ARGS) $(LANE)

tidy-lane-write:
	scripts/dev/tidy-lane.sh --write $(TIDY_LANE_ARGS) $(LANE)

tidy-ratchet: $(NINJA)
	$(call require-tool,$(CLANG_TIDY_BIN),install clang-tools)
	$(PYTHON_INTERPRETER) scripts/ci/write-compile-commands.py \
	    --build-dir "$(TIDY_RATCHET_BUILD_DIR)" --ninja "$(NINJA)"
	$(TIDY_RATCHET_COMPDB_$(LANE))
	python3 scripts/ci/tidy-ratchet.py --lane $(LANE) \
	    --build-dir $(TIDY_RATCHET_BUILD_DIR) $(TIDY_RATCHET_EXTRA_$(LANE)) $(TIDY_RATCHET_ARGS)

tidy-ratchet-write: $(NINJA)
	$(call require-tool,$(CLANG_TIDY_BIN),install clang-tools)
	$(PYTHON_INTERPRETER) scripts/ci/write-compile-commands.py \
	    --build-dir "$(TIDY_RATCHET_BUILD_DIR)" --ninja "$(NINJA)"
	$(TIDY_RATCHET_COMPDB_$(LANE))
	python3 scripts/ci/tidy-ratchet.py --lane $(LANE) --write \
	    --build-dir $(TIDY_RATCHET_BUILD_DIR) $(TIDY_RATCHET_EXTRA_$(LANE)) $(TIDY_RATCHET_ARGS)

# Rewrite every Dockerfile's base-image ARG defaults from build-config.env.
# Edit the config, run this, commit both.
base-images-sync:
	scripts/ci/check-base-image-single-source.sh --write
	scripts/ci/check-base-image-single-source.sh

# Rewrite the derived CUDA spellings ($cudaMajorMinor, cuda-toolkit-NN-N, the OCI
# description label) from build-config.env's CUDA_VERSION. Renovate owns the rest
# of the coordinated pin; edit CUDA_VERSION and the image pins, run this, commit
# all of it together (ADR-1285).
cuda-pin-sync:
	python3 scripts/ci/check-cuda-pin-lockstep.py --write
	python3 scripts/ci/check-cuda-pin-lockstep.py

# Rewrite python/requirements.txt from python/pyproject.toml [project].dependencies.
python-deps-sync:
	scripts/ci/check-python-requirements-single-source.sh --write
	scripts/ci/check-python-requirements-single-source.sh

# Run the CI lanes that a single-compiler local build cannot catch: clang,
# 32-bit, sanitizers, MSVC-hostile constructs, clang-tidy, cppcheck (ADR-1234).
# `make preflight` before pushing; `scripts/dev/preflight.sh --list` explains
# which CI context each stage stands in for.
preflight:
	scripts/dev/preflight.sh

lint-py:
	@scripts/ci/check-python-requirements-single-source.sh
	$(call require-tool,ruff,make lint-tools)
	ruff check python/ ai/ scripts/ tools/rc1-tester/
	$(call require-tool,black,make lint-tools)
	black --check python/ ai/ scripts/ tools/rc1-tester/
# mypy is advisory (leading `-`): it currently reports ~295 module-resolution
# errors ("duplicate module", "adding __init__.py somewhere") that stop it
# before it type-checks anything real. That is a mypy-configuration gap
# (needs --explicit-package-bases / a mypy_path), not type debt, and fixing it
# is tracked separately. Kept running so the output stays visible.
	@command -v mypy >/dev/null || { echo "note: mypy not installed, skipping advisory check"; exit 0; }
	-mypy ai/scripts/ ai/tests/ ai/train/ ai/lpips_export.py scripts/

ffmpeg-input-contract:
	bash ffmpeg-patches/test/check-input-contract.sh
	python3 -m unittest discover -s ffmpeg-patches/test -p 'test_input_contract.py' -v

lint-sh: ffmpeg-input-contract
	$(call require-tool,shellcheck,your package manager, e.g. pacman -S shellcheck)
	shellcheck $$(git ls-files '*.sh')
	@scripts/ci/check-default-model-single-source.sh
	@scripts/ci/check-vcs-version-not-bare-sha.sh
	@bash scripts/ci/tests/test-check-vcs-version-not-bare-sha.sh
	@scripts/ci/test-prune-corrupt-fixtures.sh
	@bash scripts/dev/test-cleanup-agent-state.sh
	@scripts/ci/check-no-tracked-venv.sh
	@scripts/ci/check-aggregator-names.sh
	@scripts/ci/check-state-md-rows.sh
	@scripts/ci/check-base-image-single-source.sh
	@python3 scripts/ci/check-cuda-pin-lockstep.py
	@python3 scripts/githooks/tests/test_install.py
	@python3 scripts/githooks/tests/test_install_hooks_env.py

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

# The files clang-format reads, one definition for `format` and `format-check`: the
# C-family sources plus `.hip` and `.metal` (which pre-commit's `types_or` cannot select;
# see the second clang-format entry of .pre-commit-config.yaml). The exact_twins.d
# fragments only borrow the `.hip` extension; the Pelorus mirror is filtered out.
CLANG_FORMAT_FILES = git ls-files '*.c' '*.h' '*.cpp' '*.hpp' '*.cu' '*.cuh' '*.hip' '*.metal' \
	| grep -v '^subprojects/' | grep -v '^core/test/data/' | grep -v '^scripts/ci/exact_twins\.d/' \
	| python3 scripts/ci/pelorus_mirror.py filter

# Formatters — writes changes.
format:
	@command -v clang-format >/dev/null && \
	 clang-format -i $$($(CLANG_FORMAT_FILES)) || true
	@command -v black >/dev/null && black python/ ai/ scripts/ tools/rc1-tester/ 2>/dev/null || true
	@command -v ruff >/dev/null && ruff check --fix-only --quiet python/ ai/ scripts/ tools/rc1-tester/ || true
	@command -v shfmt >/dev/null && shfmt -w -i 2 -ci $$(git ls-files '*.sh') || true

# Formatters — check-only (CI gate, no writes).
format-check:
	$(call require-tool,clang-format,your package manager, e.g. pacman -S clang)
	clang-format --dry-run --Werror $$($(CLANG_FORMAT_FILES))
	$(call require-tool,black,make lint-tools)
	black --check python/ ai/ scripts/ tools/rc1-tester/
	$(call require-tool,ruff,make lint-tools)
	ruff check --select I python/ ai/ scripts/ tools/rc1-tester/
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
GOLDEN_PYTEST_ARGS := \
	python/test/quality_runner_test.py \
	python/test/feature_extractor_test.py \
	python/test/vmafexec_test.py \
	python/test/vmafexec_feature_extractor_test.py \
	python/test/result_test.py \
	-v -m "not slow" --tb=short
test-netflix-golden: build-golden
	@echo "=== Netflix CPU golden-data gate (D24) ==="
	@python3 -m pytest --version >/dev/null 2>&1 || { \
	    echo "error: pytest not found — this gate cannot run without it."; \
	    echo "       install: .venv/bin/pip install pytest (see docs/development/languages.md)"; exit 1; }
	CUDA_VISIBLE_DEVICES="" VMAF_FORCE_BACKEND=cpu VMAF_BUILD_DIR="$(CURDIR)/$(GOLDEN_BUILD_DIR)" PYTHONPATH=$(CURDIR)/python python3 -m pytest \
	    $(GOLDEN_PYTEST_ARGS)

# The test suites a change affects, in cached hash-locked venvs (docs/development/test-suites.md).
#   make test-affected BASE=origin/master HEAD=HEAD [VMAF_BIN=build/tools/vmaf]
test-affected:
	@test -n "$(BASE)" -a -n "$(HEAD)" || { echo "usage: make test-affected BASE=<sha> HEAD=<sha>"; exit 2; }
	python3 scripts/ci/run_affected_suites.py --base "$(BASE)" --head "$(HEAD)" \
	    $(if $(VMAF_BIN),--vmaf-bin "$(VMAF_BIN)")

# The same assertions against an aarch64 build on an x86 host (ADR-1461).
# The harness executes $(GOLDEN_ARM64_BUILD_DIR)/tools/vmaf like a native
# program: the kernel's binfmt_misc handler runs it under qemu-aarch64, which
# finds the aarch64 loader and C library under QEMU_LD_PREFIX. This checks
# numbers, not speed: emulated NEON says nothing about time on hardware, and
# the run takes about two hours where the native gate takes minutes.
#   make test-netflix-golden-arm64                         # aarch64 GCC
#   make test-netflix-golden-arm64 GOLDEN_ARM64_CC=clang   # aarch64 clang
QEMU_LD_PREFIX ?= $(AARCH64_SYSROOT)
test-netflix-golden-arm64: build-golden-arm64
	@echo "=== Netflix CPU golden-data gate, aarch64 $(GOLDEN_ARM64_CC) under qemu-user ==="
	@python3 -m pytest --version >/dev/null 2>&1 || { \
	    echo "error: pytest not found — this gate cannot run without it."; \
	    echo "       install: .venv/bin/pip install pytest (see docs/development/languages.md)"; exit 1; }
	QEMU_LD_PREFIX="$(QEMU_LD_PREFIX)" CUDA_VISIBLE_DEVICES="" VMAF_FORCE_BACKEND=cpu VMAF_BUILD_DIR="$(CURDIR)/$(GOLDEN_ARM64_BUILD_DIR)" PYTHONPATH=$(CURDIR)/python python3 -m pytest \
	    $(GOLDEN_PYTEST_ARGS)

# Upstream parity guard (ADR-1487): this tree's CPU extractors against
# Netflix/vmaf at the recorded parity head, every emitted value at %.17g,
# scalar and default dispatch. Both trees are built and run in the dev container
# image (the pinned environment: Netflix's own values depend on the compiler and
# the C library), upstream at the pin and this tree with the golden profile.
# Fails on a difference no fragment of scripts/ci/upstream_parity.d/ covers, on
# one above its fragment's bound, and on a fragment nothing matches any more.
# The full run also repeats every request with the heap filled and fails on an
# output of this tree that changes. Exit 2 = could not compare.
#   make upstream-parity        # probe set: about two minutes from an empty work directory
#   make upstream-parity-full   # every fixture, option variant and model, twice
# docs/development/upstream-parity.md has the details.
UPSTREAM_PARITY_JOBS ?= 8
UPSTREAM_PARITY_IMAGE ?= vmaf-dev-mcp:local
UPSTREAM_PARITY_ARGS ?=
upstream-parity:
	python3 scripts/dev/upstream_parity.py --container $(UPSTREAM_PARITY_IMAGE) --mode probe \
	    --jobs $(UPSTREAM_PARITY_JOBS) $(UPSTREAM_PARITY_ARGS)

upstream-parity-full:
	python3 scripts/dev/upstream_parity.py --container $(UPSTREAM_PARITY_IMAGE) --mode full \
	    --heap-check --jobs $(UPSTREAM_PARITY_JOBS) $(UPSTREAM_PARITY_ARGS)

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
	$(PYTHON_INTERPRETER) scripts/ci/run_meson_test.py -- -C build-san --print-errorlogs

test-fast: build
	PATH="$(VIRTUAL_ENV_ABS):$$PATH" "$(VENV_PYTHON)" scripts/ci/run_meson_test.py \
	    --meson-executable "$(MESON_EXEC)" -- -C $(BUILD_DIR) --suite=fast

# ============================================================================
# Coverage gate (docs/principles.md §3 — ≥70% overall, ≥85% security-critical)
# ============================================================================

COVERAGE_DIR := build-coverage
# Local floors. CI passes its own (37 CPU / 70 GPU overall, 85 critical; see
# docs/development/coverage-gate.md): a local run without the Python suite
# measures lower, so the overall floor here is the CPU job's.
COVERAGE_MIN_OVERALL := 37
COVERAGE_MIN_CRITICAL := 85
COVERAGE_JSON := $(COVERAGE_DIR)/coverage.json

# Build with gcov instrumentation, run the meson suite, emit the gcovr report
# the Coverage Gate job emits (gcovr, not lcov: ADR-0110 / ADR-0111; lcov sums
# a source compiled into several targets and prints impossible values).
# Uses a dedicated build dir so normal `make build` isn't instrumented. The
# build flags and the serial test run are the CI job's (ADR-0110).
coverage:
	@command -v gcovr >/dev/null || { echo "gcovr not found - install gcovr (requirements/locks/gcovr.txt)"; exit 1; }
	@command -v gcov >/dev/null || { echo "gcov not found - install gcc"; exit 1; }
	@mkdir -p $(COVERAGE_DIR)
	meson setup $(COVERAGE_DIR) $(LIBVMAF_DIR) --buildtype=debug -Db_coverage=true \
	    -Denable_cuda=false -Denable_sycl=false \
	    -Dc_args=-fprofile-update=atomic -Dcpp_args=-fprofile-update=atomic --reconfigure 2>/dev/null || \
	meson setup $(COVERAGE_DIR) $(LIBVMAF_DIR) --buildtype=debug -Db_coverage=true \
	    -Denable_cuda=false -Denable_sycl=false \
	    -Dc_args=-fprofile-update=atomic -Dcpp_args=-fprofile-update=atomic
	ninja -C $(COVERAGE_DIR)
	$(PYTHON_INTERPRETER) scripts/ci/run_meson_test.py -- \
	    -C $(COVERAGE_DIR) --print-errorlogs --num-processes 1
	@echo "--- gathering coverage ---"
	gcovr --root . \
	    --filter 'core/src/.*' \
	    --exclude '.*/test/.*' --exclude '.*/tests/.*' --exclude '.*/subprojects/.*' \
	    --gcov-ignore-parse-errors=negative_hits.warn \
	    --gcov-ignore-parse-errors=suspicious_hits.warn \
	    --print-summary \
	    --txt $(COVERAGE_DIR)/coverage.txt \
	    --json-summary $(COVERAGE_JSON) \
	    --xml $(COVERAGE_DIR)/coverage.xml \
	    $(COVERAGE_DIR)
	@cp $(COVERAGE_DIR)/coverage.txt $(COVERAGE_DIR)/coverage.summary.txt

# Render HTML coverage report (open $(COVERAGE_DIR)/html/index.html).
coverage-html: coverage
	@mkdir -p $(COVERAGE_DIR)/html
	gcovr --root . \
	    --filter 'core/src/.*' \
	    --exclude '.*/test/.*' --exclude '.*/tests/.*' --exclude '.*/subprojects/.*' \
	    --gcov-ignore-parse-errors=negative_hits.warn \
	    --gcov-ignore-parse-errors=suspicious_hits.warn \
	    --html-details $(COVERAGE_DIR)/html/index.html \
	    $(COVERAGE_DIR)
	@echo "open $(COVERAGE_DIR)/html/index.html"

# Enforce the coverage thresholds from docs/principles.md §3.
# Overall floor COVERAGE_MIN_OVERALL; security-critical (core/src/dnn/, opt.cpp,
# read_json_model.cpp): COVERAGE_MIN_CRITICAL. The script reads the gcovr JSON
# summary, never an lcov .info file.
coverage-check: coverage
	@scripts/ci/coverage-check.sh $(COVERAGE_JSON) \
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

# Install regular, worktree-independent pre-commit, commit-msg, pre-push,
# and pre-rebase dispatchers (ADR-1241). Unknown custom hooks are refused;
# replaced managed hooks are retained in unique backups. Native mode changes
# only pre-commit formatting; all push and message checks remain active.
# See docs/development/pre-commit-hooks.md.
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

# silent-revert-check — local equivalent of the rule-enforcement.yml
# Silent-Revert Guard (ADR-1284). Reports work the merge of this branch would
# remove from the target that the branch never set out to touch: a file reset
# to an older blob, a target commit undone hunk-for-hunk, or lines dropped or
# resurrected by a conflict resolution.
#
# BASE_REF defaults to the live origin/master tip, not the PR's recorded base,
# for the same reason CI does: the defect is master moving after the branch was
# cut.
#
# Usage:
#   make silent-revert-check
#   make silent-revert-check BASE_REF=origin/master HEAD_REF=my-branch
BASE_REF ?= origin/master
HEAD_REF ?= HEAD
silent-revert-check:
	@python3 scripts/ci/check-silent-revert.py --base "$(BASE_REF)" --head "$(HEAD_REF)"

# ── Go workspace (ADR-0702) ─────────────────────────────────────────────────
#
# go-build:     (after node-bpf) compile all Go packages in the workspace (no output binary in the
#               foundation PR; cmd/ binaries are added by per-sweep PRs).
# go-test:      run `go test ./...` (covers pkg/version and future packages).
# go-fix:       apply authoritative Go modernizations via `go fix ./...`.
# go-fix-check: verify clean tree via `go fix -diff ./...` (fails if rewrites available).
#
# All targets require the Go toolchain declared by go.mod. If `go` is absent,
# they fail with an actionable message rather than "command not found".

# node-bpf:     generate the vmafx-node eBPF object and its bpf2go binding
#               (ADR-1622). The object is not committed; go-build and go-test
#               run it first, so the node embeds it and its tests run. Needs clang with the BPF
#               target, llvm-strip and the libbpf headers; it fails naming the
#               missing tool. `make node-bpf BPF_PIN=--require-pin` refuses a
#               clang other than BPF_CLANG_VERSION (build-config.env).
BPF_PIN ?=
node-bpf:
	@scripts/dev/gen-node-bpf.sh $(BPF_PIN)

go-build: node-bpf
	@command -v go >/dev/null || { echo "go not found — install the version declared by go.mod (https://go.dev/dl/)"; exit 1; }
	CGO_LDFLAGS="-L$(CURDIR)/core/build-cpu/src -lvmaf -lvmafx -lm" \
	LD_LIBRARY_PATH="$(CURDIR)/core/build-cpu/src$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" \
	go build ./...

go-test: node-bpf
	@command -v go >/dev/null || { echo "go not found — install the version declared by go.mod (https://go.dev/dl/)"; exit 1; }
	CGO_LDFLAGS="-L$(CURDIR)/core/build-cpu/src -lvmaf -lvmafx -lm" \
	LD_LIBRARY_PATH="$(CURDIR)/core/build-cpu/src$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" \
	go test ./...

go-fix:
	@command -v go >/dev/null || { echo "go not found — install the version declared by go.mod (https://go.dev/dl/)"; exit 1; }
	go fix ./...

go-fix-check:
	@command -v go >/dev/null || { echo "go not found — install the version declared by go.mod (https://go.dev/dl/)"; exit 1; }
	go fix -diff ./...

# go-ort-runner: build the ONNX Runtime subprocess that pkg/ai.Registry.Infer
#                execs (cmd/vmafx-ort-runner, ADR-1134) to ./vmafx-ort-runner.
#                It is a cgo binary over pkg/libvmaf, so core/build-cpu (or an
#                installed libvmaf) must exist first; a *working* runner needs
#                that libvmaf built with -Denable_dnn=enabled, otherwise every
#                call exits 3. See docs/usage/vmafx-ort-runner.md.

go-ort-runner:
	@command -v go >/dev/null || { echo "go not found — install the version declared by go.mod (https://go.dev/dl/)"; exit 1; }
	CGO_LDFLAGS="-L$(CURDIR)/core/build-cpu/src -lvmaf -lvmafx -lm" \
	LD_LIBRARY_PATH="$(CURDIR)/core/build-cpu/src$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}" \
	go build -o vmafx-ort-runner ./cmd/vmafx-ort-runner

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
# The CI workflow (.github/workflows/go-ci.yml) calls the same installer before
# `go test ./...` so the operator suite executes for real instead of skipping.

# Optional command-line override; the default lives in build-config.env.
ENVTEST_K8S_VERSION ?=

setup-envtest:
	@ENVTEST_K8S_VERSION="$(ENVTEST_K8S_VERSION)" scripts/ci/setup-envtest.sh install >/dev/null
	@echo 'envtest assets installed; export with: eval "$$(make -s setup-envtest-env)"'

# Print a shell-quoted export after checking the installed tool's exact version.
# This mode never installs a tool; setup-envtest must have completed first.
setup-envtest-env:
	@ENVTEST_K8S_VERSION="$(ENVTEST_K8S_VERSION)" scripts/ci/setup-envtest.sh env

# ── Rust workspace (ADR-0702) ────────────────────────────────────────────────
#
# rust-build: cargo check --all (no members yet; validates the workspace manifest).
# rust-test:  cargo test --all (every workspace crate; CI runs it with --all-features, ADR-1528).
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
	@echo "  make lint             — configured C/C++ + Python, shell, Markdown, Go and docs checks"
	@echo "  make lint-c           — tracked native sources in BUILD_DIR (LINT_JOBS=4; receipts under build)"
	@echo "  make lint-md          — markdownlint-cli2 on changed *.md (MDLINT_SCOPE=all for full tree, ADR-0866)"
	@echo "  make lint-actions     — actionlint on .github/workflows/ against .github/actionlint.yaml"
	@echo "  make format           — clang-format + black + ruff + shfmt (writes)"
	@echo "  make format-check     — same, no writes (CI gate)"
	@echo "  make sec              — semgrep (CERT-C + CWE + fork rules)"
	@echo "  make sbom             — SPDX + CycloneDX SBOMs via syft"
	@echo "  make pr-check         — ADR-0108 deliverables gate (PR=<num> or BODY=<file>)"
	@echo "  make silent-revert-check — ADR-1284: work this merge would remove from BASE"
	@echo "  make test-netflix-golden — D24 gate: 3 Netflix CPU test pairs"
	@echo "  make test-netflix-golden-arm64 — the same gate on an aarch64 cross build under qemu-user (GOLDEN_ARM64_CC=gcc|clang)"
	@echo "  make upstream-parity    — CPU extractors against Netflix/vmaf at the recorded parity head, probe set, in the dev container image (ADR-1487)"
	@echo "  make upstream-parity-full — the same over every fixture, option variant and model"
	@echo "  make test-sanitizers  — ASan + UBSan build + run"
	@echo "  make test-fast        — meson --suite=fast (pre-push gate)"
	@echo "  make coverage         — gcov/gcovr line coverage report"
	@echo "  make coverage-html    — render HTML coverage report"
	@echo "  make coverage-check   — enforce the local floors (37% overall / 85% critical)"
	@echo "  make assertion-density — Power-of-10 rule 5 density check"
	@echo "  make lint-tools       — install ruff/black/mypy into .venv at the pinned versions"
	@echo "  make install-hooks    — wire up pre-commit + pre-push git hooks"
	@echo "  make hiss-coverage    — replay declared HISS evidence fixtures"
	@echo "  make dedupe-check     — reject duplicate implementation families"
	@echo "                          (set VMAFX_NATIVE_HOOKS=1 for native bash; ADR-0924)"
	@echo "  make hooks-install    — legacy alias for install-hooks"
	@echo ""
	@echo "  make go-build         — go build ./... (Go workspace, ADR-0702)"
	@echo "  make node-bpf         — generate the vmafx-node eBPF object (needs clang; ADR-1622)"
	@echo "  make go-test          — go test ./... (Go workspace, ADR-0702)"
	@echo "  make go-fix           — go fix ./... (apply Go modernizations, ADR-1338)"
	@echo "  make go-fix-check     — go fix -diff ./... (check Go modernizations, ADR-1338)"
	@echo "  make go-ort-runner    — build ./vmafx-ort-runner, the ONNX subprocess behind pkg/ai (ADR-1134)"
	@echo "  make rust-build       — cargo check --all (Rust workspace, ADR-0702)"
	@echo "  make rust-test        — cargo test --all (Rust workspace, ADR-0702)"
	@echo "  make setup-envtest    — install kubebuilder envtest binaries for vmafx-operator suite"
	@echo ""
	@echo "Upstream targets: build, test, debug, install, clean, distclean, cythonize"

# cordanaLLM/praetor Governance Targets
.PHONY: verify-all compile-context audit hiss-coverage dedupe-check

verify-all:
	@standardsctl audit && standardsctl compile-context --verify && standardsctl hiss coverage --verify
	@$(MAKE) --no-print-directory dedupe-check

compile-context:
	@standardsctl compile-context

audit:
	@standardsctl audit

hiss-coverage:
	@standardsctl hiss coverage --verify

dedupe-check:
	@standardsctl dedupe scan .

# BEGIN praetor documentation gate
.PHONY: docs-lint docs-figures
verify-all: docs-lint docs-figures
docs-lint:
	@node tools/markdownlint/verify.mjs
docs-figures:
	@node tools/figures/build.mjs check
	@node tools/figures/build.mjs sources
# END praetor documentation gate
