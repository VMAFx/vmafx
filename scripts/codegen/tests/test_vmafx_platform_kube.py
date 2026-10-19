# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Kubernetes part of the platform definition and its Go emitter (ADR-2350 D13).

Positive: a small definition with every validation key parses and emits the
kubebuilder markers controller-gen reads; the real definition emits the
committed api/vmafx/v1 files. Negative: every planted definition defect is
refused. Boundary: derived Go names, a resource without short names or a
status subresource, gofmt-clean output.
"""

from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path
from typing import Any

from support import ROOT, document, run, tool
from vmafx_api import emit_kube_types, emit_proto
from vmafx_api.kube import go_name, parse_kube
from vmafx_api.loader import load
from vmafx_api.model import DefinitionError
from vmafx_api.platform import DEFINITION, external_messages, parse

GROUP = {
    "name": "g",
    "group": "demo.dev",
    "version": "v1",
    "path": "api/demo/v1",
    "go_package": "example.com/api/demo/v1",
    "doc": "Package v1 holds the demo.dev/v1 types.",
}


def _spec_fields() -> list[dict[str, Any]]:
    return [
        {"name": "size", "type": "int32", "optional": True, "minimum": 0, "default": 1, "doc": "S."},
        {"name": "tenantId", "type": "string", "pattern": "^[a-z]+$", "doc": "Tenant."},
        {"name": "enabled", "type": "bool", "optional": True, "pointer": True, "default": True,
         "doc": "On."},
        {"name": "roles", "type": "string", "repeated": True, "optional": True,
         "items_enum": ["a", "b"], "default": ["a"], "max_items": 4, "doc": "Roles."},
        {"name": "issuer", "type": "string", "format": "uri", "min_length": 1, "doc": "Issuer."},
    ]  # fmt: skip


def mini() -> dict[str, Any]:
    """A definition with one resource reaching every validation key."""
    status = [
        {"name": "phase", "type": "Phase", "optional": True, "doc": "Phase."},
        {"name": "seen", "type": "Time", "optional": True, "doc": "Seen."},
        {"name": "labels", "type": "map<string, string>", "optional": True, "doc": "Labels."},
    ]
    return {
        "groups": [dict(GROUP)],
        "enums": [
            {
                "name": "Phase",
                "group": "g",
                "doc": "Phase of a widget.",
                "values": [{"name": "Up", "doc": "Running."}, {"name": "Down"}],
            },
        ],
        "messages": [
            {"name": "WidgetSpec", "group": "g", "doc": "Spec.", "fields": _spec_fields()},
            {"name": "WidgetStatus", "group": "g", "doc": "Status.", "fields": status},
        ],
        "resources": [
            {
                "kind": "Widget",
                "group": "g",
                "doc": "Widget is a demo.",
                "short_names": ["wd"],
                "spec": "WidgetSpec",
                "status": "WidgetStatus",
                "spec_required": True,
                "printer_columns": [
                    {"name": "Phase", "type": "string", "json_path": ".status.phase"}
                ],
            },
        ],
    }


def field(data: dict[str, Any], message: str, name: str) -> dict[str, Any]:
    fields = next(m for m in data["messages"] if m["name"] == message)["fields"]
    found: dict[str, Any] = next(f for f in fields if f["name"] == name)
    return found


class KubeParseTest(unittest.TestCase):
    def test_mini_definition_parses(self) -> None:
        kube = parse_kube(mini())
        self.assertEqual([r.kind for r in kube.resources], ["Widget"])
        self.assertEqual(kube.resources[0].plural, "widgets")
        spec = kube.messages[0]
        self.assertEqual([f.go for f in spec.fields][:2], ["Size", "TenantID"])
        self.assertEqual(dict(spec.fields[3].validation)["items_enum"], ["a", "b"])

    def test_real_definition_has_the_four_resources(self) -> None:
        kube = parse_kube(document(ROOT / DEFINITION))
        kinds = {r.kind: r for r in kube.resources}
        self.assertEqual(set(kinds), {"VmafxJob", "VmafxNode", "VmafxModelTraining", "VmafxTenant"})
        self.assertTrue(kinds["VmafxTenant"].spec_required)
        self.assertFalse(kinds["VmafxJob"].spec_required)

    def _refused(self, mutate: Any, message: str) -> None:
        data = mini()
        mutate(data)
        with self.assertRaisesRegex(DefinitionError, message):
            parse_kube(data)

    def test_planted_definition_defects_are_refused(self) -> None:
        cases: list[tuple[str, Any, str]] = [
            ("unknown type", lambda d: field(d, "WidgetSpec", "size").update(type="Nope"),
             "neither a Kubernetes scalar"),
            ("unknown group", lambda d: d["messages"][0].update(group="h"), "not in"),
            ("default on required", lambda d: field(d, "WidgetSpec", "tenantId").update(default="x"),
             "a field with a default is optional"),
            ("pointer on list", lambda d: field(d, "WidgetSpec", "roles").update(pointer=True),
             "single value"),
            ("pointer on map", lambda d: field(d, "WidgetStatus", "labels").update(pointer=True),
             "single value"),
            ("enum on enum type", lambda d: field(d, "WidgetStatus", "phase").update(enum=["Up"]),
             "carries its values"),
            ("type named twice", lambda d: d["enums"][0].update(name="WidgetSpec"), "appears twice"),
            ("list name taken", lambda d: d["enums"][0].update(name="WidgetList"), "appears twice"),
            ("field twice", lambda d: d["messages"][0]["fields"].append(
                dict(field(d, "WidgetSpec", "size"))), "appears twice"),
            ("Go name twice", lambda d: field(d, "WidgetSpec", "issuer").update(go="Size"),
             "appears twice"),
            ("bad scope", lambda d: d["resources"][0].update(scope="Global"), "Namespaced or Cluster"),
            ("scalar spec", lambda d: d["resources"][0].update(spec="string"), "is not a message"),
            ("enum status", lambda d: d["resources"][0].update(status="Phase"), "is not a message"),
            ("column type", lambda d: d["resources"][0]["printer_columns"][0].update(type="text"),
             "is not one of"),
            ("JSON name", lambda d: field(d, "WidgetSpec", "size").update(name="Size"),
             "lowerCamelCase"),
            ("Go name", lambda d: field(d, "WidgetSpec", "size").update(go="size"), "not exported"),
            ("kind", lambda d: d["resources"][0].update(kind="widget"), "exported Go name"),
            ("flag type", lambda d: field(d, "WidgetSpec", "size").update(optional="yes"),
             "true or false"),
            ("empty doc", lambda d: field(d, "WidgetSpec", "size").update(doc=" "), "non-empty"),
            ("group twice", lambda d: d["groups"].append(dict(GROUP)), "appears twice"),
            ("nested default", lambda d: field(d, "WidgetSpec", "roles").update(default=[["a"]]),
             "a scalar or a list of scalars"),
            ("table default", lambda d: field(d, "WidgetSpec", "size").update(default={"a": 1}),
             "a scalar or a list of scalars"),
        ]  # fmt: skip
        for label, mutate, message in cases:
            with self.subTest(label):
                self._refused(mutate, message)

    def test_name_shared_with_a_protobuf_type_is_refused(self) -> None:
        data = document(ROOT / DEFINITION)
        clash = copy.deepcopy(next(m for m in data["messages"] if "group" in m))
        clash["name"] = next(m["name"] for m in data["messages"] if "file" in m)
        data["messages"].append(clash)
        external = external_messages(
            load(ROOT / "core" / "api" / "vmafx.toml"),
            emit_proto.IMPORT_PATH,
            emit_proto.PACKAGE,
            emit_proto.GO_PACKAGE,
        )
        with self.assertRaisesRegex(DefinitionError, "both a protobuf and a Kubernetes"):
            parse(data, external)

    def test_go_names(self) -> None:
        cases = {
            "id": "ID",
            "x": "X",
            "tenantId": "TenantID",
            "gpuVendor": "GPUVendor",
            "jwksEndpoint": "JwksEndpoint",
            "oidc": "OIDC",
            "sourceUri": "SourceURI",
            "maxGpu2": "MaxGpu2",
        }
        for json_name, want in cases.items():
            with self.subTest(json_name):
                self.assertEqual(go_name(json_name), want)


class KubeEmitTest(unittest.TestCase):
    def test_markers_of_every_validation_key(self) -> None:
        files = emit_kube_types.files(parse_kube(mini()))
        self.assertEqual(
            set(files), {"api/demo/v1/groupversion_info.go", "api/demo/v1/widget_types.go"}
        )
        text = files["api/demo/v1/widget_types.go"]
        for want in (
            "// Code generated by scripts/codegen/vmafx-api.py",
            "// +kubebuilder:validation:Minimum=0",
            "// +kubebuilder:default:=1",
            "// +kubebuilder:validation:Pattern=`^[a-z]+$`",
            'Enabled *bool `json:"enabled,omitempty"`',
            "// +kubebuilder:default:=true",
            "// +kubebuilder:validation:items:Enum=a;b",
            '// +kubebuilder:default:={"a"}',
            "// +kubebuilder:validation:MaxItems=4",
            "// +kubebuilder:validation:Format=uri",
            "// +kubebuilder:validation:MinLength=1",
            'TenantID string `json:"tenantId"`',
            'Seen *metav1.Time `json:"seen,omitempty"`',
            'Labels map[string]string `json:"labels,omitempty"`',
            "// +kubebuilder:validation:Enum=Up;Down",
            'PhaseUp Phase = "Up"',
            "// +kubebuilder:subresource:status",
            "// +kubebuilder:resource:scope=Namespaced,shortName=wd",
            '// +kubebuilder:printcolumn:name="Phase",type="string",JSONPath=".status.phase"',
            'Spec   WidgetSpec   `json:"spec"`',
            "SchemeBuilder.Register(&Widget{}, &WidgetList{})",
            "func (in *Widget) DeepCopyInto(out *Widget) {\n\tdeepCopyResource(in.TypeMeta,",
        ):
            with self.subTest(want):
                self.assertIn(want, text)
        group = files["api/demo/v1/groupversion_info.go"]
        self.assertIn("// +groupName=demo.dev", group)
        self.assertIn("func deepCopyResource[", group)

    def test_resource_without_short_names_or_status_subresource(self) -> None:
        data = mini()
        data["resources"][0].update(
            short_names=[], status_subresource=False, spec_required=False, scope="Cluster"
        )
        text = emit_kube_types.files(parse_kube(data))["api/demo/v1/widget_types.go"]
        self.assertIn("// +kubebuilder:resource:scope=Cluster\n", text)
        self.assertNotIn("subresource:status", text)
        self.assertIn('`json:"spec,omitempty"`', text)

    def test_type_shared_by_two_resources_is_emitted_once(self) -> None:
        data = mini()
        data["resources"].append(dict(data["resources"][0], kind="Gadget", short_names=[]))
        files = emit_kube_types.files(parse_kube(data))
        both = files["api/demo/v1/widget_types.go"] + files["api/demo/v1/gadget_types.go"]
        self.assertEqual(both.count("type WidgetSpec struct"), 1)
        self.assertEqual(both.count("type Phase string"), 1)

    def test_output_is_gofmt_clean(self) -> None:
        gofmt = tool("gofmt")
        if gofmt is None:
            self.skipTest("gofmt is not on PATH")
        real = parse_kube(document(ROOT / DEFINITION))
        with tempfile.TemporaryDirectory() as tmp:
            for kube in (parse_kube(mini()), real):
                for path, text in emit_kube_types.files(kube).items():
                    out = Path(tmp) / path
                    out.parent.mkdir(parents=True, exist_ok=True)
                    # LF on every host, as the generator writes them: gofmt
                    # lists a CRLF file (text mode wrote one on Windows).
                    out.write_text(text, encoding="utf-8", newline="\n")
            done = run([gofmt, "-l", tmp])
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.strip(), "", "gofmt would rewrite these files")

    def test_real_definition_emits_the_committed_types(self) -> None:
        files = emit_kube_types.files(parse_kube(document(ROOT / DEFINITION)))
        self.assertIn("api/vmafx/v1/vmafxtenant_types.go", files)
        for path, text in files.items():
            with self.subTest(path):
                self.assertEqual((ROOT / path).read_text(encoding="utf-8"), text)


if __name__ == "__main__":
    unittest.main()
