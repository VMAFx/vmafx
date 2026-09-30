#!/usr/bin/env bash
# rc3-home-gpu-retest.sh — run the RC3 verify-and-time commands that
# docs/state.md carries for the home GPU box, ryzen-4090-arc: the RTX 4090
# (CUDA), the Arc A380 (SYCL over Level Zero) and the Zen 5 iGPU gfx1036 (HIP).
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# One entry per docs/state.md row and backend. Each entry spells out the
# commands of its row; nothing is read from the markdown at run time, so a PR
# that changes a row's commands changes the entry here in the same PR
# (docs/development/rc3-home-gpu-retest.md, ADR-1386).
#
# Usage:
#   scripts/dev/rc3-home-gpu-retest.sh --list
#   scripts/dev/rc3-home-gpu-retest.sh [--backend NAME]... [--only ROW-ID]...
#       [--build-dir DIR | --build-dir BACKEND=DIR]... [--baseline DIR]
#       [--out DIR] [--no-timing] [--reps N] [--threads N] [--dry-run]
#
# Options:
#   --backend NAME       cuda, hip or sycl; repeatable (default: all three)
#   --only ROW-ID        run only this docs/state.md row; repeatable
#   --build-dir DIR      meson build dir whose tools/vmaf the entries run
#                        (default: build); BACKEND=DIR sets it for one backend
#   --baseline DIR       the --out directory of an earlier run, e.g. on master:
#                        entries also compare their GPU JSON with it
#   --out DIR            logs, JSON and the summary (default:
#                        <first build dir>/rc3-retest/<UTC time>-<commit>)
#   --list               print the entries and exit
#   --dry-run            print the commands instead of running them
#   --no-timing          skip the ms/frame measurements
#   --reps N             timing repetitions (default: 3, as the rows ask)
#   --threads N          CPU extractor threads (default: 16, as the rows ask)
#   --netflix-dir DIR    Netflix 576x324 pair (default: python/test/resource/yuv)
#   --bbb-dir DIR        BBB 3840x2160 pair (default: testdata/bbb)
#   --cuda-device N      CUDA index in PCI order (default: the RTX 4090)
#   --hip-device N       HIP device index (default: the gfx1036 agent)
#   --sycl-selector SEL  ONEAPI_DEVICE_SELECTOR (default: the A380's level_zero:N)
#   --image REF          oneAPI image to test instead of building one
#   --lock-dir DIR       per-device flock files (default: ~/.cache/vmafx-locks)
#
# Every device run holds its device's lock (cuda-4090.lock, hip-gfx1036.lock,
# sycl-a380.lock); a timed run takes it before the clock starts, and the oneAPI
# image build holds sycl-build.lock. CPU-only runs take no lock.
#
# Exit: 0 every selected entry passed or was skipped, 1 an entry missed its
# row's expectation, 2 an entry could not run, or a usage error.
set -uo pipefail
export LC_ALL=C

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "rc3-home-gpu-retest: run inside the vmafx checkout" >&2
  exit 2
}
cd "$REPO_ROOT" || exit 2

# ---------------------------------------------------------------------------
# Entries: row, backend, function, what the row asks. Keep this list in the
# order of docs/state.md.
# ---------------------------------------------------------------------------
ENTRY_ROW=()
ENTRY_BACKEND=()
ENTRY_FUNC=()
ENTRY_WHAT=()

entry() {
  ENTRY_ROW+=("$1")
  ENTRY_BACKEND+=("$2")
  ENTRY_FUNC+=("$3")
  ENTRY_WHAT+=("$4")
}

PSNR_HVS_WHAT="psnr_hvs twin vs --threads 16 CPU within ADR-1361 (5e-4 at 576x324, 3.34e-3 at 4K, 22 frames); feature_backends names the twin; with --baseline identical to it; ms/frame"
MOTION_V2_WHAT="motion_v2 twin vs --threads 16 CPU: 0.0 (576x324, 4K 22 frames); feature_backends names the twin; with --baseline identical to it; ms/frame"
MOTION_WHAT="motion twin vs CPU, integer_motion2 on the Netflix pair: 0.0 after the port (about 1.3e-5 before)"
FLOAT_SSIM_WHAT="4K float_ssim runs on the twin with no fallback warning; speed_gpu_parity.py float_ssim within 5e-5; ms/frame"
SS2_WHAT="speed_gpu_parity.py ssimulacra2 within 1e-9 (every frame identical before the port); ms/frame"
CAMBI_WHAT="cambi twin vs --threads 16 CPU: 576x324 bit-exact, 4K 50 frames within 2.2e-15; ms/frame"
SPEED_WHAT="speed_gpu_parity.py speed_chroma + speed_temporal bit-identical; ms/frame"

entry T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29 cuda check_psnr_hvs "$PSNR_HVS_WHAT"
entry T-HIP-PSNR-HVS-HOST-CONVERT-2026-09-29 hip check_psnr_hvs "$PSNR_HVS_WHAT"
entry T-GPU-MOTION-V2-INT64-VERTICAL-2026-09-29 cuda check_motion_v2 "$MOTION_V2_WHAT"
entry T-GPU-MOTION-V2-INT64-VERTICAL-2026-09-29 hip check_motion_v2 "$MOTION_V2_WHAT"
entry T-HIP-TWIN-PRIVATE-PLANE-UPLOADS-2026-09-29 hip check_twin_uploads \
  "psnr + psnr_hvs + motion_v2 in one hip run at --precision max; with --baseline every metric identical to it; ms/frame"
