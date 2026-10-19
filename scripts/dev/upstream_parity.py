#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Upstream parity guard: this tree's CPU extractors against Netflix/vmaf.

Code inherited from Netflix/vmaf evaluates as Netflix's source does; a
difference is allowed only by ADR (ADR-1487). This script checks it. It builds
Netflix/vmaf at the recorded parity head (the heading in
``docs/development/known-upstream-bugs.md``, read through
``scripts/ci/upstream_parity_pin.py``) and this tree with the golden build
profile (``scripts/ci/setup-golden-build.sh``), runs both through the C API
with one harness (``upstream_parity_harness.c``), and compares every emitted
value at ``%.17g``, on the scalar path and on the default dispatch.

Every difference must be covered by a fragment under
``scripts/ci/upstream_parity.d/`` (``scripts/ci/upstream_parity_allowlist.py``).
The run fails on a difference no fragment covers, on a difference larger than
its fragment's bound, on a fragment nothing is attributed to any more (stale),
and on a crash of this tree's harness.

Both trees are built and run in one pinned environment, the dev container
image: Netflix's own values depend on the compiler and the C library (its
``powf(x, 2)`` in ``ciede.c`` is one example), so a comparison made elsewhere
is not evidence. ``--container`` starts the guard in that image; outside it the
guard refuses to measure unless ``--unpinned`` marks the result as advisory.
``--heap-check`` runs every request of both trees a second time with the heap
filled (``MALLOC_PERTURB_``): an output of this tree that changes fails the
guard, and an upstream output that changes is undefined, so no fragment may
bound it by a finite number.

    scripts/dev/upstream_parity.py --container                 # probe set (make upstream-parity)
    scripts/dev/upstream_parity.py --container --mode full --heap-check
    scripts/dev/upstream_parity.py --from-json up.json fork.json   # compare only

Exit status: 0 parity holds; 1 it does not; 2 the comparison could not run
(no pin, no fixtures, a failed build, outside the pinned environment). A run
that could not compare is never reported as passing.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import hashlib
import io
import json
import math
import os
import platform
import shutil
import sys
import tarfile
import time
from collections.abc import Callable, Iterable, Mapping, Sequence
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from scripts.ci import upstream_parity_allowlist as allowlist  # noqa: E402
from scripts.ci import upstream_parity_pin as pin_reader  # noqa: E402
from scripts.dev import upstream_parity_matrix as matrix_data  # noqa: E402
from scripts.lib.safe_subprocess import CommandTimedOut  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

UPSTREAM_URL = "https://github.com/Netflix/vmaf.git"
HARNESS_SOURCE = Path(__file__).with_name("upstream_parity_harness.c")
CONSUMER_SOURCE = Path(__file__).with_name("compat_abi_consumer.c")
PLANT_SOURCE = Path(__file__).with_name("compat_abi_plant.c")
DEFAULT_WORKDIR = "build-upstream-parity"
# The pinned environment: the dev container image (dev/docker-compose.yml).
DEFAULT_IMAGE = "vmaf-dev-mcp:local"
IMAGE_VARIABLE = "UPSTREAM_PARITY_IMAGE"
CONTAINER_MARKERS = (Path("/.dockerenv"), Path("/run/.containerenv"))
UNPINNED_REFUSAL = (
    "not in the pinned environment: run the guard with --container (make upstream-parity), "
    "or pass --unpinned for an advisory result that is not evidence"
)
HEAP_FILL = 170  # MALLOC_PERTURB_ byte of the heap check's second run
CONTAINER_FLAG = "--container"
NETFLIX_DIR = REPO / "python" / "test" / "resource" / "yuv"
BBB_DIR = REPO / "testdata" / "bbb"
# cpumask values of both trees on x86: 63 masks every SIMD flag, 48 masks
# AVX-512 and leaves AVX2, 0 masks nothing.
CPUMASK = {"scalar": 63, "avx2": 48, "default": 0}
MODE_DISPATCH = {"probe": ("scalar", "default"), "full": ("scalar", "avx2", "default")}
EXIT_PASS = 0
EXIT_DIFFERENT = 1
EXIT_CANNOT_RUN = 2
BUILD_TIMEOUT_SECONDS = 3600.0
RUN_TIMEOUT_SECONDS = 1800.0
CONTAINER_TIMEOUT_SECONDS = 4 * 3600.0
MAX_RUN_OUTPUT_BYTES = 64 * 1_048_576
REPORT_LIMIT = 25
SCHEMA = 2
ONE_BYTE_DEPTH = 8  # samples of more bits are stored in two bytes
POOL_FIELDS = 4  # pool <name> <mean> <harmonic mean>
VALUE_FIELDS = 3  # <frame> <name> <value>, agg <name> <value>


class CannotRun(RuntimeError):
    """The comparison could not be made; the guard exits 2."""


@dataclasses.dataclass(frozen=True)
class Tree:
    """One side of the comparison, built and ready to run."""

    label: str
    commit: str
    harness: Path
    model_dir: Path
    vmaf: Path
    environment: Mapping[str, object] = dataclasses.field(default_factory=dict)
    run_env: Mapping[str, str] = dataclasses.field(default_factory=dict)


@dataclasses.dataclass(frozen=True)
class HeapCheck:
    """Outputs that changed when each tree ran again with its heap filled.

    Entries are ``(run key, value key)``; a run that ended differently is
    ``(run key, "*")``.
    """

    fill: int
    upstream: frozenset[tuple[str, str]]
    fork: frozenset[tuple[str, str]]


@dataclasses.dataclass
class Totals:
    """Counts of one comparison."""

    runs: int = 0
    both_ok: int = 0
    both_failed: int = 0
    values: int = 0
    identical: int = 0
    derived: int = 0
    fork_crashes: list[str] = dataclasses.field(default_factory=list)


# --- fixtures ---------------------------------------------------------------


def _plane_sizes(w: int, h: int, pix: str) -> tuple[tuple[int, int], ...]:
    """(width, height) of each plane as a raw file stores it (chroma rounded up)."""

    if pix == "400":
        return ((w, h),)
    ss_hor = pix != "444"
    ss_ver = pix == "420"
    chroma = ((w + ss_hor) >> ss_hor, (h + ss_ver) >> ss_ver)
    return ((w, h), chroma, chroma)


def _sample_bytes(bpc: int) -> int:
    return 2 if bpc > ONE_BYTE_DEPTH else 1


def _frame_bytes(w: int, h: int, pix: str, bpc: int) -> int:
    return sum(pw * ph for pw, ph in _plane_sizes(w, h, pix)) * _sample_bytes(bpc)


def _read_frames(path: Path, frame_bytes: int, frames: int) -> list[bytes]:
    data = path.read_bytes()[: frame_bytes * frames]
    if len(data) != frame_bytes * frames:
        raise CannotRun(f"{path} holds fewer than {frames} frames of {frame_bytes} bytes")
    return [data[i * frame_bytes : (i + 1) * frame_bytes] for i in range(frames)]


def _crop_plane(plane: bytes, w: int, sample: int, crop_w: int, crop_h: int) -> bytes:
    row = w * sample
    return b"".join(plane[y * row : y * row + crop_w * sample] for y in range(crop_h))


def _crop_frame(frame: bytes, src: matrix_data.Fixture, dst: matrix_data.Fixture) -> bytes:
    """The top-left *dst* crop of one *src* frame, plane by plane."""

    sample = _sample_bytes(src.bpc)
    out = []
    offset = 0
    src_planes = _plane_sizes(src.w, src.h, src.pix)
    dst_planes = _plane_sizes(dst.w, dst.h, dst.pix)
    for (sw, sh), (dw, dh) in zip(src_planes, dst_planes, strict=True):
        plane = frame[offset : offset + sw * sh * sample]
        out.append(_crop_plane(plane, sw, sample, dw, dh))
        offset += sw * sh * sample
    return b"".join(out)


