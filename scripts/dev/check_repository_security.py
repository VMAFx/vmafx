#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Read public GitHub metadata and fail on repository security policy drift."""

from __future__ import annotations

import argparse
import http.client
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import CommandFailed, CommandTimedOut
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / ".github/repository-security-policy.json"
API_HOST = "api.github.com"
PAGE_SIZE = 100
BYPASS_QUERY = """query {
  repository(owner: "VMAFx", name: "vmafx") {
    rulesets(first: 100) {
      pageInfo { hasNextPage }
      nodes { databaseId bypassActors(first: 1) { totalCount } }
    }
  }
}"""


def get_json(route: str) -> Any:
    """Read public metadata from a fixed host, optionally with the CI token."""
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2026-03-10",
        "User-Agent": "VMAFx-repository-security-check",
    }
    if token := os.environ.get("GH_TOKEN"):
        headers["Authorization"] = f"Bearer {token}"
    connection = http.client.HTTPSConnection(API_HOST, timeout=30)
    try:
        connection.request(
            "GET",
            route,
            headers=headers,
        )
        response = connection.getresponse()
        if response.status != http.client.OK:
            raise ValueError(f"GitHub metadata request failed: HTTP {response.status}")
        return json.load(response)
    finally:
        connection.close()


def differences(expected: Any, actual: Any, path: str = "policy") -> list[str]:
    """Compare declared values; permit API metadata but no list additions."""
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return [f"{path}: expected object"]
        errors = []
        for key, value in expected.items():
            if key not in actual:
                errors.append(f"{path}.{key}: missing")
            else:
                errors.extend(differences(value, actual[key], f"{path}.{key}"))
        return errors
    if isinstance(expected, list):
        if not isinstance(actual, list) or len(expected) != len(actual):
            return [f"{path}: list membership differs"]
        # GitHub may return rules and permitted merge methods in a different order.
        typed_rules = expected and all(
            isinstance(item, dict) and "type" in item for item in expected
        )

        def sort_key(item: Any) -> str:
            if typed_rules:
                return str(item.get("type", "")) if isinstance(item, dict) else ""
            return json.dumps(item, sort_keys=True)

        errors = []
        for index, (left, right) in enumerate(
            zip(sorted(expected, key=sort_key), sorted(actual, key=sort_key), strict=True)
        ):
            errors.extend(differences(left, right, f"{path}[{index}]"))
        return errors
    if type(expected) is not type(actual) or expected != actual:
        return [f"{path}: expected {expected!r}, received {actual!r}"]
    return []


def bypass_count(rule_id: int) -> int:
    """Read only the actor count; REST hides actors from non-admin readers."""
    gh = shutil.which("gh")
    if gh is None:
        raise FileNotFoundError("GitHub CLI is required for the bypass count read query")
    result = run_command(
        [gh, "api", "--hostname", "github.com", "graphql", "-f", f"query={BYPASS_QUERY}"],
        allowed_executables=(gh,),
        check=True,
        capture_output=True,
        text=True,
        timeout_seconds=30,
    )
    payload = json.loads(result.stdout)
    if payload.get("errors"):
        raise ValueError("GitHub rejected the bypass actor count query")
    collection = payload["data"]["repository"]["rulesets"]
    if collection["pageInfo"]["hasNextPage"] is not False:
        raise ValueError("ruleset bypass count query may be truncated")
    matches = [item for item in collection["nodes"] if item["databaseId"] == rule_id]
    if len(matches) != 1:
        raise ValueError("managed ruleset missing or duplicated in bypass count query")
    count = matches[0]["bypassActors"]["totalCount"]
    if type(count) is not int or count < 0:
        raise ValueError("invalid bypass actor count")
    return count


def inspect(policy: dict[str, Any]) -> tuple[list[str], dict[str, Any]]:
    """Check the one managed ruleset, effective master rules, and reporting flag."""
    repository = policy["repository"]
    if repository != "VMAFx/vmafx" or policy["default_branch"] != "master":
        raise ValueError("policy must target VMAFx/vmafx master")
    prefix = f"/repos/{repository}"
    data: dict[str, Any] = {
        "repository": get_json(prefix),
        "rulesets": get_json(prefix + "/rulesets?includes_parents=false&per_page=100"),
        "private_vulnerability_reporting": get_json(prefix + "/private-vulnerability-reporting"),
    }
    errors = differences(
        {"full_name": repository, "default_branch": "master"}, data["repository"], "repository"
    )
    errors.extend(
        differences(
            {"enabled": policy["private_vulnerability_reporting"]},
            data["private_vulnerability_reporting"],
            "private_vulnerability_reporting",
        )
    )
    listed = data["rulesets"]
    if not isinstance(listed, list) or len(listed) >= PAGE_SIZE:
        raise ValueError("ruleset listing is malformed or may be truncated")
    desired = policy["ruleset"]
    matching = [
        item for item in listed if isinstance(item, dict) and item.get("name") == desired["name"]
    ]
    if len(matching) != 1:
        return [*errors, "expected exactly one managed repository ruleset"], data
    rule_id = matching[0].get("id")
    if type(rule_id) is not int or rule_id <= 0:
        raise ValueError("invalid repository ruleset ID")
    data["ruleset"] = get_json(prefix + f"/rulesets/{rule_id}")
    comparable = desired
    if isinstance(data["ruleset"], dict) and "bypass_actors" not in data["ruleset"]:
        # REST hides the actor list from non-admin readers, so this path can only
        # verify HOW MANY actors bypass, not WHICH. A reader without admin rights
        # therefore cannot distinguish the declared actor from a substituted one
        # of the same count; the admin readback below compares identities exactly.
        # ADR-1252 accepts that limit as the price of a declared, countable
        # exception over an undeclared one.
        expected = desired["bypass_actors"]
        count = bypass_count(rule_id)
        data["bypass_actor_count"] = count
        if count != len(expected):
            errors.append(f"ruleset.bypass_actors: expected {len(expected)}, found {count}")
        comparable = {key: value for key, value in desired.items() if key != "bypass_actors"}
    errors.extend(differences(comparable, data["ruleset"], "ruleset"))
    effective = get_json(prefix + "/rules/branches/master?per_page=100")
    data["effective_rules"] = effective
    if not isinstance(effective, list) or len(effective) >= PAGE_SIZE:
        raise ValueError("effective rule listing is malformed or may be truncated")
    own_rules = [
        item for item in effective if isinstance(item, dict) and item.get("ruleset_id") == rule_id
    ]
    errors.extend(differences(desired["rules"], own_rules, "effective_rules"))
    return errors, data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--report", type=Path, help="write the read-only API evidence to a new file"
    )
    args = parser.parse_args()
    try:
        with POLICY.open(encoding="utf-8") as stream:
            policy = json.load(stream)
        errors, evidence = inspect(policy)
        if args.report:
            with args.report.open("x", encoding="utf-8") as stream:
                json.dump(
                    {"policy": policy, "errors": errors, "evidence": evidence}, stream, indent=2
                )
                stream.write("\n")
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        http.client.HTTPException,
        CommandFailed,
        CommandTimedOut,
    ) as exc:
        print(f"Repository security check failed: {exc}", file=sys.stderr)
        return 1
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print("VMAFx master ruleset and private vulnerability reporting match repository policy.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