entry T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29 cuda check_motion "$MOTION_WHAT"
entry T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29 hip check_motion "$MOTION_WHAT"
entry T-CUDA-FLOAT-SSIM-SCALE-GT1-2026-09-29 cuda check_float_ssim "$FLOAT_SSIM_WHAT"
entry T-HIP-FLOAT-SSIM-SCALE-GT1-2026-09-29 hip check_float_ssim "$FLOAT_SSIM_WHAT"
entry T-CUDA-SSIMULACRA2-HOST-COMBINE-2026-09-29 cuda check_ssimulacra2 "$SS2_WHAT"
entry T-HIP-SSIMULACRA2-HOST-COMBINE-2026-09-29 hip check_ssimulacra2 "$SS2_WHAT"
entry T-CUDA-CAMBI-HOST-RESIDUAL-2026-09-29 cuda check_cambi "$CAMBI_WHAT"
entry T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29 hip check_cambi "$CAMBI_WHAT"
entry T-CUDA-SPEED-HOST-RESIDUAL-2026-09-29 cuda check_speed "$SPEED_WHAT"
entry T-HIP-SPEED-HOST-RESIDUAL-2026-09-29 hip check_speed "$SPEED_WHAT"
entry T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05 hip check_adm_aim \
  "adm_hip vs CPU adm (576x324, 4K 50 frames): integer_aim / integer_adm3 identical, integer_adm2 places=4, also with the default model's options; HIP ADM tests; default-model ms/frame"
entry T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29 sycl check_oneapi_image \
  "final-oneapi2026 image, default model on the Netflix pair: --backend sycl reports sycl and a pooled VMAF within 5e-5 of --backend cpu"

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
usage() {
  sed -n '2,/^set -uo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
}

die() {
  echo "rc3-home-gpu-retest: $*" >&2
  exit 2
}

SELECTED_BACKENDS=()
ONLY_ROWS=()
DEFAULT_BUILD=build
declare -A BUILD_FOR=()
BASELINE=""
OUT=""
LIST=0
DRY=0
TIMING=1
REPS=3
THREADS=16
NETFLIX_DIR=python/test/resource/yuv
BBB_DIR=testdata/bbb
CUDA_DEV=""
HIP_DEV=""
SYCL_SEL=""
IMAGE=""
LOCK_DIR="${HOME}/.cache/vmafx-locks"

need_value() {
  [ "$#" -ge 2 ] && [ -n "$2" ] || die "$1 needs a value"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --backend)
      need_value "$@"
      case "$2" in
        cuda | hip | sycl) SELECTED_BACKENDS+=("$2") ;;
        *) die "unknown backend '$2' (cuda, hip or sycl)" ;;
      esac
      shift 2
      ;;
    --only)
      need_value "$@"
      ONLY_ROWS+=("$2")
      shift 2
      ;;
    --build-dir)
      need_value "$@"
      case "$2" in
        cuda=* | hip=* | sycl=*) BUILD_FOR["${2%%=*}"]="${2#*=}" ;;
        *) DEFAULT_BUILD="$2" ;;
      esac
      shift 2
      ;;
    --baseline)
      need_value "$@"
      BASELINE="$2"
      shift 2
      ;;
    --out)
      need_value "$@"
      OUT="$2"
      shift 2
      ;;
    --list)
      LIST=1
      shift
      ;;
    --dry-run)
      DRY=1
      shift
      ;;
    --no-timing)
      TIMING=0
      shift
      ;;
    --reps)
      need_value "$@"
      REPS="$2"
      shift 2
      ;;
    --threads)
      need_value "$@"
      THREADS="$2"
      shift 2
      ;;
    --netflix-dir)
      need_value "$@"
      NETFLIX_DIR="$2"
      shift 2
      ;;
    --bbb-dir)
      need_value "$@"
      BBB_DIR="$2"
      shift 2
      ;;
    --cuda-device)
      need_value "$@"
      CUDA_DEV="$2"
      shift 2
      ;;
    --hip-device)
      need_value "$@"
      HIP_DEV="$2"
      shift 2
      ;;
    --sycl-selector)
      need_value "$@"
      SYCL_SEL="$2"
      shift 2
      ;;
    --image)
      need_value "$@"
      IMAGE="$2"
      shift 2
      ;;
    --lock-dir)
      need_value "$@"
      LOCK_DIR="$2"
      shift 2
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *) die "unknown option '$1' (see --help)" ;;
  esac
done

for n in "$REPS" "$THREADS"; do
  [[ $n =~ ^[1-9][0-9]*$ ]] || die "--reps and --threads take a positive integer, not '$n'"
done
for n in "$CUDA_DEV" "$HIP_DEV"; do
  [[ -z $n || $n =~ ^[0-9]+$ ]] || die "--cuda-device and --hip-device take an index, not '$n'"
done
[ "${#SELECTED_BACKENDS[@]}" -gt 0 ] || SELECTED_BACKENDS=(cuda hip sycl)
for row in "${ONLY_ROWS[@]}"; do
  known=0
  for r in "${ENTRY_ROW[@]}"; do [ "$r" = "$row" ] && known=1; done
  [ "$known" = 1 ] || die "no entry for row '$row' (see --list)"
done
[ -z "$BASELINE" ] || [ -d "$BASELINE" ] || die "--baseline $BASELINE is not a directory"

