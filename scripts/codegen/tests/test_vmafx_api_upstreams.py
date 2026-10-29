# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The live upstream check of the encoder registry, against stub forges (ADR-3388).

Positive: a row whose FFmpeg encoder, maintenance and licence class match its
GitHub, GitLab, Gitiles and Bitbucket upstream is ok. Negative: each planted
defect is a mismatch (an encoder FFmpeg master does not register, a stale
maintenance class, a licence class the forge contradicts, a placeholder whose
encoder FFmpeg now ships) and an allcodecs.c
without encoder declarations is refused. Boundary: 182/183 and 729/730 days
split active, slow and dead; an archived repository is archived whatever its
age; a host without a forge API and a row without upstream are unchecked,
never ok, as is a placeholder FFmpeg does not ship yet; a hyphenated encoder
name matches its underscored symbol and libvpx its libvpx_vp8 symbol; a judged licence class is not overruled by the forge.
"""

from __future__ import annotations

import json
import unittest
from datetime import date, timedelta
from typing import Any

import support  # noqa: F401 (puts scripts/codegen on sys.path)
from vmafx_api import encoders, upstreams

TODAY = date(2026, 10, 29)
ALLCODECS = (
    b"extern const FFCodec ff_libgap_encoder;\n"
    b"extern const FFCodec ff_libgap_av2_encoder;\n"
    b"extern const FFCodec ff_libvpx_vp8_encoder;\n"
    b"extern const FFCodec ff_gap_decoder;\n"
)


def gap_row(**change: Any) -> dict[str, Any]:
    row = {
        "id": "libgap",
        "family": "vvc",
        "paradigm": "classic",
        "class": "delivery",
        "status": "planned",
        "interface": "ffmpeg",
        "ffmpeg_encoder": "libgap",
        "licence_class": "permissive",
        "maintenance": "active",
        "upstream": "https://github.com/example/gap",
        "checked": "2026-10-29",
    }
    return row | change


def registry(*rows: dict[str, Any]) -> encoders.Registry:
    return encoders.parse({"format_version": encoders.FORMAT_VERSION, "encoders": list(rows)})


def github(age_days: int, spdx: str = "MIT", archived: bool = False) -> dict[str, bytes]:
    stamp = f"{TODAY - timedelta(days=age_days)}T12:00:00Z"
    meta = {"archived": archived, "license": {"spdx_id": spdx}}
    head = [{"commit": {"committer": {"date": stamp}}}]
    return {
        "https://api.github.com/repos/example/gap": json.dumps(meta).encode(),
        "https://api.github.com/repos/example/gap/commits?per_page=1": json.dumps(head).encode(),
    }


def stub(pages: dict[str, bytes], allcodecs: bytes = ALLCODECS) -> upstreams.Fetch:
    table = {upstreams.ALLCODECS: allcodecs, **pages}

    def fetch(url: str) -> bytes:
        return table[url]

    return fetch


def verdicts(reg: encoders.Registry, pages: dict[str, bytes]) -> list[upstreams.Finding]:
    return upstreams.check(reg, stub(pages), TODAY)


class Positive(unittest.TestCase):
    def test_github_row_ok(self) -> None:
        (found,) = verdicts(registry(gap_row()), github(10))
        self.assertEqual(found.verdict, "ok", found.detail)

    def test_gitlab_row_ok(self) -> None:
        url = "https://code.videolan.org/api/v4/projects/videolan%2Fgap"
        page = json.dumps({"last_activity_at": "2026-10-01T00:00:00.000Z"}).encode()
        row = gap_row(upstream="https://code.videolan.org/videolan/gap")
        (found,) = verdicts(registry(row), {url: page})
        self.assertEqual(found.verdict, "ok", found.detail)

    def test_gitiles_row_ok(self) -> None:
        url = "https://chromium.googlesource.com/webm/gap/+log/HEAD?format=JSON&n=1"
        page = (
            b")]}'\n"
            + json.dumps({"log": [{"committer": {"time": "Thu Oct 01 10:00:00 2026"}}]}).encode()
        )
        row = gap_row(upstream="https://chromium.googlesource.com/webm/gap")
        (found,) = verdicts(registry(row), {url: page})
        self.assertEqual(found.verdict, "ok", found.detail)

    def test_bitbucket_row_ok(self) -> None:
        url = "https://api.bitbucket.org/2.0/repositories/example/gap/commits?pagelen=1"
        page = json.dumps({"values": [{"date": "2026-10-01T00:00:00+00:00"}]}).encode()
        row = gap_row(upstream="https://bitbucket.org/example/gap")
        (found,) = verdicts(registry(row), {url: page})
        self.assertEqual(found.verdict, "ok", found.detail)

    def test_hyphenated_name_matches_symbol(self) -> None:
        (found,) = verdicts(registry(gap_row(ffmpeg_encoder="libgap-av2")), github(10))
        self.assertEqual(found.verdict, "ok", found.detail)

    def test_aliased_name_matches_symbol(self) -> None:
        (found,) = verdicts(registry(gap_row(ffmpeg_encoder="libvpx")), github(10))
        self.assertEqual(found.verdict, "ok", found.detail)


class Negative(unittest.TestCase):
    def test_encoder_missing_from_ffmpeg(self) -> None:
        (found,) = verdicts(registry(gap_row(ffmpeg_encoder="libnope")), github(10))
        self.assertEqual(found.verdict, "mismatch")
        self.assertIn("FFmpeg master has no libnope", found.detail)

    def test_stale_maintenance(self) -> None:
        (found,) = verdicts(registry(gap_row()), github(400))
        self.assertEqual(found.verdict, "mismatch")
        self.assertIn("live 'slow'", found.detail)

    def test_licence_contradicted(self) -> None:
        (found,) = verdicts(registry(gap_row()), github(10, spdx="GPL-2.0-or-later"))
        self.assertEqual(found.verdict, "mismatch")
        self.assertIn("copyleft-strong", found.detail)

    def test_placeholder_flipped_once_ffmpeg_ships(self) -> None:
        (found,) = verdicts(registry(gap_row(awaits_ffmpeg=True)), github(10))
        self.assertEqual(found.verdict, "mismatch")
        self.assertIn("now has libgap", found.detail)

    def test_allcodecs_without_encoders_refused(self) -> None:
        fetch = stub(github(10), allcodecs=b"/* moved */\n")
        with self.assertRaises(ValueError):
            upstreams.check(registry(gap_row()), fetch, TODAY)


class Boundary(unittest.TestCase):
    def test_maintenance_edges(self) -> None:
        live = upstreams.Live
        for age, want in ((182, "active"), (183, "slow"), (729, "slow"), (730, "dead")):
            with self.subTest(age=age):
                state = live(TODAY - timedelta(days=age), False, "")
                self.assertEqual(upstreams.maintenance(state, TODAY), want)

    def test_archived_wins_over_age(self) -> None:
        state = upstreams.Live(TODAY, True, "")
        self.assertEqual(upstreams.maintenance(state, TODAY), "archived")

    def test_host_without_forge_api_unchecked(self) -> None:
        row = gap_row(upstream="https://example.invalid/gap")
        (found,) = verdicts(registry(row), {})
        self.assertEqual(found.verdict, "unchecked")

    def test_row_without_upstream_unchecked(self) -> None:
        (found,) = verdicts(registry(gap_row(upstream="")), {})
        self.assertEqual(found.verdict, "unchecked")

    def test_placeholder_unchecked_while_ffmpeg_lacks_it(self) -> None:
        row = gap_row(ffmpeg_encoder="gap_videotoolbox", awaits_ffmpeg=True)
        (found,) = verdicts(registry(row), github(10))
        self.assertEqual(found.verdict, "unchecked")

    def test_judged_licence_not_overruled(self) -> None:
        row = gap_row(licence_class="research-noncommercial")
        (found,) = verdicts(registry(row), github(10, spdx="MIT"))
        self.assertEqual(found.verdict, "ok", found.detail)

    def test_unclassified_spdx_not_a_mismatch(self) -> None:
        (found,) = verdicts(registry(gap_row()), github(10, spdx="NOASSERTION"))
        self.assertEqual(found.verdict, "ok", found.detail)


if __name__ == "__main__":
    unittest.main()
