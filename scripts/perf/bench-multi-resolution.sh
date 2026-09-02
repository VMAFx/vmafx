#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
#
# bench-multi-resolution.sh — multi-resolution, multi-backend, multi-metric
# throughput benchmark for VMAF (ADR-0752).
#
# Outputs a versioned JSON at --output (default: testdata/perf_multi_resolution.json).
# Each cell is a (resolution × backend × metric) triple timed over --runs runs;
# the median wall time is kept.
#
# Usage:
#   scripts/perf/bench-multi-resolution.sh [OPTIONS]
#
# Options:
#   --backends  cpu,cuda,sycl          Comma-separated list (default: cpu)
#   --resolutions 576,720,1080,1440,2160  Comma-separated heights (default: all)
#   --metrics vif,adm,motion,ssim,ms_ssim  Comma-separated (default: all)
#   --runs N   Number of timing runs per cell (default: 3)
#   --ncu      Collect ncu --metrics for CUDA cells (requires ncu on PATH)
#   --output   Path for JSON output
#   --workspace Root of the vmaf tree (default: git toplevel or VMAF_ROOT)
#   --dry-run  Print resolved plan and exit 0
#   --help
#
# Environment:
#   VMAF_BIN          Override vmaf binary path
#   VMAF_ROOT         Override workspace root
#   VMAF_CUDA_HOME    Override CUDA install prefix (fallback: CUDA_HOME, /opt/cuda)
#   VMAF_ONEAPI_SETVARS Override setvars.sh path
#   VMAF_NCU          Override ncu binary path (default: ncu)
#
# Dependencies: bash ≥ 4.3, python3, ffmpeg (for upscaling missing fixtures).
#   CUDA cells: nvcc must be on PATH or CUDA_HOME set.
#   SYCL cells: oneAPI setvars.sh must be sourceable.
#   ncu cells:  Nsight Compute ≥ 2022 (ncu binary).
#
# Fixture strategy:
#   576×324 (48f)  — testdata/{ref,dis}_576x324_48f.yuv   (in-tree)
#   720×480 (48f)  — testdata/{ref,dis}_640x480_48f.yuv   (in-tree, 640×480)
#   1080p   (48f)  — testdata/{ref,dis}_1920x1080_48f.yuv (ffmpeg-upscaled from 576)
#   1440p   (48f)  — testdata/{ref,dis}_2560x1440_48f.yuv (ffmpeg-upscaled from 576)
#   2160p   (30f)  — testdata/bbb/{ref,dis}_3840x2160_200f.yuv trimmed to 30 frames
#
# Upscaling is performed with ffmpeg scale=bilinear on the first call and
# cached in testdata/; subsequent calls reuse the cache.  Upscaled files are
# noted in the JSON metadata as source=upscaled.
#
# Metric-to-feature-flag mapping (vmaf --feature NAME):
#   vif      -> vif
#   adm      -> adm
#   motion   -> motion
#   ssim     -> float_ssim
#   ms_ssim  -> float_ms_ssim
#
# The full VMAF composite score is always collected (required for the vmaf
# binary to run a valid session); individual feature scores are extracted from
# the output JSON.
#
# NCU metrics collected per CUDA cell (requires --ncu):
#   sm__throughput.avg.pct_of_peak_sustained_elapsed
#   l1tex__t_sector_hit_rate.pct
#   lts__t_sector_hit_rate.pct
#   gpu__time_duration.sum
#
set -euo pipefail

# ─── defaults ────────────────────────────────────────────────────────────────
BACKENDS="cpu"
RESOLUTIONS="576,720,1080,1440,2160"
METRICS="vif,adm,motion,ssim,ms_ssim"
RUNS=3
USE_NCU=0
DRY_RUN=0

# ─── parse args ───────────────────────────────────────────────────────────────
usage() {
  sed -n '/^# Usage:/,/^[^#]/p' "$0" | head -n -1 | sed 's/^# \?//'
  exit 0
}

OUTPUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --backends)
      BACKENDS="$2"
      shift 2
      ;;
    --resolutions)
      RESOLUTIONS="$2"
      shift 2
      ;;
    --metrics)
      METRICS="$2"
      shift 2
      ;;
    --runs)
      RUNS="$2"
      shift 2
      ;;
    --ncu)
      USE_NCU=1
      shift
      ;;
    --output)
      OUTPUT="$2"
      shift 2
      ;;
    --workspace)
      VMAF_ROOT="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --help | -h) usage ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

