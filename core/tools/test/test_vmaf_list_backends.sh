#!/bin/sh
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# ADR-1874: `vmaf --list-backends` reports, as JSON, every backend in the order
# cpu, cuda, sycl, hip, metal, with `compiled` equal to this build's
# configuration. A backend that is not compiled in is never usable; cpu always
# is. Which compiled GPU backends are usable depends on the machine, so the
# test checks only that the field is a boolean.
#
#   test_vmaf_list_backends.sh cuda=<bool> sycl=<bool> hip=<bool> metal=<bool>
set -eu

out="list_backends.json"
./tools/vmaf --list-backends >"$out"
./tools/vmaf --list-backends --reference missing.yuv >/dev/null

python3 - "$out" "$@" <<'PY'
import json
import sys

path, *built = sys.argv[1:]
want = {"cpu": True}
for item in built:
    name, _, value = item.partition("=")
    want[name] = value == "true"
doc = json.load(open(path, encoding="utf-8"))
rows = doc["backends"]
names = [row["name"] for row in rows]
problems = []
if names != ["cpu", "cuda", "sycl", "hip", "metal"]:
    problems.append(f"backend order {names}")
for row in rows:
    name = row["name"]
    if row["compiled"] is not want.get(name):
        problems.append(f"{name}: compiled={row['compiled']}, build says {want.get(name)}")
    if not isinstance(row["usable"], bool):
        problems.append(f"{name}: usable is not a boolean")
    if not row["compiled"] and row["usable"]:
        problems.append(f"{name}: usable but not compiled")
    if row["compiled"] and not row["usable"] and not isinstance(row.get("init_status"), int):
        problems.append(f"{name}: compiled and unusable without init_status")
if rows and not (rows[0]["compiled"] and rows[0]["usable"]):
    problems.append("cpu must be compiled and usable")
if problems:
    print("FAIL: " + "; ".join(problems))
    sys.exit(1)
print("ok: " + ", ".join(f"{r['name']}={r['compiled']}/{r['usable']}" for r in rows))
PY
