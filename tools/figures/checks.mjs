// The figure checks (docs/adr/0016-figures-for-adopters.md, section 1). build.mjs is their command
// line; this module imports only node: builtins, core.mjs and serve.mjs, so the checks, like the
// render, need Node 22.18 or later and no npm package.
//
// Both site generators are checked: MkDocs, configured by an mkdocs.yml, and Astro Starlight,
// configured by an astro.config.* file (`siteFlavor` tells them apart by the file's name).
//
// * `sources` needs no site. It fails when a figure's JSON no longer matches its spec, the engine
//   files (ENGINE_FILES in core.mjs), its SVGs or the markup core.mjs renders from it; when a JSON
//   lacks a positive whole-number size for either SVG; when a spec has no JSON or a JSON no spec;
//   when a fence names a figure that does not exist or a kind the configuration does not enable;
//   when the README's portable block differs from the renderer; and when an evidence anchor's path
//   or symbol is gone. Without `--config` it reads the pages of every site configuration at the
//   repository root, an mkdocs.yml and an astro.config.* alike (`sourceSites`), so one command
//   checks the page fences of either generator.
// * `site` runs after `mkdocs build` or `astro build`. The kinds a build accepts come from its
//   configuration: in an mkdocs.yml the declared mermaid custom fence enables ```mermaid and a hooks
//   entry naming figures/mkdocs_hook.py enables ```figure; an Astro configuration that names
//   figures/astro.mjs enables ```figure and never Mermaid. A fence of a kind the configuration does
//   not enable is an error; where figures are enabled, a Mermaid fence is told to become a figure
//   fence. Every Mermaid fence must appear as a `<pre class="mermaid">`, every figure fence as a
//   `figure.praetor-figure[data-figure]` whose images resolve under the site to SVGs that embed the
//   props the player mounts (`<metadata id="figure-spec">`), on a page that loads the figure loader,
//   by a script source or an inline `import("…")`, with player.js beside it. Pages the
//   configuration's `exclude_docs` leaves out are skipped, as MkDocs skips them; Starlight pages
//   (`.md`, `.mdx` and the other extensions of STARLIGHT_SUFFIXES) whose name starts with an
//   underscore are skipped, as its docs loader skips them. The MkDocs hook writes relative figure
//   URLs; the Astro integration writes root-absolute ones under the base path the site is built
//   for, which `site --base` names.
// * `portable` renders figures for surfaces that run no JavaScript: `--base URL` replaces the figure
//   fences of wiki pages with absolute image URLs and a link to the interactive figure, `--wiki` does
//   the same with the `site_url` of mkdocs.yml, and `--write` refreshes `<!-- figure:SLUG -->` blocks
//   with repository-relative paths.
//
// The markup comes from core.mjs, which writes it into each figure's JSON as `html`; `portable` only
// fills its slots (`fillSlots`). The MkDocs hook, tools/figures/mkdocs_hook.py, runs inside MkDocs
// and so scans fences in Python; both scanners replay tools/figures/fence-fixtures.json, and this
// module splits and strips on Python's whitespace so they find the same fences.
//
// Page mapping assumes MkDocs' default `use_directory_urls: true` and Astro's default
// `build.format: 'directory'`; a Starlight page's URL is its slug (`starlightSlug`). The
// configuration reader handles
// the block-style YAML the site and the preset are written in; it checks a third-party tool's file
// and is no loader for praetor configuration. Its `exclude_docs` matcher follows the gitignore rules
// MkDocs applies through pathspec for the subset it covers: a slash at the start or in the middle
// anchors a pattern at docs_dir, a trailing slash matches directories only, and `*`, `?` and `[...]`
// (negated with `!` or `^`) match within one path component. It refuses a leading `!`, `**` and `\`
// instead of guessing.
import { readdirSync, readFileSync, realpathSync, statSync, writeFileSync } from 'node:fs';
import { basename, dirname, extname, isAbsolute, join, normalize, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ENGINE_FILES, SLOTS, SPEC_CLOSE, SPEC_OPEN, escapeHtml, markup, sha256 } from './core.mjs';
import { PLAYER_URI, basePath, statOrNull } from './serve.mjs';

/** The repository root, two levels above this file; every repository path below is relative to it. */
export const ROOT = fileURLToPath(new URL('../../', import.meta.url));
export const SPEC_DIR = 'docs/figures';
export const OUT_DIR = 'docs/assets/figures';
export const REBUILD = 'node tools/figures/build.mjs build';
const REFRESH = 'node tools/figures/build.mjs portable --write';

// HISS-02 bounds, so a pathological tree cannot make a check unbounded.
const MAX_PAGES = 4096;
const MAX_FILE_BYTES = 8 * 1024 * 1024;
export const MAX_LINES = 100_000;
const MAX_FIGURES = 256;
const MAX_ENTRIES = 131_072;

/** Where the figures hook and the Astro integration publish tools/figures/dist/loader.js; the loader imports PLAYER beside it. */
const LOADER = `${PLAYER_URI}/loader.js`;
const PLAYER = 'player.js';
/** A hooks entry ending in these two path parts enables figures: tools/figures/mkdocs_hook.py from any directory. */
const HOOK = ['figures', 'mkdocs_hook.py'];
const KINDS = Object.freeze(['mermaid', 'figure']);
export const EXPECTED_FENCE = Object.freeze({
  name: 'mermaid', class: 'mermaid', format: '!!python/name:pymdownx.superfences.fence_code_format',
});
/** MkDocs adds these to every exclude_docs (mkdocs/structure/files.py, _default_exclude). */
const DEFAULT_EXCLUDE = ['.*', '/templates/'];
/** gitignore syntax the exclude_docs matcher does not implement; a pattern using it is refused. */
const UNSUPPORTED_EXCLUDE = ['**', '\\'];
/** What to write instead of a fence the configuration does not enable, when it enables the replacement. */
const REPLACEMENT = {
  mermaid: ['figure', 'draw it as a ```figure fence naming a spec under docs/figures/ (docs/guides/figures.md)'],
};

// Python's whitespace: str.isspace(), str.split(), str.strip() and the regular expression class \s.
// JavaScript's \s adds U+FEFF and lacks U+001C-U+001F and U+0085. The hook scans fences in Python,
// so the checks split, strip and match on Python's set.
const WS = '\\t\\n\\v\\f\\r\\x1c-\\x20\\x85\\xa0\\u1680\\u2000-\\u200a\\u2028\\u2029\\u202f\\u205f\\u3000';
const S = `[${WS}]`;
const NS = `[^${WS}]`;
/** Python's `.`: any character but a line feed. JavaScript's `.` also stops at \r, U+2028 and U+2029. */
const ANY = '[^\\n]';
const EDGE_SPACE = new RegExp(`^${S}+|${S}+$`, 'g');
const LEAD_SPACE = new RegExp(`^${S}+`);
const SPACE_RUN = new RegExp(`${S}+`);
const STARTS_WITH_SPACE = new RegExp(`^${S}`);