# ─── workspace root ───────────────────────────────────────────────────────────
WORKSPACE="${VMAF_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || echo /workspace)}"

if [[ "$OUTPUT" == "" ]]; then
  OUTPUT="${WORKSPACE}/testdata/perf_multi_resolution.json"
fi

# ─── CUDA detection (no hardcoded paths) ─────────────────────────────────────
CUDA_HOME_RESOLVED="${VMAF_CUDA_HOME:-${CUDA_HOME:-}}"
if [[ -z "${CUDA_HOME_RESOLVED}" ]]; then
  for candidate in /opt/cuda /usr/local/cuda /usr/cuda; do
    if [[ -d "${candidate}" ]]; then
      CUDA_HOME_RESOLVED="${candidate}"
      break
    fi
  done
fi
if [[ -n "${CUDA_HOME_RESOLVED}" && ":${PATH}:" != *":${CUDA_HOME_RESOLVED}/bin:"* ]]; then
  export PATH="${CUDA_HOME_RESOLVED}/bin:${PATH}"
fi

# ─── oneAPI / SYCL setup ──────────────────────────────────────────────────────
# Resolution order, highest priority first:
#   1. VMAF_ONEAPI_SETVARS  — explicit override (CI pin / dev override)
#   2. $ONEAPI_ROOT/setvars.sh — environment-supplied install root
#   3. /opt/intel/oneapi/setvars.sh — canonical un-versioned symlink
#   4. Newest /opt/intel/oneapi-*/setvars.sh in lexical order — last-resort
#      fallback for hosts that only ship the versioned install dir.
# The previous code hardcoded /opt/intel/oneapi-2025.3/setvars.sh, which
# silently stops working on every oneAPI upgrade.
SYCL_AVAILABLE=0
ONEAPI_CANDIDATES=(
  "${VMAF_ONEAPI_SETVARS:-}"
  "${ONEAPI_ROOT:-/opt/intel/oneapi}/setvars.sh"
  /opt/intel/oneapi/setvars.sh
)
# Append any versioned /opt/intel/oneapi-*/setvars.sh in reverse-lexical
# order (newest first). Glob expansion may produce zero matches; suppress
# unmatched-glob errors by checking each path's existence below.
shopt -s nullglob
for versioned in $(printf '%s\n' /opt/intel/oneapi-*/setvars.sh | sort -r); do
  ONEAPI_CANDIDATES+=("${versioned}")
done
shopt -u nullglob
for cand in "${ONEAPI_CANDIDATES[@]}"; do
  if [[ -n "${cand}" && -f "${cand}" ]]; then
    set +u
    # shellcheck disable=SC1090
    source "${cand}" >/dev/null 2>&1 || true
    set -u
    SYCL_AVAILABLE=1
    break
  fi
done
if [[ ${SYCL_AVAILABLE} -eq 0 ]]; then
  echo "  [bench] WARNING: no oneAPI setvars.sh found; SYCL backend will be unavailable." >&2
  echo "  [bench]          Set ONEAPI_ROOT or VMAF_ONEAPI_SETVARS to enable." >&2
fi

# ─── vmaf binary ─────────────────────────────────────────────────────────────
VMAF="${VMAF_BIN:-${WORKSPACE}/core/build/tools/vmaf}"
if [[ ! -x "${VMAF}" ]]; then
  # fallback: container install path
  VMAF_FALLBACK="/usr/local/bin/vmaf"
  if [[ -x "${VMAF_FALLBACK}" ]]; then
    VMAF="${VMAF_FALLBACK}"
  else
    echo "ERROR: vmaf binary not found at '${VMAF}' or '${VMAF_FALLBACK}'" >&2
    echo "  Build: meson setup core/build core -Denable_cuda=true && ninja -C core/build" >&2
    echo "  Or set VMAF_BIN to an installed vmaf binary." >&2
    exit 2
  fi
fi

MODEL="${WORKSPACE}/model/vmaf_v0.6.1.json"
if [[ ! -f "${MODEL}" ]]; then
  echo "ERROR: model not found at ${MODEL}" >&2
  exit 2
