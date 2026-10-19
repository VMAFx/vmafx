# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The environment of the Go binaries in the platform definition (ADR-2350 D13).

`[[config_binaries]]` names each Go binary: its package directory, the page
whose environment table is generated, and the chart workloads that run it.
`[[config]]` is one environment variable: its name, the configuration key
golusoris maps it to (none for a variable the binary reads directly), the
binaries that read it, its type, default, documentation and whether it holds a
secret; `[config.<binary>]` overrides the type, default or documentation for
one binary. A key with an underscore inside a segment is a golusoris
CompoundKey; the generator writes each binary's list.

`[[chart_workloads]]` are the places in the chart's templates that hold an
environment list, with the indentation of its entries. `[[chart_env]]` is one
entry of those lists, in order: the workloads it belongs to, the YAML comment
lines above it, an optional condition (`when`, a template expression), and the
value: a template expression (`value`), a field reference (`field_ref`) or a
Secret key (`secret_ref`, with template expressions for `name` and `key`).
An entry the workload's binary does not read is refused.
`[[chart_maps]]` are named value maps (the backend of each GPU vendor) the
generator writes as template helpers.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any

from .entries import Entry, need
from .model import DefinitionError

ENV_NAME = re.compile(r"[A-Z][A-Z0-9_]*")
KEY = re.compile(r"[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*")
VALUES_REF = re.compile(r"\.Values\.([A-Za-z0-9_.]+)")
OVERRIDABLE = ("key", "type", "default", "doc")
CONFIG_KEYS = frozenset({"env", "key", "binaries", "type", "default", "doc", "secret"})
ENV_KEYS = frozenset({"env", "workloads", "comment", "when", "value", "field_ref", "secret_ref"})


@dataclass(frozen=True)
class ConfigBinary:
    name: str
    dir: str
    package: str
    page: str
    workloads: tuple[str, ...]


@dataclass(frozen=True)
class ConfigVar:
    env: str
    key: str
    binaries: tuple[str, ...]
    type: str
    default: str
    doc: str
    secret: bool
    overrides: tuple[tuple[str, tuple[tuple[str, str], ...]], ...]

    def field(self, binary: str, name: str) -> str:
        """`type`, `default` or `doc` as `binary` documents it."""
        for owner, values in self.overrides:
            if owner == binary:
                found = dict(values).get(name)
                if found is not None:
                    return found
        value: str = getattr(self, name)
        return value

    def compound(self, binary: str) -> bool:
        key = self.field(binary, "key")
        return any("_" in part for part in key.split(".")) if key else False


@dataclass(frozen=True)
class ChartWorkload:
    name: str
    indent: int
    doc: str


@dataclass(frozen=True)
class ChartEnv:
    env: str
    workloads: tuple[str, ...]
    comment: str
    when: str
    value: str
    field_ref: str
    secret_ref: tuple[str, str] | None


@dataclass(frozen=True)
class ChartMap:
    name: str
    source: str
    cases: tuple[tuple[str, str], ...]
    default: str
    doc: str


@dataclass(frozen=True)
class Config:
    binaries: tuple[ConfigBinary, ...]
    variables: tuple[ConfigVar, ...]
    workloads: tuple[ChartWorkload, ...]
    env: tuple[ChartEnv, ...]
    maps: tuple[ChartMap, ...]


def env_of(key: str) -> str:
    """The environment variable golusoris maps to `key`."""
    return "VMAFX_" + key.upper().replace(".", "_")


def _text(raw: Entry, name: str, where: str, required: bool = True) -> str:
    value = raw.get(name, "") if not required else need(raw, name, where)
    if not isinstance(value, str) or (required and not value.strip()):
        raise DefinitionError(f"{where}: `{name}` is a non-empty string")
    return value


