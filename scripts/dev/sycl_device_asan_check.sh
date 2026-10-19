#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# sycl_device_asan_check.sh -- does the DPC++ device AddressSanitizer work on
# this host, and what can it see? (T-SYCL-DEVICE-SANITIZER-UNPROVEN-2026-10-05,
# docs/backends/sycl/device-sanitizer.md)
#
# Builds one probe kernel with `icpx -fsycl -Xarch_device -fsanitize=address`
# (the flags `-Dsycl_device_asan=true` puts on every SYCL translation unit) and
# runs it on the first Level Zero GPU. Every case names the outcome it expects;
# a case that comes out differently fails the check:
#
#   clean            in-bounds reads                      no report
#   past-allocation  read 64 elements past one USM block  out-of-bounds-access
#   past-block       read past the end of a two-plane block  out-of-bounds-access
#   inside-block     read the second plane of one block   no report (blind spot:
#                    an overrun that stays inside one allocation is invisible)
#   struct-pointer   pointer held in a struct captured by value
#                    null-pointer-access (toolchain limitation: the pointer
#                    reads as null; when this stops reporting, the limitation
#                    is gone and the library check can be retried)
#   no-sanitizer     the past-allocation read built WITHOUT the flag
#                    no report (negative control: proves the report in the
#                    sanitized case comes from the instrumentation)
#
# Exit: 0 every case came out as expected; 1 a case did not; 77 no icpx or no
# Level Zero GPU (skipped, nothing measured).
#
# Environment: SYCL_DEVICE_LOCK (path of a flock file to hold around every
# device run), SYCL_DEVICE_TIMEOUT (seconds per run, default 60, at most 300).
set -euo pipefail

timeout_s="${SYCL_DEVICE_TIMEOUT:-60}"
if [ "$timeout_s" -gt 300 ]; then
  timeout_s=300
fi

if ! command -v icpx >/dev/null 2>&1; then
  echo "sycl_device_asan_check: icpx not on PATH (source oneAPI setvars.sh); skipped" >&2
  exit 77
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat >"$work/probe.cpp" <<'PROBE'
#include <sycl/sycl.hpp>

#include <cstdio>
#include <cstdlib>

namespace
{
struct Args {
    const unsigned short *plane;
    unsigned short *out;
};
} // namespace

int main(int argc, char **argv)
{
    const int mode = (argc > 1) ? std::atoi(argv[1]) : 0;
    sycl::queue q{sycl::gpu_selector_v};
    const size_t n = 4096;
    unsigned short *a = sycl::malloc_device<unsigned short>(n, q);
    unsigned short *block = sycl::malloc_device<unsigned short>(2 * n, q);
    unsigned short *out = sycl::malloc_device<unsigned short>(n, q);
    q.memset(a, 1, n * 2).wait();
    q.memset(block, 2, 4 * n).wait();
    const Args args{a, out};
    q.submit([&](sycl::handler &h) {
        h.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) {
            const size_t k = i[0];
            unsigned short v = 0;
            if (mode == 1) {
                v = a[k + n + 64]; /* past one allocation */
            } else if (mode == 2) {
                v = block[k + n]; /* second plane of one block: in bounds of the block */
            } else if (mode == 3) {
                v = block[k + 2 * n + 64]; /* past the block */
            } else if (mode == 4) {
                v = args.plane[k]; /* pointer inside a struct captured by value */
            } else {
                v = a[k];
            }
            out[k] = v;
        });
    }).wait();
    unsigned short r = 0;
    q.memcpy(&r, out, 2).wait();
    std::printf("probe mode=%d r=%u\n", mode, static_cast<unsigned>(r));
    sycl::free(a, q);
    sycl::free(block, q);
    sycl::free(out, q);
    return 0;
}
PROBE

icpx -fsycl -Xarch_device -fsanitize=address -g -O2 -o "$work/probe_asan" "$work/probe.cpp"
icpx -fsycl -O2 -o "$work/probe_plain" "$work/probe.cpp"

export ONEAPI_DEVICE_SELECTOR="${ONEAPI_DEVICE_SELECTOR:-level_zero:gpu}"

if ! sycl-ls 2>/dev/null | grep -q "level_zero:gpu"; then
  echo "sycl_device_asan_check: no Level Zero GPU visible; skipped" >&2
  exit 77
fi

# run_case NAME BINARY MODE EXPECTED_PATTERN|NONE
# EXPECTED_PATTERN is a fixed string that must appear in the output; NONE means
# the output must hold no sanitizer error other than the end-of-run leak notice.
fail=0
run_case() {
  local name="$1" bin="$2" mode="$3" expect="$4" log="$work/$1.log"
  # A detected error aborts the probe and a hang is cut by the timeout, so the
  # exit status is recorded, not required to be 0.
  local status=0
  if [ -n "${SYCL_DEVICE_LOCK:-}" ]; then
    flock "$SYCL_DEVICE_LOCK" timeout "$timeout_s" "$bin" "$mode" >"$log" 2>&1 || status=$?
  else
    timeout "$timeout_s" "$bin" "$mode" >"$log" 2>&1 || status=$?
  fi
  if [ "$status" -eq 124 ]; then
    echo "FAIL  $name: hung, cut after ${timeout_s}s"
    fail=1
    return
  fi
  local errors=0 leaks=0
  errors="$(grep -c '^====ERROR: DeviceSanitizer: ' "$log")" || errors=0
  leaks="$(grep -c 'detected memory leaks' "$log")" || leaks=0
  local real=$((errors - leaks))
  if [ "$expect" = "NONE" ]; then
    if [ "$real" -eq 0 ] && grep -q '^probe mode=' "$log"; then
      echo "ok    $name: no report, kernel completed"
    else
      echo "FAIL  $name: expected no report"
      sed -n '1,6p' "$log"
      fail=1
    fi
  elif grep -qF -- "$expect" "$log"; then
    echo "ok    $name: reported $expect"
  else
    echo "FAIL  $name: expected '$expect', got:"
    sed -n '1,6p' "$log"
    fail=1
  fi
}

run_case clean "$work/probe_asan" 0 NONE
run_case past-allocation "$work/probe_asan" 1 "out-of-bounds-access"
run_case past-block "$work/probe_asan" 3 "out-of-bounds-access"
run_case inside-block "$work/probe_asan" 2 NONE
run_case struct-pointer "$work/probe_asan" 4 "null-pointer-access"
run_case no-sanitizer "$work/probe_plain" 1 NONE

if [ "$fail" -ne 0 ]; then
  echo "sycl_device_asan_check: FAILED" >&2
  exit 1
fi
echo "sycl_device_asan_check: every case came out as expected"