const SUPERFENCES_ITEM = new RegExp(`^(${S}*)-${S}+pymdownx\\.superfences${S}*:${S}*(?:#${ANY}*)?$`);
const FENCE_KEY = new RegExp(`^${S}*(-${S}+)?(name|class|format)${S}*:${S}*(${NS}+)${S}*(?:#${ANY}*)?$`);
const HOOKS_BLOCK = new RegExp(`^hooks${S}*:${S}*(?:#${ANY}*)?$`);
const HOOKS_FLOW = new RegExp(`^hooks${S}*:${S}*\\[(${ANY}*)\\]${S}*(?:#${ANY}*)?$`);
const SITE_URL = new RegExp(`^site_url${S}*:${S}*['"]?([^'"#${WS}]+)`);
/** `site_url: !ENV NAME` or `site_url: !ENV [NAME, ..., default]`, as the MkDocs preset declares it. */
const SITE_URL_ENV = new RegExp(`^site_url${S}*:${S}*!ENV${S}+(\\[[^\\]\\n]*\\]|[^'"#${WS}]+)`);
const LIST_ITEM = new RegExp(`^${S}+-${S}+['"]?([^'"#${WS}]+)`);
const EXCLUDE_BLOCK = new RegExp(`^exclude_docs${S}*:${S}*[|>][-+]?${S}*(?:#${ANY}*)?$`);
const EXCLUDE_LINE = new RegExp(`^exclude_docs${S}*:${S}*['"]?([^'"#\\n]*?)['"]?${S}*(?:#${ANY}*)?$`);
/** A fence opener or closer: three or more backticks or tildes, then the info string. */
const FENCE_LINE = new RegExp(`^(${S}*)(\`{3,}|~{3,})${S}*([^${WS}\`{]*)(${ANY}*)$`);
/** A figure slug: lowercase kebab-case, the name of docs/figures/<slug>.ts and of its outputs. */
export const SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const MARKER = /<!-- figure:([a-z0-9-]+) -->\n([\s\S]*?)<!-- \/figure -->/g;
/** The slots of a figure's JSON `html` (SLOTS and markup in core.mjs). */
const SLOT = new RegExp(Object.values(SLOTS).map((slot) => slot.replace(/[{}]/g, '\\$&')).join('|'), 'g');
/** gitignore negates a bracket expression with `!` or `^`; the glob translation knows only `!`. */
const CARET_BRACKET = /\[\^/g;
const FOLD_CASE = process.platform === 'win32';

/** An input a check cannot read: a usage failure (exit status 2), never a pass. */
export class CheckError extends Error {}

const strip = (text) => text.replace(EDGE_SPACE, '');
const lstrip = (text) => text.replace(LEAD_SPACE, '');
/** Python's `str.split()` with no argument: the words between runs of whitespace. */
const words = (text) => text.split(SPACE_RUN).filter(Boolean);
const isObject = (value) => value != null && typeof value === 'object' && !Array.isArray(value);
const toPosix = (path) => path.split(sep).join('/');

/** A value written as Python's repr() writes a string, so findings read as the Python checker's did. */
export function quoted(value) {
  if (value === undefined || value === null) return 'None';
  if (typeof value !== 'string') return String(value);
  const quote = value.includes("'") && !value.includes('"') ? '"' : "'";
  const body = value.replaceAll('\\', '\\\\').replaceAll('\n', '\\n').replaceAll('\r', '\\r').replaceAll('\t', '\\t');
  return quote + (quote === "'" ? body.replaceAll("'", "\\'") : body) + quote;
}

const quotedList = (values) => `[${values.map(quoted).join(', ')}]`;

/** A path as Python's pathlib prints it: normalized, without a trailing separator. */
function clean(path) {
  const normal = normalize(path);
  return normal.length > 1 && normal.endsWith(sep) && !/^[A-Za-z]:\\$/.test(normal) ? normal.slice(0, -1) : normal;
}

/** `statOrNull` in serve.mjs, its read failure turned into a CheckError. */
function statOf(path) {
  try {
    return statOrNull(path);
  } catch (error) {
    throw new CheckError(error.message, { cause: error });
  }
}

const isFile = (path) => statOf(path)?.isFile() === true;
const isDirectory = (path) => statOf(path)?.isDirectory() === true;

/** The path with every symbolic link resolved, or the absolute path when it does not exist. */
function realOrResolved(path) {
  const absolute = resolve(path);
  try {
    return realpathSync(absolute);
  } catch (error) {
    if (error.code === 'ENOENT' || error.code === 'ENOTDIR') return absolute;
    throw new CheckError(`cannot resolve ${path}: ${error.message}`);
  }
}

/** Whether `target` is `base` or lies under it; both are absolute. */
function inside(base, target) {
  const rel = relative(base, target);
  return rel !== '..' && !rel.startsWith(`..${sep}`) && !isAbsolute(rel);
}

/** The bytes of `path`, refusing files over MAX_FILE_BYTES. */
function readBytes(path) {
  let size;
  try {
    size = statSync(path).size;
  } catch (error) {
    throw new CheckError(`cannot read ${path}: ${error.message}`);
  }
  if (size > MAX_FILE_BYTES) throw new CheckError(`${path} exceeds ${MAX_FILE_BYTES} bytes`);
  try {
    return readFileSync(path);
  } catch (error) {
    throw new CheckError(`cannot read ${path}: ${error.message}`);
  }
}

/** Strict UTF-8 that keeps a byte order mark, as Python's bytes.decode('utf-8') does. */
const UTF8 = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });

/** The UTF-8 text of `path` with LF line ends, refusing files over MAX_FILE_BYTES. */
export function readText(path) {
  const bytes = readBytes(path);
  try {
    return UTF8.decode(bytes).replaceAll('\r\n', '\n');
  } catch (error) {
    throw new CheckError(`cannot read ${path}: ${error.message}`);
  }
}

/** A JSON object from `path`. */
function readJson(path) {
  let data;
  try {
    data = JSON.parse(readText(path));
  } catch (error) {
    if (error instanceof CheckError) throw error;
    throw new CheckError(`cannot parse ${path}: ${error.message}`);
  }
  if (!isObject(data)) throw new CheckError(`${path} does not hold a JSON object`);
  return data;
}

/**
 * The lines of `text`, refusing more than MAX_LINES. Lines end at "\n" only, as Python-Markdown
 * splits them, with a trailing "\r" dropped; a final newline ends the last line.
 */
function boundedLines(text) {
  const lines = text.split('\n').map((line) => (line.endsWith('\r') ? line.slice(0, -1) : line));
  if (lines.length > 0 && lines.at(-1) === '') lines.pop();
  if (lines.length > MAX_LINES) throw new CheckError(`more than ${MAX_LINES} lines`);
  return lines;
}

/**
 * Every top-level fenced block in a Markdown page: its line span (end exclusive), indent, info
 * string and body. A fence nested inside a longer or different fence is literal text: a closer must
 * use the opener's character, be at least as long, and carry no info string. An unclosed fence runs
 * to the end of the page. `fence_blocks` in mkdocs_hook.py is the same scanner.
 */
export function fenceBlocks(text) {
  const lines = boundedLines(text);
  const blocks = [];
  let opener = null;
  lines.forEach((line, index) => {
    const match = FENCE_LINE.exec(line);
    if (!match) return;
    const [, indent, marker, info, rest] = match;
    if (!opener) opener = { start: index, indent, marker, info: info.toLowerCase() };
    else if (marker[0] === opener.marker[0] && marker.length >= opener.marker.length && !info && !strip(rest)) {
      blocks.push({ start: opener.start, end: index + 1, indent: opener.indent, info: opener.info, body: lines.slice(opener.start + 1, index) });
      opener = null;
    }
  });
  if (opener) blocks.push({ start: opener.start, end: lines.length, indent: opener.indent, info: opener.info, body: lines.slice(opener.start + 1) });
  return blocks;
}

/** The body lines of every top-level fence that opens with the `info` string. */
export const fences = (text, info) => fenceBlocks(text).filter((block) => block.info === info).map((block) => block.body);
/** The slug a ```figure fence names: its first non-blank line. */
export const figureSlug = (body) => strip(body.find((line) => strip(line)) ?? '');
/** The slug of every top-level ```figure fence in a page. */
export const figureSlugs = (text) => fences(text, 'figure').map(figureSlug);

/** The lines nested under the `- pymdownx.superfences:` entry, or [] when there is none. */
function superfencesBody(lines) {
  const index = lines.findIndex((line) => SUPERFENCES_ITEM.test(line));
  if (index < 0) return [];
  const indent = SUPERFENCES_ITEM.exec(lines[index])[1].length;
  const body = [];
  for (const nested of lines.slice(index + 1)) {
    const stripped = strip(nested);
    if (stripped && !stripped.startsWith('#') && nested.length - lstrip(nested).length <= indent) break;
    body.push(nested);
  }
  return body;
}