def _names(raw: Entry, name: str, where: str) -> tuple[str, ...]:
    value = need(raw, name, where)
    if not isinstance(value, list) or not value or not all(isinstance(v, str) for v in value):
        raise DefinitionError(f"{where}: `{name}` is a list of names")
    return tuple(value)


def _binary(raw: Entry) -> ConfigBinary:
    where = f"config_binaries[{raw.get('name', '?')}]"
    return ConfigBinary(
        name=_text(raw, "name", where),
        dir=_text(raw, "dir", where),
        package=str(raw.get("package", "main")),
        page=_text(raw, "page", where),
        workloads=tuple(raw.get("workloads", [])),
    )


def _overrides(raw: Entry, binaries: tuple[str, ...], where: str) -> tuple[Any, ...]:
    out = []
    for name in binaries:
        table = raw.get(name)
        if table is None:
            continue
        if not isinstance(table, dict) or set(table) - set(OVERRIDABLE):
            raise DefinitionError(f"{where}.{name}: overrides only {', '.join(OVERRIDABLE)}")
        out.append((name, tuple((k, str(v)) for k, v in table.items())))
    return tuple(out)


def _variable(raw: Entry) -> ConfigVar:
    where = f"config[{raw.get('env', '?')}]"
    binaries = _names(raw, "binaries", where)
    unknown = set(raw) - CONFIG_KEYS - set(binaries)
    if unknown:
        raise DefinitionError(f"{where}: unknown keys {sorted(unknown)}")
    secret = raw.get("secret", False)
    if not isinstance(secret, bool):
        raise DefinitionError(f"{where}: `secret` is true or false")
    return ConfigVar(
        env=_text(raw, "env", where),
        key=_text(raw, "key", where, required=False),
        binaries=binaries,
        type=_text(raw, "type", where),
        default=_text(raw, "default", where),
        doc=_text(raw, "doc", where),
        secret=secret,
        overrides=_overrides(raw, binaries, where),
    )


def _workload(raw: Entry) -> ChartWorkload:
    where = f"chart_workloads[{raw.get('name', '?')}]"
    indent = need(raw, "indent", where)
    if not isinstance(indent, int) or isinstance(indent, bool) or indent < 0:
        raise DefinitionError(f"{where}: `indent` is a column number")
    return ChartWorkload(
        name=_text(raw, "name", where), indent=indent, doc=_text(raw, "doc", where)
    )


def _secret_ref(raw: Entry, where: str) -> tuple[str, str] | None:
    ref = raw.get("secret_ref")
    if ref is None:
        return None
    if not isinstance(ref, dict) or set(ref) != {"name", "key"}:
        raise DefinitionError(f"{where}: `secret_ref` has a `name` and a `key` expression")
    return (str(ref["name"]), str(ref["key"]))


def _env(raw: Entry) -> ChartEnv:
    where = f"chart_env[{raw.get('env', '?')}]"
    unknown = set(raw) - ENV_KEYS
    if unknown:
        raise DefinitionError(f"{where}: unknown keys {sorted(unknown)}")
    entry = ChartEnv(
        env=_text(raw, "env", where),
        workloads=_names(raw, "workloads", where),
        comment=_text(raw, "comment", where, required=False),
        when=_text(raw, "when", where, required=False),
        value=_text(raw, "value", where, required=False),
        field_ref=_text(raw, "field_ref", where, required=False),
        secret_ref=_secret_ref(raw, where),
    )
    sources = [bool(entry.value), bool(entry.field_ref), entry.secret_ref is not None]
    if sources.count(True) != 1:
        raise DefinitionError(f"{where}: exactly one of `value`, `field_ref`, `secret_ref`")
    return entry


def _map(raw: Entry) -> ChartMap:
    where = f"chart_maps[{raw.get('name', '?')}]"
    cases = need(raw, "cases", where)
    if not isinstance(cases, dict) or not all(isinstance(v, str) for v in cases.values()):
        raise DefinitionError(f"{where}: `cases` maps values to strings")
    return ChartMap(
        name=_text(raw, "name", where),
        source=_text(raw, "source", where),
        cases=tuple(cases.items()),
        default=_text(raw, "default", where),
        doc=_text(raw, "doc", where),
    )


