# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/research/radar_collect.py (ADR-2171).

A planted feed with a new repository, a push, a release and papers must appear in the
digest; an unchanged feed must produce an empty digest; the window is a partition of
time; a source that fails is named, never skipped; untrusted titles cannot ping a
user or link an issue; and the public radar texts name no third-party company.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import shutil
import sys
import tempfile
import unittest
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "scripts" / "research" / "radar_collect.py"
REGISTRY = REPO_ROOT / "docs" / "research" / "radar" / "sources.yaml"
PUBLIC_TEXTS = (
    REGISTRY,
    REPO_ROOT / "docs" / "research" / "radar" / "README.md",
    REPO_ROOT / "docs" / "research" / "radar" / "triage.md",
    REPO_ROOT / ".github" / "workflows" / "research-radar.yml",
    MODULE_PATH,
)

# SHA-256 of lower-case word n-grams (1 to 3 words, letters and digits only) that the
# project keeps out of public text. Hashes, so this file does not repeat the names.
DENIED_NGRAMS = frozenset(
    {
        "f62af2165e372258c2af347d52d2c2e4565de80d2a0dfa77195b3114f9cc0525",
        "842483683c1b8c0ec72767ff756d6da86216fb1e41bd1ea53a4dc6fc3db11cb9",
        "6cb84400b2883c1ac3da6065679397e5b126d200bfaf0faa73e5f64cf7e6cf19",
        "bbb992e66c4411fb698895f2c8b4e630e282807795848487420f4dd95d45fa74",
        "0b99b5daa84853b09fa80c01c4e09d67c84d89c9d58ab8b57af1c7f209ff7b41",
        "3dd29bcb6e5d8cb87f02baae717a41caf2dee7e312a48abe0d6ed48a5206fba3",
        "7c6657c292985da44216c57dda1bf6d73efd247ede9e9752f3c8a34f8ec16d74",
        "ff5406b7383e48b0b273edfcf7d1e7263272935751749c9a0d4afd38db3cb2f2",
        "1cd38b20bf937895efffc247bd9b85abbacfc5bfe21bfc12a44efa47a1da2343",
        "2a9b19ae8fa716b28cfef33d669bb4dc674190ffd29fd0b14a581718e36df554",
        "c36fbc56ea2cb2fd95b48f546bbfa01b9d440f1ea5fc5d14ec24b3e695a53eba",
    }
)
MAX_NGRAM_WORDS = 3
NOW = "2026-10-05T06:00:00"
SINCE = "2026-09-28T06:00:00"