def _double_columns(row: bytes, sample: int) -> bytes:
    out = bytearray(2 * len(row))
    for byte in range(sample):
        out[byte :: 2 * sample] = row[byte::sample]
        out[sample + byte :: 2 * sample] = row[byte::sample]
    return bytes(out)


def _upsample_plane(plane: bytes, w: int, h: int, sample: int) -> bytes:
    """A half-size chroma plane repeated to twice the width and height."""

    rows = []
    for y in range(h):
        row = _double_columns(plane[y * w * sample : (y + 1) * w * sample], sample)
        rows.append(row)
        rows.append(row)
    return b"".join(rows)


def _to444_frame(frame: bytes, src: matrix_data.Fixture) -> bytes:
    sample = _sample_bytes(src.bpc)
    (lw, lh), (cw, ch), _ = _plane_sizes(src.w, src.h, src.pix)
    luma = lw * lh * sample
    chroma = cw * ch * sample
    planes = [frame[:luma]]
    for index in range(2):
        plane = frame[luma + index * chroma : luma + (index + 1) * chroma]
        planes.append(_upsample_plane(plane, cw, ch, sample))
    return b"".join(planes)


def _noise_frame(fixture: matrix_data.Fixture, side: str, index: int) -> bytes:
    """Full-range noise from SHAKE-256, the same bytes on every host."""

    count = _frame_bytes(fixture.w, fixture.h, fixture.pix, fixture.bpc)
    seed = f"vmafx upstream parity {fixture.name} {side} {index}".encode()
    data = hashlib.shake_256(seed).digest(count)
    unused_bits = 8 * _sample_bytes(fixture.bpc) - fixture.bpc
    if not unused_bits:
        return data
    out = bytearray(data)
    mask = 0xFF >> unused_bits
    out[1::2] = bytes(byte & mask for byte in data[1::2])
    return bytes(out)


def _to422_frame(frame: bytes, src: matrix_data.Fixture) -> bytes:
    """4:2:0 to 4:2:2: every chroma row twice."""

    sample = _sample_bytes(src.bpc)
    (lw, lh), (cw, ch), _ = _plane_sizes(src.w, src.h, src.pix)
    luma = lw * lh * sample
    row = cw * sample
    planes = [frame[:luma]]
    for index in range(2):
        start = luma + index * row * ch
        rows = [frame[start + y * row : start + (y + 1) * row] for y in range(ch)]
        planes.append(b"".join(line + line for line in rows))
    return b"".join(planes)


def _widen_frame(frame: bytes, bpc: int) -> bytes:
    """8-bit samples as little-endian samples of *bpc* bits (value << (bpc - 8))."""

    shift = bpc - ONE_BYTE_DEPTH
    out = bytearray(2 * len(frame))
    low = bytes((byte << shift) & 0xFF for byte in range(256))
    high = bytes(byte >> (ONE_BYTE_DEPTH - shift) for byte in range(256))
    out[0::2] = frame.translate(low)
    out[1::2] = frame.translate(high)
    return bytes(out)


def _derived_frames(fixture: matrix_data.Fixture, side: str, fixtures_dir: Path) -> list[bytes]:
    recipe = fixture.recipe[0]
    if recipe == "noise":
        return [_noise_frame(fixture, side, index) for index in range(fixture.frames)]
    source = matrix_data.fixture_by_name(fixture.recipe[1])
    source_path = fixture_paths(source, fixtures_dir)[0 if side == "ref" else 1]
    frames = _read_frames(
        source_path, _frame_bytes(source.w, source.h, source.pix, source.bpc), fixture.frames
    )
    transforms: dict[str, Callable[[bytes], bytes]] = {
        "crop420": lambda frame: _crop_frame(frame, source, fixture),
        "crop444": lambda frame: _crop_frame(frame, source, fixture),
        "to444": lambda frame: _to444_frame(frame, source),
        "to422": lambda frame: _to422_frame(frame, source),
        "to400": lambda frame: frame[: source.w * source.h],
        "widen": lambda frame: _widen_frame(frame, fixture.bpc),
    }
    if recipe not in transforms:
        raise CannotRun(f"{fixture.name}: unknown recipe {recipe!r}")
    return [transforms[recipe](frame) for frame in frames]


def fixture_paths(fixture: matrix_data.Fixture, fixtures_dir: Path) -> tuple[Path, Path]:
    """The reference and distorted file of *fixture*."""

    base = {matrix_data.NETFLIX: NETFLIX_DIR, matrix_data.BBB: BBB_DIR}.get(
        fixture.source, fixtures_dir
    )
    return base / fixture.ref, base / fixture.dis


def derive(fixture: matrix_data.Fixture, fixtures_dir: Path) -> None:
    """Write a derived fixture's two files unless they exist with the right size."""

    size = _frame_bytes(fixture.w, fixture.h, fixture.pix, fixture.bpc) * fixture.frames
    for side, path in zip(("ref", "dis"), fixture_paths(fixture, fixtures_dir), strict=True):
        if path.is_file() and path.stat().st_size == size:
            continue
        data = b"".join(_derived_frames(fixture, side, fixtures_dir))
        if len(data) != size:
            raise CannotRun(f"{fixture.name}: derived {len(data)} bytes, expected {size}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def available_fixtures(
    names: Iterable[str], fixtures_dir: Path
) -> tuple[list[str], dict[str, str]]:
    """Split *names* into usable fixtures and skipped ones with their reason.

    The three Netflix golden pairs are required: without them nothing is
    compared. Every other file is optional, and a fixture derived from a
    missing one is skipped with it. Derived fixtures are written on the way.
    """

    wanted = set(names)
    needed = set(wanted)
    for fixture in reversed(matrix_data.FIXTURES):
        if fixture.name in needed and len(fixture.recipe) > 1:
            needed.add(fixture.recipe[1])
    present: set[str] = set()
    skipped: dict[str, str] = {}
    for fixture in matrix_data.FIXTURES:
        if fixture.name not in needed:
            continue
        reason = _prepare(fixture, fixtures_dir, present)
        if reason is None:
            present.add(fixture.name)
        elif fixture.name in wanted:
            skipped[fixture.name] = reason
    usable = [fixture.name for fixture in matrix_data.FIXTURES if fixture.name in present & wanted]
    return usable, skipped


def _prepare(fixture: matrix_data.Fixture, fixtures_dir: Path, present: set[str]) -> str | None:
    """Make *fixture* available; return why it is not, or None when it is."""

    if len(fixture.recipe) > 1 and fixture.recipe[1] not in present:
        return f"its source {fixture.recipe[1]} is not installed"
    if fixture.source == matrix_data.DERIVED:
        derive(fixture, fixtures_dir)
    missing = [path for path in fixture_paths(fixture, fixtures_dir) if not path.is_file()]
    if not missing:
        return None
    if fixture.required:
        raise CannotRun(
            f"fixture {fixture.name}: {missing[0]} is missing; run scripts/test/fetch-test-yuvs.sh"
        )
    return f"{missing[0].name} is not installed"


# --- environment ------------------------------------------------------------


def _tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise CannotRun(f"{name} is not installed")
    return path


def container_image(
    environ: Mapping[str, str] | None = None, markers: Sequence[Path] = CONTAINER_MARKERS
) -> str:
    """The id of the image ``--container`` started the guard in, or "" elsewhere."""

    image = (os.environ if environ is None else environ).get(IMAGE_VARIABLE, "")
    return image if image and any(marker.exists() for marker in markers) else ""


def environment_dir(workdir: Path, image: str) -> Path:
    """Builds and run cache of one environment; the host's never mix with an image's."""

    return workdir / (f"image-{image.removeprefix('sha256:')[:12]}" if image else "host")


def _version(compiler: str) -> str:
    path = _tool(compiler)
    result = run_command(
        [path, "--version"],
        allowed_executables=(path,),
        capture_output=True,
        text=True,
        errors="replace",
    )
    lines = str(result.stdout).splitlines()
    return lines[0].strip() if result.returncode == 0 and lines else f"{compiler} (no version)"


