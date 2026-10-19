# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The environment of the Go binaries and the chart (ADR-2350 D13).

Positive: a small definition emits the exact CompoundKeys file, table and
template; the real definition reproduces the committed outputs. Negative:
every planted definition defect is refused; a hand edit of the generated
template fails the drift check. Boundary: per-binary overrides, a variable
read directly (no key), an entry in two workloads at two indentations, the
chart values column.
"""

from __future__ import annotations

import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from typing import Any

import tomllib
from support import ROOT
from vmafx_api import cli, emit_config
from vmafx_api.config import compound_keys, env_of, parse_config
from vmafx_api.loader import load
from vmafx_api.model import DefinitionError

MINI = """
[[config_binaries]]
name = "demo"
dir = "cmd/vmafx-demo"
page = "docs/demo.md"
workloads = ["demo", "demo-job"]

[[config]]
env = "VMAFX_HTTP_ADDR"
key = "http.addr"
binaries = ["demo"]
type = "`host:port`"
default = "`:8080`"
doc = "HTTP listen address."

[config.demo]
default = "`:9090`"

[[config]]
env = "VMAFX_GRPC_MAX_RECV_SIZE"
key = "grpc.max_recv_size"
binaries = ["demo"]
type = "bytes"
default = "_(gRPC default)_"
doc = "Largest message | received."

[[config]]
env = "VMAFX_TOKEN"
binaries = ["demo"]
type = "string"
default = "_(unset)_"
doc = "Bearer token."
secret = true

[[chart_maps]]
name = "demo.backend"
source = ".Values.gpu.vendor"
cases = { nvidia = "cuda" }
default = "cpu"
doc = "the backend."

[[chart_workloads]]
name = "demo"
indent = 12
doc = "the demo container."

[[chart_workloads]]
name = "demo-job"
indent = 8
doc = "the demo job."

[[chart_env]]
env = "VMAFX_HTTP_ADDR"
workloads = ["demo", "demo-job"]
comment = "# Pinned to the chart's port."
value = '":{{ .Values.demo.port }}"'

[[chart_env]]
env = "VMAFX_TOKEN"
workloads = ["demo"]
when = ".Values.demo.tokenSecret"
secret_ref = { name = "{{ .Values.demo.tokenSecret | quote }}", key = "token" }
"""

MINI_TEMPLATE_BODY = """{{/*
demo.backend — the backend.
*/}}
{{- define "demo.backend" -}}
{{- if eq .Values.gpu.vendor "nvidia" -}}
cuda
{{- else -}}
cpu
{{- end }}
{{- end }}

{{/*
vmafx.env.demo — the demo container.
*/}}
{{- define "vmafx.env.demo" }}
            # Pinned to the chart's port.
            - name: VMAFX_HTTP_ADDR
              value: ":{{ .Values.demo.port }}"
{{- if .Values.demo.tokenSecret }}
            - name: VMAFX_TOKEN
              valueFrom:
                secretKeyRef:
                  name: {{ .Values.demo.tokenSecret | quote }}
                  key: token
{{- end }}
{{- end }}