selected() { # index
  local i=$1 b row ok=0
  for b in "${SELECTED_BACKENDS[@]}"; do [ "$b" = "${ENTRY_BACKEND[$i]}" ] && ok=1; done
  [ "$ok" = 1 ] || return 1
  [ "${#ONLY_ROWS[@]}" -eq 0 ] && return 0
  for row in "${ONLY_ROWS[@]}"; do [ "$row" = "${ENTRY_ROW[$i]}" ] && return 0; done
  return 1
}

if [ "$LIST" = 1 ]; then
  for i in "${!ENTRY_ROW[@]}"; do
    selected "$i" || continue
    printf '%-56s %-5s %s\n' "${ENTRY_ROW[$i]}" "${ENTRY_BACKEND[$i]}" "${ENTRY_WHAT[$i]}"
  done
  exit 0
fi

# ---------------------------------------------------------------------------
# JSON comparisons and summaries: scripts/dev/rc3_retest_helpers.py
# ---------------------------------------------------------------------------
py() {
  python3 "$REPO_ROOT/scripts/dev/rc3_retest_helpers.py" "$@"
}

# ---------------------------------------------------------------------------
# Devices, locks, logging
# ---------------------------------------------------------------------------
declare -A LOCK_FILE=([cuda]=cuda-4090.lock [hip]=hip-gfx1036.lock [sycl]=sycl-a380.lock)
SYCL_BUILD_LOCK=sycl-build.lock
PROBE_TIMEOUT=60   # device queries
RUN_TIMEOUT=1800   # one vmaf run, one test run
LONG_TIMEOUT=14400 # a whole speed_gpu_parity.py run, the image build

oneapi_env() { # run a command with oneAPI's environment when it is not loaded
  if command -v sycl-ls >/dev/null 2>&1 || [ ! -f /opt/intel/oneapi/setvars.sh ]; then
    "$@"
  else
    # setvars.sh reads unset variables, so it cannot run under set -u.
    # shellcheck disable=SC1091
    (set +u && source /opt/intel/oneapi/setvars.sh >/dev/null 2>&1 && "$@")
  fi
}

detect_cuda() {
  [ -n "$CUDA_DEV" ] && return 0
  command -v nvidia-smi >/dev/null 2>&1 || return 1
  CUDA_DEV="$(timeout "$PROBE_TIMEOUT" nvidia-smi --query-gpu=index,name --format=csv,noheader 2>/dev/null |
    awk -F', ' '/RTX 4090/ {print $1; exit}')"
  [ -n "$CUDA_DEV" ]
}

detect_hip() {
  [ -n "$HIP_DEV" ] && return 0
  local enum=/opt/rocm/bin/rocm_agent_enumerator
  command -v rocm_agent_enumerator >/dev/null 2>&1 && enum=rocm_agent_enumerator
  HIP_DEV="$(timeout "$PROBE_TIMEOUT" "$enum" 2>/dev/null | grep -v '^gfx000$' |
    awk '$1 == "gfx1036" {print NR - 1; exit}')"
  [ -n "$HIP_DEV" ]
}

detect_sycl() {
  [ -n "$SYCL_SEL" ] && return 0
  local index
  index="$(oneapi_env timeout "$PROBE_TIMEOUT" sycl-ls 2>/dev/null |
    sed -n 's/^\[level_zero:gpu\]\[level_zero:\([0-9]*\)\].*A380.*/\1/p' | head -n 1)"
  [ -n "$index" ] || return 1
  SYCL_SEL="level_zero:$index"
}

detect_device() { # backend
  case "$1" in
    cuda) detect_cuda ;;
    hip) detect_hip ;;
    sycl) detect_sycl ;;
  esac
}

device_env() { # backend -> DEV_ENV array
  case "$1" in
    cuda) DEV_ENV=(CUDA_DEVICE_ORDER=PCI_BUS_ID "CUDA_VISIBLE_DEVICES=$CUDA_DEV") ;;
    hip) DEV_ENV=("HIP_VISIBLE_DEVICES=$HIP_DEV") ;;
    sycl) DEV_ENV=("ONEAPI_DEVICE_SELECTOR=$SYCL_SEL") ;;
  esac
}

log() {
  printf '%s\n' "$*" >>"$ELOG"
}

quoted() {
  local q
  printf -v q '%q ' "$@"
  printf '%s' "${q% }"
}

note() {
  NOTES+=("$*")
  log "=> $*"
  printf '    %s\n' "$*"
}

raise() { # FAIL|ERROR message
  if [ "$1" = ERROR ] || [ "$STATUS" != ERROR ]; then
    [ "$STATUS" = SKIP ] || STATUS="$1"
  fi
  shift
  note "$*"
}

# lock_take FILE VAR: flock FILE on a fresh descriptor and store it in the
# caller's VAR. The local is named _take_fd so that printf -v reaches the
# caller's variable (bash scoping is dynamic: a local of the same name would
# shadow it, the lock would never be dropped, and the next lock_take on the
# same file would wait for this one forever).
lock_take() {
  local _take_fd _take_waited=$SECONDS
  if [ "$DRY" = 1 ]; then
    printf -v "$2" '%s' ""
    return 0
  fi
  mkdir -p "$LOCK_DIR" && exec {_take_fd}>>"$LOCK_DIR/$1" || return 2
  flock "$_take_fd" || return 2
  _take_waited=$((SECONDS - _take_waited))
  [ "$_take_waited" -eq 0 ] || log "waited ${_take_waited} s for $1"
  printf -v "$2" '%s' "$_take_fd"
}

lock_drop() {
  local lock_fd=$1
  [ -n "$lock_fd" ] || return 0
  flock -u "$lock_fd"
  exec {lock_fd}>&-
}