fi

NCU_BIN="${VMAF_NCU:-ncu}"

# ─── scratch dir (honour $TMPDIR, trap-cleaned) ──────────────────────────────
# Use $TMPDIR when set (CI runners, containers); fall back to /tmp on hosts
# without it. Everything below mktemps inside SCRATCH_DIR so a single trap
# cleans up — no stale /tmp/vmaf-bench-*.json or /tmp/vmaf-ncu-*.csv leaks.
SCRATCH_DIR="$(mktemp -d "${TMPDIR:-/tmp}/vmaf-bench-XXXXXX")"
LOG_DIR="${SCRATCH_DIR}/logs"
mkdir -p "${LOG_DIR}"
trap 'rm -rf "${SCRATCH_DIR}"' EXIT

# ─── resolution catalogue ─────────────────────────────────────────────────────
# Maps resolution height → (width height bitdepth nframes src_description
#                            ref_path dis_path)
# All paths relative to WORKSPACE.
declare -A RES_W RES_H RES_BD RES_NF RES_SRC RES_REF RES_DIS

_set_res() {
  local key="$1"
  RES_W[$key]="$2"
  RES_H[$key]="$3"
  RES_BD[$key]="$4"
  RES_NF[$key]="$5"
  RES_SRC[$key]="$6"
  RES_REF[$key]="$7"
  RES_DIS[$key]="$8"
}

# 576 — native Netflix golden fixture
_set_res 576 576 324 8 48 native \
  testdata/ref_576x324_48f.yuv \
  testdata/dis_576x324_48f.yuv

# 720 — 640×480 native fixture (nearest 720p-class in testdata)
_set_res 720 640 480 8 48 native \
  testdata/ref_640x480_48f.yuv \
  testdata/dis_640x480_48f.yuv

# 1080 — ffmpeg-upscaled from 576×324 fixture
_set_res 1080 1920 1080 8 48 upscaled \
  testdata/ref_1920x1080_48f.yuv \
  testdata/dis_1920x1080_48f.yuv

# 1440 — ffmpeg-upscaled from 576×324 fixture
_set_res 1440 2560 1440 8 48 upscaled \
  testdata/ref_2560x1440_48f.yuv \
  testdata/dis_2560x1440_48f.yuv

# 2160 — first 30 frames of BBB 4K (trimmed to keep wall time manageable)
_set_res 2160 3840 2160 8 30 bbb_trimmed \
  testdata/bbb/ref_3840x2160_30f.yuv \
  testdata/bbb/dis_3840x2160_30f.yuv

# ─── metric → feature flag ───────────────────────────────────────────────────
declare -A METRIC_FEATURE
METRIC_FEATURE[vif]="vif"
METRIC_FEATURE[adm]="adm"
METRIC_FEATURE[motion]="motion"
METRIC_FEATURE[ssim]="float_ssim"
METRIC_FEATURE[ms_ssim]="float_ms_ssim"

# Key paths to extract per-feature pooled score from JSON
declare -A METRIC_JSON_KEY
METRIC_JSON_KEY[vif]="vif_scale0"
METRIC_JSON_KEY[adm]="adm2"
METRIC_JSON_KEY[motion]="motion"
METRIC_JSON_KEY[ssim]="float_ssim"
METRIC_JSON_KEY[ms_ssim]="float_ms_ssim"

# ─── backend → vmaf CLI flags ─────────────────────────────────────────────────
declare -A BACKEND_FLAGS
BACKEND_FLAGS[cpu]="--no_cuda --no_sycl --no_vulkan"
BACKEND_FLAGS[cuda]="--gpumask=0 --no_sycl --no_vulkan"
BACKEND_FLAGS[sycl]="--sycl_device=0 --no_cuda --no_vulkan"

# ─── helpers ─────────────────────────────────────────────────────────────────
log() { echo "  [bench] $*"; }

die() {
  echo "ERROR: $*" >&2
  exit 1
}

