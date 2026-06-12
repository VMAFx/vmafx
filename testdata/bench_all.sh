#!/bin/bash
# shellcheck disable=SC1091  # setvars.sh is Intel-provided, not part of the repo
#
# Per-backend bench across the three canonical fixture sizes
# (576×324, 1080p_5f, 4K BBB 200f). For each fixture we engage CPU,
# CUDA, SYCL, and Vulkan with the **correct** flag set per backend
# (see libvmaf/AGENTS.md §"Backend-engagement foot-guns").
#
# Earlier revisions of this script used `--no_cuda --no_sycl` for CPU
# and the singletons `--no_sycl` / `--no_cuda` for "CUDA" / "SYCL".
# Those flags are *disable*-only — they don't engage a backend. So the
# script silently ran CPU for every row, producing bit-exact pools and
# identical fps across "backends". Fixed in this revision.
set -euo pipefail

# Source the user's preferred oneAPI install if present. Honoured order:
# (1) explicit $VMAF_ONEAPI_SETVARS, (2) the pinned 2025.3 install, (3) legacy.
# Skip silently if none are available.
ONEAPI_CANDIDATES=(
  "${VMAF_ONEAPI_SETVARS:-}"
  /opt/intel/oneapi-2025.3/setvars.sh
  /opt/intel/oneapi/setvars.sh
)
for cand in "${ONEAPI_CANDIDATES[@]}"; do
  if [[ -n "${cand}" && -f "${cand}" ]]; then
    # ADR-0513: setvars.sh references variables that may be unset in the
    # calling environment (e.g. SETVARS_ARGS, ia32).  When the parent
    # shell has `set -u` active, bash aborts immediately on the first
    # such reference inside the sourced script — bypassing the `|| true`
    # guard.  Temporarily disable nounset (-u) while sourcing.
    set +u
    # shellcheck disable=SC1090  # path resolved at runtime
    source "${cand}" >/dev/null 2>&1 || true
    set -u
    break
  fi
done

cd "${VMAF_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || echo /home/kilian/dev/vmaf)}" || exit 1

# Honour $VMAF_BIN if set (e.g. for an out-of-tree build); otherwise default
# to the in-tree fork build at core/build/tools/vmaf. Earlier revisions
# fell back to /usr/local/bin/vmaf, which on most dev hosts is stuck at
# v3.0.0 (predates upstream a44e5e61's motion edge-mirror fix). Bench rows
# captured against that stale binary then drifted ~1e-3 from every fork
# build that has ever existed; see PR #305 for the bisect. Operators who
# really want the system binary should set VMAF_BIN=/usr/local/bin/vmaf
# explicitly.
VMAF="${VMAF_BIN:-core/build/tools/vmaf}"
MODEL=model/vmaf_v0.6.1.json
# ADR-0513: OUTDIR defaults to /tmp/vmaf-bench-$$ so the script works from
# read-only mounts (e.g. the vmaf-dev-mcp container where /workspace is ro).
# Set VMAF_BENCH_OUTDIR to override (e.g. testdata/bbb/results for local
# runs that want persistent per-backend JSON artefacts).
OUTDIR="${VMAF_BENCH_OUTDIR:-/tmp/vmaf-bench-$$}"
mkdir -p "$OUTDIR"

# Fail loudly if the vmaf binary is missing — the MCP `run_benchmark`
# wrapper previously returned `exit_code=1` with empty stdout/stderr
# because `set -euo pipefail` aborted on the first run() invocation
# before any output had flushed (probe finding 2026-05-17 #3).
if [[ ! -x "$VMAF" ]]; then
  echo "ERROR: vmaf binary not found or not executable at: $VMAF" >&2
  echo "  Build first: meson compile -C core/build" >&2
  echo "  Or set VMAF_BIN to an installed vmaf binary." >&2
  exit 2
fi

# `flags` is intentionally space-split into separate argv entries.
# ADR-0513: run() guards vmaf exit codes with || true so that backends
# that are unavailable (Vulkan without an ICD, HIP on a CUDA-only host)
# do not abort the whole harness under `set -euo pipefail`.
run() {
  local name="$1" ref="$2" dis="$3" w="$4" h="$5" bd="$6" flags="$7"
  local out="$OUTDIR/${name}.json"
  local start end ms score vmaf_rc
  echo -n "  $name ... "
  start=$(date +%s%N)
  # shellcheck disable=SC2086  # $flags is an intentionally-split flag list
  "$VMAF" --reference "$ref" --distorted "$dis" \
    --width "$w" --height "$h" --pixel_format 420 --bitdepth "$bd" \
    --model "path=$MODEL" --threads 1 \
    --output "$out" --json -q $flags 2>/dev/null || vmaf_rc=$?
  end=$(date +%s%N)
  ms=$(((end - start) / 1000000))
  if [[ -n "${vmaf_rc:-}" ]]; then
    echo "SKIP (vmaf exited ${vmaf_rc} — backend likely unavailable)  (${ms}ms)"
    return 0
  fi
  if [[ ! -f "$out" ]]; then
    # vmaf returned 0 but did not write the output file — this happens when
    # the requested backend is unavailable and the fallback path also fails
    # to open the output (e.g. Vulkan without ICD: "could not open file").
    echo "SKIP (no output file produced — backend likely unavailable)  (${ms}ms)"
    return 0
  fi
  score=$(python3 -c "import json; print(f'{json.load(open(\"$out\"))[\"pooled_metrics\"][\"vmaf\"][\"mean\"]:.6f}')")
  echo "${score}  (${ms}ms)"
}