load_note() { # the load, and what else holds the device, for a timing note
  local text
  text="load $(cut -d' ' -f1 /proc/loadavg)"
  if [ "$B" = cuda ] && command -v nvidia-smi >/dev/null 2>&1; then
    text+=", 4090 $(timeout "$PROBE_TIMEOUT" nvidia-smi -i "$CUDA_DEV" --query-gpu=memory.used,utilization.gpu \
      --format=csv,noheader 2>/dev/null | head -n 1) before the run"
  fi
  printf '%s' "$text"
}

# run_cmd CMD...: log the command, run it (at most CMD_TIMEOUT seconds, default
# RUN_TIMEOUT) with stdout and stderr in the log and stderr also in
# $EDIR/last.stderr; the exit status is the command's (124: timed out)
run_cmd() {
  log "+ $(quoted "$@")"
  if [ "$DRY" = 1 ]; then
    printf '    + %s\n' "$(quoted "$@")" >&2
    return 0
  fi
  timeout --kill-after=30 "${CMD_TIMEOUT:-$RUN_TIMEOUT}" "$@" >>"$ELOG" 2>"$EDIR/last.stderr"
  local rc=$?
  cat "$EDIR/last.stderr" >>"$ELOG"
  [ "$rc" -eq 0 ] || log "exit $rc"
  return "$rc"
}

# ---------------------------------------------------------------------------
# vmaf invocations
# ---------------------------------------------------------------------------
fixture_label() {
  case "$1" in
    nf) printf '576x324' ;;
    bbb) printf '3840x2160' ;;
  esac
}

# vmaf_cmd ARRAY SIDE FIXTURE FRAMES OUT ARGS...: SIDE is gpu, cpu (--threads)
# or cpu1 (serial, as the motion rows run it); FRAMES empty = the whole file
vmaf_cmd() {
  local -n cmd_ref=$1
  local side=$2 fixture=$3 frames=$4 out=$5
  shift 5
  cmd_ref=()
  [ "$side" = gpu ] && cmd_ref+=(env "${DEV_ENV[@]}")
  cmd_ref+=("$VMAF")
  case "$fixture" in
    nf) cmd_ref+=(-r "$NETFLIX_DIR/src01_hrc00_576x324.yuv" -d "$NETFLIX_DIR/src01_hrc01_576x324.yuv" -w 576 -h 324) ;;
    bbb) cmd_ref+=(-r "$BBB_DIR/ref_3840x2160_200f.yuv" -d "$BBB_DIR/dis_3840x2160_200f.yuv" -w 3840 -h 2160) ;;
  esac
  cmd_ref+=(-p 420 -b 8)
  [ -z "$frames" ] || cmd_ref+=(--frame_cnt "$frames")
  case "$side" in
    gpu) cmd_ref+=(--backend "$B") ;;
    cpu) cmd_ref+=(--backend cpu --threads "$THREADS") ;;
    cpu1) cmd_ref+=(--backend cpu) ;;
  esac
  cmd_ref+=("$@" --json -o "$out")
}

# run_side SIDE FIXTURE FRAMES OUT ARGS...: one run, under the device lock on
# the GPU side
run_side() {
  local side=$1 lock_fd="" rc
  local -a cmd
  vmaf_cmd cmd "$@"
  if [ "$side" = gpu ]; then
    lock_take "${LOCK_FILE[$B]}" lock_fd || return 2
  fi
  run_cmd "${cmd[@]}"
  rc=$?
  lock_drop "$lock_fd"
  return "$rc"
}

# baseline_check FILE STRICT: compare FILE with the same file of --baseline
baseline_check() {
  local file=$1 strict=$2 rel res rc
  [ -n "$BASELINE" ] && [ "$DRY" = 0 ] || return 0
  rel="${file#"$OUT"/}"
  if [ ! -f "$BASELINE/$rel" ]; then
    note "$rel: no file in the baseline"
    return 0
  fi
  res="$(py compare "$BASELINE/$rel" "$file" 0)"
  rc=$?
  if [ "$rc" -ne 0 ] && [ "$strict" = strict ]; then
    raise FAIL "$rel vs baseline: $res"
  else
    note "$rel vs baseline: $res"
  fi
}

# parity FIXTURE FRAMES LABEL KEYS=TOL...: GPU_ARGS on the twin, CPU_ARGS on
# CPU_SIDE, then compare each KEYS (comma list, or * for every metric) within
# TOL; TWINS must appear in feature_backends; BASELINE_STRICT decides whether
# a baseline difference fails the entry
parity() {
  local fixture=$1 frames=$2 label=$3 spec keys tol res rc
  shift 3
  local gpu_json="$EDIR/$fixture$label-$B.json" cpu_json="$EDIR/$fixture$label-cpu.json"
  local name
  name="$(fixture_label "$fixture")$label"
  if ! run_side gpu "$fixture" "$frames" "$gpu_json" "${GPU_ARGS[@]}"; then
    raise ERROR "$name: the $B run failed: $(tail -n 2 "$EDIR/last.stderr" 2>/dev/null | tr '\n' ' ')"
    return
  fi
  if ! run_side "$CPU_SIDE" "$fixture" "$frames" "$cpu_json" "${CPU_ARGS[@]}"; then
    raise ERROR "$name: the CPU run failed: $(tail -n 2 "$EDIR/last.stderr" 2>/dev/null | tr '\n' ' ')"
    return
  fi
  [ "$DRY" = 0 ] || return 0
  for spec in "$@"; do
    keys="${spec%=*}"
    tol="${spec##*=}"
    [ "$keys" = '*' ] && keys=""
    res="$(py compare "$cpu_json" "$gpu_json" "$tol" "$keys")"
    rc=$?
    case "$rc" in
      0) note "$name $B vs cpu: $res (bound $tol)" ;;
      1) raise FAIL "$name $B vs cpu: $res (bound $tol)" ;;
      *) raise ERROR "$name $B vs cpu: $res" ;;
    esac
  done
  if [ "${#TWINS[@]}" -gt 0 ]; then
    res="$(py backends "$gpu_json" "$B" "${TWINS[@]}")" || raise FAIL "$name: $res"
  fi
  baseline_check "$gpu_json" "$BASELINE_STRICT"
}