# Trim a YUV to N frames (raw YUV420p 8-bit)
trim_yuv() {
  local src="$1" dst="$2" w="$3" h="$4" nf="$5"
  local frame_bytes
  frame_bytes=$((w * h * 3 / 2))
  local needed=$((frame_bytes * nf))
  if [[ -f "${dst}" ]]; then
    local actual
    actual=$(wc -c <"${dst}")
    if [[ "${actual}" -ge "${needed}" ]]; then
      return 0
    fi
  fi
  log "Trimming ${src} → ${dst} (${nf} frames)"
  dd if="${WORKSPACE}/${src}" of="${WORKSPACE}/${dst}" bs="${frame_bytes}" count="${nf}" status=none
}

# Upscale a YUV420p 8-bit file to a new resolution via ffmpeg
upscale_yuv() {
  local src_path="$1" dst_path="$2" src_w="$3" src_h="$4" dst_w="$5" dst_h="$6" nf="$7"
  if [[ -f "${dst_path}" ]]; then
    return 0
  fi
  log "Upscaling ${src_w}x${src_h} → ${dst_w}x${dst_h} (${nf}f) via ffmpeg bilinear"
  ffmpeg -y -loglevel error \
    -f rawvideo -pix_fmt yuv420p -s "${src_w}x${src_h}" -i "${src_path}" \
    -vf "scale=${dst_w}:${dst_h}:flags=bilinear" \
    -f rawvideo -pix_fmt yuv420p "${dst_path}"
}

# Ensure all fixtures exist, generating them as needed
ensure_fixture() {
  local key="$1"
  local ref_abs="${WORKSPACE}/${RES_REF[$key]}"
  local dis_abs="${WORKSPACE}/${RES_DIS[$key]}"

  case "${RES_SRC[$key]}" in
    native)
      [[ -f "${ref_abs}" ]] || die "Native fixture missing: ${ref_abs}"
      [[ -f "${dis_abs}" ]] || die "Native fixture missing: ${dis_abs}"
      ;;
    upscaled)
      upscale_yuv \
        "${WORKSPACE}/testdata/ref_576x324_48f.yuv" "${ref_abs}" \
        576 324 "${RES_W[$key]}" "${RES_H[$key]}" "${RES_NF[$key]}"
      upscale_yuv \
        "${WORKSPACE}/testdata/dis_576x324_48f.yuv" "${dis_abs}" \
        576 324 "${RES_W[$key]}" "${RES_H[$key]}" "${RES_NF[$key]}"
      ;;
    bbb_trimmed)
      local bbb_ref="${WORKSPACE}/testdata/bbb/ref_3840x2160_200f.yuv"
      local bbb_dis="${WORKSPACE}/testdata/bbb/dis_3840x2160_200f.yuv"
      if [[ ! -f "${bbb_ref}" ]]; then
        echo "SKIP_FIXTURE: 4K BBB source not found at ${bbb_ref}" >&2
        return 1
      fi
      trim_yuv "${bbb_ref}" "${RES_REF[$key]}" \
        3840 2160 "${RES_NF[$key]}"
      trim_yuv "${bbb_dis}" "${RES_DIS[$key]}" \
        3840 2160 "${RES_NF[$key]}"
      ;;
  esac
  return 0
}

