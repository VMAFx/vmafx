#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 luoye-cpu
#
# scripts/ci/tests/test-toolchain-probe-mingw.sh — planted-failure cases for
# the MinGW cross entry of scripts/dev/toolchain-probe.sh (issue #3307).
#
# Follows the stub pattern of scripts/ci/test-check-toolchain-claims.sh:
# fake toolchain prefixes with a stub gcc printing a chosen thread model,
# -dumpversion and -print-file-name answers, and a stub objdump printing a
# crafted import table, so every refusal path runs without a real MinGW
# cross toolchain.
#
#   - Positive: win32-model driver + win32-flavoured runtime -> exit 0
#   - Positive: posix-model driver + posix-flavoured runtime -> exit 0
#   - Planted negative: win32-model driver + libwinpthread-importing runtime -> exit 1
#   - Planted negative: posix-model driver + win32-flavoured runtime -> exit 1
#   - Boundary: "13-win32" -dumpversion with resolvable DLLs -> exit 0 (no path
#     is rebuilt from the version string)
#   - Planted negative: "13-win32" -dumpversion with unresolvable DLLs -> exit 1
#   - Boundary: no cross driver in the prefix -> exit 0, absent line
#   - Planted negative: driver prints no thread model -> exit 1
#   - Planted negative: no objdump to read the import table -> exit 1
#   - Large-output (PR #3317 review): a ~2 MiB import table with the
#     libwinpthread import early or late; win32 must refuse (B24) and posix
#     must pass. The old printf|grep -q form died of SIGPIPE (pipeline
#     status 141) on an early match and misreported the imports as "none";
#     the here-string read does not.

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
probe="${repo_root}/scripts/dev/toolchain-probe.sh"

if [ ! -x "${probe}" ]; then
  echo "test-toolchain-probe-mingw: probe script missing or non-executable: ${probe}" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT INT TERM

pass_count=0
fail_count=0
probe_out=""
probe_rc=0

run_probe() {
  local prefix="$1"
  probe_rc=0
  probe_out="$(MINGW_CROSS_PREFIX="${prefix}" "${probe}" 2>&1)" || probe_rc=$?
}

expect_rc() {
  local name="$1" expected="$2"
  if [ "${probe_rc}" -eq "${expected}" ]; then
    echo "PASS: ${name} (exit=${probe_rc})"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: ${name} (got exit=${probe_rc}, expected ${expected})"
    echo "----- captured output -----"
    printf '%s\n' "${probe_out}"
    echo "---------------------------"
    fail_count=$((fail_count + 1))
  fi
}

expect_grep() {
  local name="$1" pattern="$2"
  if printf '%s\n' "${probe_out}" | grep -qF -e "${pattern}"; then
    echo "PASS: ${name}"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: ${name} (pattern not in probe output: ${pattern})"
    echo "----- captured output -----"
    printf '%s\n' "${probe_out}"
    echo "---------------------------"
    fail_count=$((fail_count + 1))
  fi
}

expect_no_grep() {
  local name="$1" pattern="$2"
  if ! printf '%s\n' "${probe_out}" | grep -qF -e "${pattern}"; then
    echo "PASS: ${name}"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: ${name} (unwanted pattern in probe output: ${pattern})"
    echo "----- captured output -----"
    printf '%s\n' "${probe_out}"
    echo "---------------------------"
    fail_count=$((fail_count + 1))
  fi
}

# Precondition for the large-output cases: the stub objdump really emits a
# multi-MB import table (the premise the SIGPIPE regression rides on).
expect_large_stub() {
  local name="$1" dir="$2" bytes=""
  bytes="$("${dir}/bin/x86_64-w64-mingw32-objdump" -p "${dir}/runtime/libstdc++-6.dll" 2>/dev/null | wc -c)"
  bytes="${bytes//[[:space:]]/}"
  if [ "${bytes}" -gt 1000000 ]; then
    echo "PASS: ${name} (stub import table ${bytes} bytes)"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: ${name} (stub import table only ${bytes} bytes, expected >1 MB)"
    fail_count=$((fail_count + 1))
  fi
}