# gpu_only FIXTURE FRAMES: one twin run whose output only the baseline judges
gpu_only() {
  local fixture=$1 frames=$2 res
  local gpu_json="$EDIR/$fixture-$B.json"
  if ! run_side gpu "$fixture" "$frames" "$gpu_json" "${GPU_ARGS[@]}"; then
    raise ERROR "$(fixture_label "$fixture"): the $B run failed: $(tail -n 2 "$EDIR/last.stderr" 2>/dev/null | tr '\n' ' ')"
    return
  fi
  [ "$DRY" = 0 ] || return 0
  if [ "${#TWINS[@]}" -gt 0 ]; then
    res="$(py backends "$gpu_json" "$B" "${TWINS[@]}")" || raise FAIL "$(fixture_label "$fixture"): $res"
  fi
  if [ -n "$BASELINE" ]; then
    baseline_check "$gpu_json" strict
  else
    note "$(fixture_label "$fixture"): recorded $fixture-$B.json for a later --baseline run"
  fi
}

# time_side SIDE FIXTURE LONG ARGS...: print the median ms/frame of REPS pairs
# of runs of 2 and LONG frames; a GPU side holds the device lock throughout
time_side() {
  local side=$1 fixture=$2 long=$3 lock_fd="" rep n t0 rc=0
  shift 3
  local -a samples=() cmd
  if [ "$side" = gpu ]; then
    lock_take "${LOCK_FILE[$B]}" lock_fd || return 2
  fi
  for ((rep = 1; rep <= REPS; rep++)); do
    for n in 2 "$long"; do
      vmaf_cmd cmd "$side" "$fixture" "$n" "$EDIR/timing.json" "$@"
      t0=$(date +%s%N)
      run_cmd "${cmd[@]}" || rc=2
      samples+=("$(($(date +%s%N) - t0))")
    done
  done
  lock_drop "$lock_fd"
  [ "$rc" -eq 0 ] || return "$rc"
  [ "$DRY" = 0 ] || return 0
  py msframe 2 "$long" "${samples[@]}"
}

# timing FIXTURE LONG: GPU_TIME_ARGS on the twin against CPU_TIME_ARGS on the
# --threads CPU, as the rows' "(t(LONG) - t(2)) / (LONG - 2), median of 3"
timing() {
  local fixture=$1 long=$2 gpu_ms cpu_ms context
  [ "$TIMING" = 1 ] || return 0
  context="$(load_note)"
  gpu_ms="$(time_side gpu "$fixture" "$long" "${GPU_TIME_ARGS[@]}")" || {
    raise ERROR "$(fixture_label "$fixture") timing: the $B runs failed"
    return
  }
  cpu_ms="$(time_side cpu "$fixture" "$long" "${CPU_TIME_ARGS[@]}")" || {
    raise ERROR "$(fixture_label "$fixture") timing: the CPU runs failed"
    return
  }
  [ "$DRY" = 0 ] || return 0
  note "ms/frame $(fixture_label "$fixture") (2 vs $long frames, median of $REPS): $B $gpu_ms, cpu$THREADS $cpu_ms ($context)"
}

# speed_parity ARGS...: scripts/dev/speed_gpu_parity.py for this backend, the
# whole run under the device lock (it times the twin itself)
speed_parity() {
  local lock_fd="" rc out="$EDIR/speed_gpu_parity.txt" context
  local -a cmd=(env "${DEV_ENV[@]}" python3 scripts/dev/speed_gpu_parity.py --backend "$B"
    --vmaf "$VMAF" --netflix-dir "$NETFLIX_DIR" --bbb-dir "$BBB_DIR"
    --threads "$THREADS" --reps "$REPS")
  [ "$TIMING" = 1 ] || cmd+=(--no-timing)
  cmd+=("$@")
  lock_take "${LOCK_FILE[$B]}" lock_fd || {
    raise ERROR "could not take ${LOCK_FILE[$B]}"
    return
  }
  context="$(load_note)"
  log "+ $(quoted "${cmd[@]}")"
  if [ "$DRY" = 1 ]; then
    printf '    + %s\n' "$(quoted "${cmd[@]}")" >&2
    lock_drop "$lock_fd"
    return
  fi
  timeout --kill-after=30 "$LONG_TIMEOUT" "${cmd[@]}" >"$out" 2>&1
  rc=$?
  lock_drop "$lock_fd"
  cat "$out" >>"$ELOG"
  local res
  res="$(py speedsum "$out")"
  [ "$TIMING" = 0 ] || res+=" ($context)"
  case "$rc" in
    0) note "speed_gpu_parity.py $*: $res" ;;
    1) raise FAIL "speed_gpu_parity.py $*: $res" ;;
    *) raise ERROR "speed_gpu_parity.py $* exit $rc: $res" ;;
  esac
}