def describe_environment(
    compilers: tuple[str, str], image: str, abi: bool = False
) -> dict[str, object]:
    """What a result document was measured in. Two documents compare only if it is equal."""

    libc = " ".join(part for part in platform.libc_ver() if part) or "unknown"
    return {
        "pinned": bool(image),
        "image": image or "none",
        "cc": _version(compilers[0]),
        "cxx": _version(compilers[1]),
        "libc": libc,
        "machine": platform.machine(),
        "abi": abi,
    }


def environment_line(environment: Mapping[str, object]) -> str:
    """The report's first line: where both documents were measured."""

    where = (
        f"image {environment.get('image')}"
        if environment.get("pinned")
        else "NOT PINNED (advisory, not evidence)"
    )
    mode_tag = " [ABI drop-in]" if environment.get("abi") else ""
    details = "; ".join(str(environment.get(key)) for key in ("cc", "libc", "machine"))
    return f"environment{mode_tag}: {where}; {details}"


def check_environments(
    upstream: Mapping[str, object], fork: Mapping[str, object], allow_unpinned: bool
) -> Mapping[str, object]:
    """The environment both documents were measured in; refuse any other pair."""

    measured = upstream.get("environment")
    if not isinstance(measured, dict) or measured != fork.get("environment"):
        raise CannotRun("the two documents were not measured in one environment")
    if not measured.get("pinned") and not allow_unpinned:
        raise CannotRun(UNPINNED_REFUSAL)
    return measured


def mount_roots(*paths: Path) -> list[Path]:
    """The fewest directories that hold every path (a worktree and its common git dir)."""

    roots: list[Path] = []
    for path in sorted({path.resolve() for path in paths}, key=lambda item: len(item.parts)):
        if not any(path.is_relative_to(root) for root in roots):
            roots.append(path)
    return roots


def inner_arguments(argv: Sequence[str]) -> list[str]:
    """The guard's arguments inside the container: no ``--container``, no fetch."""

    inner: list[str] = []
    skip = False
    for index, word in enumerate(argv):
        if skip:
            skip = False
            continue
        if word == CONTAINER_FLAG:
            following = argv[index + 1] if index + 1 < len(argv) else "-"
            skip = not following.startswith("-")
            continue
        if not word.startswith(f"{CONTAINER_FLAG}="):
            inner.append(word)
    return inner if "--no-fetch" in inner else [*inner, "--no-fetch"]


def container_command(
    docker: str, image: str, image_id: str, inner: Sequence[str], mounts: Sequence[Path]
) -> list[str]:
    """``docker run`` of the guard in *image*: no network, the caller's user, same paths."""

    argv = [docker, "run", "--rm", "--init", "--pull", "never", "--network", "none"]
    argv += ["--memory=8g", "--cpus=2"]
    argv += ["--user", f"{os.getuid()}:{os.getgid()}", "--entrypoint", "python3"]
    argv += ["--workdir", str(REPO), "--env", f"{IMAGE_VARIABLE}={image_id}"]
    argv += ["--env", "HOME=/tmp", "--env", "PYTHONDONTWRITEBYTECODE=1"]
    for root in mounts:
        argv += ["--volume", f"{root}:{root}"]
    return [*argv, image, "scripts/dev/upstream_parity.py", *inner]


def _captured(argv: Sequence[str], what: str) -> str:
    result = run_command(argv, allowed_executables=(argv[0],), capture_output=True, text=True)
    if result.returncode != 0:
        raise CannotRun(f"{what} failed ({result.returncode}): {str(result.stderr).strip()}")
    return str(result.stdout).strip()


def run_in_container(image: str, argv: Sequence[str], fetch: bool) -> int:
    """Run the guard with *argv* in *image*; its exit status is the guard's."""

    docker = _tool("docker")
    image_id = _captured([docker, "image", "inspect", "--format", "{{.Id}}", image], image)
    resolve_pin(fetch=fetch)  # the container has no network
    git = _tool("git")
    common = _captured(
        [git, "-C", str(REPO), "rev-parse", "--path-format=absolute", "--git-common-dir"],
        "git rev-parse",
    )
    mounts = mount_roots(REPO, Path(common).parent)
    command = container_command(docker, image, image_id, inner_arguments(argv), mounts)
    result = run_command(
        command, allowed_executables=(docker,), timeout_seconds=CONTAINER_TIMEOUT_SECONDS
    )
    if result.returncode in (EXIT_PASS, EXIT_DIFFERENT, EXIT_CANNOT_RUN):
        return result.returncode
    raise CannotRun(f"docker run {image} ended with {result.returncode}")


# --- builds -----------------------------------------------------------------


def _checked(
    argv: Sequence[str | Path], *, cwd: Path, log: Path, env: Mapping[str, str] | None = None
) -> None:
    """Run a build step with its output in *log*; a failure cannot be compared."""

    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("ab") as handle:
        result = run_command(
            [str(arg) for arg in argv],
            allowed_executables=(str(argv[0]),),
            cwd=cwd,
            env=dict(env) if env is not None else None,
            stdout=handle,
            stderr_to_stdout=True,
            timeout_seconds=BUILD_TIMEOUT_SECONDS,
        )
    if result.returncode != 0:
        raise CannotRun(f"{Path(str(argv[0])).name} failed ({result.returncode}); see {log}")


def resolve_pin(repo: Path = REPO, fetch: bool = True) -> str:
    """The full id of the recorded parity head, fetched from Netflix/vmaf."""

    try:
        pin = pin_reader.recorded_pin((repo / pin_reader.RECORD).read_text(encoding="utf-8"))
        runner = pin_reader.git_runner(repo)
        if fetch:
            status, _ = runner(["fetch", "--no-tags", "--quiet", UPSTREAM_URL, "master"])
            if status != 0:
                raise CannotRun(f"git fetch {UPSTREAM_URL} master failed")
        return pin_reader.resolve(pin, "FETCH_HEAD", runner)
    except (OSError, pin_reader.PinError) as error:
        raise CannotRun(f"upstream parity pin: {error}") from error


def resolve_ref(ref: str, repo: Path = REPO) -> str:
    """The full id of *ref* (a tag, branch or commit of Netflix/vmaf), fetched."""

    runner = pin_reader.git_runner(repo)
    status, _ = runner(["fetch", "--no-tags", "--quiet", UPSTREAM_URL, ref])
    if status != 0:
        raise CannotRun(f"git fetch {UPSTREAM_URL} {ref} failed")
    status, commit = runner(["rev-parse", "--verify", "--quiet", "FETCH_HEAD^{commit}"])
    if status != 0:
        raise CannotRun(f"{ref} does not name a commit of {UPSTREAM_URL}")
    return commit


def export_upstream(commit: str, destination: Path, repo: Path = REPO) -> None:
    """Unpack ``libvmaf/`` and ``model/`` of *commit* into *destination*."""

    if (destination / "libvmaf" / "meson.build").is_file():
        return
    git = _tool("git")
    result = run_command(
        [git, "-C", str(repo), "archive", "--format=tar", commit, "libvmaf", "model"],
        allowed_executables=(git,),
        capture_output=True,
        timeout_seconds=BUILD_TIMEOUT_SECONDS,
        max_output_bytes=1 << 30,
    )
    if result.returncode != 0:
        raise CannotRun(f"git archive {commit} failed")
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(bytes(result.stdout))) as archive:
        archive.extractall(destination, filter="data")


def golden_compilers(build: Path) -> tuple[str, str]:
    """The C and C++ compiler of a configured golden build: (cc, cxx).

    Both trees and both harnesses are compiled with them, so that the
    comparison is of two sources, not of two compilers.
    """

    info = build / "meson-info" / "intro-compilers.json"
    try:
        host = json.loads(info.read_text(encoding="utf-8"))["host"]
        return str(host["c"]["exelist"][0]), str(host["cpp"]["exelist"][0])
    except (OSError, ValueError, KeyError, IndexError) as error:
        raise CannotRun(f"{info}: cannot read the build's compilers: {error}") from error