# build_mingw_stub <dir> <model: win32|posix|none> <dumpversion>
#                  <winpthread-import: yes|no> <resolve: yes|no>
#                  [<large: none|early|late>]
# Plants a fake toolchain prefix: a stub cross driver answering --version,
# -dumpversion, -v and -print-file-name=<lib>, fake runtime DLLs, and a stub
# target objdump printing the crafted import table of the planted flavour.
# large=early|late swaps in a ~2 MiB import table whose libwinpthread line
# sits in the first bytes (early) or after the filler (late) — the shape
# that made the old printf|grep -q read die of SIGPIPE under pipefail.
# write_stub_gcc <dir> <model_line> <dumpversion> <resolve>
# Plants the stub cross driver answering --version, -dumpversion, -v and
# -print-file-name=<lib> (issue #3307).
write_stub_gcc() {
  local dir="$1" model_line="$2" dumpversion="$3" resolve="$4"
  cat >"${dir}/bin/x86_64-w64-mingw32-gcc" <<EOF
#!/bin/sh
# Stub cross driver planted by test-toolchain-probe-mingw.sh (issue #3307).
STUB_MODEL_LINE="${model_line}"
STUB_DUMPVERSION="${dumpversion}"
STUB_STAGE="${dir}"
STUB_RESOLVE="${resolve}"
case "\$1" in
  --version)
    echo "x86_64-w64-mingw32-gcc (GCC) 13.2.0 (stub)"
    ;;
  -dumpversion)
    echo "\${STUB_DUMPVERSION}"
    ;;
  -v)
    printf 'Using built-in specs.\nTarget: x86_64-w64-mingw32\n%s\ngcc version 13.2.0 (GCC)\n' "\${STUB_MODEL_LINE}"
    ;;
  -print-file-name=*)
    if [ "\${STUB_RESOLVE}" = yes ]; then
      echo "\${STUB_STAGE}/runtime/\${1#-print-file-name=}"
    else
      echo "\${1#-print-file-name=}"
    fi
    ;;
  *)
    echo "stub gcc: unsupported argument: \$1" >&2
    exit 1
    ;;
esac
EOF
  chmod +x "${dir}/bin/x86_64-w64-mingw32-gcc"
}

# write_stub_objdump <dir> <winpthread: yes|no> <large: none|early|late>
# Plants the stub target objdump printing the crafted import table of the
# planted flavour. The large=early|late SIGPIPE shape is documented on
# build_mingw_stub above.
write_stub_objdump() {
  local dir="$1" winpthread="$2" large="$3"
  cat >"${dir}/bin/x86_64-w64-mingw32-objdump" <<EOF
#!/bin/sh
# Stub objdump planted by test-toolchain-probe-mingw.sh (issue #3307): the
# crafted import table of the planted flavour.
STUB_WINPTHREAD="${winpthread}"
STUB_LARGE="${large}"
if [ "\$1" != "-p" ]; then
  echo "stub objdump: unsupported arguments: \$*" >&2
  exit 1
fi
case "\$2" in
  *libstdc++-6.dll)
    if [ "\${STUB_LARGE}" != none ]; then
      # ~2 MiB import table (29000 filler lines x 69 bytes): the
      # libwinpthread import in the first bytes (early) or after the
      # filler (late). With the old printf|grep -q form in the probe, an
      # early match lets grep exit first and the capture pipeline dies of
      # SIGPIPE under pipefail — status 141 read as "no match" (PR #3317).
      printf 'peer\n\tDLL Name: libstdc++-6.dll\n'
      if [ "\${STUB_LARGE}" = early ]; then
        printf '\tDLL Name: libwinpthread-1.dll\n'
      fi
      awk 'BEGIN { for (i = 0; i < 29000; i++) print "import-filler-line 0123456789012345678901234567890123456789012345"; }'
      if [ "\${STUB_LARGE}" = late ]; then
        printf '\tDLL Name: libwinpthread-1.dll\n'
      fi
    elif [ "\${STUB_WINPTHREAD}" = yes ]; then
      printf 'peer\n\tDLL Name: libstdc++-6.dll\n\tDLL Name: libwinpthread-1.dll\n'
    else
      printf 'peer\n\tDLL Name: libstdc++-6.dll\n'
    fi
    ;;
  *)
    printf 'peer\n\tDLL Name: libwinpthread-1.dll\n'
    ;;