/** The name, class and format of every custom fence the superfences entry declares. */
export function declaredFences(text) {
  const declared = [];
  for (const line of superfencesBody(boundedLines(text))) {
    const match = FENCE_KEY.exec(line);
    if (!match) continue;
    if (match[1] || declared.length === 0) declared.push({});
    declared.at(-1)[match[2]] = match[3].replace(/^['"]+|['"]+$/g, '');
  }
  return declared;
}

const sameFence = (fence) => Object.keys(fence).length === Object.keys(EXPECTED_FENCE).length &&
  Object.entries(EXPECTED_FENCE).every(([key, value]) => fence[key] === value);

/** Why an mkdocs.yml does not declare the mermaid fence, or null when it does. */
export function configError(text) {
  if (declaredFences(text).some(sameFence)) return null;
  const wanted = Object.entries(EXPECTED_FENCE).map(([key, value]) => `${key}: ${value}`).join(', ');
  return `pymdownx.superfences declares no custom fence with ${wanted}`;
}

const unquote = (item) => strip(item).replace(/^['"]+|['"]+$/g, '');

/** The paths the top-level `hooks:` key lists, in block or flow style. */
export function declaredHooks(text) {
  const lines = boundedLines(text);
  for (const [index, line] of lines.entries()) {
    const flow = HOOKS_FLOW.exec(line);
    if (flow) return flow[1].split(',').filter((item) => strip(item)).map(unquote);
    if (HOOKS_BLOCK.test(line)) return blockHooks(lines.slice(index + 1));
  }
  return [];
}

function blockHooks(following) {
  const hooks = [];
  for (const nested of following) {
    const item = LIST_ITEM.exec(nested);
    if (!item && strip(nested) && !lstrip(nested).startsWith('#')) break;
    if (item) hooks.push(item[1]);
  }
  return hooks;
}

/** The patterns of the top-level `exclude_docs` key: a block scalar, or one plain value. */
export function excludedPatterns(text) {
  const lines = boundedLines(text);
  for (const [index, line] of lines.entries()) {
    if (EXCLUDE_BLOCK.test(line)) return blockScalarLines(lines.slice(index + 1));
    const single = EXCLUDE_LINE.exec(line);
    if (single) return strip(single[1]) ? [strip(single[1])] : [];
  }
  return [];
}

/** The non-blank, non-comment lines of a block scalar: the indented lines that follow its key. */
function blockScalarLines(following) {
  const patterns = [];
  for (const line of following) {
    if (strip(line) && !STARTS_WITH_SPACE.test(line)) break;
    const entry = strip(line);
    if (entry && !entry.startsWith('#')) patterns.push(entry);
  }
  return patterns;
}

const escapeRegExp = (text) => text.replace(/[.*+?^${}()|[\]\\/-]/g, '\\$&');

/**
 * The members of a bracket expression `pattern[start, end)` as a regular-expression class body, as
 * Python's fnmatch.translate writes it: empty ranges dropped, a hyphen that makes no range escaped.
 */
function bracketBody(pattern, start, end) {
  const stuff = pattern.slice(start, end);
  if (!stuff.includes('-')) return stuff.replaceAll('\\', '\\\\');
  const chunks = [];
  let from = start;
  let k = pattern[start] === '!' ? start + 2 : start + 1;
  for (let guard = 0; guard <= end - start; guard++) {
    k = pattern.indexOf('-', k);
    if (k < 0 || k >= end) break;
    chunks.push(pattern.slice(from, k));
    from = k + 1;
    k += 3;
  }
  const last = pattern.slice(from, end);
  if (last) chunks.push(last);
  else chunks[chunks.length - 1] += '-';
  for (let i = chunks.length - 1; i > 0; i--) {
    if (chunks[i - 1].at(-1) > chunks[i][0]) chunks.splice(i - 1, 2, chunks[i - 1].slice(0, -1) + chunks[i].slice(1));
  }
  return chunks.map((chunk) => chunk.replaceAll('\\', '\\\\').replaceAll('-', '\\-')).join('-');
}

/** One bracket expression starting after the `[` at `start`: its regular expression and where it ends. */
function bracket(pattern, start) {
  let end = start;
  if (pattern[end] === '!') end += 1;
  if (pattern[end] === ']') end += 1;
  end = pattern.indexOf(']', end);
  if (end < 0) return { source: '\\[', next: start };
  const body = bracketBody(pattern, start, end);
  if (!body) return { source: '(?!)', next: end + 1 };
  if (body === '!') return { source: '[\\s\\S]', next: end + 1 };
  let members = body.replace(/[&~|\]]/g, '\\$&');
  if (members[0] === '!') members = `^${members.slice(1)}`;
  else if (members[0] === '^' || members[0] === '[') members = `\\${members}`;
  return { source: `[${members}]`, next: end + 1 };
}

/** A glob as Python's fnmatch.translate reads it: `*`, `?` and `[...]`, negated with `!`. */
export function globRegExp(pattern) {
  let source = '';
  let i = 0;
  for (let guard = 0; i < pattern.length && guard <= pattern.length; guard++) {
    const c = pattern[i];
    i += 1;
    if (c === '*') source += '[\\s\\S]*';
    else if (c === '?') source += '[\\s\\S]';
    else if (c === '[') {
      const part = bracket(pattern, i);
      source += part.source;
      i = part.next;
    } else source += escapeRegExp(c);
  }
  return new RegExp(`^(?:${source})$`);
}

const globMatches = (name, glob) => globRegExp(glob).test(name);

/**
 * Whether a gitignore-style `pattern` excludes the docs-relative path split into `parts`. It is
 * matched component by component, so no wildcard crosses a `/`. A pattern with a slash at its start
 * or in its middle is anchored at docs_dir and matches a leading run of components (a matched
 * directory excludes everything under it); one without matches any single component. A trailing
 * slash matches a directory only, never the page's own file name. A bare `/` matches nothing.
 */
export function patternMatches(parts, pattern) {
  if (pattern.startsWith('!') || UNSUPPORTED_EXCLUDE.some((token) => pattern.includes(token))) {
    throw new CheckError(`exclude_docs pattern ${quoted(pattern)} uses gitignore syntax this check does not implement ` +
      `(a leading !, ${UNSUPPORTED_EXCLUDE.join(', ')})`);
  }
  const directory = pattern.endsWith('/');
  const body = directory ? pattern.slice(0, -1) : pattern;
  const globs = (body.startsWith('/') ? body.slice(1) : body).split('/').map((glob) => glob.replace(CARET_BRACKET, '[!'));
  if (globs.length === 1 && globs[0] === '') return false;
  const names = directory ? parts.slice(0, -1) : parts;
  if (!body.includes('/')) return names.some((name) => globMatches(name, globs[0]));
  return globs.length <= names.length && globs.every((glob, index) => globMatches(names[index], glob));
}

/** Whether MkDocs leaves the page at `parts` out: its default exclusions or `patterns`. */
export const isExcluded = (parts, patterns) => [...DEFAULT_EXCLUDE, ...patterns].some((pattern) => patternMatches(parts, pattern));

/** The value of `!ENV NAME` or `!ENV [NAME, ..., default]`: the first variable set, else the default. */
function environmentValue(spec) {
  const items = spec.startsWith('[') ? spec.slice(1, -1).split(',').map(unquote).filter(Boolean) : [spec];
  const names = items.length > 1 ? items.slice(0, -1) : items;
  const set = names.find((name) => Object.hasOwn(process.env, name));
  if (set !== undefined) return process.env[set];
  if (items.length > 1) return items.at(-1);
  throw new CheckError(`site_url reads the environment variable ${names.join(', ')}, which is not set`);
}

/** The top-level `site_url` of an mkdocs.yml, reading a `!ENV` tag from the environment. */
export function siteUrl(text) {
  for (const line of boundedLines(text)) {
    const env = SITE_URL_ENV.exec(line);
    if (env) return environmentValue(env[1]);
    const match = SITE_URL.exec(line);
    if (match) return match[1];
  }
  throw new CheckError('the MkDocs configuration declares no site_url');
}

/** The path parts of a hooks entry, as Python's pathlib splits it. */
const hookParts = (hook) => hook.split(FOLD_CASE ? /[\\/]/ : '/').filter((part) => part && part !== '.');

/** The diagram kinds an mkdocs.yml renders: the mermaid fence and the figures hook. */
export function enabledKinds(text) {
  const kinds = new Set();
  if (configError(text) === null) kinds.add('mermaid');
  const enabled = declaredHooks(text).some((hook) => hookParts(hook).slice(-2).join('/') === HOOK.join('/'));
  if (enabled) kinds.add('figure');
  return kinds;
}

/** Compares two paths split into parts, as pathlib sorts paths. */
function compareParts(a, b) {
  const fold = FOLD_CASE ? (part) => part.toLowerCase() : (part) => part;
  for (let i = 0; i < Math.min(a.length, b.length); i++) {
    const [x, y] = [fold(a[i]), fold(b[i])];
    if (x !== y) return x < y ? -1 : 1;
  }
  return a.length - b.length;
}

const hasSuffix = (name, suffix) => (FOLD_CASE ? name.toLowerCase() : name).endsWith(suffix);

/** The entries of `dir`, or [] when it cannot be listed, as pathlib's glob skips such a directory. */
function entries(dir) {
  try {
    return readdirSync(dir, { withFileTypes: true });
  } catch (error) {
    if (['ENOENT', 'ENOTDIR', 'EACCES', 'EPERM'].includes(error.code)) return [];
    throw new CheckError(`cannot list ${dir}: ${error.message}`);
  }
}

/**
 * Every file under `dir` whose name ends in one of `suffixes`, as `Path.rglob('*.md')` finds the
 * `.md` ones (symbolic links to directories are not followed), sorted.
 */
function markdownParts(dir, suffixes) {
  const found = [];
  const stack = [[]];
  let seen = 0;
  for (let visited = 0; stack.length > 0 && visited < MAX_ENTRIES; visited++) {
    const parts = stack.pop();
    const listed = entries(join(dir, ...parts));
    seen += listed.length;
    if (seen > MAX_ENTRIES) break;
    for (const entry of listed) {
      if (suffixes.some((suffix) => hasSuffix(entry.name, suffix))) found.push([...parts, entry.name]);
      if (entry.isDirectory()) stack.push([...parts, entry.name]);
    }
  }
  if (stack.length > 0 || seen > MAX_ENTRIES) throw new CheckError(`more than ${MAX_ENTRIES} entries under ${dir}`);
  return found.sort(compareParts);
}

/** The pages under `docsDir` whose names end in one of `suffixes` and that `skip` does not leave out. */
function pagesUnder(docsDir, suffixes, skip) {
  const pages = [];
  for (const parts of markdownParts(docsDir, suffixes)) {
    if (skip(parts)) continue;
    pages.push(join(docsDir, ...parts));
    if (pages.length > MAX_PAGES) throw new CheckError(`more than ${MAX_PAGES} Markdown pages under ${docsDir}`);
  }
  return pages;
}

/** Every page MkDocs builds from `docsDir`: its default exclusions and `excluded` left out. */
export const markdownPages = (docsDir, excluded = []) => pagesUnder(docsDir, ['.md'], (parts) => isExcluded(parts, excluded));

/** The HTML file MkDocs writes for `page` with directory URLs. */
export function pageOutput(docsDir, siteDir, page) {
  const rel = relative(docsDir, page);
  const stem = basename(rel, extname(rel));
  if (stem === 'index' || stem === 'README') return join(siteDir, dirname(rel), 'index.html');
  return join(siteDir, dirname(rel), stem, 'index.html');
}

/**
 * The file extensions Starlight's docs loader reads (`docsExtensions` in
 * @astrojs/starlight/loaders.ts, 0.32). Its glob skips a file whose name starts with an underscore
 * and, as every glob without the `dot` option does, a dot-file or dot-directory.
 */
export const STARLIGHT_SUFFIXES = Object.freeze(['.markdown', '.mdown', '.mkdn', '.mkd', '.mdwn', '.md', '.mdx']);
/** What github-slugger drops from a path segment: all but letters, marks, numbers, connector punctuation, space and hyphen. */
const SLUG_DROP = /[^\p{L}\p{M}\p{N}\p{Pc} -]/gu;
const FRONTMATTER_SLUG = /^slug[ \t]*:[ \t]*(['"]?)(.*?)\1[ \t]*(?:#.*)?$/;

/** Every page Starlight builds from its docs collection directory `docsDir`. */
export const starlightPages = (docsDir) => pagesUnder(docsDir, STARLIGHT_SUFFIXES,
  (parts) => parts.some((part) => part.startsWith('.')) || parts.at(-1).startsWith('_'));

/** The `slug` a page's front matter declares, or null. */
export function frontmatterSlug(text) {
  const lines = boundedLines(text);
  if (strip(lines[0] ?? '') !== '---') return null;
  for (const line of lines.slice(1)) {
    if (strip(line) === '---') return null;
    const match = FRONTMATTER_SLUG.exec(line);
    if (match) return match[2];
  }
  return null;
}

/**
 * The slug Astro's glob loader gives a page (`generateIdDefault` in
 * astro/dist/content/loaders/glob.js, Astro 5): the front matter's `slug`, else the path without its
 * extension, each segment lowercased with github-slugger's dropped characters removed and spaces
 * turned into hyphens, and a final `/index` cut.
 */
export function starlightSlug(docsDir, page, text) {
  const declared = frontmatterSlug(text);
  if (declared !== null) return declared;
  const rel = relative(docsDir, page);
  const segments = rel.slice(0, rel.length - extname(rel).length).split(sep);
  return segments.map((segment) => segment.toLowerCase().replace(SLUG_DROP, '').replaceAll(' ', '-')).join('/').replace(/\/index$/, '');
}

/** The HTML file Astro writes for a Starlight page with its default `build.format: 'directory'`. */
export function starlightOutput(docsDir, siteDir, page, text) {
  const slug = starlightSlug(docsDir, page, text).split('/').filter(Boolean);
  if (slug.length === 0 || (slug.length === 1 && slug[0] === 'index')) return join(siteDir, 'index.html');
  return join(siteDir, ...slug, 'index.html');
}

/**
 * A string literal in an Astro configuration that names the integration: tools/figures/astro.mjs
 * from any directory, with forward slashes or (escaped) backslashes.
 */
const ASTRO_INTEGRATION = /['"`](?:[^'"`\n]*[\\/])?figures[\\/]+astro\.mjs['"`]/;

/** The diagram kinds an Astro configuration renders: figures when it names the integration, Mermaid never. */
export const astroKinds = (text) => new Set(ASTRO_INTEGRATION.test(text) ? ['figure'] : []);

/**
 * How one site generator builds pages: MkDocs from an mkdocs.yml, Astro Starlight from an
 * astro.config.* file. `output` names the HTML file a page becomes; `base` says whether figure
 * URLs are root-absolute under the site's base path (`site --base`); `publisher` names what
 * publishes the loader. `docs` is the pages directory the generator reads by default, relative to
 * the repository root: MkDocs' `docs_dir`, and the `docs` collection Starlight's docsLoader reads
 * under Astro's `srcDir`.
 */
const MKDOCS = Object.freeze({
  generator: 'MkDocs', build: 'mkdocs build', publisher: 'the figures hook', base: false, docs: 'docs',
  kinds: enabledKinds,
  pages: (docsDir, text) => markdownPages(docsDir, excludedPatterns(text)),
  output: (docsDir, siteDir, page) => pageOutput(docsDir, siteDir, page),
});
const STARLIGHT = Object.freeze({
  generator: 'Astro', build: 'astro build', publisher: 'tools/figures/astro.mjs', base: true, docs: 'src/content/docs',
  kinds: astroKinds,
  pages: (docsDir) => starlightPages(docsDir),
  output: starlightOutput,
});
const ASTRO_CONFIG = /^astro\.config\.[cm]?[jt]s$/i;

/** The generator a configuration file belongs to, told by its name: astro.config.* is Astro, any other file MkDocs. */
export const siteFlavor = (config) => (ASTRO_CONFIG.test(basename(config)) ? STARLIGHT : MKDOCS);

/**
 * The configuration file names each generator looks for in a project root, in its own order:
 * MkDocs 1.6 (`_open_config_file` in mkdocs/config/base.py) and Astro 5 (`configPaths` in
 * astro/dist/core/config/config.js).
 */
const SITE_CONFIGS = Object.freeze([
  Object.freeze(['mkdocs.yml', 'mkdocs.yaml']),
  Object.freeze(['astro.config.mjs', 'astro.config.js', 'astro.config.ts', 'astro.config.mts', 'astro.config.cjs', 'astro.config.cts']),
]);

/**
 * The site configurations at the repository root `root`: for each generator, the file it would load
 * there, MkDocs first. Empty when the repository has neither.
 */
export const siteConfigs = (root) => SITE_CONFIGS.map((names) => names.find((name) => isFile(join(root, name)))).filter(Boolean);

/**
 * The sites whose pages `sources` reads, each a `config` and a `docs` directory relative to `root`.
 * A named `config` is the one site; otherwise every configuration `siteConfigs` finds, and MkDocs'
 * defaults when it finds none. A site's `docs` is the one named, else its generator's default. A
 * named `docs` without a named `config` is ambiguous when the root holds two configurations.
 */
export function sourceSites(root, docs = undefined, config = undefined) {
  const configs = config === undefined ? siteConfigs(root) : [config];
  if (configs.length === 0) return [{ config: SITE_CONFIGS[0][0], docs: docs ?? MKDOCS.docs }];
  if (docs !== undefined && configs.length > 1) {
    throw new CheckError(`--docs ${docs} names one pages directory, but the repository root holds ${configs.join(' and ')}; ` +
      'name the configuration it belongs to with --config');
  }
  return configs.map((name) => ({ config: name, docs: docs ?? siteFlavor(name).docs }));
}

/** The finding for a disabled `kind`, naming its replacement when the configuration enables it. */
function kindError(page, kind, kinds) {
  const message = `${page}: \`\`\`${kind} fence, but the configuration does not enable ${kind} diagrams`;
  const [replacement, advice] = REPLACEMENT[kind] ?? ['', ''];
  return kinds.has(replacement) ? `${message}; ${advice}` : message;
}

/** A finding for every diagram fence on `page` whose kind the configuration does not enable. */
export function kindErrors(page, text, kinds) {
  const found = new Set(fenceBlocks(text).map((block) => block.info));
  return KINDS.filter((kind) => found.has(kind) && !kinds.has(kind)).sort().map((kind) => kindError(page, kind, kinds));
}

// ---------------------------------------------------------------------------------------------
// Built pages: an HTML scanner that reads start and end tags as Python's html.parser does
// ---------------------------------------------------------------------------------------------

// html.parser's tolerant patterns (Python 3.14 Lib/html/parser.py): tagfind_tolerant,
// attrfind_tolerant and locatetagend.
const TAG_NAME = /([a-zA-Z][^\t\n\r\f />]*)(?:[\t\n\r\f ]|\/(?!>))*/y;
const ATTRIBUTE = /((?<=['"\t\n\r\f /])[^\t\n\r\f />][^\t\n\r\f /=>]*)([\t\n\r\f ]*=[\t\n\r\f ]*('[^']*'|"[^"]*"|(?!['"])[^>\t\n\r\f ]*))?(?:[\t\n\r\f ]|\/(?!>))*/y;
const TAG_END = /[a-zA-Z][^\t\n\r\f />]*[\t\n\r\f /]*(?:(?<=['"\t\n\r\f /])[^\t\n\r\f />][^\t\n\r\f /=>]*(?:[\t\n\r\f ]*=[\t\n\r\f ]*(?:'[^']*'|"[^"]*"|(?!['"])[^>\t\n\r\f ]*))?[\t\n\r\f /]*)*>?/y;
const COMMENT_ABRUPT = /-?>/y;
const COMMENT_CLOSE = /--!?>/g;
/** Elements whose content is text up to their end tag: CDATA_CONTENT_ELEMENTS and RCDATA_CONTENT_ELEMENTS. */
const RAW_TEXT = new Map(['script', 'style', 'xmp', 'iframe', 'noembed', 'noframes', 'textarea', 'title']
  .map((tag) => [tag, new RegExp(`</${tag}(?=[\\t\\n\\r\\f />])`, 'ig')]));
// <plaintext> never ends: everything after it is text.
RAW_TEXT.set('plaintext', /(?!)/g);
const ATTRIBUTE_REFERENCE = /&(#[0-9]+|#[xX][0-9a-fA-F]+|[a-zA-Z][a-zA-Z0-9]*)[;=]?/g;
/** The named references an attribute value decodes: the XML five, spelled as html5 lists them. */
const NAMED_REFERENCES = { amp: '&', 'amp;': '&', lt: '<', 'lt;': '<', gt: '>', 'gt;': '>', quot: '"', 'quot;': '"', 'apos;': "'" };

/** A numeric character reference: its code point, or U+FFFD for 0, a surrogate or a value beyond U+10FFFF. */
function numericReference(reference) {
  const tail = reference.endsWith('=') ? '=' : '';
  const digits = reference.slice(2).replace(/[;=]$/, '');
  const code = /^[xX]/.test(digits) ? Number.parseInt(digits.slice(1), 16) : Number.parseInt(digits, 10);
  const invalid = code === 0 || (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff;
  return (invalid ? '\ufffd' : String.fromCodePoint(code)) + tail;
}

/** An attribute value with its character references decoded, as html.parser decodes one (a subset of named references). */
function decodeAttribute(value) {
  return value.replace(ATTRIBUTE_REFERENCE, (reference) => {
    if (reference.startsWith('&#')) return numericReference(reference);
    const name = reference.slice(1);
    return !reference.endsWith('=') && Object.hasOwn(NAMED_REFERENCES, name) ? NAMED_REFERENCES[name] : reference;
  });
}

function attributeValue(match) {
  if (!match[2]) return null;
  const raw = match[3];
  const value = (raw[0] === "'" || raw[0] === '"') && raw.at(-1) === raw[0] ? raw.slice(1, -1) : raw;
  return value ? decodeAttribute(value) : value;
}

/** The end of the tag whose name starts at `at`, or -1 when the page ends inside it. */
function tagEnd(html, at) {
  TAG_END.lastIndex = at;
  TAG_END.exec(html);
  return html[TAG_END.lastIndex - 1] === '>' ? TAG_END.lastIndex : -1;
}

/** The URL attribute of each image element inside a figure. */
const IMAGE_URL = { img: 'src', source: 'srcset' };

function handleStart(scan, tag, attributes) {
  const classes = words(attributes.get('class') ?? '');
  if (tag === 'pre' && classes.includes('mermaid')) scan.mermaid += 1;
  else if (tag === 'script' && attributes.get('src')) scan.scripts.push(attributes.get('src'));
  else if (tag === 'script') scan.inline = true;
  else if (tag === 'figure') figureStart(scan, attributes, classes);
  else if (scan.depth && Object.hasOwn(IMAGE_URL, tag)) scan.figures.at(-1).urls.push(attributes.get(IMAGE_URL[tag]) || '');
}

function figureStart(scan, attributes, classes) {
  if (scan.depth) scan.depth += 1;
  else if (classes.includes('praetor-figure') && attributes.get('data-figure') && scan.figures.length < MAX_FIGURES) {
    scan.figures.push({ slug: attributes.get('data-figure'), urls: [] });
    scan.depth = 1;
  }
}

function handleEnd(scan, tag) {
  if (tag === 'figure' && scan.depth) scan.depth -= 1;
}

/** A start tag at `at`: where scanning resumes, and the raw-text element it opens, if any. */
function startTag(html, at, scan) {
  const end = tagEnd(html, at + 1);
  if (end < 0) return { next: -1, raw: null };
  TAG_NAME.lastIndex = at + 1;
  const tag = TAG_NAME.exec(html)[1].toLowerCase();
  const attributes = new Map();
  let k = TAG_NAME.lastIndex;
  for (let guard = 0; k < end && guard < end; guard++) {
    ATTRIBUTE.lastIndex = k;
    const match = ATTRIBUTE.exec(html);
    if (!match) break;
    attributes.set(match[1].toLowerCase(), attributeValue(match));
    k = ATTRIBUTE.lastIndex;
  }
  const rest = strip(html.slice(k, end));
  if (rest !== '>' && rest !== '/>') return { next: end, raw: null };
  handleStart(scan, tag, attributes);
  if (rest === '/>') handleEnd(scan, tag);
  return { next: end, raw: rest === '>' && RAW_TEXT.has(tag) ? tag : null };
}

const after = (html, text, from) => {
  const found = html.indexOf(text, from);
  return found < 0 ? -1 : found + text.length;
};

/** An end tag at `at`: where scanning resumes. */
function endTag(html, at, scan) {
  if (html.indexOf('>', at + 2) < 0) return -1;
  if (!/[a-zA-Z]/.test(html[at + 2] ?? '')) return html[at + 2] === '>' ? at + 3 : after(html, '>', at + 2);
  const end = tagEnd(html, at + 2);
  if (end < 0) return -1;
  TAG_NAME.lastIndex = at + 2;
  handleEnd(scan, TAG_NAME.exec(html)[1].toLowerCase());
  return end;
}

function commentEnd(html, at) {
  COMMENT_ABRUPT.lastIndex = at + 4;
  let match = COMMENT_ABRUPT.exec(html);
  if (!match) {
    COMMENT_CLOSE.lastIndex = at + 4;
    match = COMMENT_CLOSE.exec(html);
  }
  return match ? match.index + match[0].length : -1;
}

function declarationEnd(html, at) {
  if (html.startsWith('<![CDATA[', at)) return after(html, ']]>', at + 9);
  if (html.slice(at, at + 9).toLowerCase() === '<!doctype') return after(html, '>', at + 9);
  return after(html, '>', at + 2);
}

/** The markup at `at`, which starts with `<`: where scanning resumes (-1: the page ends inside it). */
function markupAt(html, at, scan) {
  if (/[a-zA-Z]/.test(html[at + 1] ?? '')) return startTag(html, at, scan);
  if (html.startsWith('</', at)) return { next: endTag(html, at, scan), raw: null };
  if (html.startsWith('<!--', at)) return { next: commentEnd(html, at), raw: null };
  if (html.startsWith('<?', at)) return { next: after(html, '>', at + 2), raw: null };
  if (html.startsWith('<!', at)) return { next: declarationEnd(html, at), raw: null };
  return { next: at + 1, raw: null };
}

/** Where the next markup starts: the next `<`, or inside a raw-text element only its end tag. */
function nextMarkup(html, from, raw) {
  if (!raw) return html.indexOf('<', from);
  const end = RAW_TEXT.get(raw);
  end.lastIndex = from;
  return end.exec(html)?.index ?? -1;
}

/**
 * A module an inline script imports by a string literal, `import("URL")`: the head script the Astro
 * integration writes to load the figure loader.
 */
const INLINE_IMPORT = /\bimport\(\s*(['"])([^'"\n]+)\1\s*\)/g;

/** Records the modules the inline script whose text starts at `from` imports, as script sources. */
function inlineImports(html, from, scan) {
  const end = nextMarkup(html, from, 'script');
  const code = html.slice(from, end < 0 ? html.length : end);
  scan.scripts.push(...Array.from(code.matchAll(INLINE_IMPORT), (match) => match[2]).slice(0, MAX_FIGURES));
}

/**
 * What a built page holds that the diagram checks read: Mermaid <pre> elements, figure elements
 * with their image URLs, and the scripts it loads: each script's source and each module an inline
 * script imports by a string literal. Markup the page ends inside is ignored, as html.parser
 * ignores it at close().
 */
export function scanPage(html) {
  const scan = { mermaid: 0, figures: [], scripts: [], depth: 0, inline: false };
  let at = 0;
  let raw = null;
  for (let guard = 0; at < html.length && guard <= html.length; guard++) {
    const start = nextMarkup(html, at, raw);
    if (start < 0) break;
    const step = markupAt(html, start, scan);
    if (step.next < 0) break;
    if (step.raw === 'script' && scan.inline) inlineImports(html, step.next, scan);
    scan.inline = false;
    [at, raw] = [step.next, step.raw];
  }
  return { mermaid: scan.mermaid, figures: scan.figures, scripts: scan.scripts };
}

/** How many Mermaid diagrams Material will draw from a built page. */
export const renderedDiagrams = (html) => scanPage(html).mermaid;

/**
 * The file a URL on the page at `output` names, with every symbolic link resolved: a relative URL
 * from the page's directory, a root-absolute one from the site directory once the site's base path
 * is cut.
 */
function urlTarget(output, built, url) {
  const path = url.split('#')[0].split('?')[0];
  if (built.base && path.startsWith(built.base)) return realOrResolved(join(built.dir, path.slice(built.base.length)));
  return realOrResolved(resolve(dirname(output), path));
}

/**
 * Whether a URL on the page at `output` names a file inside the built site: a relative URL, or, on
 * a site with a base path (Astro), a root-absolute URL under that base path.
 */
function resolves(output, built, url) {
  if (!url || url.includes('://') || url.startsWith('//') || url.startsWith('data:')) return false;
  if (url.startsWith('/') && !(built.base && url.startsWith(built.base))) return false;
  const target = urlTarget(output, built, url);
  return isFile(target) && inside(realOrResolved(built.dir), target);
}

/**
 * Why an SVG does not embed the props the player mounts, or null. The loader reads the same
 * markers in the browser (`specFromSvg` in tools/figures/loader.ts), which cannot import this
 * Node module; figures.test.mjs holds the two readers to the same verdicts.
 */
export function svgSpecError(svg) {
  const start = svg.indexOf(SPEC_OPEN);
  if (start < 0) return 'carries no <metadata id="figure-spec">';
  const end = svg.indexOf(SPEC_CLOSE, start + SPEC_OPEN.length);
  if (end < 0) return 'does not close its <metadata id="figure-spec">';
  let spec;
  try {
    spec = JSON.parse(svg.slice(start + SPEC_OPEN.length, end));
  } catch (error) {
    return `embeds a figure spec that is not JSON (${error.message})`;
  }
  const props = isObject(spec) ? spec.props : null;
  return isObject(props) && isObject(props.layout) && Array.isArray(props.edges) ? null : 'embeds a figure spec without props.layout and props.edges';
}

/** Spec findings for every figure image that resolves; `built.seen` keeps each SVG's verdict, so it is read once per check. */
function specErrors(page, output, built, figures) {
  const errors = [];
  for (const figure of figures) {
    for (const url of figure.urls.filter((candidate) => resolves(output, built, candidate))) {
      const target = urlTarget(output, built, url);
      if (!built.seen.has(target)) built.seen.set(target, svgSpecError(readText(target)));
      if (built.seen.get(target)) errors.push(`${page}: figure ${figure.slug}: ${url} ${built.seen.get(target)}; rebuild with: ${REBUILD}`);
    }
  }
  return errors;
}

/** Why the page cannot mount its figures: no loader script that resolves, or no player.js beside it. */
function loaderErrors(page, output, built, scripts) {
  const loader = scripts.find((src) => src.endsWith(LOADER) && resolves(output, built, src));
  if (!loader) return [`${page}: holds figures but loads no ${LOADER} (${built.flavor.publisher} publishes it from tools/figures/dist/)`];
  if (isFile(join(dirname(urlTarget(output, built, loader)), PLAYER))) return [];
  return [`${page}: loads ${loader}, but no ${PLAYER} sits beside it for the loader to import`];
}

function mermaidError(page, output, expected, scanned) {
  if (scanned.mermaid === expected) return [];
  return [`${page}: ${expected} mermaid block(s), ${scanned.mermaid} rendered as diagrams in ${output}; ` +
    'a block without <pre class="mermaid"> ships as a code listing'];
}

/** Why the built page does not show its figures: count, slugs, image URLs, embedded specs and the loader. */
function figureErrors(page, output, built, slugs, scanned) {
  const shown = scanned.figures.map((figure) => figure.slug);
  if (shown.length !== slugs.length || shown.some((slug, index) => slug !== slugs[index])) {
    return [`${page}: figure fence(s) ${quotedList(slugs)}, but ${output} holds figure.praetor-figure ${quotedList(shown)}`];
  }
  const errors = scanned.figures.flatMap((figure) => figure.urls.filter((url) => !resolves(output, built, url))
    .map((url) => `${page}: figure ${figure.slug}: ${url || '(empty URL)'} does not resolve to a file under ${built.dir}`));
  return [...errors, ...specErrors(page, output, built, scanned.figures), ...loaderErrors(page, output, built, scanned.scripts)];
}

/** Findings for one page and how many diagrams it holds. */
function pageErrors(built, page) {
  const text = readText(page);
  const expected = fences(text, 'mermaid').length;
  const slugs = figureSlugs(text);
  const count = expected + slugs.length;
  if (count === 0) return { errors: [], count };
  const errors = kindErrors(page, text, built.kinds);
  const output = built.flavor.output(built.docs, built.dir, page, text);
  if (!isFile(output)) return { errors: [...errors, `${page}: ${count} diagram(s) but ${built.flavor.generator} wrote no ${output}`], count };
  const scanned = scanPage(readText(output));
  if (expected) errors.push(...mermaidError(page, output, expected, scanned));
  if (slugs.length) errors.push(...figureErrors(page, output, built, slugs, scanned));
  return { errors, count };
}

/**
 * Configuration and rendered-page findings for one built site, and how many diagrams were checked.
 * The configuration's name tells the generator (`siteFlavor`). `base` is the base path an Astro
 * site is built for ('/' when omitted); an MkDocs site takes none, because the figures hook writes
 * relative URLs.
 */
export function checkSite(config, docs, site, base = undefined) {
  const flavor = siteFlavor(config);
  if (base !== undefined && !flavor.base) {
    throw new CheckError(`a base path applies to an Astro site; ${config} is an ${flavor.generator} configuration, whose figure URLs are relative`);
  }
  const [docsDir, siteDir] = [clean(docs), clean(site)];
  if (!isDirectory(docsDir)) throw new CheckError(`${docsDir} is not a directory`);
  if (!isFile(join(siteDir, 'index.html'))) throw new CheckError(`${siteDir} holds no built site (no index.html); run ${flavor.build} first`);
  const text = readText(clean(config));
  const built = { flavor, docs: docsDir, dir: siteDir, base: flavor.base ? basePath(base) : null, kinds: flavor.kinds(text), seen: new Map() };
  const errors = [];
  let diagrams = 0;
  for (const page of flavor.pages(docsDir, text)) {
    const found = pageErrors(built, page);
    errors.push(...found.errors);
    diagrams += found.count;
  }
  return { errors, diagrams };
}

// ---------------------------------------------------------------------------------------------
// Sources and portable blocks
// ---------------------------------------------------------------------------------------------

/**
 * sha256sum-style manifest of the files `rels` under `root`, in the order given, hashed: one value
 * that changes when the bytes or the name of any of them does.
 */
export function filesDigest(root, rels) {
  const lines = rels.map((rel) => `${sha256(readFileSync(join(root, rel)))}  ${rel}\n`);
  return sha256(lines.join(''));
}

/** The digest of ENGINE_FILES: one value that changes when any of them does. */
export function engineHash(root = ROOT) {
  return filesDigest(root, ENGINE_FILES);
}

/**
 * The figure's markup: its JSON `html`, rendered by core.mjs, with the slots filled. `base` replaces
 * `{{base}}` as given, `link` replaces `{{link}}` HTML-escaped, and without a `link` the line that
 * holds `{{link}}` is dropped. The slots are filled in one pass, so a value that looks like a slot
 * stays literal. `render_block` in mkdocs_hook.py fills the base the same way.
 */
export function fillSlots(meta, base, link = null) {
  let template = meta.html;
  if (typeof template !== 'string' || !template) {
    throw new CheckError(`figure ${quoted(meta.slug)}: its JSON records no html; rebuild with: ${REBUILD}`);
  }
  if (!link) template = template.split('\n').filter((line) => !line.includes(SLOTS.link)).join('\n');
  const values = { [SLOTS.base]: base, [SLOTS.link]: escapeHtml(link ?? '') };
  return template.replace(SLOT, (slot) => values[slot]);
}

/** The JSON of figure `slug` under `figuresDir`, or CheckError when there is none. */
export function figureMeta(figuresDir, slug) {
  if (!SLUG.test(slug)) throw new CheckError(`figure slug ${quoted(slug)} is not lowercase kebab-case`);
  const path = join(figuresDir, `${slug}.json`);
  if (!isFile(path)) throw new CheckError(`figure ${quoted(slug)} has no ${toPosix(path)}; add docs/figures/${slug}.ts and run ${REBUILD}`);
  return readJson(path);
}

/** Runs `render`, turning a CheckError into a recorded finding and a null result. */
function attempt(render, errors) {
  try {
    return render();
  } catch (error) {
    if (!(error instanceof CheckError)) throw error;
    errors.push(error.message);
    return null;
  }
}

/**
 * `markdown` with every top-level ```figure fence replaced by its filled markup, and the findings.
 * `link` maps a slug to the URL of its interactive figure, or is null. A fence whose figure cannot
 * be rendered is left as it is and reported.
 */
export function expand(markdown, base, figuresDir, link = null) {
  const lines = markdown.split('\n');
  const errors = [];
  for (const block of fenceBlocks(markdown).filter((b) => b.info === 'figure').reverse()) {
    const slug = figureSlug(block.body);
    const rendered = attempt(() => fillSlots(figureMeta(figuresDir, slug), base, link ? link(slug) : null), errors);
    if (rendered !== null) lines.splice(block.start, block.end - block.start, ...rendered.split('\n').map((line) => block.indent + line));
  }
  return { text: lines.join('\n'), errors: errors.reverse() };
}

/** `text` with every `<!-- figure:SLUG -->` block re-rendered, and the findings for those that failed. */
export function refreshMarkers(text, base, figuresDir) {
  const errors = [];
  let count = 0;
  const refreshed = text.replace(MARKER, (whole, slug) => {
    count += 1;
    if (count > MAX_FIGURES) return whole;
    const block = attempt(() => fillSlots(figureMeta(figuresDir, slug), base), errors);
    return block === null ? whole : `<!-- figure:${slug} -->\n${block}\n<!-- /figure -->`;
  });
  return { text: refreshed, errors };
}

/** The repository-relative URL prefix of docs/assets/figures as seen from `document`. */
function relativeBase(root, document) {
  const rel = relative(realOrResolved(root), realOrResolved(document));
  if (rel === '..' || rel.startsWith(`..${sep}`) || isAbsolute(rel)) throw new CheckError(`${document} is outside the repository root ${root}`);
  const depth = rel ? rel.split(sep).length - 1 : 0;
  return [...Array.from({ length: depth }, () => '..'), ...OUT_DIR.split('/')].join('/');
}

/** Evidence anchors whose path is missing or whose symbol no longer occurs in it. */
function evidenceErrors(root, slug, evidence) {
  const where = `${OUT_DIR}/${slug}.json`;
  const anchors = Array.isArray(evidence) ? evidence : typeof evidence === 'string' ? [...evidence] : [];
  const base = realOrResolved(root);
  const errors = [];
  for (const anchor of anchors.slice(0, MAX_FIGURES)) {
    const text = String(anchor);
    const cut = text.indexOf(':');
    const [path, symbol] = cut < 0 ? [text, ''] : [text.slice(0, cut), text.slice(cut + 1)];
    const target = realOrResolved(resolve(root, path));
    if (!symbol || !inside(base, target) || !isFile(target)) {
      errors.push(`${where}: evidence ${quoted(anchor)} names no file in the repository`);
      continue;
    }
    const content = readText(target);
    if (!symbol.split('.').every((part) => content.includes(part))) errors.push(`${where}: evidence ${quoted(anchor)}: ${symbol} no longer occurs in ${path}`);
  }
  return errors;
}

/** Why a figure's JSON no longer matches its spec, the engine or its SVGs. */
function hashErrors(root, slug, meta, engine) {
  const where = `${OUT_DIR}/${slug}.json`;
  const bound = [['spec', `${SPEC_DIR}/${slug}.ts`, meta.spec_sha256], ['SVG', `${OUT_DIR}/${slug}.svg`, meta.svg_sha256],
    ['static SVG', `${OUT_DIR}/${slug}.static.svg`, meta.static_sha256]];
  const errors = [];
  for (const [what, path, recorded] of bound) {
    if (!isFile(join(root, path))) errors.push(`${where}: ${path} is missing; rebuild with: ${REBUILD}`);
    else if (sha256(readBytes(join(root, path))) !== recorded) errors.push(`${where} is stale: the ${what} ${path} changed; rebuild with: ${REBUILD}`);
  }
  if ((isObject(meta.engine) ? meta.engine.sha256 : undefined) !== engine) {
    errors.push(`${where} is stale: the figure engine changed; rebuild with: ${REBUILD}`);
  }
  return errors;
}

/** A finding for each image, animated and static, whose size the figure's JSON lacks or records as a fraction. */
export function sizeErrors(slug, meta) {
  return ['', 'static_'].filter((prefix) => ![meta[`${prefix}width`], meta[`${prefix}height`]].every((v) => Number.isInteger(v) && v > 0))
    .map((prefix) => `${OUT_DIR}/${slug}.json: figure ${quoted(meta.slug)}: its JSON records no positive whole-number ` +
      `${prefix}width and ${prefix}height; rebuild with: ${REBUILD}`);
}

/** A finding when the JSON's `html` is not the markup core.mjs renders from the rest of the JSON. */
export function htmlErrors(slug, meta) {
  let expected = null;
  try {
    expected = markup(meta);
  } catch (error) {
    if (!(error instanceof TypeError)) throw error;
  }
  if (meta.html === expected) return [];
  return [`${OUT_DIR}/${slug}.json is stale: its html is not the markup tools/figures/core.mjs renders from it; rebuild with: ${REBUILD}`];
}

/** The stems of the files in `dir` that end in `suffix`. */
function stems(dir, suffix) {
  if (!isDirectory(dir)) return new Set();
  return new Set(entries(dir).filter((entry) => hasSuffix(entry.name, suffix)).map((entry) => entry.name.slice(0, -suffix.length)));
}

/** Spec/JSON pairing, hash binding, sizes, markup and evidence for every figure, and the known slugs. */
function figureSourceErrors(root) {
  const specs = stems(join(root, SPEC_DIR), '.ts');
  const metas = stems(join(root, OUT_DIR), '.json');
  if (new Set([...specs, ...metas]).size > MAX_FIGURES) throw new CheckError(`more than ${MAX_FIGURES} figures`);
  const both = [...specs].filter((slug) => metas.has(slug)).sort();
  const errors = [...specs].filter((slug) => !metas.has(slug)).sort()
    .map((slug) => `${SPEC_DIR}/${slug}.ts has no ${OUT_DIR}/${slug}.json; rebuild with: ${REBUILD}`);
  errors.push(...[...metas].filter((slug) => !specs.has(slug)).sort()
    .map((slug) => `${OUT_DIR}/${slug}.json has no spec ${SPEC_DIR}/${slug}.ts; delete it or restore the spec`));
  const engine = both.length ? engineHash(root) : '';
  for (const slug of both) {
    const meta = readJson(join(root, OUT_DIR, `${slug}.json`));
    const sizes = sizeErrors(slug, meta);
    errors.push(...hashErrors(root, slug, meta, engine), ...evidenceErrors(root, slug, meta.evidence || []), ...sizes);
    if (sizes.length === 0) errors.push(...htmlErrors(slug, meta));
  }
  return { errors, known: new Set(both), specs };
}

/**
 * The finding for a ```figure fence on `page` naming `slug`, which `figures` does not know: a spec
 * not rendered yet, as on the first run after a spec or a preset is copied in, names the command
 * that renders it.
 */
function unknownSlugError(page, slug, figures) {
  const found = `${page}: \`\`\`figure fence names ${quoted(slug)}`;
  if (figures.specs.has(slug)) return `${found}, whose spec ${SPEC_DIR}/${slug}.ts has not been rendered; render it with: ${REBUILD}`;
  return `${found}, which has no spec and JSON`;
}

/** Disabled fence kinds and unknown figure slugs on every page the configuration builds. */
function pageSourceErrors(root, docs, config, figures) {
  const configPath = resolve(root, config);
  const flavor = siteFlavor(configPath);
  const settings = isFile(configPath) ? readText(configPath) : null;
  const kinds = settings === null ? new Set(KINDS) : flavor.kinds(settings);
  const errors = [];
  for (const page of flavor.pages(resolve(root, docs), settings ?? '')) {
    const text = readText(page);
    const shown = relative(root, page);
    errors.push(...kindErrors(shown, text, kinds));
    errors.push(...figureSlugs(text).filter((slug) => !figures.known.has(slug)).map((slug) => unknownSlugError(shown, slug, figures)));
  }
  return errors;
}

/**
 * Every source-side finding: figure bindings, fence slugs, fence kinds and the README block. The
 * pages are those of every site `sourceSites` names: the `config` and `docs` given, or each site
 * configuration at the repository root.
 */
export function sources(root, docs = undefined, config = undefined, readme = 'README.md') {
  const figures = figureSourceErrors(root);
  const { errors } = figures;
  for (const site of sourceSites(root, docs, config)) errors.push(...pageSourceErrors(root, site.docs, site.config, figures));
  const readmePath = resolve(root, readme);
  if (isFile(readmePath)) {
    const text = readText(readmePath);
    const refreshed = refreshMarkers(text, relativeBase(root, readmePath), join(root, OUT_DIR));
    errors.push(...refreshed.errors.map((message) => `${readme}: ${message}`));
    if (refreshed.text !== text) {
      errors.push(`${readme}: a portable figure block differs from the renderer; refresh it with: ${REFRESH} ${readme}`);
    }
  }
  return errors;
}

/** One page rendered for a JavaScript-free reader: repository-relative blocks without `base`, wiki fences with it. */
function portablePage(path, text, base, root) {
  if (base === null) return refreshMarkers(text, relativeBase(root, path), join(root, OUT_DIR));
  const site = base.replace(/\/+$/, '');
  const stem = basename(path, extname(path));
  const link = (slug) => `${site}/wiki/${stem}/#fig-${slug}`;
  return expand(text, `${site}/assets/figures`, join(root, OUT_DIR), link);
}

/** Rewrites `files` for JavaScript-free readers; writes nothing when any figure fails. */
export function portable(files, base, root) {
  const pages = files.map((path) => {
    const original = readText(path);
    return { path, original, ...portablePage(path, original, base, root) };
  });
  const errors = pages.flatMap((page) => page.errors.map((message) => `${page.path}: ${message}`));
  if (errors.length) return errors;
  for (const page of pages) if (page.text !== page.original) writeFileSync(page.path, page.text, 'utf8');
  return [];
}