def build_harness(tree_source: Path, build: Path, output: Path, log: Path, cc: str) -> None:
    """Compile the harness against one tree's headers and static library.

    The harness needs the tree's feature collector. A tree with the internal
    accessor ``vmaf_feature_collector_get()`` is asked for it; the other is
    linked with ``--wrap`` (see the harness source for why not both).
    """

    library = build / "src" / "libvmaf.a"
    if not library.is_file():
        raise CannotRun(f"{library} was not built")
    output.parent.mkdir(parents=True, exist_ok=True)
    includes = (
        tree_source / "include",
        build / "include",
        tree_source / "src",
        build / "src",
        build,
    )
    private = tree_source / "src" / "libvmaf_priv.h"
    accessor = private.is_file() and "vmaf_feature_collector_get" in private.read_text(
        encoding="utf-8"
    )
    hook = "-DPARITY_COLLECTOR_ACCESSOR" if accessor else "-Wl,--wrap=vmaf_feature_collector_init"
    argv: list[str | Path] = [_tool(cc), "-O1", "-std=gnu11", "-o", output, HARNESS_SOURCE]
    argv += [f"-I{path}" for path in includes]
    argv += [hook, library, "-lm", "-lpthread", "-lstdc++"]
    _checked(argv, cwd=REPO, log=log)


def upstream_baseline_args() -> list[str]:
    """Meson arguments that hold the upstream build to the pinned image's baseline ISA.

    Netflix's build leaves floating-point contraction at the compiler's default, so a
    host compiler that defaults to x86-64-v3 (Ubuntu's amd64v3 variant) fuses its
    multiply-adds and moves its scores; the fork turns contraction off (ADR-1461).
    The pinned image's GCC defaults to x86-64; the SIMD libraries keep their own flags.
    """

    if platform.machine() not in ("x86_64", "AMD64"):
        return []
    return ["-Dc_args=-march=x86-64", "-Dcpp_args=-march=x86-64"]


def build_upstream(commit: str, workdir: Path, jobs: int, compilers: tuple[str, str]) -> Tree:
    """Export, configure and build Netflix/vmaf at *commit* (CPU only)."""

    cc, cxx = compilers
    root = workdir / f"upstream-{commit[:12]}"
    build = root / "build"
    logs = workdir / "logs"
    export_upstream(commit, root)
    if not (build / "build.ninja").is_file():
        setup: list[str | Path] = [_tool("meson"), "setup", build, root / "libvmaf"]
        setup += ["--buildtype", "release", "-Denable_float=true", "-Denable_cuda=false"]
        setup += ["-Denable_docs=false", "-Denable_tests=false", "-Db_lto=false"]
        setup += upstream_baseline_args()
        env = dict(os.environ, CC=cc, CXX=cxx)
        _checked(setup, cwd=REPO, log=logs / "upstream-setup.log", env=env)
    if golden_compilers(build)[0] != cc:
        raise CannotRun(f"{build} was configured with another compiler than {cc}; remove it")
    ninja: list[str | Path] = [_tool("ninja"), "-C", build, "-j", str(jobs)]
    _checked([*ninja, "src/libvmaf.a", "tools/vmaf"], cwd=REPO, log=logs / "upstream-ninja.log")
    harness = workdir / "bin" / f"harness-upstream-{commit[:12]}"
    build_harness(root / "libvmaf", build, harness, logs / "upstream-harness.log", cc)
    return Tree("upstream", commit, harness, root / "model", build / "tools" / "vmaf")


def build_fork(build: Path, workdir: Path, jobs: int) -> Tree:
    """Build this tree with the golden profile and compile its harness."""

    logs = workdir / "logs"
    env = dict(os.environ, GOLDEN_NINJA_JOBS=str(jobs))
    script = REPO / "scripts" / "ci" / "setup-golden-build.sh"
    setup: list[str | Path] = [_tool("bash"), script, build, REPO / "core"]
    _checked(setup, cwd=REPO, log=logs / "fork-setup.log", env=env)
    ninja: list[str | Path] = [_tool("ninja"), "-C", build, "-j", str(jobs), "src/libvmaf.a"]
    _checked(ninja, cwd=REPO, log=logs / "fork-ninja.log")
    harness = workdir / "bin" / "harness-fork"
    build_harness(
        REPO / "core", build, harness, logs / "fork-harness.log", golden_compilers(build)[0]
    )
    git = _tool("git")
    head = run_command(
        [git, "-C", str(REPO), "rev-parse", "HEAD"],
        allowed_executables=(git,),
        capture_output=True,
        text=True,
    )
    return Tree("fork", head.stdout.strip(), harness, REPO / "model", build / "tools" / "vmaf")