esac
exit 0
EOF
  chmod +x "${dir}/bin/x86_64-w64-mingw32-objdump"
}

build_mingw_stub() {
  local dir="$1" model="$2" dumpversion="$3" winpthread="$4" resolve="$5"
  local large="${6:-none}"
  local model_line=""
  if [ "${model}" != "none" ]; then
    model_line="Thread model: ${model}"
  fi
  mkdir -p "${dir}/bin" "${dir}/runtime"
  for f in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    printf 'planted payload, not a PE: the stub objdump owns the import table\n' >"${dir}/runtime/${f}"
  done
  write_stub_gcc "${dir}" "${model_line}" "${dumpversion}" "${resolve}"
  write_stub_objdump "${dir}" "${winpthread}" "${large}"
}

# ---------- Case 1: Positive — win32 model + win32-flavoured runtime ----------
build_mingw_stub "${work}/case1" win32 13.2.0 no yes
run_probe "${work}/case1"
expect_rc "positive: win32 driver + win32 runtime -> exit 0" 0
expect_grep "case1 block shows the thread model" "thread-model win32"
expect_grep "case1 block shows the flavored dumpversion" "dumpversion 13.2.0"
expect_grep "case1 resolves libstdc++-6.dll via -print-file-name" "runtime libstdc++-6.dll="
expect_grep "case1 resolves libgcc_s_seh-1.dll via -print-file-name" "runtime libgcc_s_seh-1.dll="
expect_grep "case1 pairing asserted" "pairing ok"
expect_no_grep "case1 no refusal lines" "pairing-mismatch"
expect_no_grep "case1 no unresolved lines" "runtime-unresolved"

# ---------- Case 2: Planted negative — win32 model + posix-flavoured runtime (B24) ----------
build_mingw_stub "${work}/case2" win32 13.2.0 yes yes
run_probe "${work}/case2"
expect_rc "planted: win32 driver + libwinpthread-importing runtime -> exit 1" 1
expect_grep "case2 refusal names the pairing mismatch" "pairing-mismatch"
expect_grep "case2 refusal names the thread model" "thread-model win32"
expect_grep "case2 refusal names the B24 regression" "B24"

# ---------- Case 3: Positive — posix model + posix-flavoured runtime ----------
build_mingw_stub "${work}/case3" posix 13.2.0 yes yes
run_probe "${work}/case3"
expect_rc "positive: posix driver + posix runtime -> exit 0" 0
expect_grep "case3 block shows the thread model" "thread-model posix"
expect_grep "case3 pairing asserted" "pairing ok"

# ---------- Case 4: Planted negative — posix model + win32-flavoured runtime ----------
build_mingw_stub "${work}/case4" posix 13.2.0 no yes
run_probe "${work}/case4"
expect_rc "planted: posix driver + runtime without libwinpthread -> exit 1" 1
expect_grep "case4 refusal names the pairing mismatch" "pairing-mismatch thread-model posix"

# ---------- Case 5: Boundary — "13-win32" -dumpversion with resolvable DLLs ----------
build_mingw_stub "${work}/case5" win32 13-win32 no yes
run_probe "${work}/case5"
expect_rc "boundary: 13-win32 dumpversion with resolvable DLLs -> exit 0" 0
expect_grep "case5 block shows the flavored version string" "dumpversion 13-win32"
expect_grep "case5 DLLs still resolved (no path rebuilt from -dumpversion)" "runtime libstdc++-6.dll="
expect_grep "case5 pairing still asserted" "pairing ok"

# ---------- Case 6: Planted negative — "13-win32" -dumpversion, nothing resolvable ----------
build_mingw_stub "${work}/case6" win32 13-win32 no no
run_probe "${work}/case6"
expect_rc "planted: 13-win32 dumpversion with unresolvable DLLs -> exit 1" 1
expect_grep "case6 shows the flavored version string" "dumpversion 13-win32"
expect_grep "case6 refuses the unresolved libstdc++-6.dll" "runtime-unresolved libstdc++-6.dll"
expect_grep "case6 refuses the unresolved libgcc_s_seh-1.dll" "runtime-unresolved libgcc_s_seh-1.dll"
expect_no_grep "case6 no pairing lines" "pairing"