def _load() -> Any:
    spec = importlib.util.spec_from_file_location("radar_collect", MODULE_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["radar_collect"] = module
    spec.loader.exec_module(module)
    return module


rc = _load()


def denied_ngrams_in(text: str) -> list[str]:
    """The denied word n-grams found in ``text`` (as hashes)."""
    words = re.findall(r"[a-z0-9]+", text.lower())
    found = []
    for size in range(1, MAX_NGRAM_WORDS + 1):
        for i in range(len(words) - size + 1):
            digest = hashlib.sha256(" ".join(words[i : i + size]).encode()).hexdigest()
            if digest in DENIED_NGRAMS:
                found.append(digest)
    return found


def source(**kw: Any) -> Any:
    base = {
        "id": "s1",
        "kind": "github_owner",
        "url": "https://example.org",
        "why": "w",
        "licence_note": "n",
        "owner": "acme",
    }
    base.update(kw)
    return rc.Source(**base)


def window() -> Any:
    return rc.Window(since=rc.parse_time(SINCE), now=rc.parse_time(NOW))


def repo(name: str, created: str, pushed: str, **kw: Any) -> dict[str, Any]:
    rec = {
        "full_name": f"acme/{name}",
        "name": name,
        "html_url": f"https://github.com/acme/{name}",
        "description": "d",
        "created_at": created,
        "pushed_at": pushed,
        "fork": False,
        "archived": False,
    }
    rec.update(kw)
    return rec


def atom(entries: list[tuple[str, str, str]]) -> str:
    body = "".join(
        f"<entry><id>http://arxiv.org/abs/{i}v1</id><title>{t}</title>"
        f"<published>{p}</published></entry>"
        for i, t, p in entries
    )
    return f'<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom">{body}</feed>'


class PlantedFeedTest(unittest.TestCase):
    """A change in the feed appears in the digest."""

    def collect(self, src: Any, table: dict[str, Any]) -> Any:
        return rc.collect([src], window(), rc.fixture_fetcher(table))

    def test_new_repo_push_and_release_appear(self) -> None:
        recs = [
            repo("brand-new", "2026-10-01T10:00:00Z", "2026-10-02T10:00:00Z"),
            repo("busy", "2020-01-01T00:00:00Z", "2026-10-03T10:00:00Z"),
            repo("quiet", "2020-01-01T00:00:00Z", "2026-01-01T00:00:00Z"),
        ]
        releases = [
            {
                "name": "v2",
                "published_at": "2026-10-03T09:00:00Z",
                "html_url": "https://github.com/acme/busy/releases/tag/v2",
                "draft": False,
            }
        ]
        table = {
            rc.github_repos_url("acme"): recs,
            rc.github_releases_url("acme/brand-new"): [],
            rc.github_releases_url("acme/busy"): releases,
        }
        result = self.collect(source(), table)
        digest = rc.render_digest(result, window())
        self.assertIn("## New repositories (1)", digest)
        self.assertIn("acme/brand-new", digest)
        self.assertIn("## Releases (1)", digest)
        self.assertIn("acme/busy v2", digest)
        self.assertIn("## Pushes (1)", digest)
        self.assertNotIn("acme/quiet", digest)
        self.assertTrue(digest.startswith("# Research radar digest 2026-W41"))

    def test_paper_from_arxiv_and_openalex_is_listed_once(self) -> None:
        src_a = source(id="a", kind="arxiv_query", query="q1", owner="")
        src_o = source(id="o", kind="openalex_query", query="q2", owner="")
        title = "A New Video Quality Metric"
        table = {
            rc.arxiv_url("q1"): atom([("2610.00001", title, "2026-10-01T08:00:00Z")]),
            rc.openalex_url("q2", window()): {
                "results": [
                    {
                        "display_name": title.lower(),
                        "publication_date": "2026-10-01",
                        "doi": "https://doi.org/10.1/x",
                    }
                ]
            },
        }
        result = rc.collect([src_a, src_o], window(), rc.fixture_fetcher(table))
        self.assertEqual(len(result.items), 1)
        self.assertIn("## New papers (1)", rc.render_digest(result, window()))

    def test_unchanged_feed_gives_an_empty_digest(self) -> None:
        recs = [repo("old", "2020-01-01T00:00:00Z", "2026-09-01T00:00:00Z")]
        src_a = source(id="a", kind="arxiv_query", query="q1", owner="")
        table = {
            rc.github_repos_url("acme"): recs,
            rc.arxiv_url("q1"): atom([("2609.00001", "Old", "2026-09-01T08:00:00Z")]),
        }
        result = rc.collect([source(), src_a], window(), rc.fixture_fetcher(table))
        self.assertEqual(result.errors, [])
        self.assertEqual(rc.render_digest(result, window()), "")

    def test_forks_archived_and_filtered_names_are_skipped(self) -> None:
        recs = [
            repo("forked", "2026-10-01T00:00:00Z", "2026-10-01T00:00:00Z", fork=True),
            repo("gone", "2026-10-01T00:00:00Z", "2026-10-01T00:00:00Z", archived=True),
            repo("other", "2026-10-01T00:00:00Z", "2026-10-01T00:00:00Z"),
        ]
        table = {rc.github_repos_url("acme"): recs}
        result = self.collect(source(name_filter="^vq"), table)
        self.assertEqual(result.items, [])

    def test_manual_sources_are_not_collected(self) -> None:
        result = rc.collect([source(kind="manual", owner="")], window(), rc.fixture_fetcher({}))
        self.assertEqual((result.items, result.errors, result.sources_read), ([], [], 0))

    def test_releases_only_ignores_pushes(self) -> None:
        recs = repo("busy", "2020-01-01T00:00:00Z", "2026-10-03T10:00:00Z")
        table = {rc.github_repo_url("acme/busy"): recs, rc.github_releases_url("acme/busy"): []}
        src = source(kind="github_repo", repo="acme/busy", owner="", releases_only=True)
        self.assertEqual(self.collect(src, table).items, [])


class WindowTest(unittest.TestCase):
    """Consecutive windows partition time."""

    def test_time_boundaries(self) -> None:
        win = window()
        self.assertTrue(win.holds_time(SINCE + "Z"))
        self.assertFalse(win.holds_time(NOW + "Z"))
        just_before = (rc.parse_time(NOW) - timedelta(seconds=1)).isoformat()
        self.assertTrue(win.holds_time(just_before))
        self.assertFalse(win.holds_time((rc.parse_time(SINCE) - timedelta(seconds=1)).isoformat()))

    def test_date_boundaries_exclude_today_and_include_the_first_day(self) -> None:
        win = window()
        self.assertTrue(win.holds_date("2026-09-28"))
        self.assertTrue(win.holds_date("2026-10-04"))
        self.assertFalse(win.holds_date("2026-10-05"))
        self.assertFalse(win.holds_date("2026-09-27"))
        self.assertFalse(win.holds_date(""))

    def test_next_window_starts_where_this_one_ends(self) -> None:
        now = rc.parse_time(NOW)
        nxt = rc.Window(since=now, now=now + timedelta(days=7))
        stamp = "2026-10-05T05:59:59+00:00"
        self.assertTrue(window().holds_time(stamp))
        self.assertFalse(nxt.holds_time(stamp))
        self.assertTrue(nxt.holds_time("2026-10-05T06:00:00+00:00"))


class FailureAndSafetyTest(unittest.TestCase):
    """Failures are visible and untrusted text is neutralised."""

    def test_failed_source_is_named_in_the_digest(self) -> None:
        result = rc.collect([source()], window(), rc.fixture_fetcher({}))
        self.assertEqual(len(result.errors), 1)
        digest = rc.render_digest(result, window())
        self.assertIn("## Sources that could not be read", digest)
        self.assertIn("s1", digest)

    def test_doctype_in_a_feed_is_refused(self) -> None:
        src = source(id="a", kind="arxiv_query", query="q1", owner="")
        bomb = '<?xml version="1.0"?><!DOCTYPE x [<!ENTITY a "aaaa">]><feed/>'
        result = rc.collect([src], window(), rc.fixture_fetcher({rc.arxiv_url("q1"): bomb}))
        self.assertIn("document type declaration", result.errors[0])

    def test_titles_cannot_ping_or_link(self) -> None:
        text = rc.clean_text("Hi @someone see #12 [x](http://e) | " + "w " * 200)
        self.assertNotIn("@s", text)
        self.assertNotIn("#1", text)
        self.assertNotIn("[", text)
        self.assertNotIn("|", text)
        self.assertLessEqual(len(text), rc.MAX_TITLE_CHARS)

    def test_a_long_section_is_capped(self) -> None:
        items = [
            rc.Item("New papers", "s", f"t{i}", f"https://e/{i}", "2026-10-01T00:00:00Z")
            for i in range(rc.MAX_ITEMS_PER_SECTION + 5)
        ]
        digest = rc.render_digest(rc.Collected(items=items, sources_read=1), window())
        self.assertIn("... and 5 more", digest)

    def test_digest_is_deterministic(self) -> None:
        recs = [repo(n, "2026-10-01T00:00:00Z", "2026-10-02T00:00:00Z") for n in ("b", "a")]
        table = {
            rc.github_repos_url("acme"): recs,
            rc.github_releases_url("acme/a"): [],
            rc.github_releases_url("acme/b"): [],
        }
        first = rc.render_digest(
            rc.collect([source()], window(), rc.fixture_fetcher(table)), window()
        )
        second = rc.render_digest(
            rc.collect([source()], window(), rc.fixture_fetcher(dict(reversed(table.items())))),
            window(),
        )
        self.assertEqual(first, second)


class RegistryTest(unittest.TestCase):
    """The registry validates; a broken one is refused."""

    def write(self, text: str) -> Path:
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, directory)
        path = directory / "r.yaml"
        path.write_text(text, encoding="utf-8")
        return path

    def test_the_real_registry_loads_and_covers_every_kind(self) -> None:
        sources = rc.load_registry(REGISTRY)
        self.assertGreaterEqual(len(sources), 30)
        self.assertEqual({s.kind for s in sources}, set(rc.KINDS))
        owners = {s.owner for s in sources if s.kind == "github_owner"}
        self.assertTrue({"utlive", "pavancm"} <= owners)

    def test_defects_are_refused(self) -> None:
        good = (
            "- {id: aa, kind: github_repo, repo: o/r, url: 'https://x', why: w, "
            "licence_note: n}\n"
        )
        cases = {
            "schema: 1\nsources: []\n": "non-empty",
            "schema: 2\nsources:\n" + good: "schema",
            "schema: 1\nsources:\n" + good + good: "duplicate",
            "schema: 1\nsources:\n- {id: aa, kind: nope, url: u, why: w, licence_note: n}\n": "kind",
            "schema: 1\nsources:\n- {id: aa, kind: github_repo, repo: bad, url: u, why: w, "
            "licence_note: n}\n": "owner/name",
            "schema: 1\nsources:\n- {id: aa, kind: arxiv_query, url: u, why: w, licence_note: n}\n": "query",
            "schema: 1\nsources:\n- {id: aa, kind: manual, url: u, why: w}\n": "licence_note",
        }
        for text, needle in cases.items():
            with self.subTest(needle=needle), self.assertRaisesRegex(rc.RegistryError, needle):
                rc.load_registry(self.write(text))