{{/*
vmafx.env.demo-job — the demo job.
*/}}
{{- define "vmafx.env.demo-job" }}
        # Pinned to the chart's port.
        - name: VMAFX_HTTP_ADDR
          value: ":{{ .Values.demo.port }}"
{{- end }}
"""


def mini() -> dict[str, Any]:
    data: dict[str, Any] = tomllib.loads(MINI)
    return data


def config_of(data: dict[str, Any]) -> Any:
    config = parse_config(data)
    assert config is not None
    return config


class ConfigEmitTest(unittest.TestCase):
    def test_compound_keys_file(self) -> None:
        config = config_of(mini())
        self.assertEqual(compound_keys(config, "demo"), ["grpc.max_recv_size"])
        text = emit_config.go_files(config)["cmd/vmafx-demo/config_keys.gen.go"]
        self.assertIn('var compoundKeys = []string{\n\t"grpc.max_recv_size",\n}\n', text)
        prose = " ".join(text.replace("//", "").split())
        self.assertIn("VMAFX_GRPC_MAX_RECV_SIZE reaches grpc.max_recv_size", prose)

    def test_table(self) -> None:
        config = config_of(mini())
        rows = emit_config.table(config, config.binaries[0])
        self.assertEqual(
            rows[3],
            "| `VMAFX_HTTP_ADDR` | `http.addr` | `host:port` | `:9090` | `demo.port` | HTTP listen address. |",
        )
        self.assertIn("Largest message \\| received.", rows[4])
        self.assertEqual(
            rows[5],
            "| `VMAFX_TOKEN` | read directly | string | _(unset)_ | `demo.tokenSecret` | Secret. Bearer token. |",
        )

    def test_template(self) -> None:
        text = emit_config.template_text(config_of(mini()))
        spdx = "SPDX-" + "License-Identifier"  # split: not this file's header
        self.assertTrue(text.startswith("{{- /*\n  " + spdx + ": EUPL-1.2"))
        self.assertIn("DO NOT EDIT.", text)
        self.assertTrue(text.endswith(MINI_TEMPLATE_BODY), text)

    def test_env_names(self) -> None:
        self.assertEqual(env_of("auth.tenants.refresh"), "VMAFX_AUTH_TENANTS_REFRESH")
        self.assertEqual(env_of("operator.metrics_addr"), "VMAFX_OPERATOR_METRICS_ADDR")


def _refused(test: unittest.TestCase, mutate: Any, message: str) -> None:
    data = mini()
    mutate(data)
    with test.assertRaisesRegex(DefinitionError, message):
        parse_config(data)


DEFECTS: list[tuple[str, Any, str]] = [
    ("key and name disagree", lambda d: d["config"][0].update(key="http.address"), "maps to"),
    ("lower-case name", lambda d: d["config"][2].update(env="vmafx_token"), "upper case"),
    ("unknown binary", lambda d: d["config"][0].update(binaries=["demo", "x"]), "not in"),
    ("variable twice", lambda d: d["config"].append(dict(d["config"][0])), "appears twice"),
    ("unknown key", lambda d: d["config"][0].update(colour="x"), "unknown keys"),
    ("override of the name", lambda d: d["config"][0].update(demo={"env": "X"}), "overrides only"),
    ("secret not a flag", lambda d: d["config"][2].update(secret=1), "true or false"),
    ("unknown workload", lambda d: d["chart_env"][0].update(workloads=["nope"]), "not in"),
    ("not read by the binary", lambda d: d["chart_env"][0].update(env="VMAFX_OTHER"), "reads no"),
    ("two value sources", lambda d: d["chart_env"][0].update(field_ref="metadata.name"), "exactly one"),
    ("no value source", lambda d: d["chart_env"][0].pop("value"), "exactly one"),
    ("comment without #", lambda d: d["chart_env"][0].update(comment="Pinned."), "start with #"),
    ("secret_ref shape", lambda d: d["chart_env"][1].update(secret_ref={"name": "x"}), "`name` and a `key`"),
    ("workload twice", lambda d: d["chart_workloads"].append(dict(d["chart_workloads"][0])), "appears twice"),
    ("negative indent", lambda d: d["chart_workloads"][0].update(indent=-1), "column number"),
    ("map cases", lambda d: d["chart_maps"][0].update(cases={"nvidia": 1}), "maps values to strings"),
]  # fmt: skip


class ConfigValidationTest(unittest.TestCase):
    def test_planted_definition_defects_are_refused(self) -> None:
        for label, mutate, message in DEFECTS:
            with self.subTest(label):
                _refused(self, mutate, message)

    def test_without_tables_there_is_no_config(self) -> None:
        self.assertIsNone(parse_config({}))


def quiet(function: Any, *args: Any) -> int:
    with redirect_stdout(io.StringIO()):
        code: int = function(*args)
    return code


class RealDefinitionTest(unittest.TestCase):
    def test_hand_edit_of_the_template_fails_the_drift_check(self) -> None:
        core = load(ROOT / "core" / "api" / "vmafx.toml")
        files = {
            path: text
            for path, text in cli.render(core, ROOT, cli.platform_of(core, ROOT)).items()
            if path == emit_config.TEMPLATE or path.endswith(emit_config.GO_FILE)
        }
        self.assertIn(emit_config.TEMPLATE, files)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            quiet(cli.write, root, files)
            self.assertEqual(quiet(cli.check, root, files), 0)
            target = root / emit_config.TEMPLATE
            target.write_text(target.read_text() + "{{/* hand edit */}}\n")
            self.assertEqual(quiet(cli.check, root, files), 1)

    def test_committed_template_and_keys_are_current(self) -> None:
        core = load(ROOT / "core" / "api" / "vmafx.toml")
        platform = cli.platform_of(core, ROOT)
        assert platform is not None and platform.config is not None
        for path, text in emit_config.files(platform.config).items():
            with self.subTest(path):
                self.assertEqual((ROOT / path).read_text(encoding="utf-8"), text)


if __name__ == "__main__":
    unittest.main()