# ---------------------------------------------------------------------------
# Row checks. B, VMAF, BUILD, EDIR and DEV_ENV are set by the runner.
# ---------------------------------------------------------------------------

# T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29, T-HIP-PSNR-HVS-HOST-CONVERT-2026-09-29
check_psnr_hvs() {
  TWINS=("psnr_hvs_$B")
  GPU_ARGS=(--no_prediction --feature psnr_hvs --precision max)
  CPU_ARGS=("${GPU_ARGS[@]}")
  CPU_SIDE=cpu
  BASELINE_STRICT=strict
  parity nf "" "" '*=5e-4'
  parity bbb 22 "" '*=3.34e-3'
  GPU_TIME_ARGS=(--no_prediction --feature psnr_hvs -q)
  CPU_TIME_ARGS=("${GPU_TIME_ARGS[@]}")
  timing nf 48
  timing bbb 22
}

# T-GPU-MOTION-V2-INT64-VERTICAL-2026-09-29 (CUDA and HIP halves)
check_motion_v2() {
  TWINS=("motion_v2_$B")
  GPU_ARGS=(--no_prediction --feature motion_v2 --precision max)
  CPU_ARGS=("${GPU_ARGS[@]}")
  CPU_SIDE=cpu
  BASELINE_STRICT=strict
  parity nf "" "" '*=0'
  parity bbb 22 "" '*=0'
  GPU_TIME_ARGS=(--no_prediction --feature motion_v2 -q)
  CPU_TIME_ARGS=("${GPU_TIME_ARGS[@]}")
  timing nf 48
  timing bbb 22
}

# T-HIP-TWIN-PRIVATE-PLANE-UPLOADS-2026-09-29
check_twin_uploads() {
  TWINS=(psnr_hip psnr_hvs_hip motion_v2_hip)
  GPU_ARGS=(--no_prediction --feature psnr --feature psnr_hvs --feature motion_v2 --precision max)
  gpu_only nf ""
  gpu_only bbb 22
  GPU_TIME_ARGS=(--no_prediction --feature psnr --feature psnr_hvs --feature motion_v2 -q)
  CPU_TIME_ARGS=("${GPU_TIME_ARGS[@]}")
  timing nf 48
  timing bbb 22
}

# T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29, T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29
check_motion() {
  TWINS=("motion_$B")
  GPU_ARGS=(--no_prediction --feature motion --precision=max -q)
  CPU_ARGS=("${GPU_ARGS[@]}")
  CPU_SIDE=cpu1
  BASELINE_STRICT=info
  parity nf "" "" 'integer_motion2=0'
}

# T-CUDA-FLOAT-SSIM-SCALE-GT1-2026-09-29, T-HIP-FLOAT-SSIM-SCALE-GT1-2026-09-29
check_float_ssim() {
  local json="$EDIR/bbb-float_ssim-$B.json" res warning
  if ! run_side gpu bbb 2 "$json" --no_prediction --feature float_ssim; then
    raise ERROR "3840x2160 float_ssim: the $B run failed: $(tail -n 2 "$EDIR/last.stderr" 2>/dev/null | tr '\n' ' ')"
  elif [ "$DRY" = 0 ]; then
    warning="$(grep -m 1 'warning' "$EDIR/last.stderr")"
    [ -z "$warning" ] || raise FAIL "3840x2160 float_ssim: $warning"
    if res="$(py backends "$json" "$B" "float_ssim_$B")"; then
      note "3840x2160 float_ssim: $res"
    else
      raise FAIL "3840x2160 float_ssim: $res"
    fi
  fi
  speed_parity --feature float_ssim --max-abs-diff 5e-5
}

# T-CUDA-SSIMULACRA2-HOST-COMBINE-2026-09-29, T-HIP-SSIMULACRA2-HOST-COMBINE-2026-09-29
check_ssimulacra2() {
  speed_parity --feature ssimulacra2 --max-abs-diff 1e-9
}

# T-CUDA-CAMBI-HOST-RESIDUAL-2026-09-29, T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29
check_cambi() {
  TWINS=("cambi_$B")
  GPU_ARGS=(--no_prediction --feature "cambi_$B" --precision max)
  CPU_ARGS=(--no_prediction --feature cambi --precision max)
  CPU_SIDE=cpu
  BASELINE_STRICT=info
  parity nf "" "" 'cambi=0'
  parity bbb 50 "" 'cambi=2.2e-15'
  GPU_TIME_ARGS=(--no_prediction --feature "cambi_$B" -q)
  CPU_TIME_ARGS=(--no_prediction --feature cambi -q)
  timing bbb 22
  timing nf 22
}

# T-CUDA-SPEED-HOST-RESIDUAL-2026-09-29, T-HIP-SPEED-HOST-RESIDUAL-2026-09-29
check_speed() {
  speed_parity
}