# Collect key ncu metrics for one vmaf invocation (CUDA only)
# Returns a JSON fragment or empty string on failure
run_ncu_cell() {
  local ref="$1" dis="$2" w="$3" h="$4" bd="$5" feature_flag="$6" out_json="$7"
  if ! command -v "${NCU_BIN}" &>/dev/null; then
    echo "{\"error\": \"ncu not on PATH\"}"
    return 0
  fi
  local ncu_out ncu_err ncu_error=""
  ncu_out=$(mktemp "${SCRATCH_DIR}/vmaf-ncu-XXXXX.csv")
  ncu_err=$(mktemp "${SCRATCH_DIR}/vmaf-ncu-XXXXX.err")
  # Capture ncu stderr instead of `|| true`-discarding it. ncu permission
  # errors (ERR_NVGPUCTRPERM), missing driver counters, and similar setup
  # failures are otherwise reported as a silent empty CSV downstream.
  # shellcheck disable=SC2086
  if ! "${NCU_BIN}" --csv --metrics \
    "sm__throughput.avg.pct_of_peak_sustained_elapsed,l1tex__t_sector_hit_rate.pct,lts__t_sector_hit_rate.pct,gpu__time_duration.sum" \
    "${VMAF}" \
    --reference "${ref}" --distorted "${dis}" \
    --width "${w}" --height "${h}" \
    --pixel_format 420 --bitdepth "${bd}" \
    --feature "${feature_flag}" \
    --model "path=${MODEL}" --threads 1 \
    --output "${out_json}" --json -q \
    ${BACKEND_FLAGS[cuda]} 2>"${ncu_err}"; then
    if [[ -s "${ncu_err}" ]]; then
      ncu_error="$(head -c 4096 "${ncu_err}")"
    else
      ncu_error="ncu exited non-zero with no stderr"
    fi
  fi
  python3 - "${ncu_out}" "${ncu_error}" <<'PYEOF'
import csv, json, sys
path = sys.argv[1]
ncu_error = sys.argv[2] if len(sys.argv) > 2 else ""
try:
    rows = list(csv.DictReader(open(path)))
    # aggregate: mean sm throughput, mean L1 hit, mean L2 hit, total gpu time
    sm   = [float(r.get("sm__throughput.avg.pct_of_peak_sustained_elapsed","0") or 0) for r in rows]
    l1   = [float(r.get("l1tex__t_sector_hit_rate.pct","0") or 0) for r in rows]
    l2   = [float(r.get("lts__t_sector_hit_rate.pct","0") or 0) for r in rows]
    gpu  = [float(r.get("gpu__time_duration.sum","0") or 0) for r in rows]
    result = {
        "sm_throughput_pct": round(sum(sm)/len(sm),2) if sm else None,
        "l1_hit_rate_pct":   round(sum(l1)/len(l1),2) if l1 else None,
        "l2_hit_rate_pct":   round(sum(l2)/len(l2),2) if l2 else None,
        "gpu_time_us_total": round(sum(gpu)/1000,2)   if gpu else None,
        "kernel_count":      len(rows),
    }
    if ncu_error:
        result["ncu_error"] = ncu_error
    print(json.dumps(result))
except Exception as e:
    err = {"error": str(e)}
    if ncu_error:
        err["ncu_error"] = ncu_error
    print(json.dumps(err))
PYEOF
}