compare() {
  local tag="$1"
  python3 - "$tag" "$OUTDIR" <<'PYEOF'
import json, sys
tag = sys.argv[1]
outdir = sys.argv[2]
backends = [
    (f"{tag}_cpu", "CPU"),
    (f"{tag}_cuda", "CUDA"),
    (f"{tag}_sycl", "SYCL"),
    # Vulkan backend removed per ADR-0726; no _vulkan row produced.
]
# Per-backend output key counts diverge — CPU emits 14-15 keys
# (incl. integer_aim / integer_motion3 / integer_adm3); CUDA / SYCL
# emit ~12. A matching key count between two backends is one signal
# that they ran the same code path — useful when verifying that flags
# actually engaged the intended backend.
cpu_scores = None
for key, name in backends:
    try:
        with open(f"{outdir}/{key}.json") as f:
            d = json.load(f)
        scores = [fr["metrics"]["vmaf"] for fr in d["frames"]]
        mean = d["pooled_metrics"]["vmaf"]["mean"]
        nkeys = len(d["frames"][0]["metrics"])
        if cpu_scores is None:
            cpu_scores = scores
            print(f"  {name:8s}: {mean:.6f} (ref, {len(scores)} frames, {nkeys} keys)")
        else:
            diffs = [abs(c-g) for c,g in zip(cpu_scores, scores)]
            mx = max(diffs)
            avg = sum(diffs)/len(diffs)
            bad = [(i,d) for i,d in enumerate(diffs) if d > 0.01]
            st = "PASS" if mx < 0.01 else ("WARN" if mx < 0.1 else "FAIL")
            print(f"  {name:8s}: {mean:.6f}  {st} keys={nkeys} max_diff={mx:.8f} avg_diff={avg:.8f}")
            if bad:
                for i, d in bad[:5]:
                    print(f"      frame {i}: cpu={cpu_scores[i]:.6f} gpu={scores[i]:.6f} diff={d:.6f}")
    except FileNotFoundError:
        print(f"  {name:8s}: SKIP (output not produced — backend likely unavailable)")
    except Exception as e:
        print(f"  {name:8s}: ERROR {e}")
PYEOF
}

# Backend-engagement flag sets (see libvmaf/AGENTS.md §"Backend-engagement
# foot-guns"). The `--backend $name` exclusive selector is the canonical
# CLI surface as of 2026-04-28; the explicit-flag form below is kept
# because it works even on libvmaf builds without the selector.
#
#   CPU:    --no_cuda --no_sycl --no_vulkan
#   CUDA:   --gpumask=0 --no_sycl --no_vulkan
#   SYCL:   --sycl_device=0 --no_cuda --no_vulkan
#
# Verifying engagement after a run: the JSON's `frames[0].metrics`
# key set differs per backend (CPU 14-15, CUDA 11-12, SYCL ~15,
# Vulkan ~34). Same key-count + same pool across two rows is a
# strong signal that both ran the same code path.
FLAGS_CPU="--no_cuda --no_sycl --no_vulkan"
FLAGS_CUDA="--gpumask=0 --no_sycl --no_vulkan"
FLAGS_SYCL="--sycl_device=0 --no_cuda --no_vulkan"
# FLAGS_VULKAN removed per ADR-0726

run_test() {
  local tag="$1" ref="$2" dis="$3" w="$4" h="$5" bd="$6"
  run "${tag}_cpu" "$ref" "$dis" "$w" "$h" "$bd" "$FLAGS_CPU"
  run "${tag}_cuda" "$ref" "$dis" "$w" "$h" "$bd" "$FLAGS_CUDA"
  run "${tag}_sycl" "$ref" "$dis" "$w" "$h" "$bd" "$FLAGS_SYCL"
  # Vulkan backend removed per ADR-0726; run_test no longer invokes it.
}

echo "========================================="
echo "Test 1: Official 576x324 (48 frames, 8-bit)"
echo "========================================="
REF=python/test/resource/yuv/src01_hrc00_576x324.yuv
DIS=python/test/resource/yuv/src01_hrc01_576x324.yuv
run_test t1 "$REF" "$DIS" 576 324 8
echo "Comparison:"
compare t1

echo ""
echo "========================================="
echo "Test 2: 1080p (5 frames, 8-bit)"
echo "========================================="
REF=python/test/resource/yuv/src01_hrc00_1920x1080_5frames.yuv
DIS=python/test/resource/yuv/src01_hrc01_1920x1080_5frames.yuv
run_test t2 "$REF" "$DIS" 1920 1080 8
echo "Comparison:"
compare t2

echo ""
echo "========================================="
echo "Test 3: BBB 4K (200 frames, 8-bit)"
echo "========================================="
REF=testdata/bbb/ref_3840x2160_200f.yuv
DIS=testdata/bbb/dis_3840x2160_200f.yuv
run_test t3 "$REF" "$DIS" 3840 2160 8
echo "Comparison:"
compare t3

echo ""
echo "========================================="
echo "DONE"
echo "========================================="