# T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05 (HIP half)
check_adm_aim() {
  local opts=adm_csf_mode=2:adm_dlm_weight=0.7:adm_enhn_gain_limit=1.0:adm_min_val=0.5:adm_noise_weight=0.02
  local sfx=_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02 fixture frames lock_hip=""
  TWINS=(adm_hip)
  CPU_SIDE=cpu
  BASELINE_STRICT=info
  for fixture in nf bbb; do
    frames=""
    [ "$fixture" = bbb ] && frames=50
    GPU_ARGS=(--feature adm_hip --no_prediction --precision max)
    CPU_ARGS=(--feature adm --no_prediction --precision max)
    parity "$fixture" "$frames" "" 'integer_aim,integer_adm3=0' 'integer_adm2=5e-5'
    GPU_ARGS=(--feature "adm_hip=$opts" --no_prediction --precision max)
    CPU_ARGS=(--feature "adm=$opts" --no_prediction --precision max)
    parity "$fixture" "$frames" -opts "integer_aim$sfx,integer_adm3$sfx=0" "integer_adm2$sfx=5e-5"
  done
  lock_take "${LOCK_FILE[hip]}" lock_hip || raise ERROR "could not take ${LOCK_FILE[hip]}"
  if run_cmd env "${DEV_ENV[@]}" python3 scripts/ci/run_meson_test.py -- -C "$BUILD" --no-rebuild \
    test_hip_adm_parity test_hip_adm_small_border test_hip_adm_wide_rounding; then
    [ "$DRY" = 1 ] || note "meson: test_hip_adm_parity, test_hip_adm_small_border, test_hip_adm_wide_rounding pass"
  else
    raise FAIL "meson: a HIP ADM test failed (log)"
  fi
  lock_drop "$lock_hip"
  # The row's python/test/gpu_default_model_test.py step runs after its hip
  # entry is flipped to expect aim / adm3, a source change that belongs to the
  # PR closing the row; as committed it asserts their absence, so it is not run.
  GPU_TIME_ARGS=(-q)
  CPU_TIME_ARGS=(-q)
  timing nf 22
  timing bbb 22
}

# T-RELEASE-ONEAPI-IMAGE-B580-SIGSEGV-2026-09-29: the image, on the A380. The
# row's `git fetch ... fix/release-oneapi-image-runtime` step predates #1629's
# merge; the image is built from this checkout.
check_oneapi_image() {
  local image=${IMAGE:-vmafx:oneapi2026-check} lock_fd="" gid backend res rc
  local -a build=(docker build -f docker/Dockerfile.production-gpu --target final-oneapi2026
    --build-arg VMAF_BUILD_JOBS=4 -t "$image")
  command -v docker >/dev/null 2>&1 || {
    raise SKIP "docker is not installed"
    return
  }
  if [ -z "$IMAGE" ]; then
    [ -z "${GITHUB_TOKEN:-}" ] || build+=(--secret "id=github_token,env=GITHUB_TOKEN")
    lock_take "$SYCL_BUILD_LOCK" lock_fd || {
      raise ERROR "could not take $SYCL_BUILD_LOCK"
      return
    }
    CMD_TIMEOUT=$LONG_TIMEOUT run_cmd "${build[@]}" .
    rc=$?
    lock_drop "$lock_fd"
    [ "$rc" -eq 0 ] || {
      raise ERROR "docker build exit $rc: $(tail -n 3 "$EDIR/last.stderr" 2>/dev/null | tr '\n' ' ')"
      return
    }
  fi
  gid="$(getent group render | cut -d: -f3)"
  for backend in cpu sycl; do
    local -a run=(docker run --rm --device /dev/dri)
    [ -z "$gid" ] || run+=(--group-add "$gid")
    run+=(-e "ONEAPI_DEVICE_SELECTOR=$SYCL_SEL" -v "$REPO_ROOT/testdata:/t:ro" "$image"
      --backend "$backend" --reference /t/ref_576x324_48f.yuv --distorted /t/dis_576x324_48f.yuv
      --width 576 --height 324 --pixel_format 420 --bitdepth 8 --json --output /dev/stdout)
    lock_fd=""
    if [ "$backend" = sycl ]; then
      lock_take "${LOCK_FILE[sycl]}" lock_fd || {
        raise ERROR "could not take ${LOCK_FILE[sycl]}"
        return
      }
    fi
    log "+ $(quoted "${run[@]}") > $EDIR/image-$backend.json"
    if [ "$DRY" = 1 ]; then
      printf '    + %s\n' "$(quoted "${run[@]}")" >&2
      rc=0
    else
      timeout --kill-after=30 "$RUN_TIMEOUT" "${run[@]}" >"$EDIR/image-$backend.json" 2>"$EDIR/last.stderr"
      rc=$?
      cat "$EDIR/last.stderr" >>"$ELOG"
    fi
    lock_drop "$lock_fd"
    [ "$rc" -eq 0 ] || {
      raise ERROR "image --backend $backend exit $rc: $(tail -n 2 "$EDIR/last.stderr" | tr '\n' ' ')"
      return
    }
  done
  [ "$DRY" = 0 ] || return 0
  res="$(py image "$EDIR/image-cpu.json" "$EDIR/image-sycl.json" 5e-5)"
  case $? in
    0) note "$image on $SYCL_SEL: $res" ;;
    1) raise FAIL "$image on $SYCL_SEL: $res (bound 5e-5)" ;;
    *) raise ERROR "$image: $res" ;;
  esac
}

# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------
# A child inherits the lock descriptors held when it starts, so a child that
# outlived the kit would keep a device locked for everyone: stop the children
# when the kit is stopped.
trap 'pkill -TERM -P "$$" 2>/dev/null; exit 143' INT TERM

run_check() { # the entry's function, called by name through this map
  case "$1" in
    check_psnr_hvs) check_psnr_hvs ;;
    check_motion_v2) check_motion_v2 ;;
    check_twin_uploads) check_twin_uploads ;;
    check_motion) check_motion ;;
    check_float_ssim) check_float_ssim ;;
    check_ssimulacra2) check_ssimulacra2 ;;
    check_cambi) check_cambi ;;
    check_speed) check_speed ;;
    check_adm_aim) check_adm_aim ;;
    check_oneapi_image) check_oneapi_image ;;
    *) raise ERROR "no check function '$1'" ;;
  esac
}