def parse_config(data: dict[str, Any]) -> Config | None:
    """The environment tables of the platform definition, validated; None without them."""
    if "config" not in data and "config_binaries" not in data:
        return None
    config = Config(
        binaries=tuple(_binary(b) for b in data.get("config_binaries", [])),
        variables=tuple(_variable(v) for v in data.get("config", [])),
        workloads=tuple(_workload(w) for w in data.get("chart_workloads", [])),
        env=tuple(_env(e) for e in data.get("chart_env", [])),
        maps=tuple(_map(m) for m in data.get("chart_maps", [])),
    )
    validate_config(config)
    return config


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def _unique(label: str, values: list[Any]) -> None:
    seen: set[Any] = set()
    for value in values:
        if value in seen:
            raise DefinitionError(f"{label}: {value} appears twice")
        seen.add(value)


def _check_variable(var: ConfigVar, binaries: set[str]) -> None:
    where = f"config[{var.env}]"
    if not ENV_NAME.fullmatch(var.env):
        raise DefinitionError(f"{where}: an environment variable name is upper case")
    for key in {var.field(b, "key") for b in var.binaries} - {""}:
        if not KEY.fullmatch(key) or env_of(key) != var.env:
            raise DefinitionError(f"{where}: key {key} maps to {env_of(key)}, not {var.env}")
    for name in var.binaries:
        if name not in binaries:
            raise DefinitionError(f"{where}: binary {name} is not in [[config_binaries]]")


def _check_env(config: Config) -> None:
    workloads = {w.name for w in config.workloads}
    owners = {w: b.name for b in config.binaries for w in b.workloads}
    readers = {(v.env, b) for v in config.variables for b in v.binaries}
    for entry in config.env:
        where = f"chart_env[{entry.env}]"
        for workload in entry.workloads:
            if workload not in workloads:
                raise DefinitionError(f"{where}: workload {workload} is not in [[chart_workloads]]")
            binary = owners.get(workload)
            if binary is not None and (entry.env, binary) not in readers:
                raise DefinitionError(f"{where}: vmafx-{binary} reads no {entry.env}")
        for line in entry.comment.splitlines():
            if not line.startswith("#"):
                raise DefinitionError(f"{where}: `comment` lines start with #")


def validate_config(config: Config) -> None:
    _unique("config_binaries: name", [b.name for b in config.binaries])
    _unique(
        "config: binary and variable", [(b, v.env) for v in config.variables for b in v.binaries]
    )
    _unique("chart_workloads: name", [w.name for w in config.workloads])
    _unique("chart_maps: name", [m.name for m in config.maps])
    owners = [w for b in config.binaries for w in b.workloads]
    _unique("config_binaries: workload", owners)
    names = {b.name for b in config.binaries}
    for var in config.variables:
        _check_variable(var, names)
    _check_env(config)


def compound_keys(config: Config, binary: str) -> list[str]:
    """The CompoundKeys golusoris needs for `binary`, sorted."""
    return sorted(
        {
            v.field(binary, "key")
            for v in config.variables
            if binary in v.binaries and v.compound(binary)
        }
    )


def chart_values(config: Config, env: str, workloads: tuple[str, ...]) -> list[str]:
    """The chart values the entries of `env` in `workloads` read, in order."""
    found: list[str] = []
    for entry in config.env:
        if entry.env != env or not set(entry.workloads) & set(workloads):
            continue
        texts = [entry.when, entry.value, *(entry.secret_ref or ())]
        texts += [m.source for m in config.maps for t in texts if f'include "{m.name}"' in t]
        for text in texts:
            found += [m for m in VALUES_REF.findall(text) if m not in found]
    return found