# ---------- Case 7: Boundary — no cross driver in the prefix -> absent, exit 0 ----------
mkdir -p "${work}/case7/bin"
run_probe "${work}/case7"
expect_rc "boundary: no cross driver -> exit 0 with an absent line" 0
expect_grep "case7 absent line names the driver" "x86_64-w64-mingw32-gcc absent searched="
expect_grep "case7 absent line names the searched prefix" "${work}/case7/bin"
expect_no_grep "case7 no pairing lines" "pairing"

# ---------- Case 8: Planted negative — driver prints no thread model ----------
build_mingw_stub "${work}/case8" none 13.2.0 no yes
run_probe "${work}/case8"
expect_rc "planted: no thread-model line -> exit 1" 1
expect_grep "case8 block shows the unknown model" "thread-model unknown"
expect_grep "case8 refusal says the pairing is unverified" "pairing-unverified"
expect_no_grep "case8 no green pairing line" "pairing ok"

# ---------- Case 9: Planted negative — no objdump to read the import table ----------
build_mingw_stub "${work}/case9" win32 13.2.0 no yes
rm -f "${work}/case9/bin/x86_64-w64-mingw32-objdump"
run_probe "${work}/case9"
# With a plain objdump on PATH the fake DLL is still unreadable, so both the
# "no objdump found" and the "objdump exited non-zero" paths land here.
expect_rc "planted: no readable import table -> exit 1" 1
expect_grep "case9 refusal says the pairing is unverified" "pairing-unverified"
expect_no_grep "case9 no green pairing line" "pairing ok"

# ---------- Case 10: Large-output — ~2 MiB table, libwinpthread early, win32 ----------
build_mingw_stub "${work}/case10" win32 13.2.0 yes yes early
expect_large_stub "case10 stub emits a multi-MB import table (early)" "${work}/case10"
run_probe "${work}/case10"
expect_rc "large early: win32 driver + libwinpthread-importing runtime -> exit 1" 1
expect_grep "case10 refusal names the pairing mismatch" "pairing-mismatch thread-model win32"
expect_grep "case10 refusal names the B24 regression" "B24"

# ---------- Case 11: Large-output — ~2 MiB table, libwinpthread late, win32 ----------
build_mingw_stub "${work}/case11" win32 13.2.0 yes yes late
expect_large_stub "case11 stub emits a multi-MB import table (late)" "${work}/case11"
run_probe "${work}/case11"
expect_rc "large late: win32 driver + libwinpthread-importing runtime -> exit 1" 1
expect_grep "case11 refusal names the pairing mismatch" "pairing-mismatch thread-model win32"
expect_grep "case11 refusal names the B24 regression" "B24"

# ---------- Case 12: Large-output — ~2 MiB table, libwinpthread early, posix ----------
build_mingw_stub "${work}/case12" posix 13.2.0 yes yes early
expect_large_stub "case12 stub emits a multi-MB import table (early)" "${work}/case12"
run_probe "${work}/case12"
expect_rc "large early: posix driver + posix runtime -> exit 0" 0
expect_grep "case12 pairing asserted over a multi-MB table" "pairing ok thread-model posix"
expect_no_grep "case12 no refusal lines" "pairing-mismatch"

# ---------- Case 13: Large-output — ~2 MiB table, libwinpthread late, posix ----------
build_mingw_stub "${work}/case13" posix 13.2.0 yes yes late
expect_large_stub "case13 stub emits a multi-MB import table (late)" "${work}/case13"
run_probe "${work}/case13"
expect_rc "large late: posix driver + posix runtime -> exit 0" 0
expect_grep "case13 pairing asserted over a multi-MB table" "pairing ok thread-model posix"
expect_no_grep "case13 no refusal lines" "pairing-mismatch"

echo ""
echo "test-toolchain-probe-mingw: ${pass_count} passed, ${fail_count} failed"

if [ "${fail_count}" -ne 0 ]; then
  exit 1
fi

exit 0