class PublicTextTest(unittest.TestCase):
    """The public radar texts name no denied third party."""

    def test_public_texts_are_clean(self) -> None:
        for path in PUBLIC_TEXTS:
            with self.subTest(path=path.name):
                self.assertTrue(path.is_file(), path)
                self.assertEqual(denied_ngrams_in(path.read_text(encoding="utf-8")), [])

    def test_the_check_refuses_a_planted_name(self) -> None:
        self.assertTrue(denied_ngrams_in("a partner, Surf" + "meter, and others"))
        self.assertTrue(denied_ngrams_in("the AV" + "EQ lab"))
        self.assertTrue(denied_ngrams_in("Main " + "Concept codec"))
        self.assertEqual(denied_ngrams_in("a neutral sentence about SSIM"), [])


class CliTest(unittest.TestCase):
    """The command line writes the digest file and reports the verdict."""

    def run_cli(self, table: dict[str, Any], *extra: str) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            fixtures, out = Path(tmp) / "f.json", Path(tmp) / "digest.md"
            fixtures.write_text(json.dumps(table), encoding="utf-8")
            reg = Path(tmp) / "r.yaml"
            reg.write_text(
                "schema: 1\nsources:\n- {id: aa, kind: github_owner, owner: acme, url: 'https://x', "
                "why: w, licence_note: n}\n",
                encoding="utf-8",
            )
            code = rc.main(
                [
                    "--registry",
                    str(reg),
                    "--fixtures",
                    str(fixtures),
                    "--output",
                    str(out),
                    "--now",
                    NOW,
                    *extra,
                ]
            )
            return code, out.read_text(encoding="utf-8") if out.exists() else ""

    def test_changed_feed_writes_a_digest(self) -> None:
        recs = [repo("fresh", "2026-10-01T00:00:00Z", "2026-10-01T00:00:00Z")]
        table = {rc.github_repos_url("acme"): recs, rc.github_releases_url("acme/fresh"): []}
        code, text = self.run_cli(table)
        self.assertEqual(code, 0)
        self.assertIn("acme/fresh", text)

    def test_unchanged_feed_writes_an_empty_file(self) -> None:
        recs = [repo("old", "2020-01-01T00:00:00Z", "2020-01-02T00:00:00Z")]
        code, text = self.run_cli({rc.github_repos_url("acme"): recs})
        self.assertEqual((code, text), (0, ""))

    def test_every_source_failing_is_exit_2(self) -> None:
        code, text = self.run_cli({})
        self.assertEqual(code, 2)
        self.assertIn("could not be read", text)

    def test_bad_window_is_a_usage_error(self) -> None:
        code, _ = self.run_cli({}, "--days", "0")
        self.assertEqual(code, 2)

    def test_now_defaults_to_the_current_time(self) -> None:
        yesterday = (datetime.now(UTC) - timedelta(days=1)).isoformat()
        recs = [repo("today", yesterday, yesterday)]
        table = {rc.github_repos_url("acme"): recs, rc.github_releases_url("acme/today"): []}
        with tempfile.TemporaryDirectory() as tmp:
            fixtures, out = Path(tmp) / "f.json", Path(tmp) / "digest.md"
            fixtures.write_text(json.dumps(table), encoding="utf-8")
            reg = Path(tmp) / "r.yaml"
            reg.write_text(
                "schema: 1\nsources:\n- {id: aa, kind: github_owner, owner: acme, "
                "url: 'https://x', why: w, licence_note: n}\n",
                encoding="utf-8",
            )
            code = rc.main(
                ["--registry", str(reg), "--fixtures", str(fixtures), "--output", str(out)]
            )
            self.assertEqual(code, 0)
            self.assertIn("acme/today", out.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