# Run one (resolution × backend × metric) cell, return timing and score JSON
# Outputs to stdout: a JSON object
run_cell() {
  local res_key="$1" backend="$2" metric="$3"
  local w="${RES_W[$res_key]}" h="${RES_H[$res_key]}" bd="${RES_BD[$res_key]}"
  local nf="${RES_NF[$res_key]}"
  local ref="${WORKSPACE}/${RES_REF[$res_key]}"
  local dis="${WORKSPACE}/${RES_DIS[$res_key]}"
  local feature="${METRIC_FEATURE[$metric]}"
  local json_key="${METRIC_JSON_KEY[$metric]}"
  # shellcheck disable=SC2086
  local flags="${BACKEND_FLAGS[$backend]}"
  local out_json
  out_json=$(mktemp "${SCRATCH_DIR}/vmaf-bench-XXXXX.json")

  local times=()
  local vmaf_score feature_score
  vmaf_score="null"
  feature_score="null"

  local run_ok=0
  local skip_reason=""
  local vmaf_error=""

  for ((i = 0; i < RUNS; i++)); do
    local t_start t_end elapsed_ms rc err_file
    err_file="${LOG_DIR}/vmaf-${res_key}-${backend}-${metric}-$(date +%s%N).err"
    t_start=$(date +%s%N)
    # Capture stderr instead of discarding it — without this, OOM, missing
    # model, GPU init failures and the like silently emit null scores and
    # the user has no signal. The per-call err file is read on rc != 0 and
    # surfaced in the cell JSON as "vmaf_error".
    # shellcheck disable=SC2086
    "${VMAF}" \
      --reference "${ref}" --distorted "${dis}" \
      --width "${w}" --height "${h}" \
      --pixel_format 420 --bitdepth "${bd}" \
      --feature "${feature}" \
      --model "path=${MODEL}" --threads 1 \
      --output "${out_json}" --json -q \
      ${flags} 2>"${err_file}"
    rc=$?
    t_end=$(date +%s%N)
    elapsed_ms=$(((t_end - t_start) / 1000000))
    if [[ ${rc} -ne 0 ]]; then
      skip_reason="vmaf exited ${rc}"
      # Read stderr (cap at 4 KiB to keep the JSON manageable; the full log
      # stays on disk for the duration of the run via SCRATCH_DIR).
      if [[ -s "${err_file}" ]]; then
        vmaf_error="$(head -c 4096 "${err_file}")"
      fi
      break
    fi
    if [[ ! -f "${out_json}" ]]; then
      skip_reason="no output file"
      break
    fi
    times+=("${elapsed_ms}")
    run_ok=1
    # Parse scores only once (first successful run)
    if [[ ${#times[@]} -eq 1 ]]; then
      local parse_result
      parse_result=$(
        python3 - "${out_json}" "${json_key}" 2>/dev/null <<'PYEOF'
import json, sys
path = sys.argv[1]
feature_key = sys.argv[2]
try:
    d = json.load(open(path))
    vmaf = d["pooled_metrics"]["vmaf"]["mean"]
    feat = None
    pm = d.get("pooled_metrics", {})
    if feature_key in pm:
        feat = pm[feature_key]["mean"]
    # search alternatives
    if feat is None:
        for k, v in pm.items():
            if feature_key in k and isinstance(v, dict) and "mean" in v:
                feat = v["mean"]
                break
    print(f"{vmaf:.6f} {feat if feat is not None else 'null'}")
except Exception as e:
    print(f"null null # {e}")
PYEOF
      )
      vmaf_score=$(echo "${parse_result}" | awk '{print $1}')
      feature_score=$(echo "${parse_result}" | awk '{print $2}')
    fi
  done

  rm -f "${out_json}"

  local median_ms="null"
  local fps="null"
  if [[ ${run_ok} -eq 1 && ${#times[@]} -gt 0 ]]; then
    # Compute median via python
    local times_csv
    times_csv=$(
      IFS=,
      echo "${times[*]}"
    )
    median_ms=$(python3 -c "
t = sorted([${times_csv}])
n = len(t)
m = t[n//2] if n%2 else (t[n//2-1]+t[n//2])//2
print(m)
")
    fps=$(python3 -c "print(round(${nf} * 1000 / ${median_ms}, 2))" 2>/dev/null || echo "null")
  fi

  # ncu (CUDA only, if requested)
  local ncu_json="{}"
  if [[ ${USE_NCU} -eq 1 && "${backend}" == "cuda" && ${run_ok} -eq 1 ]]; then
    local ncu_out
    ncu_out=$(mktemp "${SCRATCH_DIR}/vmaf-bench-ncu-XXXXX.json")
    ncu_json=$(run_ncu_cell "${ref}" "${dis}" "${w}" "${h}" "${bd}" "${feature}" "${ncu_out}")
  fi

  python3 - \
    "${res_key}" "${backend}" "${metric}" \
    "${w}" "${h}" "${bd}" "${nf}" \
    "${median_ms}" "${fps}" \
    "${vmaf_score}" "${feature_score}" \
    "${run_ok}" "${skip_reason:-}" \
    "${ncu_json}" \
    "${RES_SRC[$res_key]}" \
    "${vmaf_error:-}" \
    <<'PYEOF'
import json, sys
a = sys.argv
skip = (a[12] == "0")
vmaf_err = a[16] if len(a) > 16 else ""
obj = {
    "resolution": a[1],
    "backend":    a[2],
    "metric":     a[3],
    "width":      int(a[4]),
    "height":     int(a[5]),
    "bitdepth":   int(a[6]),
    "frames":     int(a[7]),
    "fixture_source": a[15],
    "median_ms":  None if a[8] == "null" else int(a[8]),
    "fps":        None if a[9] == "null" else float(a[9]),
    "vmaf_score": None if a[10] == "null" else float(a[10]),
    "feature_score": None if a[11] == "null" else float(a[11]),
    "status":     "skip" if skip else "ok",
    "skip_reason": a[13] if skip else None,
    "vmaf_error": vmaf_err if vmaf_err else None,
    "ncu": json.loads(a[14]),
}
print(json.dumps(obj))
PYEOF
}

# ─── hardware / env metadata ─────────────────────────────────────────────────
collect_hardware_meta() {
  python3 - "${VMAF}" "${WORKSPACE}" <<'PYEOF'
import json, subprocess, os, sys, platform, shutil, re
vmaf_bin = sys.argv[1]
workspace = sys.argv[2]

def run(cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        return r.stdout.strip()
    except Exception:
        return None

# CPU
cpu_model = None
try:
    with open("/proc/cpuinfo") as f:
        for line in f:
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
except Exception:
    pass

# GPU (nvidia-smi)
gpu_model = run(["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"])
driver_ver = run(["nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"])
cuda_ver = run(["nvcc", "--version"])
cuda_ver = re.search(r"release (\S+)", cuda_ver).group(1) if cuda_ver else None

# vmaf version
vmaf_ver = run([vmaf_bin, "--version"])

# git hash
git_hash = run(["git", "-C", workspace, "rev-parse", "--short=10", "HEAD"])

# kernel
kernel = platform.release()

result = {
    "cpu_model": cpu_model,
    "gpu_model": gpu_model,
    "nvidia_driver": driver_ver,
    "cuda_version": cuda_ver,
    "vmaf_binary": vmaf_bin,
    "vmaf_version": vmaf_ver,
    "git_hash": git_hash,
    "kernel": kernel,
    "os": platform.platform(),
}
print(json.dumps(result))
PYEOF
}

# ─── dry-run plan ────────────────────────────────────────────────────────────
if [[ ${DRY_RUN} -eq 1 ]]; then
  echo "=== bench-multi-resolution.sh DRY RUN ==="
  echo "workspace:   ${WORKSPACE}"
  echo "vmaf:        ${VMAF}"
  echo "model:       ${MODEL}"
  echo "output:      ${OUTPUT}"
  echo "backends:    ${BACKENDS}"
  echo "resolutions: ${RESOLUTIONS}"
  echo "metrics:     ${METRICS}"
  echo "runs:        ${RUNS}"
  echo "ncu:         ${USE_NCU}"
  echo ""
  IFS=',' read -ra _BE <<<"${BACKENDS}"
  IFS=',' read -ra _RES <<<"${RESOLUTIONS}"
  IFS=',' read -ra _MET <<<"${METRICS}"
  total=$((${#_BE[@]} * ${#_RES[@]} * ${#_MET[@]}))
  echo "cells: ${#_BE[@]} backends × ${#_RES[@]} resolutions × ${#_MET[@]} metrics = ${total} cells"
  echo ""
  for res in "${_RES[@]}"; do
    for be in "${_BE[@]}"; do
      for met in "${_MET[@]}"; do
        echo "  ${res}p / ${be} / ${met}  ->  ${WORKSPACE}/${RES_REF[$res]}"
      done
    done
  done
  exit 0
fi

# ─── main loop ───────────────────────────────────────────────────────────────
echo "=== bench-multi-resolution.sh ==="
echo "vmaf:    ${VMAF}"
echo "output:  ${OUTPUT}"
echo "runs:    ${RUNS}"
echo ""

IFS=',' read -ra BE_LIST <<<"${BACKENDS}"
IFS=',' read -ra RES_LIST <<<"${RESOLUTIONS}"
IFS=',' read -ra MET_LIST <<<"${METRICS}"

# Validate backends
for be in "${BE_LIST[@]}"; do
  if [[ -z "${BACKEND_FLAGS[$be]+_}" ]]; then
    die "Unknown backend '${be}'. Valid: cpu, cuda, sycl"
  fi
  if [[ "${be}" == "sycl" && ${SYCL_AVAILABLE} -eq 0 ]]; then
    echo "WARNING: SYCL requested but no oneAPI setvars.sh found; SYCL cells will likely fail." >&2
  fi
done

# Ensure fixtures
declare -A FIXTURE_OK
for res in "${RES_LIST[@]}"; do
  if [[ -z "${RES_W[$res]+_}" ]]; then
    die "Unknown resolution key '${res}'. Valid: 576, 720, 1080, 1440, 2160"
  fi
  if ensure_fixture "${res}" 2>&1; then
    FIXTURE_OK[$res]=1
  else
    FIXTURE_OK[$res]=0
    echo "WARNING: fixture for ${res}p unavailable — those cells will be skipped."
  fi
done

# Validate metrics
for met in "${MET_LIST[@]}"; do
  if [[ -z "${METRIC_FEATURE[$met]+_}" ]]; then
    die "Unknown metric '${met}'. Valid: vif, adm, motion, ssim, ms_ssim"
  fi
done

# Collect hardware metadata
echo "Collecting hardware metadata..."
HW_META=$(collect_hardware_meta)
echo "  git: $(echo "${HW_META}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("git_hash","?"))')"
echo ""

# Run all cells
CELL_JSONS=()
TOTAL=$((${#BE_LIST[@]} * ${#RES_LIST[@]} * ${#MET_LIST[@]}))
COUNT=0

for res in "${RES_LIST[@]}"; do
  if [[ "${FIXTURE_OK[$res]:-0}" -eq 0 ]]; then
    for be in "${BE_LIST[@]}"; do
      for met in "${MET_LIST[@]}"; do
        COUNT=$((COUNT + 1))
        cell_json=$(python3 -c "
import json
print(json.dumps({'resolution': '${res}', 'backend': '${be}', 'metric': '${met}',
                  'status': 'skip', 'skip_reason': 'fixture_unavailable',
                  'median_ms': None, 'fps': None, 'vmaf_score': None,
                  'feature_score': None, 'ncu': {}}))")
        CELL_JSONS+=("${cell_json}")
      done
    done
    continue
  fi
  for be in "${BE_LIST[@]}"; do
    for met in "${MET_LIST[@]}"; do
      COUNT=$((COUNT + 1))
      label="${res}p/${be}/${met}"
      printf "  [%3d/%3d] %-30s ... " "${COUNT}" "${TOTAL}" "${label}"
      cell_json=$(run_cell "${res}" "${be}" "${met}")
      CELL_JSONS+=("${cell_json}")
      # Print one-line summary
      echo "${cell_json}" | python3 -c "
import json, sys
d = json.load(sys.stdin)
if d['status'] == 'ok':
    print(f\"VMAF {d['vmaf_score']:.4f}  {d['fps']:.1f} fps  ({d['median_ms']}ms)\")
else:
    print(f\"SKIP — {d.get('skip_reason', '?')}\")"
    done
  done
done

echo ""
echo "Writing ${OUTPUT}..."

# Assemble final JSON
python3 - "${OUTPUT}" "${HW_META}" "${RUNS}" "${TOTAL}" "${USE_NCU}" \
  "${CELL_JSONS[@]+"${CELL_JSONS[@]}"}" \
  <<'PYEOF'
import json, sys, datetime
output_path = sys.argv[1]
hw_meta = json.loads(sys.argv[2])
runs = int(sys.argv[3])
total = int(sys.argv[4])
use_ncu = int(sys.argv[5])
cells = [json.loads(s) for s in sys.argv[6:]]

import pathlib
pathlib.Path(output_path).parent.mkdir(parents=True, exist_ok=True)

result = {
    "schema_version": "1",
    "description": "Multi-resolution multi-backend VMAF throughput baseline",
    "timestamp": datetime.datetime.utcnow().isoformat() + "Z",
    "hardware": hw_meta,
    "toolkit": {
        "runs_per_cell": runs,
        "timing": "median wall time (ms)",
        "ncu_enabled": bool(use_ncu),
    },
    "fixture_notes": {
        "576":  "native testdata/ref_576x324_48f.yuv (Netflix golden)",
        "720":  "native testdata/ref_640x480_48f.yuv (640x480)",
        "1080": "ffmpeg bilinear upscale from 576x324 fixture",
        "1440": "ffmpeg bilinear upscale from 576x324 fixture",
        "2160": "first 30 frames of testdata/bbb/ref_3840x2160_200f.yuv",
    },
    "total_cells": total,
    "ok_cells": sum(1 for c in cells if c.get("status") == "ok"),
    "skipped_cells": sum(1 for c in cells if c.get("status") == "skip"),
    "runs": cells,
}

pathlib.Path(output_path).write_text(json.dumps(result, indent=2), encoding="utf-8")
print(f"Wrote {len(cells)} cells to {output_path}")
ok = result["ok_cells"]
skip = result["skipped_cells"]
print(f"  ok={ok}  skipped={skip}")
PYEOF

echo "Done."
