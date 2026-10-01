"""MkDocs hook: render each ```figure fence as the figure markup, and serve the figure stylesheet.

Listed under `hooks:` in mkdocs.yml as tools/figures/mkdocs_hook.py
(docs/adr/0015-interactive-figures-from-vendored-interfig.md, section 4). It uses only the Python
standard library and the MkDocs API and imports no checker (docs/adr/0016-figures-for-adopters.md,
section 6): the figure checks run in Node, `node tools/figures/build.mjs sources|site|portable`.

Each fence becomes the markup tools/figures/core.mjs wrote into docs/assets/figures/<slug>.json as
`html`; the hook only fills its `{{base}}` slot and drops its `{{link}}` line (`render_block`). The
fence scanner is the one behaviour this file shares with tools/figures/checks.mjs, because MkDocs runs
hooks in-process: both replay tools/figures/fence-fixtures.json, so a fence nested inside a longer
fence is left alone here exactly as the `site` check expects. A fence naming a figure without
docs/assets/figures/<slug>.json is logged as a warning, which fails `mkdocs build --strict`.

figures.css and the committed player files under dist/ (loader.js, player.js and their
THIRD-PARTY-LICENSES.txt, written by tools/figures/bundle.mjs) sit beside this file, outside
docs_dir, so the hook publishes them: `on_files` adds each to the site as a generated file, the CSS
at CSS_URI and the player files under DIST_URI, and `on_config` links the stylesheet and the loader,
as a module script, from every page. A site needs no `extra_css` or `extra_javascript` entry for
them and no bundling step: the loader reads each figure's props from its SVG
(docs/adr/0016-figures-for-adopters.md, section 3).
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
import re
from typing import NamedTuple

log = logging.getLogger("mkdocs.plugins.praetor_figures")

CSS_FILE = Path(__file__).with_name("figures.css")
CSS_URI = "assets/stylesheets/figures.css"
DIST_DIR = Path(__file__).with_name("dist")
DIST_URI = "assets/javascripts/figures"
LOADER_URI = f"{DIST_URI}/loader.js"
# dist/ holds three files (DIST_FILES in bundle.mjs); the bound only keeps the listing finite.
MAX_DIST_FILES = 16
REBUILD = "node tools/figures/build.mjs build"

# Bounded so a pathological page cannot make the hook unbounded (HISS-02).
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_LINES = 100_000

# A fence opener or closer: three or more backticks or tildes, then the info string.
FENCE_LINE = re.compile(r"^(\s*)(`{3,}|~{3,})\s*([^\s`{]*)(.*)$")
SLUG = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
# The slots of a figure's JSON `html` (SLOTS and markup in tools/figures/core.mjs). A site shows the
# interactive figure itself, so the line holding the link to it is dropped.
BASE_SLOT = "{{base}}"
LINK_SLOT = "{{link}}"


class CheckError(Exception):
    """A figure the hook cannot render; logged as a warning, which fails a strict build."""


class Fence(NamedTuple):
    """One top-level fenced block: its line span (end exclusive), indent, info string and body."""

    start: int
    end: int
    indent: str
    info: str
    body: list[str]


def bounded_lines(text: str) -> list[str]:
    """The lines of `text`, refusing more than MAX_LINES.

    Lines end at "\n" only, as Python-Markdown splits them, with a trailing "\r" dropped.
    str.splitlines() also breaks at form feeds, U+2028 and other separators, which would
    shift every index expand() uses to splice a rendered figure into the page.
    """
    lines = [line.removesuffix("\r") for line in text.split("\n")]
    if lines and lines[-1] == "":
        lines.pop()  # a final newline ends the last line; it does not start another
    if len(lines) > MAX_LINES:
        raise CheckError(f"more than {MAX_LINES} lines")
    return lines


def fence_blocks(text: str) -> list[Fence]:
    """Every top-level fenced block in a Markdown page (`fenceBlocks` in checks.mjs).

    A fence nested inside a longer or different fence is literal text, not a block: a closer
    must use the opener's character, be at least as long, and carry no info string. An unclosed
    fence runs to the end of the page.
    """
    lines = bounded_lines(text)
    blocks: list[Fence] = []
    opener: tuple[int, str, str, str] | None = None
    for index, line in enumerate(lines):
        match = FENCE_LINE.match(line)
        if match is None:
            continue
        indent, marker, info, rest = match.groups()
        if opener is None:
            opener = (index, indent, marker, info.lower())
        elif marker[0] == opener[2][0] and len(marker) >= len(opener[2]) and not info and not rest.strip():
            blocks.append(Fence(opener[0], index + 1, opener[1], opener[3], lines[opener[0] + 1:index]))
            opener = None
    if opener is not None:
        blocks.append(Fence(opener[0], len(lines), opener[1], opener[3], lines[opener[0] + 1:]))
    return blocks


def figure_slug(body: list[str]) -> str:
    """The slug a ```figure fence names: its first non-blank line."""
    return next((line.strip() for line in body if line.strip()), "")


def site_base(page_url: str) -> str:
    """The relative path from a built page back to the site root, for MkDocs' page.url."""
    depth = page_url.count("/")
    return "/".join([".."] * depth) if depth else "."


def figure_meta(figures_dir: Path, slug: str) -> dict:
    """The build's JSON for `slug`, or CheckError when there is none or it cannot be read."""
    if not SLUG.match(slug):
        raise CheckError(f"figure slug {slug!r} is not lowercase kebab-case")
    path = figures_dir / f"{slug}.json"
    if not path.is_file():
        raise CheckError(f"figure {slug!r} has no {path.as_posix()}; add docs/figures/{slug}.ts and run {REBUILD}")
    try:
        if path.stat().st_size > MAX_FILE_BYTES:
            raise CheckError(f"{path} exceeds {MAX_FILE_BYTES} bytes")
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CheckError(f"cannot read {path}: {error}") from error
    if not isinstance(data, dict):
        raise CheckError(f"{path} does not hold a JSON object")
    return data


def render_block(meta: dict, base: str) -> str:
    """The figure's markup: its JSON `html`, rendered by tools/figures/core.mjs, with `base` filled in.

    This fills a slot and renders nothing itself; `markup` in core.mjs documents the slots. `base`
    replaces `{{base}}` as given and the line that holds `{{link}}` is dropped, as `fillSlots` in
    checks.mjs does without a link. A base that looks like a slot stays literal.
    """
    template = meta.get("html")
    if not isinstance(template, str) or not template:
        raise CheckError(f"figure {meta.get('slug')!r}: its JSON records no html; rebuild with: {REBUILD}")
    lines = [line for line in template.split("\n") if LINK_SLOT not in line]
    return base.join("\n".join(lines).split(BASE_SLOT))


def expand(markdown: str, base: str, figures_dir: Path) -> tuple[str, list[str]]:
    """`markdown` with every top-level ```figure fence replaced by its rendered block.

    A fence whose figure cannot be rendered is left as it is and reported, so a strict MkDocs
    build fails on the hook's warning.
    """
    lines = markdown.split("\n")
    errors: list[str] = []
    for block in reversed([b for b in fence_blocks(markdown) if b.info == "figure"]):
        try:
            rendered = render_block(figure_meta(figures_dir, figure_slug(block.body)), base)
        except CheckError as error:
            errors.append(str(error))
            continue
        lines[block.start:block.end] = [block.indent + line for line in rendered.split("\n")]
    return "\n".join(lines), list(reversed(errors))


def on_config(config):
    """Link the figure stylesheet and the loader, as a module script, from every page, once each."""
    if CSS_URI not in config["extra_css"]:
        config["extra_css"].append(CSS_URI)
    if all(str(script) != LOADER_URI for script in config["extra_javascript"]):
        from mkdocs.config.config_options import ExtraScriptValue  # imported here so the hook loads without MkDocs in tests

        loader = ExtraScriptValue(LOADER_URI)
        loader.type = "module"
        config["extra_javascript"].append(loader)
    return config


def published_files() -> list[tuple[str, Path]]:
    """Each file the hook publishes, as (site URI, source path): figures.css, then every file in dist/.

    A missing dist/ is not an error here: the site then shows every figure as its SVG, and the
    `site` check (`node tools/figures/build.mjs site`) reports the loader it cannot find.
    """
    names = sorted(path.name for path in DIST_DIR.iterdir() if path.is_file()) if DIST_DIR.is_dir() else []
    if len(names) > MAX_DIST_FILES:
        raise CheckError(f"{DIST_DIR.as_posix()} holds more than {MAX_DIST_FILES} files")
    return [(CSS_URI, CSS_FILE)] + [(f"{DIST_URI}/{name}", DIST_DIR / name) for name in names]


def on_files(files, config):
    """Add figures.css and the player files to the site; a docs_dir file at one of their paths is kept and reported."""
    try:
        published = published_files()
    except CheckError as error:
        log.warning("%s", error)
        return files
    for uri, source in published:
        if uri in files.src_uris:
            log.warning("%s: docs_dir already holds this path, so the figures hook does not publish %s over it",
                        uri, source.name)
    added = [(uri, source) for uri, source in published if uri not in files.src_uris]
    if added:
        from mkdocs.structure.files import File  # imported here so the hook loads without MkDocs in tests

        for uri, source in added:
            files.append(File.generated(config, uri, abs_src_path=str(source)))
    return files


def on_page_markdown(markdown, page, config, files):  # noqa: ARG001 - MkDocs hook signature
    """Replace the page's figure fences before MkDocs converts the Markdown."""
    figures = Path(config["docs_dir"]) / "assets" / "figures"
    try:
        text, errors = expand(markdown, site_base(page.url) + "/assets/figures", figures)
    except CheckError as error:
        text, errors = markdown, [str(error)]
    for error in errors:
        log.warning("%s: %s", page.file.src_uri, error)
    return text