def check_soname(library: Path, expected_soname: str) -> None:
    """Assert that *library* has *expected_soname* in its dynamic section."""

    readelf = _tool("readelf")
    result = run_command(
        [readelf, "-d", str(library)],
        allowed_executables=(readelf,),
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise CannotRun(f"readelf -d {library} failed ({result.returncode}): {result.stderr}")
    pattern = f"Library soname: [{expected_soname}]"
    if pattern not in result.stdout:
        raise CannotRun(f"{library}: missing expected SONAME {expected_soname}")


def check_needed(library: Path, expected_needed: str) -> None:
    """Assert that *library* declares a DT_NEEDED dependency on *expected_needed*."""

    readelf = _tool("readelf")
    result = run_command(
        [readelf, "-d", str(library)],
        allowed_executables=(readelf,),
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise CannotRun(f"readelf -d {library} failed ({result.returncode}): {result.stderr}")
    pattern = f"Shared library: [{expected_needed}]"
    if pattern not in result.stdout:
        raise CannotRun(f"{library}: missing expected DT_NEEDED dependency on {expected_needed}")


def assert_loaded_library(
    consumer: Path, lib_dir: Path, expected_so: str, extra_so: str | None = None
) -> None:
    """Assert via ldd that *consumer* under LD_LIBRARY_PATH resolves *expected_so* to *lib_dir*."""

    ldd = _tool("ldd")
    env = dict(os.environ, LD_LIBRARY_PATH=str(lib_dir))
    result = run_command(
        [ldd, str(consumer)],
        allowed_executables=(ldd,),
        env=env,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise CannotRun(f"ldd {consumer} failed ({result.returncode}): {result.stderr}")

    def _resolve(so_name: str) -> Path:
        for raw_line in result.stdout.splitlines():
            line = raw_line.strip()
            if line.startswith(f"{so_name} =>"):
                parts = line.split("=>", 1)[1].strip().split("(", 1)
                return Path(parts[0].strip()).resolve()
        raise CannotRun(f"{consumer}: {so_name} not found in ldd output:\n{result.stdout}")

    resolved = _resolve(expected_so)
    target = (lib_dir / expected_so).resolve()
    if resolved != target:
        raise CannotRun(f"{consumer}: {expected_so} resolved to {resolved}, expected {target}")
    print(f"abi runtime libvmaf [{lib_dir.name}]: {resolved}", file=sys.stderr)

    if extra_so:
        extra_resolved = _resolve(extra_so)
        extra_target = (lib_dir / extra_so).resolve()
        if extra_resolved != extra_target:
            raise CannotRun(
                f"{consumer}: {extra_so} resolved to {extra_resolved}, expected {extra_target}"
            )
        print(f"abi runtime libvmafx [{lib_dir.name}]: {extra_resolved}", file=sys.stderr)


def build_compat_consumer(
    upstream_source: Path,
    upstream_build: Path,
    output: Path,
    log: Path,
    cc: str,
) -> None:
    """Compile the unmodified upstream consumer binary against upstream headers and libvmaf.so."""

    output.parent.mkdir(parents=True, exist_ok=True)
    includes = (
        upstream_source / "include",
        upstream_build / "include",
    )
    argv: list[str | Path] = [
        _tool(cc),
        "-O1",
        "-std=gnu11",
        "-o",
        output,
        CONSUMER_SOURCE,
    ]
    argv += [f"-I{path}" for path in includes]
    argv += [f"-L{upstream_build / 'src'}", "-lvmaf", "-lm", "-lpthread"]
    _checked(argv, cwd=REPO, log=log)


def build_compat_plant(
    output: Path,
    log: Path,
    cc: str,
    upstream_source: Path,
    upstream_build: Path,
) -> None:
    """Compile the LD_PRELOAD plant shim."""

    output.parent.mkdir(parents=True, exist_ok=True)
    includes = (
        upstream_source / "include",
        upstream_build / "include",
    )
    argv: list[str | Path] = [
        _tool(cc),
        "-shared",
        "-fPIC",
        "-O2",
        "-D_GNU_SOURCE",
        "-o",
        output,
        PLANT_SOURCE,
    ]
    argv += [f"-I{path}" for path in includes]
    argv += ["-ldl", "-lm"]
    _checked(argv, cwd=REPO, log=log)


def build_upstream_abi(
    commit: str, workdir: Path, jobs: int, compilers: tuple[str, str]
) -> tuple[Path, Path, Path]:
    """Export, configure and build Netflix/vmaf at *commit* as a shared library."""

    cc, cxx = compilers
    root = workdir / f"upstream-{commit[:12]}"
    build = root / "build-abi"
    logs = workdir / "logs"
    export_upstream(commit, root)
    if not (build / "build.ninja").is_file():
        setup: list[str | Path] = [_tool("meson"), "setup", build, root / "libvmaf"]
        setup += ["--buildtype", "release", "-Denable_float=true", "-Denable_cuda=false"]
        setup += [
            "-Denable_docs=false",
            "-Denable_tests=false",
            "-Db_lto=false",
            "-Ddefault_library=shared",
            *upstream_baseline_args(),
        ]
        env = dict(os.environ, CC=cc, CXX=cxx)
        _checked(setup, cwd=REPO, log=logs / "upstream-setup-abi.log", env=env)
    if golden_compilers(build)[0] != cc:
        raise CannotRun(f"{build} was configured with another compiler than {cc}; remove it")
    ninja: list[str | Path] = [_tool("ninja"), "-C", build, "-j", str(jobs)]
    _checked([*ninja, "tools/vmaf"], cwd=REPO, log=logs / "upstream-ninja-abi.log")
    so_path = build / "src" / "libvmaf.so"
    check_soname(so_path, "libvmaf.so.3")
    return root, build, so_path


def build_fork_abi(build: Path, workdir: Path, jobs: int) -> tuple[Path, Path]:
    """Build this tree with the golden profile and shared library."""

    logs = workdir / "logs"
    env = dict(os.environ, GOLDEN_NINJA_JOBS=str(jobs))
    script = REPO / "scripts" / "ci" / "setup-golden-build.sh"
    setup: list[str | Path] = [
        _tool("bash"),
        script,
        build,
        REPO / "core",
    ]
    _checked(setup, cwd=REPO, log=logs / "fork-setup-abi.log", env=env)
    vmaf_so = build / "src" / "libvmaf.so"
    vmafx_so = build / "src" / "libvmafx.so"
    check_soname(vmaf_so, "libvmaf.so.3")
    check_soname(vmafx_so, "libvmafx.so.1")
    check_needed(vmaf_so, "libvmafx.so.1")
    return vmaf_so, vmafx_so


def build_trees_abi(
    commit: str,
    fork_build: Path,
    workdir: Path,
    jobs: int,
    image: str = "",
    plant: bool = False,
) -> tuple[Tree, Tree]:
    """Build fork and upstream shared libraries and the compat ABI consumer."""
    logs = workdir / "logs"
    build_fork_abi(fork_build, workdir, jobs)
    compilers = golden_compilers(fork_build)
    upstream_root, upstream_build, _ = build_upstream_abi(commit, workdir, jobs, compilers)
    consumer = workdir / "bin" / "compat-abi-consumer"
    build_compat_consumer(
        upstream_root / "libvmaf",
        upstream_build,
        consumer,
        logs / "compat-consumer.log",
        compilers[0],
    )
    assert_loaded_library(consumer, upstream_build / "src", "libvmaf.so.3")
    assert_loaded_library(consumer, fork_build / "src", "libvmaf.so.3", "libvmafx.so.1")

    up_env = {"LD_LIBRARY_PATH": str(upstream_build / "src")}
    fork_env: dict[str, str] = {"LD_LIBRARY_PATH": str(fork_build / "src")}
    if plant:
        plant_so = workdir / "lib" / "compat_abi_plant.so"
        build_compat_plant(
            plant_so,
            logs / "compat-plant.log",
            compilers[0],
            upstream_root / "libvmaf",
            upstream_build,
        )
        fork_env["LD_PRELOAD"] = str(plant_so)

    env = describe_environment(compilers, image, abi=True)
    git = _tool("git")
    head = run_command(
        [git, "-C", str(REPO), "rev-parse", "HEAD"],
        allowed_executables=(git,),
        capture_output=True,
        text=True,
    ).stdout.strip()

    up_so = upstream_build / "src" / "libvmaf.so"
    up_tree = Tree("upstream", commit, consumer, upstream_root / "model", up_so, env, up_env)
    fork_so = fork_build / "src" / "libvmaf.so"
    fork_tree = Tree("fork", head, consumer, REPO / "model", fork_so, env, fork_env)
    return up_tree, fork_tree


def build_trees(
    commit: str,
    fork_build: Path,
    workdir: Path,
    jobs: int,
    image: str = "",
    abi: bool = False,
    plant: bool = False,
) -> tuple[Tree, Tree]:
    """Build this tree, then upstream at *commit* with the same compilers.

    *workdir* is the environment's directory (``environment_dir()``); *image*
    is the pinned image's id, or "" for an advisory run.
    """

    if abi:
        return build_trees_abi(commit, fork_build, workdir, jobs, image, plant)
    fork_tree = build_fork(fork_build, workdir, jobs)
    compilers = golden_compilers(fork_build)
    upstream_tree = build_upstream(commit, workdir, jobs, compilers)
    environment = describe_environment(compilers, image, abi=False)
    return (
        dataclasses.replace(upstream_tree, environment=environment),
        dataclasses.replace(fork_tree, environment=environment),
    )


def default_fork_build(environment_workdir: Path, abi: bool = False) -> Path:
    """The golden-profile build of this tree in one environment's directory."""

    return environment_workdir / ("fork-golden-abi" if abi else "fork-golden")


# --- runs -------------------------------------------------------------------


def parse_output(text: str, returncode: int) -> dict[str, object]:
    """One harness run as ``{"status", "detail", "values", "pooled"}``."""

    values: dict[str, str] = {}
    pooled: dict[str, str] = {}
    problems: list[str] = []
    ended = False
    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "END":
            ended = True
        elif fields[0] == "pool" and len(fields) == POOL_FIELDS:
            pooled[f"mean|{fields[1]}"] = fields[2]
            pooled[f"harmonic|{fields[1]}"] = fields[3]
        elif fields[0] == "agg" and len(fields) == VALUE_FIELDS:
            values[f"agg|{fields[1]}"] = fields[2]
        elif fields[0].isdigit() and len(fields) == VALUE_FIELDS:
            values[f"{fields[0]}|{fields[1]}"] = fields[2]
        else:
            problems.append(line)
    if returncode < 0:
        return {"status": "crash", "detail": f"signal {-returncode}", "values": {}, "pooled": {}}
    if returncode != 0 or problems or not ended:
        detail = problems[0] if problems else f"exit {returncode}"
        return {"status": "error", "detail": detail, "values": {}, "pooled": {}}
    return {"status": "ok", "detail": "", "values": values, "pooled": pooled}


def _request_digest(argv: Sequence[str], inputs: Sequence[Path]) -> str:
    """Identifies a cached run: the request and the size of the files it reads."""

    parts = [*argv[1:], *(str(path.stat().st_size) for path in inputs)]
    return hashlib.sha256("\0".join(parts).encode()).hexdigest()


def run_environment(
    heap_fill: int,
    caller: Mapping[str, str] = os.environ,
    extra_env: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """The harness's environment: the caller's, with extra variables and heap fill."""

    source = dict(caller)
    if extra_env:
        source.update(extra_env)
    env = {key: value for key, value in source.items() if key != "MALLOC_PERTURB_"}
    if heap_fill:
        env["MALLOC_PERTURB_"] = str(heap_fill)
    return env


def _run_one(
    tree: Tree,
    run: matrix_data.Run,
    dispatch: str,
    fixtures_dir: Path,
    cache: Path,
    env: Mapping[str, str],
) -> dict[str, object]:
    fixture = matrix_data.fixture_by_name(run.fixture)
    ref, dis = fixture_paths(fixture, fixtures_dir)
    argv = [str(tree.harness), str(ref), str(dis), str(fixture.w), str(fixture.h), fixture.pix]
    argv += [str(fixture.bpc), str(CPUMASK[dispatch]), str(fixture.frames)]
    argv.append(run.spec.replace("{model}", str(tree.model_dir)))
    digest = _request_digest(argv, (ref, dis))
    out = cache / dispatch / run.fixture / f"{run.name}.json"
    if out.is_file():
        cached = dict(json.loads(out.read_text(encoding="utf-8")))
        if cached.pop("request", None) == digest:
            return cached
    try:
        result = run_command(
            argv,
            allowed_executables=(str(tree.harness),),
            env=env,
            capture_output=True,
            text=True,
            errors="replace",
            timeout_seconds=RUN_TIMEOUT_SECONDS,
            max_output_bytes=MAX_RUN_OUTPUT_BYTES,
        )
        parsed = parse_output(result.stdout, result.returncode)
    except CommandTimedOut:
        parsed = {"status": "crash", "detail": "timeout", "values": {}, "pooled": {}}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({**parsed, "request": digest}), encoding="utf-8")
    return parsed


def run_key(dispatch: str, fixture: str, name: str) -> str:
    """The document key of one run."""

    return f"{dispatch}/{fixture}/{name}"


def run_tree(
    tree: Tree,
    runs: Sequence[matrix_data.Run],
    dispatches: Sequence[str],
    fixtures_dir: Path,
    workdir: Path,
    jobs: int,
    heap_fill: int = 0,
) -> dict[str, object]:
    """Run *runs* on *tree* at every dispatch; return its result document.

    The run cache is keyed by the harness binary, the environment and the
    heap fill, so a cached result is never one of another build or image.
    """

    identity = tree.harness.read_bytes() + json.dumps(
        {
            "environment": tree.environment,
            "heap_fill": heap_fill,
            "run_env": dict(tree.run_env),
        },
        sort_keys=True,
    ).encode("utf-8")
    cache = workdir / "runs" / f"{tree.label}-{hashlib.sha256(identity).hexdigest()[:16]}"
    env = run_environment(heap_fill, extra_env=tree.run_env)
    work = [(dispatch, run) for dispatch in dispatches for run in runs]
    results: dict[str, object] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        outputs = pool.map(
            lambda item: _run_one(tree, item[1], item[0], fixtures_dir, cache, env), work
        )
        for (dispatch, run), parsed in zip(work, outputs, strict=True):
            results[run_key(dispatch, run.fixture, run.name)] = parsed
    return {
        "schema": SCHEMA,
        "tree": tree.label,
        "commit": tree.commit,
        "environment": dict(tree.environment),
        "heap_fill": heap_fill,
        "runs": results,
    }


def unstable_outputs(
    first: Mapping[str, object], second: Mapping[str, object]
) -> frozenset[tuple[str, str]]:
    """Every output of one tree that differs between two of its documents.

    ``(run key, value key)``, pooled values as ``pool-<method>|<metric>``; a
    run whose status differs is ``(run key, "*")`` as a whole.
    """

    runs_a, runs_b = dict(first["runs"]), dict(second["runs"])  # type: ignore[call-overload]
    unstable: set[tuple[str, str]] = set()
    for key in sorted(runs_a.keys() & runs_b.keys()):
        run_a, run_b = runs_a[key], runs_b[key]
        if run_a["status"] != run_b["status"]:
            unstable.add((key, "*"))
            continue
        for section, prefix in (("values", ""), ("pooled", "pool-")):
            outputs_a, outputs_b = dict(run_a[section]), dict(run_b[section])
            for name in sorted(outputs_a.keys() | outputs_b.keys()):
                if value_difference(outputs_a.get(name, ""), outputs_b.get(name, "")) is not None:
                    unstable.add((key, prefix + name))
    return frozenset(unstable)


# --- comparison ---------------------------------------------------------------


def value_difference(upstream: str, fork: str) -> float | None:
    """None when the two printed values are the same number, else the size.

    Equal text is equal. Two NaNs are equal whatever their sign. A zero of the
    other sign is a difference of size 0, and a value that is not finite on
    one side is a difference of infinite size.
    """

    if upstream == fork:
        return None
    try:
        a, b = float(upstream), float(fork)
    except ValueError:
        return math.inf
    if math.isnan(a) and math.isnan(b):
        return None
    if math.isfinite(a) and math.isfinite(b):
        return abs(a - b)
    return math.inf


def _split_key(key: str) -> tuple[str, str]:
    frame, _, metric = key.partition("|")
    return frame, metric


def _compare_values(
    place: tuple[str, str, str],
    upstream: Mapping[str, str],
    fork: Mapping[str, str],
    totals: Totals,
) -> list[allowlist.Difference]:
    differences: list[allowlist.Difference] = []
    for key in sorted(upstream.keys() & fork.keys()):
        totals.values += 1
        size = value_difference(upstream[key], fork[key])
        if size is None:
            totals.identical += 1
            continue
        frame, metric = _split_key(key)
        differences.append(
            allowlist.Difference(*place, "value", metric, frame, upstream[key], fork[key], size)
        )
    return differences


def _compare_names(
    place: tuple[str, str, str], upstream: Mapping[str, str], fork: Mapping[str, str]
) -> list[allowlist.Difference]:
    names_up = {_split_key(key)[1] for key in upstream}
    names_fork = {_split_key(key)[1] for key in fork}
    only_up = [
        allowlist.Difference(*place, "name", metric, "*", "emitted", "", 0.0)
        for metric in sorted(names_up - names_fork)
    ]
    only_fork = [
        allowlist.Difference(*place, "name", metric, "*", "", "emitted", 0.0)
        for metric in sorted(names_fork - names_up)
    ]
    return only_up + only_fork


def _compare_pooled(
    place: tuple[str, str, str],
    upstream: Mapping[str, str],
    fork: Mapping[str, str],
    frame_differences: Sequence[allowlist.Difference],
    totals: Totals,
) -> list[allowlist.Difference]:
    """Pooled values: a difference is its own finding only when the frames agree.

    A pooled value follows from the per-frame values. Where those differ the
    pooled difference is derived and counted, not bounded; where they are
    identical a pooled difference is a difference in the pooling arithmetic.
    """

    moved = {difference.metric for difference in frame_differences}
    differences: list[allowlist.Difference] = []
    for key in sorted(upstream.keys() & fork.keys()):
        totals.values += 1
        size = value_difference(upstream[key], fork[key])
        if size is None:
            totals.identical += 1
            continue
        method, metric = _split_key(key)
        if metric in moved:
            totals.derived += 1
            continue
        differences.append(
            allowlist.Difference(
                *place, "value", metric, f"pool-{method}", upstream[key], fork[key], size
            )
        )
    return differences


def _compare_run(
    place: tuple[str, str, str],
    upstream: Mapping[str, object],
    fork: Mapping[str, object],
    totals: Totals,
) -> list[allowlist.Difference]:
    status_up, status_fork = str(upstream["status"]), str(fork["status"])
    if status_fork == "crash":
        totals.fork_crashes.append(f"{'/'.join(place)} ({fork.get('detail', '')})")
    if status_up != status_fork:
        return [allowlist.Difference(*place, "status", "", "", status_up, status_fork, 0.0)]
    if status_up != "ok":
        totals.both_failed += 1
        return []
    totals.both_ok += 1
    values_up, values_fork = dict(upstream["values"]), dict(fork["values"])  # type: ignore[call-overload]
    differences = _compare_values(place, values_up, values_fork, totals)
    pooled = _compare_pooled(
        place,
        dict(upstream["pooled"]),  # type: ignore[call-overload]
        dict(fork["pooled"]),  # type: ignore[call-overload]
        differences,
        totals,
    )
    return differences + _compare_names(place, values_up, values_fork) + pooled


def compare_documents(
    upstream: Mapping[str, object], fork: Mapping[str, object]
) -> tuple[list[allowlist.Difference], list[tuple[str, str, str]], Totals]:
    """Every difference between two result documents, and what was compared."""

    runs_up, runs_fork = dict(upstream["runs"]), dict(fork["runs"])  # type: ignore[call-overload]
    if runs_up.keys() != runs_fork.keys():
        odd = sorted(runs_up.keys() ^ runs_fork.keys())
        raise CannotRun(f"the two documents hold different runs, e.g. {odd[0]} ({len(odd)} in all)")
    totals = Totals()
    differences: list[allowlist.Difference] = []
    executed: list[tuple[str, str, str]] = []
    for key in sorted(runs_up):
        dispatch, fixture, name = key.split("/", 2)
        place = (dispatch, fixture, name)
        executed.append(place)
        totals.runs += 1
        differences += _compare_run(place, runs_up[key], runs_fork[key], totals)
    return differences, executed, totals


# --- report -------------------------------------------------------------------


def _describe(difference: allowlist.Difference) -> str:
    if difference.what == "status":
        return f"{difference.where()}: upstream {difference.upstream}, fork {difference.fork}"
    if difference.what == "name":
        emitter = "fork" if difference.fork else "upstream"
        return f"{difference.where()}: only {emitter} emits it"
    return (
        f"{difference.where()}: upstream {difference.upstream}, fork {difference.fork} "
        f"(|diff| {difference.size:.3g})"
    )


def _listing(title: str, lines: Sequence[str]) -> list[str]:
    if not lines:
        return []
    shown = [f"  {line}" for line in lines[:REPORT_LIMIT]]
    if len(lines) > REPORT_LIMIT:
        shown.append(f"  ... and {len(lines) - REPORT_LIMIT} more")
    return [f"{title} ({len(lines)}):", *shown]


def _covered_lines(
    result: allowlist.Classification, fragments: Sequence[allowlist.Fragment]
) -> list[str]:
    lines = []
    for fragment in fragments:
        items = result.covered[fragment.name]
        if not items:
            continue
        largest = max(item.size for item in items)
        bound = "" if fragment.bound is None else f", bound {fragment.bound:g}"
        cite = ", ".join(fragment.adrs) or f"branch {fragment.branch}"
        lines.append(
            f"  {fragment.name} [{fragment.kind}] {len(items)} differences, "
            f"max {largest:.3g}{bound} ({cite})"
        )
    return lines


def report(
    result: allowlist.Classification,
    fragments: Sequence[allowlist.Fragment],
    totals: Totals,
    skipped: Mapping[str, str],
    complete: bool = True,
) -> list[str]:
    """The lines of the guard's report.

    In a filtered run set (*complete* false) a fragment without a difference
    is listed as not judged, not as stale.
    """

    covered = sum(len(items) for items in result.covered.values())
    lines = [
        f"runs compared: {totals.runs} (both ran: {totals.both_ok}, both failed: {totals.both_failed}, "
        f"ended differently: {totals.runs - totals.both_ok - totals.both_failed})",
        f"values compared: {totals.values}, identical: {totals.identical}, "
        f"pooled values that follow a per-frame difference: {totals.derived}",
        f"differences covered by a fragment: {covered}",
        *_covered_lines(result, fragments),
        f"differences not covered: {len(result.uncovered)}, above their fragment's bound: "
        f"{len(result.exceeded)}, stale fragments: "
        + (str(len(result.stale)) if complete else "not judged (filtered run set)"),
    ]
    lines += [f"fixture not compared: {name}: {reason}" for name, reason in sorted(skipped.items())]
    lines += _listing("NOT COVERED", [_describe(item) for item in result.uncovered])
    lines += _listing(
        "ABOVE BOUND",
        [
            f"{_describe(item)}; {fragment.name} allows {fragment.bound:g}"
            for item, fragment in result.exceeded
        ],
    )
    stale_title = (
        "STALE (nothing is attributed to it any more: remove the fragment)"
        if complete
        else "fragments without a difference in this slice of the matrix (not judged)"
    )
    lines += _listing(stale_title, [fragment.name for fragment in result.stale])
    lines += _listing(
        "fragments with no run in scope (not judged)",
        [fragment.name for fragment in result.unexercised],
    )
    lines += _listing("FORK HARNESS CRASHED", totals.fork_crashes)
    return lines


def summarise_uncovered(differences: Iterable[allowlist.Difference]) -> list[str]:
    """Uncovered differences grouped by run, metric and fixture, largest first."""

    groups: dict[tuple[str, str, str, str], list[allowlist.Difference]] = {}
    for difference in differences:
        key = (
            difference.run,
            difference.metric or difference.what,
            difference.fixture,
            difference.dispatch,
        )
        groups.setdefault(key, []).append(difference)
    lines = []
    for (run, metric, fixture, dispatch), items in sorted(groups.items()):
        largest = max(items, key=lambda item: item.size)
        lines.append(
            f"{run} {metric} {fixture} {dispatch}: {len(items)} [{largest.what}] "
            f"max {largest.size:.3g} (upstream {largest.upstream}, fork {largest.fork})"
        )
    return lines


# --- command line ---------------------------------------------------------------


def _is_unstable(item: allowlist.Difference, unstable: frozenset[tuple[str, str]]) -> bool:
    key = run_key(item.dispatch, item.fixture, item.run)
    return (key, "*") in unstable or (key, f"{item.frame}|{item.metric}") in unstable


def heap_findings(
    result: allowlist.Classification,
    fragments: Sequence[allowlist.Fragment],
    heap: HeapCheck | None,
) -> tuple[list[str], bool]:
    """The heap check's report lines, and whether it fails the guard.

    An output of this tree that changes with the heap fill is a defect of
    this tree. An upstream output that changes is undefined: a fragment may
    cover it, but only without a finite bound.
    """

    if heap is None:
        return [], False
    bounds = {fragment.name: fragment.bound for fragment in fragments}
    finite = [
        f"{_describe(item)}; {name} has bound {bound:g}"
        for name, items in result.covered.items()
        if (bound := bounds.get(name)) is not None and math.isfinite(bound)
        for item in items
        if _is_unstable(item, heap.upstream)
    ]
    runs = {key for key, _ in heap.upstream}
    lines = [
        f"heap check (MALLOC_PERTURB_={heap.fill}): upstream outputs that depend on heap "
        f"contents: {len(heap.upstream)} in {len(runs)} runs; this tree: {len(heap.fork)}"
    ]
    lines += _listing(
        "THIS TREE'S OUTPUT DEPENDS ON HEAP CONTENTS",
        [f"{key} {name}" for key, name in sorted(heap.fork)],
    )
    lines += _listing(
        "FINITE BOUND OVER AN UNDEFINED UPSTREAM VALUE (the bound must be inf, and say why)",
        finite,
    )
    return lines, bool(heap.fork or finite)


def judge(
    upstream: Mapping[str, object],
    fork: Mapping[str, object],
    fragments: Sequence[allowlist.Fragment],
    skipped: Mapping[str, str],
    summary: bool = False,
    complete: bool = True,
    heap: HeapCheck | None = None,
    allow_unpinned: bool = False,
) -> tuple[int, list[str]]:
    """Compare two documents against the allowlist: (exit status, report).

    *complete* says the documents hold a whole probe or full matrix. A
    filtered run set cannot tell a stale fragment from one whose witnesses
    were not run, so staleness is reported there but does not fail. Documents
    from another environment than the pinned one are refused unless
    *allow_unpinned*; their verdict is then marked advisory.
    """

    environment = check_environments(upstream, fork, allow_unpinned)
    differences, executed, totals = compare_documents(upstream, fork)
    result = allowlist.classify(differences, fragments, executed)
    lines = [environment_line(environment), *report(result, fragments, totals, skipped, complete)]
    heap_lines, heap_failed = heap_findings(result, fragments, heap)
    lines += heap_lines
    if summary:
        loose = [*result.uncovered, *(item for item, _ in result.exceeded)]
        lines += _listing("uncovered, grouped", summarise_uncovered(loose))
    stale = bool(result.stale) and complete
    failed = result.uncovered or result.exceeded or stale or totals.fork_crashes or heap_failed
    advisory = "" if environment.get("pinned") else " (advisory: environment not pinned)"
    lines.append(f"upstream parity: {'FAIL' if failed else 'PASS'}{advisory}")
    return (EXIT_DIFFERENT if failed else EXIT_PASS), lines


def _load(path: Path) -> dict[str, object]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise CannotRun(f"{path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != SCHEMA or "runs" not in document:
        raise CannotRun(f"{path}: not a result document of schema {SCHEMA}")
    return document


def _selection(
    args: argparse.Namespace, fixtures_dir: Path
) -> tuple[list[matrix_data.Run], list[str], dict[str, str]]:
    """The runs and dispatches the arguments ask for, and the fixtures skipped."""

    dispatches = [
        name for name in MODE_DISPATCH[args.mode] if not args.dispatch or name in args.dispatch
    ]
    if platform.machine() not in ("x86_64", "AMD64"):
        dispatches = [name for name in dispatches if name != "avx2"]
    runs = [
        run
        for run in matrix_data.matrix(args.mode)
        if not args.only or any(text in run.name for text in args.only)
    ]
    usable, skipped = available_fixtures({run.fixture for run in runs}, fixtures_dir)
    runs = [run for run in runs if run.fixture in usable]
    if not runs or not dispatches:
        raise CannotRun("nothing to run")
    return runs, dispatches, skipped


def measure(
    args: argparse.Namespace,
) -> tuple[dict[str, object], dict[str, object], dict[str, str], HeapCheck | None]:
    """Build both trees, run the matrix of the requested mode, return both documents."""

    image = container_image()
    if not image and not args.unpinned:
        raise CannotRun(UNPINNED_REFUSAL)
    workdir = (REPO / args.workdir).resolve()
    fixtures_dir = workdir / "fixtures"
    runs, dispatches, skipped = _selection(args, fixtures_dir)
    envdir = environment_dir(workdir, image)
    abi = bool(args.abi or args.plant)
    fork_build = (
        (REPO / args.fork_build).resolve()
        if args.fork_build
        else default_fork_build(envdir, abi=abi)
    )
    trees = build_trees(
        resolve_pin(fetch=not args.no_fetch),
        fork_build,
        envdir,
        args.jobs,
        image,
        abi=abi,
        plant=args.plant,
    )
    started = time.monotonic()
    upstream, fork = (
        run_tree(tree, runs, dispatches, fixtures_dir, envdir, args.jobs) for tree in trees
    )
    heap = None
    if args.heap_check:
        refilled = [
            run_tree(tree, runs, dispatches, fixtures_dir, envdir, args.jobs, HEAP_FILL)
            for tree in trees
        ]
        heap = HeapCheck(
            HEAP_FILL, unstable_outputs(upstream, refilled[0]), unstable_outputs(fork, refilled[1])
        )
    print(
        f"upstream {trees[0].commit[:12]}, fork {trees[1].commit[:12]}, mode {args.mode}, "
        f"dispatch {', '.join(dispatches)}: {len(runs) * len(dispatches)} runs per tree"
        f"{' (twice: heap check)' if heap else ''} in {time.monotonic() - started:.0f} s"
    )
    return upstream, fork, skipped, heap


def _write_documents(
    directory: Path,
    upstream: Mapping[str, object],
    fork: Mapping[str, object],
    heap: HeapCheck | None,
) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name, document in (("upstream.json", upstream), ("fork.json", fork)):
        (directory / name).write_text(json.dumps(document), encoding="utf-8")
    if heap is not None:
        record = {
            "fill": heap.fill,
            "upstream": sorted(heap.upstream),
            "fork": sorted(heap.fork),
        }
        (directory / "heap.json").write_text(json.dumps(record), encoding="utf-8")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n", 1)[0])
    parser.add_argument("--mode", choices=sorted(MODE_DISPATCH), default="probe")
    parser.add_argument(
        "--container",
        nargs="?",
        const=DEFAULT_IMAGE,
        metavar="IMAGE",
        help=f"run in the pinned environment, this image (default {DEFAULT_IMAGE})",
    )
    parser.add_argument(
        "--unpinned",
        action="store_true",
        help="measure outside the pinned environment; the result is advisory, not evidence",
    )
    parser.add_argument(
        "--heap-check",
        action="store_true",
        help=f"run every request again with MALLOC_PERTURB_={HEAP_FILL} and judge what changes",
    )
    parser.add_argument("--workdir", default=DEFAULT_WORKDIR, help="builds, fixtures and run cache")
    parser.add_argument(
        "--fork-build", help="golden-profile build directory (default: in the workdir)"
    )
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument(
        "--dispatch", action="append", choices=sorted(CPUMASK), help="limit the dispatches"
    )
    parser.add_argument(
        "--only", action="append", help="limit the runs to names containing this text"
    )
    parser.add_argument(
        "--no-fetch", action="store_true", help="use the FETCH_HEAD already present"
    )
    parser.add_argument("--fragments", type=Path, default=allowlist.FRAGMENTS_DIR)
    parser.add_argument("--adr-dir", type=Path, default=allowlist.ADR_DIR)
    parser.add_argument("--from-json", nargs=2, type=Path, metavar=("UPSTREAM", "FORK"))
    parser.add_argument("--write-json", type=Path, metavar="DIR", help="save both result documents")
    parser.add_argument("--summary", action="store_true", help="group what is not covered")
    parser.add_argument(
        "--abi",
        action="store_true",
        help="run compat ABI drop-in parity verification via unmodified upstream consumer",
    )
    parser.add_argument(
        "--plant",
        action="store_true",
        help="inject LD_PRELOAD plant into fork run to verify parity failure detection",
    )
    parsed = parser.parse_args(argv)
    if parsed.plant:
        parsed.abi = True
    return parsed


def _compare(args: argparse.Namespace) -> tuple[int, list[str]]:
    fragments = allowlist.load_fragments(args.fragments, args.adr_dir)
    heap = None
    if args.from_json:
        upstream, fork = _load(args.from_json[0]), _load(args.from_json[1])
        skipped: dict[str, str] = {}
    else:
        upstream, fork, skipped, heap = measure(args)
    if args.write_json:
        _write_documents(args.write_json, upstream, fork, heap)
    complete = not (args.only or args.dispatch)
    return judge(upstream, fork, fragments, skipped, args.summary, complete, heap, args.unpinned)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    args = parse_args(arguments)
    try:
        if args.container and (args.from_json or args.unpinned):
            raise CannotRun("--container measures; it takes neither --from-json nor --unpinned")
        if args.container:
            return run_in_container(args.container, arguments, fetch=not args.no_fetch)
        status, lines = _compare(args)
    except (CannotRun, allowlist.AllowlistError) as error:
        print(f"upstream parity: could not compare: {error}", file=sys.stderr)
        return EXIT_CANNOT_RUN
    print("\n".join(lines))
    return status


if __name__ == "__main__":
    raise SystemExit(main())