build_dir_for() {
  printf '%s' "${BUILD_FOR[$1]:-$DEFAULT_BUILD}"
}

build_has() { # build-dir backend: the meson options enable it
  py buildopts "$1" "$2" >/dev/null 2>&1
}

COMMIT="$(git rev-parse --short HEAD)"
if [ -z "$OUT" ]; then
  OUT="$(build_dir_for "${SELECTED_BACKENDS[0]}")/rc3-retest/$(date -u +%Y%m%dT%H%M%SZ)-$COMMIT"
fi
mkdir -p "$OUT" || die "cannot create $OUT"
OUT="$(cd "$OUT" && pwd)"
SUMMARY="$OUT/summary.tsv"
: >"$SUMMARY"

declare -A DEVICE_OK=()
declare -A DEVICE_WHY=()
for b in "${SELECTED_BACKENDS[@]}"; do
  if detect_device "$b"; then
    DEVICE_OK[$b]=1
  else
    DEVICE_OK[$b]=0
    case "$b" in
      cuda) DEVICE_WHY[$b]="no RTX 4090 in nvidia-smi (pass --cuda-device)" ;;
      hip) DEVICE_WHY[$b]="no gfx1036 agent in rocm_agent_enumerator (pass --hip-device)" ;;
      sycl) DEVICE_WHY[$b]="no A380 in sycl-ls (pass --sycl-selector)" ;;
    esac
  fi
done

{
  echo "rc3-home-gpu-retest $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(hostname)"
  echo "commit $(git rev-parse HEAD) ($(git describe --always --dirty 2>/dev/null))"
  echo "load $(cat /proc/loadavg), $(nproc) CPUs"
  for b in "${SELECTED_BACKENDS[@]}"; do
    device_env "$b"
    echo "$b: build $(build_dir_for "$b"), ${DEV_ENV[*]:-no device}"
  done
  command -v nvidia-smi >/dev/null 2>&1 &&
    nvidia-smi --query-gpu=index,name,memory.used,memory.total,utilization.gpu --format=csv,noheader
  echo "baseline: ${BASELINE:-none}; reps $REPS; threads $THREADS; timing $TIMING"
} >"$OUT/host.txt" 2>&1
cat "$OUT/host.txt"

OVERALL=0
for i in "${!ENTRY_ROW[@]}"; do
  selected "$i" || continue
  ROW="${ENTRY_ROW[$i]}"
  B="${ENTRY_BACKEND[$i]}"
  EDIR="$OUT/$ROW/$B"
  ELOG="$EDIR/log.txt"
  mkdir -p "$EDIR"
  : >"$ELOG"
  STATUS=PASS
  [ "$DRY" = 0 ] || STATUS="DRY-RUN"
  NOTES=()
  BUILD="$(build_dir_for "$B")"
  VMAF="$REPO_ROOT/$BUILD/tools/vmaf"
  [[ $BUILD == /* ]] && VMAF="$BUILD/tools/vmaf"
  DEV_ENV=()
  TWINS=()
  GPU_ARGS=()
  CPU_ARGS=()
  GPU_TIME_ARGS=()
  CPU_TIME_ARGS=()
  printf '\n== %s [%s]\n' "$ROW" "$B"
  log "$ROW [$B]: ${ENTRY_WHAT[$i]}"
  if [ "${DEVICE_OK[$B]}" != 1 ]; then
    STATUS=SKIP
    note "${DEVICE_WHY[$B]}"
  elif [ "${ENTRY_FUNC[$i]}" != check_oneapi_image ] && [ "$DRY" = 0 ] && ! build_has "$BUILD" "$B"; then
    STATUS=SKIP
    note "$BUILD does not build the $B backend (--build-dir $B=DIR)"
  elif [ "${ENTRY_FUNC[$i]}" != check_oneapi_image ] && [ "$DRY" = 0 ] && [ ! -x "$VMAF" ]; then
    STATUS=ERROR
    note "$VMAF is missing: build $BUILD first"
  else
    missing=""
    if [ "${ENTRY_FUNC[$i]}" != check_oneapi_image ]; then
      for f in "$NETFLIX_DIR/src01_hrc00_576x324.yuv" "$NETFLIX_DIR/src01_hrc01_576x324.yuv" \
        "$BBB_DIR/ref_3840x2160_200f.yuv" "$BBB_DIR/dis_3840x2160_200f.yuv"; do
        [ -f "$f" ] || missing+=" $f"
      done
    fi
    if [ -n "$missing" ] && [ "$DRY" = 0 ]; then
      STATUS=ERROR
      note "fixture missing:$missing (docs/development/rc3-home-gpu-retest.md)"
    else
      device_env "$B"
      run_check "${ENTRY_FUNC[$i]}"
    fi
  fi
  joined="$(printf '%s; ' "${NOTES[@]}")"
  printf '%s\t%s\t%s\t%s\n' "$ROW" "$B" "$STATUS" "${joined%; }" >>"$SUMMARY"
  printf '   %s\n' "$STATUS"
  case "$STATUS" in
    FAIL) [ "$OVERALL" -eq 2 ] || OVERALL=1 ;;
    ERROR) OVERALL=2 ;;
  esac
done

py table "$SUMMARY" >"$OUT/summary.md"
printf '\n'
cat "$OUT/summary.md"
printf '\nlogs: %s\n' "$OUT"
exit "$OVERALL"
