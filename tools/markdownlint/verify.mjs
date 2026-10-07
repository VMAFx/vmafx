// SPDX-FileCopyrightText: 2026 lusoris <lusoris@pm.me>
//
// SPDX-License-Identifier: EUPL-1.2

import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { spawnSync } from "node:child_process";
import { createRequire } from "node:module";
import { performance } from "node:perf_hooks";
import { fileURLToPath, pathToFileURL } from "node:url";

// Inventory bounds. documentation.max_files and documentation.max_file_bytes in .standards.yaml
// raise the file-count and per-file bounds from their defaults up to their ceilings; nothing
// raises the aggregate bound. internal/config/documentation.go validates the same ranges for
// audit, and TestDocumentationSettingsMirrorConfig keeps the two in step. The per-file ceiling
// is a memory bound: the markdownlint library lints a 4 MiB file of linked and code-spanned
// bullets in a 1.5 GiB heap (lintMemorySelfTest replays it with LINT_MEMORY_HEAP_MB), while a
// file of linked bullets at 8 MiB needed more than 1.5 GiB, close to Node's default heap on a
// 7 GB runner. markdownlint-cli2, which the gate ran before, needed the same: it lints through
// the same library.
const DEFAULT_MAX_FILES = 4_096;
const MAX_FILES_CEILING = 16_384;
const DEFAULT_MAX_FILE_BYTES = 1_048_576;
const MAX_FILE_BYTES_CEILING = 4_194_304;
const MAX_TOTAL_BYTES = 67_108_864;
// The time budget of one style-lint child. documentation.lint_timeout_seconds in .standards.yaml
// sets it from 1 s up to the ceiling; internal/config/documentation.go validates the same range
// for audit. A child past its budget fails the gate with a report naming the batch and its
// suspects (lintBudgetReport); nothing is re-run. The ceiling is the gate deadline below: no
// child runs past that deadline, so a larger budget could never run out first.
const DEFAULT_LINT_TIMEOUT_SECONDS = 120;
const LINT_TIMEOUT_SECONDS_CEILING = 480;
// The deadline of one whole gate or --only run, counted from its start. The hosted job (Workflow
// in tools/markdownlint/assets.go) stops after HOSTED_JOB_MINUTES; the gate ends within 8 of them
// and leaves 2 for checkout, Node setup and the figure checks. Every command of the run, the
// locked install, the private-link rule and each lint child included, gets the smaller of its own
// limit and the time left before the deadline (commandLimit). Lint children run one after another,
// so their budgets alone do not bound the run: without the deadline, a slow second batch after a
// slow first one would outlast the job, and the runner would cancel it without a report (#784).
// TestGateDeadlineFitsHostedJob keeps the deadline, the job limit and the budget ceiling in step.
// The self-test runs without a deadline.
const GATE_DEADLINE_SECONDS = 480;
const HOSTED_JOB_MINUTES = 10;
const DEADLINE_NOTE = `; the deadline keeps the gate inside the hosted job's ${HOSTED_JOB_MINUTES}-minute ` +
  "limit, and no setting moves it";
const MAX_BUDGET_SUSPECTS = 5;
const MANIFEST_FILE = ".standards.yaml";
const MAX_MANIFEST_BYTES = 1_048_576;
const MAX_MANIFEST_DEPTH = 64;
const MAX_MANIFEST_ALIASES = 1_024;
const MAX_STYLE_EXCLUSIONS = 64;
const MAX_STYLE_EXCLUSION_BYTES = 256;
const DOCUMENTATION_KEYS = new Set(["max_files", "max_file_bytes", "lint_timeout_seconds", "style_exclude"]);
// Brace lists in one path segment of a declared exclusion expand to at most this many
// alternatives; a list past it is refused rather than expanded (HISS-02).
const MAX_BRACE_ALTERNATIVES = 64;
const GLOBSTAR = "**";
const DEFAULT_SETTINGS = Object.freeze({
  declared: false,
  maxFiles: DEFAULT_MAX_FILES,
  maxFileBytes: DEFAULT_MAX_FILE_BYTES,
  lintTimeoutSeconds: DEFAULT_LINT_TIMEOUT_SECONDS,
  styleExclude: Object.freeze([]),
});
const MAX_GIT_OUTPUT_BYTES = 16_777_216;
const MAX_CAPTURE_BYTES = 1_048_576;
const MAX_DIAGNOSTIC_OUTPUT_BYTES = 65_536;
const MAX_DIAGNOSTIC_OUTPUT_LINES = 200;
const MAX_FAILURE_DETAIL_BYTES = 4_096;
const MAX_FAILURE_DETAIL_LINES = 20;
const MAX_PATH_BYTES = 4_096;
const MAX_TRACKED_ENTRIES = 65_536;
const MAX_TRACKED_SYMLINKS = 2_048;
const MAX_SYMLINK_TARGET_BYTES = 4_096;
const TRUNCATION_MARKER_BYTES = 256;
const MAX_COMMAND_BYTES = 24_000;
const DEFAULT_COMMAND_TIMEOUT_MS = 120_000;
const INSTALL_TIMEOUT_MS = 300_000;
const NPM_CI_ARGS = Object.freeze(["ci", "--ignore-scripts", "--no-audit", "--no-fund"]);
// The gate lints in a child process of its own script: `node verify.mjs --lint <install> <file>...`
// from the repository root; `node verify.mjs --only <file>...` lints the named style-selected
// files alone in one such child, the way to time a suspect. The child imports the markdownlint
// library's synchronous entry from the locked install and reads its rules from the config mapping
// of markdownlint-cli2.yaml; the file keeps the name and the noProgress key markdownlint-cli2 read,
// so adopted copies need no change, and every other key is refused.
const LINT_MODE = "--lint";
const ONLY_MODE = "--only";
const MARKDOWNLINT_ENTRY = "./lib/exports-sync.mjs";
const LINT_CONFIG_FILE = "markdownlint-cli2.yaml";
const LINT_CONFIG_KEYS = new Set(["config", "noProgress"]);
const MARKDOWN_SUFFIXES = [".md", ".markdown", ".mdx", ".md.tmpl", ".markdown.tmpl", ".mdx.tmpl"];
const SCRATCH_ROOTS = new Set([".workingdir", ".workingdir2"]);
const FILESYSTEM_SYMLINK_UNAVAILABLE = new Set(["EPERM", "EACCES", "ENOSYS"]);
const TOOL_FILES = [
  "package.json",
  "package-lock.json",
  LINT_CONFIG_FILE,
  "no-private-scratch-links.mjs",
];
// Style-only exclusions. These files are generated release/agent surfaces or
// carry a separate format contract (caveman, fixture bytes, or vendored source).
// The private-link rule never uses these exclusions: it receives every Markdown
// path returned by the bounded Git inventory below.
const STYLE_EXCLUDED_FILES = new Set([
  ".github/copilot-instructions.md",
  "AGENTS.md",
  "CHANGELOG.md",
  "CLAUDE.md",
]);
const STYLE_EXCLUDED_PREFIXES = [
  ".agents/agents/",
  ".agents/plugins/",
  ".agents/skills/",
  ".claude/",
  ".codex/",
  ".cursor/",
  ".gemini/",
  ".github/agents/",
  ".paperclip/",
  ".standards/worktrees/",
  ".workingdir/",
  ".workingdir2/",
  "internal/caveman/testdata/",
  "node_modules/",
  // Vendored upstream source is kept byte-identical to its pin
  // (tools/figures/third_party/interfig/vendor.json), so its Markdown follows upstream's style,
  // not this gate's. Praetor's own notes beside it stay selected: only the upstream/ subtree is
  // excluded.
  "tools/figures/third_party/interfig/upstream/",
  "vendor/",
];

class GateFailure extends Error {
  constructor(message, status = 2) {
    super(message);
    this.status = status;
  }
}

function fail(message, status = 2) {
  throw new GateFailure(message, status);
}

// gateDeadline is the performance.now() reading by which the running gate must end: Infinity
// outside withGateDeadline, so the self-test and its fixtures run without one.
let gateDeadline = Infinity;

// withGateDeadline runs one gate or --only run under a deadline the given milliseconds from now,
// and lifts the deadline when the run ends or fails.
function withGateDeadline(milliseconds, run) {
  gateDeadline = performance.now() + milliseconds;
  try {
    return run();
  } finally {
    gateDeadline = Infinity;
  }
}

// commandLimit is the time one command may take, in ms: its own limit, or the time left before
// the gate deadline when that is shorter (deadline: true). A command started at or past the
// deadline gets 1 ms and fails at once, naming the deadline.
function commandLimit(own) {
  const left = Math.floor(gateDeadline - performance.now());
  return left < own ? { ms: Math.max(left, 1), deadline: true } : { ms: own, deadline: false };
}

// timeoutMessage names the limit a stopped command ran into.
function timeoutMessage(commandName, limit) {
  return limit.deadline ? `${commandName} ran past the gate's ${GATE_DEADLINE_SECONDS} s deadline${DEADLINE_NOTE}` :
    `${commandName} exceeded ${limit.ms} ms`;
}

function boundedOutput(value, maxBytes, maxLines) {
  let cursor = 0;
  let bytes = 0;
  let lines = 0;
  let output = "";
  while (cursor < value.length && lines < maxLines && bytes < maxBytes) {
    const newline = value.indexOf("\n", cursor);
    const end = newline < 0 ? value.length : newline + 1;
    const segment = value.slice(cursor, end);
    const segmentBytes = Buffer.byteLength(segment);
    if (bytes + segmentBytes > maxBytes) {
      break;
    }
    output += segment;
    bytes += segmentBytes;
    lines += 1;
    cursor = end;
  }
  return { output, bytes, lines, truncated: cursor < value.length };
}

function outputBudget() {
  return {
    bytes: MAX_DIAGNOSTIC_OUTPUT_BYTES - TRUNCATION_MARKER_BYTES,
    lines: MAX_DIAGNOSTIC_OUTPUT_LINES - 1,
    truncated: false,
  };
}

function emitBounded(value, stream, budget, label) {
  if (!value || budget.truncated) {
    return budget.truncated;
  }
  const marker = `markdown-governance: ${label} diagnostics truncated at ` +
    `${MAX_DIAGNOSTIC_OUTPUT_LINES} lines/${MAX_DIAGNOSTIC_OUTPUT_BYTES} bytes\n`;
  assert.ok(Buffer.byteLength(marker) <= TRUNCATION_MARKER_BYTES);
  const selected = boundedOutput(value, budget.bytes, budget.lines);
  stream.write(selected.output);
  budget.bytes -= selected.bytes;
  budget.lines -= selected.lines;
  if (selected.truncated) {
    stream.write(marker);
    budget.truncated = true;
  }
  return selected.truncated;
}

// command runs one child process under the smaller of its own timeout and the time left before the
// gate deadline. A child stopped by either fails the gate with status 2: timeoutReport, when given,
// receives whether the deadline was the limit and writes the message.
function command(commandName, args, options = {}) {
  const limit = commandLimit(options.timeout ?? DEFAULT_COMMAND_TIMEOUT_MS);
  const result = spawnSync(commandName, args, {
    cwd: options.cwd,
    encoding: options.binary ? null : "utf8",
    env: { ...process.env, ...options.env },
    input: options.input,
    maxBuffer: options.maxBuffer ?? MAX_CAPTURE_BYTES,
    stdio: "pipe",
    timeout: limit.ms,
    windowsHide: true,
  });
  if (result.error) {
    if (result.error.code === "ETIMEDOUT") {
      fail(options.timeoutReport?.(limit.deadline) ?? timeoutMessage(commandName, limit));
    }
    fail(`${commandName} failed to start or exceeded ${options.maxBuffer ?? MAX_CAPTURE_BYTES} captured bytes: ${result.error.message}`);
  }
  if (result.signal) {
    fail(`${commandName} terminated by ${result.signal}`);
  }
  if (result.status !== 0 && !options.allowFailure) {
    const rawOutput = result.stderr?.length ? result.stderr : result.stdout;
    const rawDetail = (Buffer.isBuffer(rawOutput) ? rawOutput.toString("utf8") : rawOutput || "").trim();
    const detail = boundedOutput(rawDetail, MAX_FAILURE_DETAIL_BYTES, MAX_FAILURE_DETAIL_LINES).output.trim();
    fail(`${commandName} exited ${result.status}${detail ? `: ${detail}` : ""}`);
  }
  return result;
}

function repositoryRoot() {
  const result = command("git", ["rev-parse", "--show-toplevel"], { maxBuffer: MAX_GIT_OUTPUT_BYTES });
  const root = result.stdout.trim();
  if (root === "") {
    fail("git rev-parse returned an empty repository root");
  }
  return fs.realpathSync(root);
}

// The documentation block of .standards.yaml is the only repository input that changes what the
// gate enforces, and only within the bounds above. The manifest is read as audit reads it: one
// bounded regular file, one YAML document, unknown documentation keys refused.
function loadDependency(temporary, name) {
  return createRequire(path.join(temporary, "package.json"))(name);
}

function isMapping(value) {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    return false;
  }
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function readManifestText(root) {
  const full = path.join(root, MANIFEST_FILE);
  const stat = fs.lstatSync(full, { throwIfNoEntry: false });
  if (stat === undefined) {
    return null;
  }
  if (stat.isSymbolicLink() || !stat.isFile()) {
    fail(`refusing non-regular manifest ${MANIFEST_FILE}`);
  }
  if (stat.size > MAX_MANIFEST_BYTES) {
    fail(`${MANIFEST_FILE} is ${stat.size} bytes; maximum is ${MAX_MANIFEST_BYTES}`);
  }
  return fs.readFileSync(full, "utf8");
}

function documentationBlock(yaml, text) {
  let documents;
  try {
    documents = yaml.loadAll(text, {
      filename: MANIFEST_FILE,
      maxAliases: MAX_MANIFEST_ALIASES,
      maxDepth: MAX_MANIFEST_DEPTH,
    });
  } catch (error) {
    fail(`${MANIFEST_FILE} is not valid YAML: ${error.message}`);
  }
  if (documents.length > 1) {
    fail(`${MANIFEST_FILE} holds ${documents.length} YAML documents; expected one`);
  }
  const manifest = documents[0] ?? null;
  if (manifest === null) {
    return null;
  }
  if (!isMapping(manifest)) {
    fail(`${MANIFEST_FILE} must be a YAML mapping`);
  }
  return Object.hasOwn(manifest, "documentation") ? manifest.documentation : null;
}

// boundedSetting reads one integer setting: an absent key keeps defaultValue, and a value outside
// minimum..ceiling is refused. The inventory bounds take their default as the minimum, so they can
// be raised but not lowered.
function boundedSetting(value, key, defaultValue, ceiling, minimum = defaultValue) {
  if (value === undefined) {
    return defaultValue;
  }
  if (!Number.isSafeInteger(value) || value < minimum || value > ceiling) {
    const got = typeof value === "number" || value === null ? String(value) : `a ${typeof value}`;
    fail(`${MANIFEST_FILE} documentation.${key} must be an integer from ${minimum} to ${ceiling}; got ${got}`);
  }
  return value;
}

// styleExclusionProblem names why a declared glob is refused, or returns null. A glob must be a
// repository-relative path pattern: no absolute, drive-letter or negated form, no empty, `.` or
// `..` segment, no backslash, and at least one letter or digit, so a pattern of wildcards alone
// (`**`, `*/**`) cannot stand in for "every file".
function styleExclusionProblem(pattern) {
  if (typeof pattern !== "string" || pattern === "") {
    return "must be a non-empty string";
  }
  if (Buffer.byteLength(pattern) > MAX_STYLE_EXCLUSION_BYTES) {
    return `exceeds ${MAX_STYLE_EXCLUSION_BYTES} bytes`;
  }
  if (/[\0\r\n\\]/u.test(pattern)) {
    return "must not contain NUL, a line break or a backslash";
  }
  if (pattern.startsWith("/") || pattern.startsWith("!") || /^[A-Za-z]:/u.test(pattern)) {
    return "must be repository-relative, not absolute or negated";
  }
  if (pattern.split("/").some((segment) => segment === "" || segment === "." || segment === "..")) {
    return "must not contain an empty, . or .. segment; write dir/** to exclude a directory";
  }
  if (!/[\p{L}\p{N}]/u.test(pattern)) {
    return "must name a path: wildcards alone would match every file";
  }
  return null;
}

// Declared exclusions are matched here, not through micromatch: micromatch expands braces through
// the braces package, whose stack-exhaustion advisory has no fixed release (#736). The grammar is
// the part of micromatch's, with dot files included, that the gate supports, and it matches as
// micromatch 4.0.8 with { dot: true } does: `*` and `?` stay inside one path segment and match a
// leading dot; `[abc]`, `[a-z]` and `[^a]` match one character; `{a,b}` lists alternatives
// inside one segment; a `**` segment spans any number of segments, none included, except that a
// trailing `/**` after a segment ending in `*` needs at least one more segment, and consecutive
// `**` segments count as one; and a path spelled exactly as the glob matches it, as picomatch
// compares the two before its regular expression. Matching is case-sensitive on every platform,
// and on Windows a backslash in a path is a separator.
//
// The characters are an allow-list, not a deny-list: a glob may hold letters, combining marks and
// digits of any script, the space, and only the ASCII punctuation in GLOB_PUNCTUATION, which is
// the grammar's own `. _ - / * ? [ ] ^ { } ,` and the characters micromatch read as themselves,
// `! # $ % & ' + : ; < = > @ ~` and the backtick. Any other character is refused with its code
// point, so a character nobody compared with micromatch is never matched differently. Of the
// ASCII punctuation that leaves `"`, which micromatch reads as a quote around literal text,
// `( | )`, which build extglobs and groups, and the backslash, an escape there.
//
// Every other shape is refused when the settings are read, never matched differently: POSIX
// classes, nested lists, brace ranges, `**` inside a segment, an unmatched bracket or brace, a
// `[!a]` class, a class range spanning `/`, a `+` straight after `]`, `{` or `}` (a
// regular-expression repeat in micromatch), a brace alternative of `*` alone (micromatch lets it
// match nothing), `.*` inside a brace list, and a run of `+`, `$` or `^` in a glob of one segment.
// picomatch compiles a glob of one segment without brackets or braces through a shortcut that
// escapes only the first character of such a run: `c++.md` matched `c+.md`, and `a$$*` matched
// `a$`. styleExclusionGrammarSelfTest pins each shape against the answers micromatch gave.
const GLOB_PUNCTUATION = "._-/*?[]^{},!#$%&'+:;<=>@`~";
const GLOB_UNSUPPORTED_CHARACTER = new RegExp(
  `[^\\p{L}\\p{M}\\p{N} ${GLOB_PUNCTUATION.replace(/[\\\]\[^/-]/gu, "\\$&")}]`, "u");
const GROUP_PROBLEM = "extglobs and regex groups are not supported";
const CHARACTER_PROBLEMS = new Map([
  ["\"", "micromatch reads it as a quote around literal text"],
  ["(", GROUP_PROBLEM],
  [")", GROUP_PROBLEM],
  ["|", GROUP_PROBLEM],
]);
const ONE_SEGMENT_RUN = /\+\+|\$\$|\^\^/u;
const GLOB_STAR_TOKEN = Object.freeze({ type: "star" });
const GLOB_ANY_TOKEN = Object.freeze({ type: "any" });
const ANY_SEGMENT = Object.freeze({ globstar: false, wildcard: true, alternatives: [[GLOB_STAR_TOKEN]] });
const SLASH_CODE = 0x2f;
// picomatch's REGEX_SPECIAL_CHARS: a class body holding none of them also matches its own text.
const PICOMATCH_REGEX_CHARS = /[-*+?.^${}(|)[\]]/u;
// picomatch's REGEX_BACKSLASH: the backslashes its Windows path format turns into /.
const PICOMATCH_BACKSLASH = /\\(?![*+?^${}(|)[\]])/gu;
const REPEAT_PROBLEM = "puts + straight after ], { or }, where micromatch reads it as a regular-expression " +
  "repeat, not as the character +";

// classEnd returns the index of the ] closing the [ ] class opened at index, or the problem.
function classEnd(segment, index) {
  const end = segment.indexOf("]", index + 1);
  if (end < 0) {
    return "has a [ without a closing ]";
  }
  if (segment.slice(index + 1, end).includes("[")) {
    return "uses [ inside a [ ] class; POSIX classes such as [:alpha:] are not supported";
  }
  if (segment[end + 1] === "+") {
    return REPEAT_PROBLEM;
  }
  return end;
}

// braceParts splits one segment into literal text and brace lists, skipping [ ] classes so a
// brace or comma inside a class stays literal. It returns the parts or the problem refusing them.
function braceParts(segment) {
  const parts = [];
  let literal = "";
  let list = null;
  for (let index = 0; index < segment.length && index < MAX_STYLE_EXCLUSION_BYTES; index += 1) {
    const character = segment[index];
    if (character === "]") {
      return "has a ] without an opening [";
    }
    if (character === "{" || character === "}" || (character === "," && list !== null)) {
      const state = braceDelimiter(character, segment[index + 1], parts, { list, literal });
      if (typeof state === "string") {
        return state;
      }
      ({ list, literal } = state);
      continue;
    }
    if (character !== "[") {
      literal += character;
      continue;
    }
    const end = classEnd(segment, index);
    if (typeof end === "string") {
      return end;
    }
    literal += segment.slice(index, end + 1);
    index = end;
  }
  if (list !== null) {
    return "has a { without a closing }";
  }
  parts.push(literal);
  return parts;
}

// braceDelimiter applies one {, } or list comma, followed by next, to the open list and the
// literal text read so far: it opens a list, closes it into parts, or starts the next
// alternative. It returns the new state or the problem refusing the delimiter.
function braceDelimiter(character, next, parts, { list, literal }) {
  if (character === "{") {
    if (list !== null) {
      return "nests a brace list inside another; nested lists are not supported";
    }
    parts.push(literal);
    return next === "+" ? REPEAT_PROBLEM : { list: [], literal: "" };
  }
  if (list === null) {
    return "has a } without an opening {";
  }
  list.push(literal);
  if (character === ",") {
    return { list, literal: "" };
  }
  const problem = closedListProblem(list, next);
  if (problem !== null) {
    return problem;
  }
  parts.push(list);
  return { list: null, literal: "" };
}

// closedListProblem names why a brace list just closed, followed by next, is refused, or returns
// null. Inside a list picomatch reads every . outside a class as a dot token and makes a * right
// after it match differently: `a{x,.*}` does not match `a.`, while `a.*` does.
function closedListProblem(list, next) {
  if (list.some((alternative) => alternative.includes(".."))) {
    return "uses .. inside a brace list; brace ranges are not supported";
  }
  if (list.some((alternative) => alternative.replace(/\[[^\]]*\]/gu, "c").includes(".*"))) {
    return "puts .* inside a brace list, which micromatch matches differently from .* outside one; " +
      "list those alternatives as separate globs";
  }
  if (list.length < 2) {
    return "has a brace list without a comma; write at least two alternatives";
  }
  return next === "+" ? REPEAT_PROBLEM : null;
}

// expandBraces returns every alternative one segment's brace lists spell, or the problem.
function expandBraces(segment) {
  const parts = braceParts(segment);
  if (typeof parts === "string") {
    return parts;
  }
  let alternatives = [""];
  for (let index = 0; index < parts.length && index < MAX_STYLE_EXCLUSION_BYTES; index += 1) {
    const choices = typeof parts[index] === "string" ? [parts[index]] : parts[index];
    if (alternatives.length * choices.length > MAX_BRACE_ALTERNATIVES) {
      return `expands to more than ${MAX_BRACE_ALTERNATIVES} alternatives in one segment`;
    }
    alternatives = alternatives.flatMap((prefix) => choices.map((choice) => prefix + choice));
  }
  return [...new Set(alternatives)];
}

// classRanges reads the characters and a-z ranges of a [ ] class body from start as [low, high]
// pairs of UTF-16 code units, the units micromatch's regular expressions compare.
function classRanges(body, start) {
  const ranges = [];
  for (let index = start; index < body.length && index < MAX_STYLE_EXCLUSION_BYTES; index += 1) {
    const low = body.charCodeAt(index);
    const ranged = body[index + 1] === "-" && index + 2 < body.length;
    ranges.push([low, ranged ? body.charCodeAt(index + 2) : low]);
    index += ranged ? 2 : 0;
  }
  return ranges;
}

function rangeText([low, high]) {
  return `${String.fromCharCode(low)}-${String.fromCharCode(high)}`;
}

// classToken reads the body of a [ ] class: an optional leading ^, then characters and a-z ranges.
// As in micromatch, a body without a character from picomatch's regular-expression set
// (`[ab]`, not `[a-c]`, `[^a]` or `[{a]`) also matches its own bracketed text, and literal holds
// that text. micromatch reads a leading ! as the character !, not as negation, and lets a range
// spanning / match a path separator, since only a negated class has / added; both are refused.
function classToken(body) {
  if (body.startsWith("!")) {
    return "starts a [ ] class with !, which micromatch reads as the character !; write [^...] to negate";
  }
  const negated = body.startsWith("^");
  const start = negated ? 1 : 0;
  if (body.length === start) {
    return "has an empty [ ] class; a class may not begin with ]";
  }
  const ranges = classRanges(body, start);
  const reversed = ranges.find(([low, high]) => high < low);
  if (reversed !== undefined) {
    return `has the reversed range ${rangeText(reversed)} in a [ ] class`;
  }
  const spanning = negated ? undefined : ranges.find(([low, high]) => low <= SLASH_CODE && SLASH_CODE <= high);
  if (spanning !== undefined) {
    return `has the range ${rangeText(spanning)} in a [ ] class, which spans / and so lets micromatch match ` +
      "a path separator";
  }
  const literal = PICOMATCH_REGEX_CHARS.test(body) ? null : `[${body}]`;
  return { type: "class", negated, ranges, literal };
}

function literalTokens(text) {
  return Array.from({ length: text.length }, (_, index) => ({ type: "literal", code: text.charCodeAt(index) }));
}

// bracketVariants returns every token list one alternative spells when each class that also
// matches its bracketed text is read either way, or the problem when they exceed the bound.
function bracketVariants(tokens) {
  let variants = [[]];
  for (let index = 0; index < tokens.length && index < MAX_STYLE_EXCLUSION_BYTES; index += 1) {
    const token = tokens[index];
    const choices = token.type === "class" && token.literal !== null ? [[token], literalTokens(token.literal)] : [[token]];
    if (variants.length * choices.length > MAX_BRACE_ALTERNATIVES) {
      return `expands to more than ${MAX_BRACE_ALTERNATIVES} alternatives in one segment`;
    }
    variants = variants.flatMap((prefix) => choices.map((choice) => [...prefix, ...choice]));
  }
  return variants;
}

// segmentTokens reads one brace-free alternative into literal, `*`, `?` and class tokens.
function segmentTokens(text) {
  const tokens = [];
  for (let index = 0; index < text.length && index < MAX_STYLE_EXCLUSION_BYTES; index += 1) {
    const character = text[index];
    if (character === "*") {
      if (tokens.at(-1) !== GLOB_STAR_TOKEN) {
        tokens.push(GLOB_STAR_TOKEN);
      }
    } else if (character === "?") {
      tokens.push(GLOB_ANY_TOKEN);
    } else if (character === "[") {
      const end = text.indexOf("]", index + 1);
      const token = classToken(text.slice(index + 1, end));
      if (typeof token === "string") {
        return token;
      }
      tokens.push(token);
      index = end;
    } else {
      tokens.push({ type: "literal", code: text.charCodeAt(index) });
    }
  }
  return tokens;
}

// alternativeProblem names why one brace alternative of segment is refused, or returns null. A
// brace alternative of `*` alone may match nothing in micromatch: `docs/x/**/{*,draft}` matches
// `docs/x` there, as a whole-segment `*` never does.
function alternativeProblem(alternative, segment) {
  if (alternative === "" || alternative === "." || alternative === "..") {
    return "has a brace list that leaves an empty, . or .. segment";
  }
  if (alternative !== segment && /^\*+$/u.test(alternative)) {
    return "has a brace alternative of * alone, which micromatch lets match nothing; list that " +
      "alternative as a separate glob";
  }
  return null;
}

// compileSegment turns one path segment of a declared exclusion into a globstar or a list of
// token alternatives, or returns the problem refusing it.
function compileSegment(segment) {
  if (segment === GLOBSTAR) {
    return { globstar: true, wildcard: true, alternatives: [] };
  }
  if (segment.includes(GLOBSTAR)) {
    return "uses ** inside a path segment; ** must be a whole segment";
  }
  const expanded = expandBraces(segment);
  if (typeof expanded === "string") {
    return expanded;
  }
  const alternatives = [];
  for (let index = 0; index < expanded.length && index < MAX_BRACE_ALTERNATIVES; index += 1) {
    const problem = alternativeProblem(expanded[index], segment);
    if (problem !== null) {
      return problem;
    }
    const tokens = segmentTokens(expanded[index]);
    const variants = typeof tokens === "string" ? tokens : bracketVariants(tokens);
    if (typeof variants === "string") {
      return variants;
    }
    if (alternatives.length + variants.length > MAX_BRACE_ALTERNATIVES) {
      return `expands to more than ${MAX_BRACE_ALTERNATIVES} alternatives in one segment`;
    }
    alternatives.push(...variants);
  }
  const wildcard = expanded.some((alternative) => !/[\p{L}\p{N}]/u.test(alternative));
  return { globstar: false, wildcard, alternatives };
}

// characterProblem names the first character of pattern outside the allow-list, with its code
// point and the reason, or returns null.
function characterProblem(pattern) {
  const match = GLOB_UNSUPPORTED_CHARACTER.exec(pattern);
  if (match === null) {
    return null;
  }
  const codePoint = match[0].codePointAt(0).toString(16).toUpperCase().padStart(4, "0");
  const reason = CHARACTER_PROBLEMS.get(match[0]) ?? "a glob may hold letters, combining marks and digits " +
    `of any script, the space and only the ASCII punctuation ${GLOB_PUNCTUATION}`;
  return `contains ${JSON.stringify(match[0])} (U+${codePoint}); ${reason}`;
}

// parseStyleExclusion compiles one declared exclusion, already checked by styleExclusionProblem,
// into its segments, or returns { problem } naming the character or shape the gate does not
// support.
function parseStyleExclusion(pattern) {
  const unsupported = characterProblem(pattern);
  if (unsupported !== null) {
    return { problem: unsupported };
  }
  if (!pattern.includes("/") && ONE_SEGMENT_RUN.test(pattern)) {
    return { problem: "repeats +, $ or ^ in a glob of one segment, where micromatch escapes only the first " +
      "character of the run, so c++.md matched c+.md" };
  }
  const raw = pattern.split("/").filter((segment, index, all) =>
    !(segment === GLOBSTAR && index > 0 && all[index - 1] === GLOBSTAR));
  const segments = [];
  for (let index = 0; index < raw.length && index < MAX_STYLE_EXCLUSION_BYTES; index += 1) {
    const segment = compileSegment(raw[index]);
    if (typeof segment === "string") {
      return { problem: segment };
    }
    segments.push(segment);
  }
  if (segments.every((segment) => segment.wildcard)) {
    return { problem: "must name a path: a brace alternative of wildcards alone would match every file" };
  }
  if (raw.length > 1 && raw.at(-1) === GLOBSTAR && raw.at(-2).endsWith("*")) {
    segments.splice(segments.length - 1, 0, ANY_SEGMENT);
  }
  return { segments };
}

function tokenMatches(token, code) {
  if (token.type === "any") {
    return true;
  }
  if (token.type === "literal") {
    return token.code === code;
  }
  return token.ranges.some(([low, high]) => code >= low && code <= high) !== token.negated;
}

// tokensMatch matches one segment's tokens against one path segment: `*` takes any run, falling
// back to the last `*` on a mismatch, so the work is bounded by the product of both lengths.
function tokensMatch(tokens, text) {
  let token = 0;
  let position = 0;
  let star = -1;
  let starPosition = 0;
  const bound = (tokens.length + 1) * (text.length + 1);
  for (let step = 0; position < text.length && step < bound; step += 1) {
    if (token < tokens.length && tokens[token] === GLOB_STAR_TOKEN) {
      star = token;
      starPosition = position;
      token += 1;
    } else if (token < tokens.length && tokenMatches(tokens[token], text.charCodeAt(position))) {
      token += 1;
      position += 1;
    } else if (star >= 0) {
      token = star + 1;
      starPosition += 1;
      position = starPosition;
    } else {
      return false;
    }
  }
  while (token < tokens.length && tokens[token] === GLOB_STAR_TOKEN) {
    token += 1;
  }
  return position === text.length && token === tokens.length;
}

// segmentsMatch matches compiled segments against path segments; a globstar spans any number of
// path segments, none included. The table is bounded by both lengths (HISS-01: no recursion).
function segmentsMatch(segments, parts) {
  let reached = segments.map(() => false);
  reached.push(false);
  reached[0] = true;
  for (let index = 0; index < segments.length && segments[index].globstar; index += 1) {
    reached[index + 1] = true;
  }
  for (let part = 0; part < parts.length && part < MAX_PATH_BYTES; part += 1) {
    const next = [false];
    for (let index = 1; index <= segments.length; index += 1) {
      const segment = segments[index - 1];
      next.push(segment.globstar ? next[index - 1] || reached[index] :
        reached[index - 1] && segment.alternatives.some((tokens) => tokensMatch(tokens, parts[part])));
    }
    reached = next;
  }
  return reached[segments.length];
}

// styleExclusionMatcher returns a predicate over repository-relative paths for one declared
// exclusion; platform decides whether a backslash separates path segments, as it does on Windows.
// A path spelled exactly as the glob matches, as picomatch tests that before its expression; on
// Windows picomatch first turns each backslash into / except one before a glob character
// (PICOMATCH_BACKSLASH), so `docs\{a,c}.md` is not the spelling of `docs/{a,c}.md` there. Such a
// kept backslash stays an ordinary character for picomatch's classes, so `[^a]` could match it,
// while this matcher reads every backslash as a separator. The gate's paths come from git
// ls-files, which separates segments with / on every platform, and a Windows file name cannot
// hold a backslash, so no such path reaches the matcher.
function styleExclusionMatcher(pattern, platform) {
  const parsed = parseStyleExclusion(pattern);
  if (parsed.problem !== undefined) {
    fail(`${MANIFEST_FILE} documentation.style_exclude ${JSON.stringify(pattern)} ${parsed.problem}`);
  }
  const windows = platform === "win32";
  return (relative) => {
    const spelled = windows ? relative.replace(PICOMATCH_BACKSLASH, "/") : relative;
    const parts = (windows ? relative.replaceAll("\\", "/") : relative).split("/");
    return spelled === pattern || segmentsMatch(parsed.segments, parts);
  };
}

function styleExclusions(value) {
  if (value === undefined) {
    return [];
  }
  if (!Array.isArray(value)) {
    fail(`${MANIFEST_FILE} documentation.style_exclude must be a list of globs`);
  }
  if (value.length > MAX_STYLE_EXCLUSIONS) {
    fail(`${MANIFEST_FILE} documentation.style_exclude has ${value.length} globs; maximum is ${MAX_STYLE_EXCLUSIONS}`);
  }
  const seen = new Set();
  for (let index = 0; index < value.length && index < MAX_STYLE_EXCLUSIONS; index += 1) {
    const problem = styleExclusionProblem(value[index]) ?? parseStyleExclusion(value[index]).problem;
    if (problem !== null && problem !== undefined) {
      fail(`${MANIFEST_FILE} documentation.style_exclude[${index}] ${problem}`);
    }
    if (seen.has(value[index])) {
      fail(`${MANIFEST_FILE} documentation.style_exclude[${index}] repeats ${JSON.stringify(value[index])}`);
    }
    seen.add(value[index]);
  }
  return [...value];
}

function documentationSettings(block) {
  if (block === null || block === undefined) {
    return DEFAULT_SETTINGS;
  }
  if (!isMapping(block)) {
    fail(`${MANIFEST_FILE} documentation must be a mapping`);
  }
  const unknown = Object.keys(block).find((key) => !DOCUMENTATION_KEYS.has(key));
  if (unknown !== undefined) {
    fail(`${MANIFEST_FILE} documentation has unknown key ${JSON.stringify(unknown.slice(0, 64))}`);
  }
  return Object.freeze({
    declared: true,
    maxFiles: boundedSetting(block.max_files, "max_files", DEFAULT_MAX_FILES, MAX_FILES_CEILING),
    maxFileBytes: boundedSetting(block.max_file_bytes, "max_file_bytes", DEFAULT_MAX_FILE_BYTES,
      MAX_FILE_BYTES_CEILING),
    lintTimeoutSeconds: boundedSetting(block.lint_timeout_seconds, "lint_timeout_seconds",
      DEFAULT_LINT_TIMEOUT_SECONDS, LINT_TIMEOUT_SECONDS_CEILING, 1),
    styleExclude: Object.freeze(styleExclusions(block.style_exclude)),
  });
}

function repositorySettings(root, yaml) {
  const text = readManifestText(root);
  return text === null ? DEFAULT_SETTINGS : documentationSettings(documentationBlock(yaml, text));
}

// boundHint names the setting that raises a bound still below its ceiling.
function boundHint(key, value, ceiling) {
  return value < ceiling ? `; documentation.${key} in ${MANIFEST_FILE} raises it up to ${ceiling}` : "";
}

function checkFileCount(count, settings) {
  if (count > settings.maxFiles) {
    fail(`Markdown inventory has ${count} files; maximum is ${settings.maxFiles}` +
      boundHint("max_files", settings.maxFiles, MAX_FILES_CEILING));
  }
}

function checkFileSize(relative, size, settings) {
  if (size > settings.maxFileBytes) {
    fail(`${relative} is ${size} bytes; per-file maximum is ${settings.maxFileBytes}` +
      boundHint("max_file_bytes", settings.maxFileBytes, MAX_FILE_BYTES_CEILING));
  }
}

function escapesDirectory(base, target) {
  const confined = path.relative(base, target);
  return confined === ".." || confined.startsWith(`..${path.sep}`) || path.isAbsolute(confined);
}

function isStyleSelected(relative) {
  const normalized = relative.replaceAll("\\", "/");
  const lower = normalized.toLowerCase();
  if (!MARKDOWN_SUFFIXES.some((suffix) => lower.endsWith(suffix))) {
    return false;
  }
  if (STYLE_EXCLUDED_FILES.has(normalized)) {
    return false;
  }
  if (lower.split("/").includes("node_modules")) {
    return false;
  }
  return !STYLE_EXCLUDED_PREFIXES.some((prefix) => normalized.startsWith(prefix));
}

// styleSelection applies the built-in style exclusions, then the repository's declared ones, and
// counts the files each declared glob removes. Declared exclusions narrow the style run only: the
// private-link rule still reads every inventory file. They may never empty it: a declaration that
// removes every file the built-in selection styles fails the gate.
function styleSelection(files, settings, platform) {
  const matchers = settings.styleExclude.map((pattern) => styleExclusionMatcher(pattern, platform));
  const counts = matchers.map(() => 0);
  const styled = [];
  let eligible = 0;
  for (let index = 0; index < files.length && index < MAX_FILES_CEILING; index += 1) {
    if (!isStyleSelected(files[index])) {
      continue;
    }
    eligible += 1;
    let excluded = false;
    for (let pattern = 0; pattern < matchers.length && pattern < MAX_STYLE_EXCLUSIONS; pattern += 1) {
      if (matchers[pattern](files[index])) {
        counts[pattern] += 1;
        excluded = true;
      }
    }
    if (!excluded) {
      styled.push(files[index]);
    }
  }
  if (eligible > 0 && styled.length === 0) {
    fail(`${MANIFEST_FILE} documentation.style_exclude excludes all ${eligible} style-selected Markdown ` +
      "files; the style rules must check at least one");
  }
  return { styled, excluded: eligible - styled.length, counts };
}

function reportSettings(settings, selection) {
  if (!settings.declared) {
    return;
  }
  process.stdout.write(`markdown-governance: ${MANIFEST_FILE} documentation bounds ${settings.maxFiles} files, ` +
    `${settings.maxFileBytes} bytes per file, ${settings.lintTimeoutSeconds} s per lint child (defaults ` +
    `${DEFAULT_MAX_FILES}, ${DEFAULT_MAX_FILE_BYTES}, ${DEFAULT_LINT_TIMEOUT_SECONDS}; ceilings ` +
    `${MAX_FILES_CEILING}, ${MAX_FILE_BYTES_CEILING}, ${LINT_TIMEOUT_SECONDS_CEILING})\n`);
  for (let index = 0; index < settings.styleExclude.length && index < MAX_STYLE_EXCLUSIONS; index += 1) {
    process.stdout.write(`markdown-governance: style exclusion ${JSON.stringify(settings.styleExclude[index])} ` +
      `matched ${selection.counts[index]} files\n`);
  }
}

function summaryLine(settings, selection, scratchCount) {
  const excluded = settings.styleExclude.length > 0 ?
    ` (${selection.excluded} excluded by documentation.style_exclude)` : "";
  return `markdown-governance: styled ${selection.styled.length} public Markdown files${excluded}; ` +
    `checked ${scratchCount} tracked/non-ignored Markdown files for private links\n`;
}

function parseTrackedSymlinkRecords(records) {
  if (records.length > MAX_TRACKED_ENTRIES) {
    fail(`tracked inventory has ${records.length} entries; maximum is ${MAX_TRACKED_ENTRIES}`);
  }
  const symlinks = [];
  for (let index = 0; index < records.length && index < MAX_TRACKED_ENTRIES; index += 1) {
    const separator = records[index].indexOf("\t");
    if (separator < 0) {
      fail(`malformed tracked inventory entry at index ${index}`);
    }
    const metadata = /^(\d{6}) ([0-9a-f]{40}|[0-9a-f]{64}) ([0-3])$/u.exec(
      records[index].slice(0, separator),
    );
    const relative = records[index].slice(separator + 1);
    if (metadata === null || relative === "" || Buffer.byteLength(relative) > MAX_PATH_BYTES ||
      /[\0\t\r\n]/u.test(relative)) {
      fail(`unsafe or malformed tracked inventory entry at index ${index}`);
    }
    if (metadata[1] === "120000") {
      symlinks.push({ oid: metadata[2], relative });
      if (symlinks.length > MAX_TRACKED_SYMLINKS) {
        fail(`tracked symlink inventory exceeds ${MAX_TRACKED_SYMLINKS} entries`);
      }
    }
  }
  return symlinks;
}

function trackedSymlinkRecords(root) {
  const result = command("git", ["ls-files", "-z", "--stage"], {
    cwd: root,
    maxBuffer: MAX_GIT_OUTPUT_BYTES,
  });
  return parseTrackedSymlinkRecords(result.stdout.split("\0").filter(Boolean));
}

function readSymlinkTargets(root, records) {
  const objectIDs = [...new Set(records.map((record) => record.oid))];
  if (objectIDs.length === 0) {
    return new Map();
  }
  const result = command("git", ["cat-file", "--batch"], {
    binary: true,
    cwd: root,
    input: `${objectIDs.join("\n")}\n`,
    maxBuffer: MAX_GIT_OUTPUT_BYTES,
  });
  const targets = new Map();
  let cursor = 0;
  for (let index = 0; index < objectIDs.length && index < MAX_TRACKED_SYMLINKS; index += 1) {
    const headerEnd = result.stdout.indexOf(0x0a, cursor);
    if (headerEnd < 0) {
      fail(`git cat-file omitted symlink header at index ${index}`);
    }
    const header = result.stdout.subarray(cursor, headerEnd).toString("ascii");
    const parsed = /^([0-9a-f]{40}|[0-9a-f]{64}) blob ([0-9]+)$/u.exec(header);
    if (parsed === null || parsed[1] !== objectIDs[index]) {
      fail(`git cat-file returned malformed symlink header at index ${index}`);
    }
    const size = Number.parseInt(parsed[2], 10);
    if (!Number.isSafeInteger(size) || size > MAX_SYMLINK_TARGET_BYTES) {
      fail(`tracked symlink target exceeds ${MAX_SYMLINK_TARGET_BYTES} bytes at index ${index}`);
    }
    const contentStart = headerEnd + 1;
    const contentEnd = contentStart + size;
    if (contentEnd >= result.stdout.length || result.stdout[contentEnd] !== 0x0a) {
      fail(`git cat-file truncated symlink target at index ${index}`);
    }
    const bytes = result.stdout.subarray(contentStart, contentEnd);
    const target = bytes.toString("utf8");
    if (!Buffer.from(target, "utf8").equals(bytes)) {
      fail(`tracked symlink target is not UTF-8 at index ${index}`);
    }
    targets.set(objectIDs[index], target);
    cursor = contentEnd + 1;
  }
  if (cursor !== result.stdout.length) {
    fail("git cat-file returned trailing symlink data");
  }
  return targets;
}

function privateScratchRoot(target) {
  if (Buffer.byteLength(target) > MAX_SYMLINK_TARGET_BYTES || /[\0\r\n]/u.test(target)) {
    fail("tracked symlink has an unsafe or oversized target");
  }
  const normalized = path.posix.normalize(target.replaceAll("\\", "/"));
  const parts = normalized.split("/").filter(Boolean);
  return parts.map((part) => part.toLowerCase()).find((part) => SCRATCH_ROOTS.has(part)) ?? null;
}

function auditTrackedSymlinkTargets(root) {
  const records = trackedSymlinkRecords(root);
  const targets = readSymlinkTargets(root, records);
  for (let index = 0; index < records.length && index < MAX_TRACKED_SYMLINKS; index += 1) {
    const target = targets.get(records[index].oid);
    if (target === undefined) {
      fail(`tracked symlink ${records[index].relative} lacks an indexed target`);
    }
    const scratchRoot = privateScratchRoot(target);
    if (scratchRoot !== null) {
      fail(`tracked symlink ${records[index].relative} targets private scratch root ${scratchRoot}`);
    }
  }
}

function inventory(root, settings = DEFAULT_SETTINGS) {
  auditTrackedSymlinkTargets(root);
  const result = command("git", [
    "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--",
    ...MARKDOWN_SUFFIXES.map((suffix) => `:(icase,glob)**/*${suffix}`),
  ], { cwd: root, maxBuffer: MAX_GIT_OUTPUT_BYTES });
  const selected = result.stdout.split("\0").filter(Boolean).sort();
  checkFileCount(selected.length, settings);
  let totalBytes = 0;
  for (let index = 0; index < selected.length && index < settings.maxFiles; index += 1) {
    const relative = selected[index];
    // U+2028 and U+2029 are refused with the line breaks: micromatch's patterns never match
    // them, so a path holding one would be selected differently from the micromatch rules.
    if (Buffer.byteLength(relative) > MAX_PATH_BYTES || /[\0\r\n\u2028\u2029]/u.test(relative)) {
      fail(`refusing unsafe or oversized Markdown path at inventory index ${index}`);
    }
    const full = path.join(root, relative);
    const stat = fs.lstatSync(full);
    if (stat.isSymbolicLink()) {
      fail(`refusing symbolic-link Markdown source ${relative}`);
    }
    if (!stat.isFile()) {
      fail(`refusing non-file Markdown source ${relative}`);
    }
    if (escapesDirectory(root, fs.realpathSync(full))) {
      fail(`Markdown source escapes repository root: ${relative}`);
    }
    checkFileSize(relative, stat.size, settings);
    totalBytes += stat.size;
    if (totalBytes > MAX_TOTAL_BYTES) {
      fail(`Markdown inventory is ${totalBytes} bytes; maximum is ${MAX_TOTAL_BYTES}`);
    }
  }
  return selected;
}

// Windows cannot start the npm batch shim without a shell: since the CVE-2024-27980 fix,
// spawnSync rejects a .cmd or .bat file with EINVAL unless `shell` is set, and enabling `shell`
// with an argument list is deprecated (DEP0190). The Windows Node distribution ships npm
// beside node.exe, so the gate runs that npm-cli.js through the running Node binary and
// never involves cmd.exe. Other platforms resolve `npm` on PATH.
function npmInvocation(platform, execPath, args) {
  if (platform !== "win32") {
    return { file: "npm", args: [...args] };
  }
  const cli = path.join(path.dirname(execPath), "node_modules", "npm", "bin", "npm-cli.js");
  if (fs.statSync(cli, { throwIfNoEntry: false })?.isFile() !== true) {
    fail(`npm CLI not found beside ${execPath} at ${cli}; on Windows the gate runs npm ` +
      "through the Node binary because the npm batch shim cannot start without a shell");
  }
  return { file: execPath, args: [cli, ...args] };
}

function install(toolDir, temporary) {
  for (let index = 0; index < TOOL_FILES.length && index < TOOL_FILES.length; index += 1) {
    fs.copyFileSync(path.join(toolDir, TOOL_FILES[index]), path.join(temporary, TOOL_FILES[index]));
  }
  const npm = npmInvocation(process.platform, process.execPath, NPM_CI_ARGS);
  command(npm.file, npm.args, {
    cwd: temporary,
    timeout: INSTALL_TIMEOUT_MS,
  });
}

function npmInvocationSelfTest(temporary) {
  const nodeDir = path.join(temporary, "node distribution");
  const execPath = path.join(nodeDir, "node.exe");
  const cli = path.join(nodeDir, "node_modules", "npm", "bin", "npm-cli.js");
  for (const platform of ["linux", "darwin"]) {
    assert.deepEqual(npmInvocation(platform, execPath, NPM_CI_ARGS), { file: "npm", args: NPM_CI_ARGS });
  }
  assert.throws(() => npmInvocation("win32", execPath, NPM_CI_ARGS), /npm CLI not found beside/u);
  fs.mkdirSync(cli, { recursive: true });
  assert.throws(() => npmInvocation("win32", execPath, NPM_CI_ARGS), /npm CLI not found beside/u);
  fs.rmdirSync(cli);
  fs.writeFileSync(cli, "");
  const windows = npmInvocation("win32", execPath, NPM_CI_ARGS);
  assert.deepEqual(windows, { file: execPath, args: [cli, ...NPM_CI_ARGS] });
  assert.ok(!/\.(?:cmd|bat)$/iu.test(windows.file));
  fs.rmSync(nodeDir, { recursive: true, force: true });
  process.stdout.write("npm invocation fixtures: PATH npm off Windows, Node-run npm-cli.js on Windows, " +
    "missing CLI fails closed\n");
}

function filesystemSymlinkUnavailable(error) {
  return FILESYSTEM_SYMLINK_UNAVAILABLE.has(error?.code);
}

function filesystemSymlinkSkipDiagnostic(platform, code) {
  return `Markdown filesystem symlink fixture skipped on ${platform}: ${code}; ` +
    "alternate coverage: Git-index 120000 symlink target fixture\n";
}

function inventorySelfTest(temporary) {
  const fixture = writeInventoryFixture(temporary);
  const allFiles = inventory(fs.realpathSync(fixture));
  assert.deepEqual(allFiles, ["AGENTS.md", "README.md", "docs/guide.md", "docs/page.mdx",
    "notes.markdown", "templates/Card.mdx.tmpl", "templates/README.md.tmpl",
    "tools/figures/third_party/interfig/VENDOR.md", "tools/figures/third_party/interfig/upstream/README.md",
    "vendor/README.md"]);
  symlinkInventorySelfTest(fixture);
  scratchRuleSelfTest(fixture, temporary, allFiles);
  styleSelectionSelfTest(fixture, temporary, allFiles);
}

// writeInventoryFixture builds the Git repository the inventory self-test reads: public,
// generated, template, vendored and private Markdown, some tracked and some not.
function writeInventoryFixture(temporary) {
  const fixture = path.join(temporary, "inventory-fixture");
  fs.mkdirSync(path.join(fixture, "docs"), { recursive: true });
  fs.mkdirSync(path.join(fixture, "templates"), { recursive: true });
  fs.mkdirSync(path.join(fixture, "vendor"), { recursive: true });
  const interfig = path.join(fixture, "tools", "figures", "third_party", "interfig");
  fs.mkdirSync(path.join(interfig, "upstream"), { recursive: true });
  fs.mkdirSync(path.join(fixture, ".workingdir"), { recursive: true });
  fs.writeFileSync(path.join(fixture, ".gitignore"), "/.workingdir/\n**/node_modules/\n");
  fs.writeFileSync(path.join(fixture, "AGENTS.md"),
    "#Malformed generated surface\n\n[private](.workingdir/OPEN.md)\n");
  fs.writeFileSync(path.join(fixture, "README.md"), "# Root\n");
  fs.writeFileSync(path.join(fixture, "docs", "guide.md"), "# Guide\n");
  fs.writeFileSync(path.join(fixture, "notes.markdown"), "# Notes\n");
  fs.writeFileSync(path.join(fixture, "docs", "page.mdx"),
    "# MDX page\n\n[public](../README.md)\n\n<Card href=\"../README.md\">Public</Card>\n");
  fs.writeFileSync(path.join(fixture, "templates", "README.md.tmpl"), "# Template\n");
  fs.writeFileSync(path.join(fixture, "templates", "Card.mdx.tmpl"), "# MDX template\n");
  fs.writeFileSync(path.join(fixture, "vendor", "README.md"), "#Malformed vendor surface\n");
  fs.writeFileSync(path.join(interfig, "upstream", "README.md"), "#Malformed vendored upstream surface\n");
  fs.writeFileSync(path.join(interfig, "VENDOR.md"), "# Vendor notes\n");
  fs.writeFileSync(path.join(fixture, ".workingdir", "private.md"), "# Private\n");
  command("git", ["init", "--quiet"], { cwd: fixture });
  command("git", ["add", "--", ".gitignore", "AGENTS.md", "README.md", "docs/guide.md",
    "tools/figures/third_party/interfig/VENDOR.md", "tools/figures/third_party/interfig/upstream/README.md",
    "vendor/README.md"], { cwd: fixture });
  return fixture;
}

// symlinkInventorySelfTest refuses symbolic-link Markdown and tracked symlinks into a private
// scratch root, and pins the tracked-inventory bounds.
function symlinkInventorySelfTest(fixture) {
  for (const code of FILESYSTEM_SYMLINK_UNAVAILABLE) {
    assert.equal(filesystemSymlinkUnavailable({ code }), true);
  }
  assert.equal(filesystemSymlinkUnavailable({ code: "EIO" }), false);
  assert.equal(filesystemSymlinkUnavailable(null), false);
  assert.equal(filesystemSymlinkSkipDiagnostic("win32", "EPERM"),
    "Markdown filesystem symlink fixture skipped on win32: EPERM; " +
    "alternate coverage: Git-index 120000 symlink target fixture\n");
  const linkedMarkdown = path.join(fixture, "docs", "linked.md");
  try {
    fs.symlinkSync("../README.md", linkedMarkdown);
    assert.throws(() => inventory(fs.realpathSync(fixture)), /refusing symbolic-link Markdown source/u);
    fs.unlinkSync(linkedMarkdown);
  } catch (error) {
    if (!filesystemSymlinkUnavailable(error)) {
      throw error;
    }
    process.stdout.write(filesystemSymlinkSkipDiagnostic(process.platform, error.code));
  }
  const aliasPath = path.join(fixture, "docs", "public.txt");
  const aliasMarkdown = path.join(fixture, "docs", "alias.md");
  fs.writeFileSync(aliasPath, "../.workingdir/private.txt");
  fs.writeFileSync(aliasMarkdown, "# Alias\n\n--8<-- \"docs/public.txt\"\n");
  const aliasOID = command("git", ["hash-object", "-w", "docs/public.txt"], { cwd: fixture }).stdout.trim();
  command("git", ["update-index", "--add", "--cacheinfo", `120000,${aliasOID},docs/public.txt`], {
    cwd: fixture,
  });
  command("git", ["add", "--", "docs/alias.md"], { cwd: fixture });
  assert.throws(() => inventory(fs.realpathSync(fixture)),
    /tracked symlink docs\/public\.txt targets private scratch root \.workingdir/u);
  command("git", ["update-index", "--force-remove", "--", "docs/public.txt", "docs/alias.md"], { cwd: fixture });
  fs.unlinkSync(aliasPath);
  fs.unlinkSync(aliasMarkdown);
  const regularRecord = `100644 ${"0".repeat(40)} 0\tREADME.md`;
  assert.doesNotThrow(() => parseTrackedSymlinkRecords(new Array(MAX_TRACKED_ENTRIES).fill(regularRecord)));
  assert.throws(() => parseTrackedSymlinkRecords(new Array(MAX_TRACKED_ENTRIES + 1).fill(regularRecord)),
    /tracked inventory has 65537 entries/u);
  const symlinkRecord = `120000 ${"0".repeat(40)} 0\tdocs/public.txt`;
  assert.equal(parseTrackedSymlinkRecords(new Array(MAX_TRACKED_SYMLINKS).fill(symlinkRecord)).length,
    MAX_TRACKED_SYMLINKS);
  assert.throws(() => parseTrackedSymlinkRecords(new Array(MAX_TRACKED_SYMLINKS + 1).fill(symlinkRecord)),
    /tracked symlink inventory exceeds 2048 entries/u);
  assert.equal(privateScratchRoot("x".repeat(MAX_SYMLINK_TARGET_BYTES)), null);
  assert.throws(() => privateScratchRoot("x".repeat(MAX_SYMLINK_TARGET_BYTES + 1)),
    /unsafe or oversized target/u);
}

// scratchRuleSelfTest runs the scratch-link rule over the fixture, also through a symlinked
// ancestor, for a private link in Markdown, in MDX and in a JSX attribute.
function scratchRuleSelfTest(fixture, temporary, allFiles) {
  assert.equal(runScratchRule(fixture, temporary, allFiles, false, false), 1);
  // The same rule reached through a symlinked ancestor, as every macOS temp directory is
  // (/var -> /private/var). A junction needs no privilege on Windows; elsewhere the type is ignored.
  const symlinkedTemporary = path.join(temporary, "symlinked-ancestor");
  try {
    fs.symlinkSync(temporary, symlinkedTemporary, "junction");
    try {
      assert.equal(runScratchRule(fixture, symlinkedTemporary, allFiles, false, false), 1);
    } finally {
      fs.unlinkSync(symlinkedTemporary);
    }
  } catch (error) {
    if (!filesystemSymlinkUnavailable(error)) {
      throw error;
    }
    process.stdout.write(filesystemSymlinkSkipDiagnostic(process.platform, error.code));
  }
  fs.writeFileSync(path.join(fixture, "AGENTS.md"), "#Malformed generated surface\n");
  assert.equal(runScratchRule(fixture, temporary, allFiles, false, false), 0);
  fs.writeFileSync(path.join(fixture, "docs", "page.mdx"),
    "# MDX page\n\n[private](../.workingdir/OPEN.md)\n");
  assert.equal(runScratchRule(fixture, temporary, allFiles, false, false), 1);
  fs.writeFileSync(path.join(fixture, "docs", "page.mdx"),
    "# MDX page\n\n<Card href=\"../.workingdir/OPEN.md\">Private</Card>\n");
  assert.equal(runScratchRule(fixture, temporary, allFiles, false, false), 1);
  fs.writeFileSync(path.join(fixture, "docs", "page.mdx"),
    "# MDX page\n\n[public](../README.md)\n\n<Card href=\"../README.md\">Public</Card>\n");
}

// styleSelectionSelfTest keeps generated and vendored upstream Markdown out of the style run,
// fails malformed public Markdown, and pins the diagnostic output bounds.
function styleSelectionSelfTest(fixture, temporary, allFiles) {
  const styleFiles = allFiles.filter(isStyleSelected);
  // The vendored upstream README is malformed on purpose and must stay out of the style run; the
  // praetor-owned VENDOR.md beside it is the boundary and stays in.
  assert.deepEqual(styleFiles, ["README.md", "docs/guide.md", "docs/page.mdx", "notes.markdown",
    "templates/Card.mdx.tmpl", "templates/README.md.tmpl", "tools/figures/third_party/interfig/VENDOR.md"]);
  assert.equal(isStyleSelected("tools/figures/third_party/interfig/upstream/src/README.md"), false);
  assert.equal(isStyleSelected("tools/figures/third_party/interfig/upstreamish/README.md"), true);
  assert.equal(runMarkdownlint(fixture, temporary, styleFiles, false), 0);
  fs.writeFileSync(path.join(fixture, "docs", "malformed.md"), "#Malformed public Markdown\n");
  const malformedFiles = inventory(fs.realpathSync(fixture)).filter(isStyleSelected);
  assert.equal(runMarkdownlint(fixture, temporary, malformedFiles, false), 1);
  const exactLines = "x\n".repeat(MAX_DIAGNOSTIC_OUTPUT_LINES);
  assert.equal(boundedOutput(exactLines, MAX_DIAGNOSTIC_OUTPUT_BYTES, MAX_DIAGNOSTIC_OUTPUT_LINES).truncated, false);
  assert.equal(boundedOutput(`${exactLines}x\n`, MAX_DIAGNOSTIC_OUTPUT_BYTES,
    MAX_DIAGNOSTIC_OUTPUT_LINES).truncated, true);
  const exactBytes = "x".repeat(MAX_DIAGNOSTIC_OUTPUT_BYTES);
  assert.equal(boundedOutput(exactBytes, MAX_DIAGNOSTIC_OUTPUT_BYTES,
    MAX_DIAGNOSTIC_OUTPUT_LINES).truncated, false);
  assert.equal(boundedOutput(`${exactBytes}x`, MAX_DIAGNOSTIC_OUTPUT_BYTES,
    MAX_DIAGNOSTIC_OUTPUT_LINES).truncated, true);
  process.stdout.write("Markdown style fixtures: valid, malformed, generated/vendor exclusions, inventory bounds pass\n");
}

function settingsFrom(yaml, text) {
  return documentationSettings(documentationBlock(yaml, text));
}

function globList(globs) {
  return `documentation:\n  style_exclude:\n${globs.map((glob) => `    - ${JSON.stringify(glob)}\n`).join("")}`;
}

// Positive: no manifest, an empty, comment-only or document-start-only one, and one without a
// documentation block keep the defaults, and declared values are taken as written. Boundary: both
// bounds at their default and at their ceiling, 64 globs and a 256-byte glob pass. Negative: one
// past either end of a range, a non-integer, an unknown key, a non-mapping, a second document, and
// every glob shape that is absolute, negated, escaping, empty or wildcards alone fails.
function settingsSelfTest(temporary) {
  const yaml = loadDependency(temporary, "js-yaml");
  for (const text of ["", "# comment only\n", "---\n", "version: 1\n", "version: 1\ndocumentation:\n"]) {
    assert.equal(settingsFrom(yaml, text), DEFAULT_SETTINGS, JSON.stringify(text));
  }
  assert.deepEqual(settingsFrom(yaml, "documentation:\n  max_files: 8192\n  style_exclude:\n    - changelog.d/**\n"),
    { declared: true, maxFiles: 8_192, maxFileBytes: DEFAULT_MAX_FILE_BYTES,
      lintTimeoutSeconds: DEFAULT_LINT_TIMEOUT_SECONDS, styleExclude: ["changelog.d/**"] });
  for (const [files, bytes] of [[DEFAULT_MAX_FILES, DEFAULT_MAX_FILE_BYTES], [MAX_FILES_CEILING, MAX_FILE_BYTES_CEILING]]) {
    const settings = settingsFrom(yaml, `documentation:\n  max_files: ${files}\n  max_file_bytes: ${bytes}\n`);
    assert.deepEqual([settings.maxFiles, settings.maxFileBytes], [files, bytes]);
  }
  const globs = (count) => Array.from({ length: count }, (_, index) => `docs/generated-${index}/**`);
  assert.equal(settingsFrom(yaml, globList(globs(MAX_STYLE_EXCLUSIONS))).styleExclude.length, MAX_STYLE_EXCLUSIONS);
  assert.throws(() => settingsFrom(yaml, globList(globs(MAX_STYLE_EXCLUSIONS + 1))), /has 65 globs; maximum is 64/u);
  const longest = `docs/${"a".repeat(MAX_STYLE_EXCLUSION_BYTES - 8)}/**`;
  assert.equal(Buffer.byteLength(longest), MAX_STYLE_EXCLUSION_BYTES);
  assert.deepEqual(settingsFrom(yaml, globList([longest])).styleExclude, [longest]);
  assert.throws(() => settingsFrom(yaml, globList([`${longest}x`])), /style_exclude\[0\] exceeds 256 bytes/u);
  const refused = [
    [`documentation:\n  max_files: ${MAX_FILES_CEILING + 1}\n`, /max_files must be an integer from 4096 to 16384; got 16385/u],
    [`documentation:\n  max_files: ${DEFAULT_MAX_FILES - 1}\n`, /max_files must be an integer from 4096 to 16384; got 4095/u],
    [`documentation:\n  max_file_bytes: ${MAX_FILE_BYTES_CEILING + 1}\n`,
      /max_file_bytes must be an integer from 1048576 to 4194304; got 4194305/u],
    [`documentation:\n  max_file_bytes: ${DEFAULT_MAX_FILE_BYTES - 1}\n`, /got 1048575/u],
    ["documentation:\n  max_files: \"8192\"\n", /got a string/u],
    ["documentation:\n  max_files: 8192.5\n", /got 8192\.5/u],
    ["documentation:\n  max_files:\n", /max_files must be an integer from 4096 to 16384; got null/u],
    ["documentation:\n  style_exclude:\n", /style_exclude must be a list of globs/u],
    ["documentation:\n  max_total_bytes: 1\n", /documentation has unknown key "max_total_bytes"/u],
    ["documentation:\n  - max_files\n", /documentation must be a mapping/u],
    ["- documentation\n", /must be a YAML mapping/u],
    ["version: 1\n---\nversion: 2\n", /holds 2 YAML documents; expected one/u],
    ["documentation: [\n", /is not valid YAML/u],
    ["documentation:\n  style_exclude: docs/**\n", /style_exclude must be a list of globs/u],
    ["documentation:\n  style_exclude:\n    - 7\n", /style_exclude\[0\] must be a non-empty string/u],
    [globList(["docs/**", "docs/**"]), /style_exclude\[1\] repeats "docs\/\*\*"/u],
  ];
  for (const [text, message] of refused) {
    assert.throws(() => settingsFrom(yaml, text), message, text);
  }
  for (const glob of ["", "**", "*", "**/*", "*/**", "?*.*", "/docs/**", "!docs/**", "C:/docs/**", "../docs/**",
    "docs/../x/**", "./docs/**", "docs/", "docs//x.md", "docs\\x.md"]) {
    assert.throws(() => settingsFrom(yaml, globList([glob])), /documentation\.style_exclude\[0\] /u, glob);
  }
  manifestFileSelfTest(temporary, yaml);
  process.stdout.write("documentation settings fixtures: defaults, declared, ranges, globs, manifest bounds pass\n");
}

function manifestFileSelfTest(temporary, yaml) {
  const root = path.join(temporary, "settings-fixture");
  const manifest = path.join(root, MANIFEST_FILE);
  fs.mkdirSync(root, { recursive: true });
  assert.equal(repositorySettings(root, yaml), DEFAULT_SETTINGS);
  fs.writeFileSync(manifest, "version: 1\ndocumentation:\n  max_files: 8192\n");
  assert.equal(repositorySettings(root, yaml).maxFiles, 8_192);
  fs.writeFileSync(manifest, `# ${"x".repeat(MAX_MANIFEST_BYTES - 3)}\n`);
  assert.equal(repositorySettings(root, yaml), DEFAULT_SETTINGS);
  fs.appendFileSync(manifest, "x");
  assert.throws(() => repositorySettings(root, yaml), /\.standards\.yaml is 1048577 bytes; maximum is 1048576/u);
  fs.unlinkSync(manifest);
  const target = path.join(root, "manifest-target.yaml");
  fs.writeFileSync(target, "documentation:\n  max_files: 8192\n");
  try {
    fs.symlinkSync(target, manifest);
    assert.throws(() => repositorySettings(root, yaml), /refusing non-regular manifest \.standards\.yaml/u);
    fs.unlinkSync(manifest);
  } catch (error) {
    if (!filesystemSymlinkUnavailable(error)) {
      throw error;
    }
    process.stdout.write(`manifest symlink fixture skipped on ${process.platform}: ${error.code}; ` +
      "alternate coverage: the non-file manifest fixture\n");
  }
  fs.mkdirSync(manifest);
  assert.throws(() => repositorySettings(root, yaml), /refusing non-regular manifest \.standards\.yaml/u);
}

function writeFixtureFiles(root, files) {
  for (const [relative, text] of Object.entries(files)) {
    fs.mkdirSync(path.dirname(path.join(root, relative)), { recursive: true });
    fs.writeFileSync(path.join(root, relative), text);
  }
}

// lintCommand runs the lint child on files from root, as runMarkdownlint does, and returns the result.
function lintCommand(root, temporary, files, nodeOptions = []) {
  return command(process.execPath, [...nodeOptions, fileURLToPath(import.meta.url), LINT_MODE, temporary, ...files], {
    cwd: root,
    allowFailure: true,
  });
}

// Negative: markdownlint configuration files anywhere in the repository, loosening (rules off,
// everything ignored, rules replaced by code) or tightening (an 80-column MD013 against the
// locked MD013: false), change nothing, and a configuration module is never executed: the lint
// child hands the library file text and the locked rules only. Boundary: a lint child given no
// file reports nothing and passes.
function hermeticConfigSelfTest(temporary) {
  const fixture = path.join(temporary, "hermetic-fixture");
  const marker = path.join(temporary, "adopter-configuration-ran");
  writeFixtureFiles(fixture, {
    ".markdownlint.json": "{ \"default\": false }\n",
    ".markdownlint-cli2.jsonc": "{ \"ignores\": [\"**\"], \"config\": { \"default\": false } }\n",
    ".markdownlint-cli2.yaml": "config:\n  default: false\n",
    "docs/.markdownlint.yaml": "default: false\n",
    "docs/.markdownlint-cli2.mjs": `import fs from "node:fs";\nfs.writeFileSync(${JSON.stringify(marker)}, "ran");\n` +
      "export default { config: { default: false } };\n",
    "docs/.markdownlint.cjs": `require("node:fs").writeFileSync(${JSON.stringify(marker)}, "ran");\n` +
      "module.exports = { default: false };\n",
    "README.md": "#Malformed root\n",
    "docs/guide.md": "#Malformed guide\n",
  });
  const files = ["README.md", "docs/guide.md"];
  assert.equal(runMarkdownlint(fixture, temporary, files, false), 1);
  const direct = lintCommand(fixture, temporary, files);
  assert.equal(direct.status, 1);
  assert.equal(direct.stderr,
    "docs/guide.md:1:1 error MD018/no-missing-space-atx No space after hash on atx style heading " +
    "[Context: \"#Malformed guide\"]\n" +
    "README.md:1:1 error MD018/no-missing-space-atx No space after hash on atx style heading " +
    "[Context: \"#Malformed root\"]\n");
  assert.equal(fs.existsSync(marker), false);
  const longLine = `# Guide\n\n${"word ".repeat(40).trim()}\n`;
  writeFixtureFiles(fixture, {
    ".markdownlint.json": "{ \"MD013\": { \"line_length\": 80 } }\n",
    ".markdownlint-cli2.jsonc": "{ \"config\": { \"MD013\": { \"line_length\": 80 } } }\n",
    ".markdownlint-cli2.yaml": "config:\n  MD013:\n    line_length: 80\n",
    "docs/.markdownlint.yaml": "MD013:\n  line_length: 80\n",
    "docs/.markdownlint-cli2.mjs": "export default { config: { MD013: { line_length: 80 } } };\n",
    "README.md": longLine,
    "docs/guide.md": longLine,
  });
  assert.equal(runMarkdownlint(fixture, temporary, files, false), 0);
  assert.equal(fs.existsSync(marker), false);
  const empty = lintCommand(fixture, temporary, []);
  assert.deepEqual([empty.status, empty.stdout, empty.stderr], [0, "", ""]);
  process.stdout.write("hermetic configuration fixtures: repository markdownlint files ignored, never executed\n");
}

// Positive: the locked markdownlint-cli2.yaml yields its config mapping, noProgress is accepted, and
// inline markdownlint-configure-file comments parse as JSONC (comments, trailing comma), TOML or
// YAML, as markdownlint-cli2 parsed them. Negative: a key markdownlint-cli2 read beyond config
// (ignores, customRules), a missing or non-mapping config, a list and invalid YAML are refused, and
// text no parser reads throws in each. Boundary: a config mapping alone suffices.
function lintConfigurationSelfTest(temporary) {
  const yaml = loadDependency(temporary, "js-yaml");
  const locked = lintConfiguration(yaml, fs.readFileSync(path.join(temporary, LINT_CONFIG_FILE), "utf8"));
  assert.equal(locked.default, true);
  assert.equal(locked.MD013, false);
  assert.deepEqual(lintConfiguration(yaml, "config:\n  default: true\n"), { default: true });
  assert.deepEqual(lintConfiguration(yaml, "config: {}\nnoProgress: false\n"), {});
  for (const [text, message] of [
    ["config: {}\nignores: [\"**\"]\n", /has unsupported key "ignores"; the gate reads config/u],
    ["customRules: [\"./rule.cjs\"]\nconfig: {}\n", /has unsupported key "customRules"/u],
    ["noProgress: true\n", /must be a mapping with a config mapping/u],
    ["config: [MD013]\n", /must be a mapping with a config mapping/u],
    ["- config\n", /must be a mapping with a config mapping/u],
    ["", /markdownlint-cli2\.yaml is not valid YAML/u],
    ["config: [\n", /markdownlint-cli2\.yaml is not valid YAML/u],
  ]) {
    assert.throws(() => lintConfiguration(yaml, text), message, text);
  }
  const [jsonc, toml, yamlParser] = configurationParsers(temporary, yaml);
  assert.deepEqual(jsonc("{ /* off */ \"MD018\": false, }"), { MD018: false });
  assert.deepEqual(toml("MD018 = false\n"), Object.assign(Object.create(null), { MD018: false }));
  assert.deepEqual(yamlParser("MD018: false\n"), { MD018: false });
  assert.throws(() => jsonc("MD018: false"), /Unable to parse JSONC content/u);
  assert.throws(() => toml("{ not: [toml"));
  assert.throws(() => yamlParser("{ not: [yaml"));
  process.stdout.write("lint configuration fixtures: locked config read, other keys refused, inline parsers in order\n");
}

// LINT_OUTPUT_FIXTURES and LINT_OUTPUT_EXPECTED pin what a failing style run prints. The expected
// lines are the output markdownlint-cli2 0.23.3 printed for these files with the locked
// configuration, captured before the gate moved to the library (#736): the format
// (file:line[:column] severity rule description [detail] [Context]), the order (file by
// localeCompare, then line, then rule name), inline configure-file comments in JSONC, TOML and
// YAML, an unparseable one ignored, TOML front matter, CRLF line endings, MDX and templates.
const LINT_OUTPUT_FIXTURES = Object.freeze({
  "README.md": "#Root\n",
  "docs/a b.md": "#space name\n",
  "docs/Zeta.md": "#zeta\n",
  "docs/alpha.md": "# Alpha\n\nText  \n\n\n## Two\n### Skip\n",
  "docs/crlf.md": "# CRLF\r\n\r\ntext  \r\n#bad\r\n",
  "docs/emphasis.md": "# Emph\n\n* * *\n___\nsome *emph * here and __strong__ vs **strong** and _e_ vs *e*\n",
  "docs/inline-jsonc.md": "<!-- markdownlint-configure-file { /* c */ \"MD018\": false, } -->\n#No space jsonc\n",
  "docs/inline-toml.md": "<!-- markdownlint-configure-file\nMD018 = false\n-->\n#No space toml\n",
  "docs/inline-yaml.md": "<!-- markdownlint-configure-file\nMD018: false\n-->\n#No space yaml\n",
  "docs/configure-bad.md": "<!-- markdownlint-configure-file { not: [valid -->\n#bad configure\n",
  "docs/disable.md": "# D\n\n<!-- markdownlint-disable-next-line MD018 -->\n#skip\n#bad\n",
  "docs/frontmatter.md": "+++\ntitle = \"x\"\n+++\n#bad\n",
  "docs/tables.md": "# Tables\n\n| a | b |\n|---|---|\n| 1 | 2 | 3 |\n",
  "docs/unicode.md": "# Ünïcödé \u{1F600}\n\n\u{1F600} *x *\n",
  "docs/notrailing.md": "# No newline",
  "docs/page.mdx": "# MDX\n\n<Card href=\"../README.md\">Public</Card>\n\n#bad\n",
  "templates/T.md.tmpl": "# {{ .Name }}\n\n- {{ . }}\n#bad\n",
  "docs/empty.md": "",
});
const MD018 = "error MD018/no-missing-space-atx No space after hash on atx style heading";
const LINT_OUTPUT_EXPECTED = Object.freeze([
  `docs/a b.md:1:1 ${MD018} [Context: "#space name"]`,
  "docs/alpha.md:5 error MD012/no-multiple-blanks Multiple consecutive blank lines [Expected: 1; Actual: 2]",
  "docs/alpha.md:6 error MD022/blanks-around-headings Headings should be surrounded by blank lines " +
    "[Expected: 1; Actual: 0; Below] [Context: \"## Two\"]",
  "docs/alpha.md:7 error MD022/blanks-around-headings Headings should be surrounded by blank lines " +
    "[Expected: 1; Actual: 0; Above] [Context: \"### Skip\"]",
  `docs/configure-bad.md:2:1 ${MD018} [Context: "#bad configure"]`,
  `docs/crlf.md:4:1 ${MD018} [Context: "#bad"]`,
  `docs/disable.md:5:1 ${MD018} [Context: "#bad"]`,
  "docs/emphasis.md:4 error MD035/hr-style Horizontal rule style [Expected: * * *; Actual: ___]",
  "docs/emphasis.md:5:11 error MD037/no-space-in-emphasis Spaces inside emphasis markers [Context: \"h *\"]",
  "docs/emphasis.md:5:59 error MD049/emphasis-style Emphasis style [Expected: underscore; Actual: asterisk]",
  "docs/emphasis.md:5:61 error MD049/emphasis-style Emphasis style [Expected: underscore; Actual: asterisk]",
  "docs/emphasis.md:5:37 error MD050/strong-style Strong style [Expected: underscore; Actual: asterisk]",
  "docs/emphasis.md:5:45 error MD050/strong-style Strong style [Expected: underscore; Actual: asterisk]",
  `docs/frontmatter.md:4:1 ${MD018} [Context: "#bad"]`,
  "docs/notrailing.md:1:12 error MD047/single-trailing-newline Files should end with a single newline character",
  `docs/page.mdx:5:1 ${MD018} [Context: "#bad"]`,
  "docs/tables.md:5:9 error MD056/table-column-count Table column count " +
    "[Expected: 2; Actual: 3; Too many cells, extra data will be missing]",
  "docs/unicode.md:3:6 error MD037/no-space-in-emphasis Spaces inside emphasis markers [Context: \"x *\"]",
  `docs/Zeta.md:1:1 ${MD018} [Context: "#zeta"]`,
  `README.md:1:1 ${MD018} [Context: "#Root"]`,
  `templates/T.md.tmpl:4:1 ${MD018} [Context: "#bad"]`,
]);

// Positive: the lint child prints exactly what markdownlint-cli2 printed for the same files,
// whatever order the files come in. Negative: a symbolic link or a missing file is refused.
function lintOutputSelfTest(temporary) {
  const fixture = path.join(temporary, "lint-output-fixture");
  writeFixtureFiles(fixture, LINT_OUTPUT_FIXTURES);
  const files = Object.keys(LINT_OUTPUT_FIXTURES);
  for (const order of [[...files].sort(), [...files].reverse()]) {
    const result = lintCommand(fixture, temporary, order);
    assert.deepEqual([result.status, result.stdout], [1, ""]);
    assert.equal(result.stderr, `${LINT_OUTPUT_EXPECTED.join("\n")}\n`);
  }
  const missing = lintCommand(fixture, temporary, ["docs/absent.md"]);
  assert.equal(missing.status, 2);
  assert.match(missing.stderr, /refusing to lint non-file Markdown source docs\/absent\.md/u);
  try {
    fs.symlinkSync("README.md", path.join(fixture, "linked.md"));
    assert.match(lintCommand(fixture, temporary, ["linked.md"]).stderr, /refusing to lint non-file Markdown source/u);
  } catch (error) {
    if (!filesystemSymlinkUnavailable(error)) {
      throw error;
    }
    process.stdout.write(filesystemSymlinkSkipDiagnostic(process.platform, error.code));
  }
  process.stdout.write("lint output fixtures: findings, order and format match markdownlint-cli2 0.23.3\n");
}

// markdownlint parses math with micromark-extension-math, whose module load imports katex: the
// package.json override lifts katex past the extension's ^0.16.0 range to a release free of
// GHSA-238p-pmpm-9mq7 (#793), so this fixture proves the overridden katex still serves the
// extension. Positive: emphasis and reversed-link text inside inline and block math is math, not
// a finding, and the extension's HTML side renders both through katex. Negative: the same text
// outside math is reported, and TeX that katex cannot parse throws.
const MATH_FIXTURES = Object.freeze({
  "docs/math.md": "# Math\n\nInline $x *y * z$ and $(a)[b]$ here.\n\n$$\nx *y * z\n$$\n",
  "docs/plain.md": "# Plain\n\nInline x *y * z and (a)[b] here.\n\nx *y * z\n",
});
const MATH_EXPECTED = Object.freeze([
  "docs/plain.md:3:21 error MD011/no-reversed-links Reversed link syntax [(a)[b]]",
  "docs/plain.md:3:12 error MD037/no-space-in-emphasis Spaces inside emphasis markers [Context: \"y *\"]",
  "docs/plain.md:5:5 error MD037/no-space-in-emphasis Spaces inside emphasis markers [Context: \"y *\"]",
]);
function mathSelfTest(temporary) {
  const fixture = path.join(temporary, "math-fixture");
  writeFixtureFiles(fixture, MATH_FIXTURES);
  const result = lintCommand(fixture, temporary, Object.keys(MATH_FIXTURES));
  assert.deepEqual([result.status, result.stdout], [1, ""]);
  assert.equal(result.stderr, `${MATH_EXPECTED.join("\n")}\n`);
  const { micromark } = loadDependency(temporary, "micromark");
  const { math, mathHtml } = loadDependency(temporary, "micromark-extension-math");
  const render = (text) => micromark(text, { extensions: [math()], htmlExtensions: [mathHtml()] });
  const html = render(MATH_FIXTURES["docs/math.md"]);
  assert.match(html, /<span class="math math-inline"><span class="katex">/u);
  assert.match(html, /<div class="math math-display"><span class="katex-display">/u);
  assert.throws(() => render("$\\frac{a$\n"), /KaTeX parse error/u);
  const katex = loadDependency(temporary, "katex/package.json").version;
  process.stdout.write(`math fixtures: inline and block math lint as math, katex ${katex} renders both\n`);
}

// The per-file ceiling is a memory bound (see MAX_FILE_BYTES_CEILING). Boundary: a file exactly at
// the ceiling, of linked and code-spanned bullets, lints clean in a lint child capped at
// LINT_MEMORY_HEAP_MB. Negative: one byte past the ceiling is refused before it is read.
const LINT_MEMORY_HEAP_MB = 1_536;
function lintMemorySelfTest(temporary) {
  const fixture = path.join(temporary, "lint-memory-fixture");
  const bullet = "- Lorem ipsum dolor sit amet, [consectetur](https://example.invalid/x) `adipiscing` elit.\n";
  const count = Math.floor((MAX_FILE_BYTES_CEILING - 16) / bullet.length);
  const headingBytes = MAX_FILE_BYTES_CEILING - count * bullet.length;
  const markdown = `# ${"a".repeat(headingBytes - 4)}\n\n${bullet.repeat(count)}`;
  assert.equal(Buffer.byteLength(markdown), MAX_FILE_BYTES_CEILING);
  writeFixtureFiles(fixture, { "docs/ceiling.md": markdown, "docs/over.md": `${markdown}a` });
  const ceiling = lintCommand(fixture, temporary, ["docs/ceiling.md"], [`--max-old-space-size=${LINT_MEMORY_HEAP_MB}`]);
  assert.deepEqual([ceiling.status, ceiling.stderr], [0, ""]);
  const over = lintCommand(fixture, temporary, ["docs/over.md"]);
  assert.equal(over.status, 2);
  assert.match(over.stderr, /docs\/over\.md is 4194305 bytes; per-file ceiling is 4194304/u);
  process.stdout.write(`lint memory fixture: a ${MAX_FILE_BYTES_CEILING}-byte file lints in a ` +
    `${LINT_MEMORY_HEAP_MB} MiB heap\n`);
}

// Positive: declared globs remove fragments, generated indexes and fixtures from the style run
// and report what each removed, while unmatched and dot-directory globs behave as globs. Negative:
// a declaration that removes every styled file fails the gate, and an excluded file still fails
// the private-link rule. Boundary: all but one file excluded passes, and a repository whose only
// Markdown is built-in excluded has nothing for a declaration to empty.
function styleExclusionSelfTest(temporary) {
  const platform = "linux";
  const files = ["AGENTS.md", "README.md", "changelog.d/fixed/fragment.md", "docs/adr/_index_fragments/0001.md",
    "docs/adr/_index_fragments/nested/0002.md", "docs/guide.md", "pkg/bench/testdata/golden.md"];
  const declared = (styleExclude) => ({ ...DEFAULT_SETTINGS, declared: true, styleExclude });
  const selection = styleSelection(files,
    declared(["changelog.d/**", "docs/adr/_index_fragments/*.md", "**/testdata/**", "unused/**"]), platform);
  assert.deepEqual(selection, {
    styled: ["README.md", "docs/adr/_index_fragments/nested/0002.md", "docs/guide.md"],
    excluded: 3,
    counts: [1, 1, 1, 0],
  });
  assert.deepEqual(styleSelection(files, DEFAULT_SETTINGS, platform).styled, files.filter(isStyleSelected));
  assert.deepEqual(styleSelection([".github/ISSUE_TEMPLATE/bug.md", "README.md"], declared([".github/**"]),
    platform).styled, ["README.md"]);
  assert.throws(() => styleSelection(files, declared(["README.md", "docs/**", "changelog.d/**", "pkg/**"]), platform),
    /documentation\.style_exclude excludes all 6 style-selected Markdown files/u);
  assert.deepEqual(styleSelection(files, declared(["docs/**", "changelog.d/**", "pkg/**"]), platform).styled,
    ["README.md"]);
  assert.deepEqual(styleSelection(["AGENTS.md"], declared(["docs/**"]), platform).styled, []);
  const fixture = path.join(temporary, "exclusion-fixture");
  writeFixtureFiles(fixture, {
    ".gitignore": "/.workingdir/\n",
    [MANIFEST_FILE]: globList(["changelog.d/**"]),
    "README.md": "# Root\n",
    "changelog.d/fixed/fragment.md": "#fixed a bug, notes in [scratch](../../.workingdir/OPEN.md)\n",
  });
  command("git", ["init", "--quiet"], { cwd: fixture });
  const real = fs.realpathSync(fixture);
  const settings = repositorySettings(real, loadDependency(temporary, "js-yaml"));
  const all = inventory(real, settings);
  const fixtureSelection = styleSelection(all, settings, platform);
  assert.deepEqual(fixtureSelection.styled, ["README.md"]);
  assert.equal(summaryLine(settings, fixtureSelection, all.length), "markdown-governance: styled 1 public Markdown " +
    "files (1 excluded by documentation.style_exclude); checked 2 tracked/non-ignored Markdown files for private links\n");
  assert.equal(runMarkdownlint(fixture, temporary, fixtureSelection.styled, false), 0);
  assert.equal(runMarkdownlint(fixture, temporary, all.filter(isStyleSelected), false), 1);
  assert.equal(runScratchRule(fixture, temporary, all, false, false), 1);
  process.stdout.write("style exclusion fixtures: declared globs, counts, never everything, private links kept\n");
}

// STYLE_EXCLUSION_TABLE holds, for each pattern, the paths of STYLE_EXCLUSION_PATHS that
// micromatch 4.0.8 with { dot: true } matched, recorded before the gate dropped it (#736), and
// CHARACTER_TABLE does the same for CHARACTER_PATHS: the ASCII punctuation the gate reads as
// itself, in a glob of one segment and in a longer one, + repeated in a longer glob, a combining
// mark, and letters whose case pairs differ. Random differentials of the matcher against
// micromatch found each refused shape. An earlier count of no difference over about 54 million
// pairs came from a generator that never wrote a double quote or a run of +, $ or ^ in a glob of
// one segment, both of which micromatch matched differently. Drawing globs and paths from every
// printable ASCII character, tab and a Unicode sample (no-break space, dotted and dotless I, the
// Kelvin sign, combining marks, a letter outside the Basic Multilingual Plane), 8 runs found no
// mismatch over 112,136,472 glob-path pairs from 113,607 accepted globs: no mismatch found over
// that alphabet, which is a measurement, not a proof of equivalence.
// LITERAL_STYLE_PATHS spell glob characters, or a character outside ASCII, in a file name.
const LITERAL_STYLE_PATHS = Object.freeze(["docs/{a,c}.md", "docs/[a-b].md", "docs/[{a]x.md", "docs/{x.md",
  "docs/é.md", "docs/1+.md"]);
const STYLE_EXCLUSION_PATHS = Object.freeze(["README.md", "docs/guide.md", "docs/.hidden/note.md",
  "docs/x/y/deep.md", "docs/a.md", "docs/b.md", "docs/c.md", "docs/[ab].md", "pkg/testdata/golden.md",
  "testdata/top.md", ".github/ISSUE_TEMPLATE/bug.md", "Docs/guide.md", "changelog.d/fixed/x.md", "docs/x.markdown",
  ...LITERAL_STYLE_PATHS]);
const STYLE_EXCLUSION_TABLE = Object.freeze([
  ["docs/**", ["docs/guide.md", "docs/.hidden/note.md", "docs/x/y/deep.md", "docs/a.md", "docs/b.md", "docs/c.md",
    "docs/[ab].md", "docs/x.markdown", ...LITERAL_STYLE_PATHS]],
  ["**/testdata/**", ["pkg/testdata/golden.md", "testdata/top.md"]],
  [".github/**", [".github/ISSUE_TEMPLATE/bug.md"]],
  ["docs/*.md", ["docs/guide.md", "docs/a.md", "docs/b.md", "docs/c.md", "docs/[ab].md", ...LITERAL_STYLE_PATHS]],
  ["docs/?.md", ["docs/a.md", "docs/b.md", "docs/c.md", "docs/é.md"]],
  ["docs/[ab].md", ["docs/a.md", "docs/b.md", "docs/[ab].md"]],
  ["docs/[a-b].md", ["docs/a.md", "docs/b.md", "docs/[a-b].md"]],
  ["docs/[^a].md", ["docs/b.md", "docs/c.md", "docs/é.md"]],
  ["docs/{a,c}.md", ["docs/a.md", "docs/c.md", "docs/{a,c}.md"]],
  ["docs/*.{md,markdown}", ["docs/guide.md", "docs/a.md", "docs/b.md", "docs/c.md", "docs/[ab].md", "docs/x.markdown",
    ...LITERAL_STYLE_PATHS]],
  ["docs/*/**", ["docs/.hidden/note.md", "docs/x/y/deep.md"]],
  ["docs/**/*.md", ["docs/guide.md", "docs/.hidden/note.md", "docs/x/y/deep.md", "docs/a.md", "docs/b.md", "docs/c.md",
    "docs/[ab].md", ...LITERAL_STYLE_PATHS]],
  ["*.md", ["README.md"]],
  ["changelog.d/**/x.md", ["changelog.d/fixed/x.md"]],
  ["README.md/**", ["README.md"]],
  ["docs/[{a]*.md", ["docs/a.md", "docs/{a,c}.md", "docs/{x.md"]],
  ["docs/[^ -~].md", ["docs/é.md"]],
  ["docs/?+.md", ["docs/1+.md"]],
  ["docs/{a,c}.*", ["docs/a.md", "docs/c.md"]],
  ["docs/{x,c.[m]*}", ["docs/c.md"]],
  ["docs/{x*,c}.md", ["docs/c.md"]],
]);

// LITERAL_PUNCTUATION is the ASCII punctuation micromatch read as itself outside a class or list.
const LITERAL_PUNCTUATION = "!#$%&'+,:;<=>@^`~";
const CHARACTER_PATHS = Object.freeze([`a ${LITERAL_PUNCTUATION}.md`, `a ${LITERAL_PUNCTUATION}.mdx`,
  `docs/a ${LITERAL_PUNCTUATION}.md`, "docs/c++/x.md", "docs/c+/x.md", "c+.md", "c++.md", "docs/e\u0301.md",
  "docs/\u00e9.md", "docs/\u0130\u0131\u212a.md", "docs/iik.md", "docs/a$/x.md", "docs/$/x.md"]);
const CHARACTER_TABLE = Object.freeze([
  [`? ${LITERAL_PUNCTUATION}.md`, [`a ${LITERAL_PUNCTUATION}.md`]],
  [`docs/? ${LITERAL_PUNCTUATION}.md`, [`docs/a ${LITERAL_PUNCTUATION}.md`]],
  ["docs/c++/**", ["docs/c++/x.md"]],
  ["c+.md", ["c+.md"]],
  ["docs/{c+,c++}/**", ["docs/c++/x.md", "docs/c+/x.md"]],
  ["docs/*$/**", ["docs/a$/x.md", "docs/$/x.md"]],
  ["docs/e\u0301.*", ["docs/e\u0301.md"]],
  ["docs/\u0130\u0131\u212a.md", ["docs/\u0130\u0131\u212a.md"]],
]);
const REPEAT_REFUSAL = /puts \+ straight after \], \{ or \}, where micromatch reads it as a regular-expression repeat/u;
const RUN_REFUSAL = /repeats \+, \$ or \^ in a glob of one segment, where micromatch escapes only the first character/u;
const GROUP_REFUSAL = /contains "[()|]" \(U\+00(28|29|7C)\); extglobs and regex groups are not supported/u;
const REFUSED_STYLE_EXCLUSIONS = Object.freeze([
  ["docs/@(a|b).md", GROUP_REFUSAL],
  ["docs/+(a).md", GROUP_REFUSAL],
  ["docs/a|b.md", GROUP_REFUSAL],
  ["docs/\"draft\"/**", /contains "\\"" \(U\+0022\); micromatch reads it as a quote around literal text/u],
  ["docs/a\tb.md", /contains "\\t" \(U\+0009\); a glob may hold letters/u],
  ["docs/a\u00a0b.md", /contains "\u00a0" \(U\+00A0\)/u],
  ["docs/\u{1f600}.md", /contains "\u{1f600}" \(U\+1F600\)/u],
  ["docs/a\u200db.md", /contains "\u200d" \(U\+200D\)/u],
  ["docs/[[:alpha:]].md", /POSIX classes such as \[:alpha:\] are not supported/u],
  ["docs/{1..3}.md", /brace ranges are not supported/u],
  ["docs/{a,{b,c}}.md", /nested lists are not supported/u],
  ["docs/{a}.md", /brace list without a comma/u],
  ["docs/{a,b.md", /has a \{ without a closing \}/u],
  ["docs/a}.md", /has a \} without an opening \{/u],
  ["docs/[ab.md", /has a \[ without a closing \]/u],
  ["docs/a]b.md", /has a \] without an opening \[/u],
  ["docs/[].md", /has an empty \[ \] class/u],
  ["docs/[!a].md", /starts a \[ \] class with !, which micromatch reads as the character !/u],
  ["docs/[b-a].md", /has the reversed range b-a/u],
  ["docs/a**.md", /\*\* must be a whole segment/u],
  ["docs/**.md", /\*\* must be a whole segment/u],
  ["docs/{,}/x.md", /leaves an empty, \. or \.\. segment/u],
  ["{?,docs}/**", /a brace alternative of wildcards alone would match every file/u],
  ["docs/[0-9]+.md", REPEAT_REFUSAL],
  ["docs/v{1,2}+.md", REPEAT_REFUSAL],
  ["docs/{+,a}.md", REPEAT_REFUSAL],
  ["c++.md", RUN_REFUSAL],
  ["a$$*.md", RUN_REFUSAL],
  ["^^a*.md", RUN_REFUSAL],
  ["docs/a[ -~]b.md", /has the range  -~ in a \[ \] class, which spans \/ and so lets micromatch match a path/u],
  ["docs/[--z].md", /has the range --z in a \[ \] class, which spans \//u],
  ["docs/x/**/{*,draft}", /has a brace alternative of \* alone, which micromatch lets match nothing/u],
  ["docs/*{,a}", /has a brace alternative of \* alone/u],
  ["{*,docs}/**", /has a brace alternative of \* alone/u],
  ["docs/{x,.*}", /puts \.\* inside a brace list, which micromatch matches differently/u],
  ["docs/{x,[a].*}", /puts \.\* inside a brace list/u],
  [`docs/${"{a,b}".repeat(7)}.md`, /expands to more than 64 alternatives in one segment/u],
  [`docs/${"[ab]".repeat(7)}.md`, /expands to more than 64 alternatives in one segment/u],
]);

// Positive: every pattern in STYLE_EXCLUSION_TABLE and CHARACTER_TABLE matches exactly the paths
// micromatch matched, a path spelled as the glob included, and on Windows a backslash separates
// segments, though one before a glob character does not make the glob's own spelling. Negative:
// every shape and character the gate does not support is refused with the reason, never matched
// differently. Boundary: the tables keep the accepted neighbours of each refused shape (+ after ?,
// a negated range spanning /, .* after a list, a class between . and * in a list, a * with more
// text in a list, one + in a glob of one segment, ++ in a longer glob); a segment spelling exactly
// MAX_BRACE_ALTERNATIVES alternatives passes and twice that is refused; and an empty exclusion
// list styles every file the built-in selection styles.
function styleExclusionGrammarSelfTest() {
  for (const [paths, table] of [[STYLE_EXCLUSION_PATHS, STYLE_EXCLUSION_TABLE], [CHARACTER_PATHS, CHARACTER_TABLE]]) {
    for (const [pattern, matched] of table) {
      assert.deepEqual(paths.filter(styleExclusionMatcher(pattern, "linux")), matched, pattern);
    }
  }
  assert.equal(styleExclusionMatcher("docs/*.md", "win32")("docs\\guide.md"), true);
  assert.equal(styleExclusionMatcher("docs/*.md", "linux")("docs\\guide.md"), false);
  assert.equal(styleExclusionMatcher("**/testdata/**", "win32")("pkg\\testdata\\golden.md"), true);
  assert.equal(styleExclusionMatcher("docs/{a,c}.md", "win32")("docs\\a.md"), true);
  assert.equal(styleExclusionMatcher("docs/{a,c}.md", "win32")("docs\\{a,c}.md"), false);
  assert.equal(styleExclusionMatcher("docs/[ab].md", "win32")("docs\\[ab].md"), true);
  assert.equal(styleExclusionMatcher("docs/{a,c}.md", "linux")("docs/{a,c}.mdx"), false);
  for (const [pattern, message] of REFUSED_STYLE_EXCLUSIONS) {
    assert.throws(() => styleExclusions([pattern]), message, pattern);
    assert.throws(() => styleExclusions([pattern]), /documentation\.style_exclude\[0\] /u, pattern);
  }
  for (const pattern of [`docs/${"{a,b}".repeat(6)}.md`, `docs/${"[ab]".repeat(6)}.md`]) {
    assert.deepEqual(styleExclusions([pattern]), [pattern]);
    assert.equal(styleExclusionMatcher(pattern, "linux")("docs/abbaab.md"), true);
  }
  styleExclusionCharacterSelfTest();
  const files = ["AGENTS.md", "README.md", "docs/guide.md"];
  assert.deepEqual(styleSelection(files, { ...DEFAULT_SETTINGS, declared: true, styleExclude: [] }, "linux"),
    { styled: ["README.md", "docs/guide.md"], excluded: 0, counts: [] });
  process.stdout.write("style exclusion grammar fixtures: micromatch answers kept, unsupported shapes refused\n");
}

// Negative: of the printable ASCII characters exactly " ( ) \ | are refused, and a tab, a
// no-break space, an emoji, a joiner and a line separator are refused with their code points.
// Positive: letters, combining marks and digits of other scripts pass. Boundary: each character
// of LITERAL_PUNCTUATION and the space passes inside a file name and matches only itself there.
function styleExclusionCharacterSelfTest() {
  const ascii = Array.from({ length: 0x7f - 0x20 }, (_, index) => String.fromCharCode(0x20 + index));
  assert.equal(ascii.filter((character) => characterProblem(character) !== null).join(""), "\"()\\|");
  for (const character of ["\t", "\u00a0", "\u{1f600}", "\u200d", "\u2028"]) {
    const codePoint = character.codePointAt(0).toString(16).toUpperCase().padStart(4, "0");
    assert.match(characterProblem(`docs/${character}.md`), new RegExp(`^contains .+ \\(U\\+${codePoint}\\); a glob`, "su"));
  }
  assert.equal(characterProblem("docs/\u00e9e\u0301\u093f\u0130\u0131\u212a\u4e2d\u{1d400}\u0663.md"), null);
  for (const character of ` ${LITERAL_PUNCTUATION}`) {
    const pattern = `docs/a${character}b.md`;
    assert.deepEqual(styleExclusions([pattern]), [pattern], pattern);
    const matcher = styleExclusionMatcher(pattern, "linux");
    assert.deepEqual([matcher(pattern), matcher("docs/ab.md"), matcher("docs/a_b.md")], [true, false, false], pattern);
  }
}

// Boundary: a file exactly at a raised per-file bound passes inventory, the private-link rule and
// the style rules, and one byte more fails; the file count passes exactly at a raised bound and
// fails one past it. Negative: at the defaults the same file fails and the message names the
// setting that raises the bound; at a ceiling the message offers no further raise.
function raisedBoundSelfTest(temporary) {
  const fixture = path.join(temporary, "raised-bound-fixture");
  const raised = DEFAULT_MAX_FILE_BYTES + 4_096;
  const heading = "# Large document\n\n";
  writeFixtureFiles(fixture, { "docs/large.md": `${heading}${"a".repeat(raised - heading.length - 1)}\n` });
  const large = path.join(fixture, "docs", "large.md");
  assert.equal(fs.statSync(large).size, raised);
  command("git", ["init", "--quiet"], { cwd: fixture });
  const real = fs.realpathSync(fixture);
  assert.throws(() => inventory(real), new RegExp("docs/large\\.md is 1052672 bytes; per-file maximum is 1048576; " +
    "documentation\\.max_file_bytes in \\.standards\\.yaml raises it up to 4194304", "u"));
  const settings = { ...DEFAULT_SETTINGS, declared: true, maxFileBytes: raised };
  const files = inventory(real, settings);
  assert.deepEqual(files, ["docs/large.md"]);
  assert.equal(runScratchRule(fixture, temporary, files, false, false), 0);
  assert.equal(runMarkdownlint(fixture, temporary, files, false), 0);
  fs.appendFileSync(large, "a");
  assert.throws(() => inventory(real, settings), /docs\/large\.md is 1052673 bytes; per-file maximum is 1052672;/u);
  const counted = { ...DEFAULT_SETTINGS, declared: true, maxFiles: 8_192 };
  assert.doesNotThrow(() => checkFileCount(8_192, counted));
  assert.throws(() => checkFileCount(8_193, counted),
    /Markdown inventory has 8193 files; maximum is 8192; documentation\.max_files in \.standards\.yaml/u);
  const ceiling = { ...DEFAULT_SETTINGS, declared: true, maxFiles: MAX_FILES_CEILING, maxFileBytes: MAX_FILE_BYTES_CEILING };
  assert.doesNotThrow(() => checkFileCount(MAX_FILES_CEILING, ceiling));
  assert.throws(() => checkFileCount(MAX_FILES_CEILING + 1, ceiling),
    (error) => error.message === "Markdown inventory has 16385 files; maximum is 16384");
  assert.doesNotThrow(() => checkFileSize("docs/x.md", MAX_FILE_BYTES_CEILING, ceiling));
  assert.throws(() => checkFileSize("docs/x.md", MAX_FILE_BYTES_CEILING + 1, ceiling),
    (error) => error.message === "docs/x.md is 4194305 bytes; per-file maximum is 4194304");
  process.stdout.write("raised bound fixtures: exactly at a raised cap passes, one past fails\n");
}

// The planted lint child sleeps this long, past a 1 s budget and well inside a 10 s one, and
// ignores the paths it is given.
const PLANTED_CHILD_MS = 2_500;
const PLANTED_CHILD = Object.freeze([process.execPath, "-e",
  `Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ${PLANTED_CHILD_MS});`]);
// A gate deadline this far ahead outlasts one planted child, startup included, and runs out
// during the second: it is below twice the sleep.
const PLANTED_DEADLINE_MS = 4_900;
// Each of this many paths of this many bytes, plus its separator, fills one lint batch exactly.
const FULL_BATCH_PATHS = 100;
const FULL_BATCH_PATH_BYTES = 239;

function budgetFailure(expected) {
  return (error) => {
    assert.ok(error instanceof GateFailure, error.stack);
    assert.deepEqual([error.status, error.message], [2, expected]);
    return true;
  };
}

// Negative: a planted lint child sleeping past a 1 s budget fails the gate with status 2, naming the
// batch, its bytes, its five largest files as suspects, largest first, the setting that raises the
// budget and the command that lints a suspect alone. Positive: the setting raises the budget to
// 10 s and the same batch passes. Boundary: 1 s and the ceiling are accepted, one past either end
// is refused naming the key, and at the ceiling a one-file batch report offers no further step.
function lintBudgetSelfTest(temporary) {
  const yaml = loadDependency(temporary, "js-yaml");
  const fixture = path.join(temporary, "lint-budget-fixture");
  const files = Array.from({ length: 7 }, (_, index) => `docs/page-${index}.md`);
  writeFixtureFiles(fixture,
    Object.fromEntries(files.map((file, index) => [file, `# Page\n\n${"a".repeat(index * 100)}\n`])));
  const budget = (seconds) => settingsFrom(yaml, `documentation:\n  lint_timeout_seconds: ${seconds}\n`);
  const suspects = [6, 5, 4, 3, 2].map((index) => `  docs/page-${index}.md (${9 + index * 100} bytes)`);
  assert.throws(() => runMarkdownlint(fixture, temporary, files, false, budget(1), PLANTED_CHILD), budgetFailure(
    "lint batch 1 of 1 (7 files, 2163 bytes) exceeded its 1 s budget; documentation.lint_timeout_seconds in " +
    ".standards.yaml raises it up to 480\nsuspects, the batch's largest files (size is no proof):\n" +
    `${suspects.join("\n")}\nlint a suspect alone to time it: node tools/markdownlint/verify.mjs --only docs/page-6.md`));
  assert.equal(runMarkdownlint(fixture, temporary, files, false, budget(10), PLANTED_CHILD), 0);
  assert.equal(budget(LINT_TIMEOUT_SECONDS_CEILING).lintTimeoutSeconds, LINT_TIMEOUT_SECONDS_CEILING);
  assert.equal(budget(1).lintTimeoutSeconds, 1);
  assert.equal(settingsFrom(yaml, "documentation:\n  max_files: 8192\n").lintTimeoutSeconds,
    DEFAULT_LINT_TIMEOUT_SECONDS);
  for (const seconds of [0, -1, LINT_TIMEOUT_SECONDS_CEILING + 1, "\"300\"", "1.5"]) {
    assert.throws(() => budget(seconds),
      /^Error: \.standards\.yaml documentation\.lint_timeout_seconds must be an integer from 1 to 480; got /u);
  }
  assert.equal(lintBudgetReport(fixture, ["docs/page-1.md"], "3 of 3", LINT_TIMEOUT_SECONDS_CEILING),
    "lint batch 3 of 3 (1 file, 109 bytes) exceeded its 480 s budget\n" +
    "suspects, the batch's largest files (size is no proof):\n  docs/page-1.md (109 bytes)");
  process.stdout.write("lint budget fixtures: a child past its budget names the batch and its suspects, " +
    "the setting raises the budget, a budget outside 1 to 480 s is refused\n");
}

// Negative: a lint child that keeps within its 10 s budget still fails the gate with status 2 once
// the gate deadline runs out: two batches of one planted child each, under a deadline that outlasts
// the first child but not the second, stop in batch 2 of 2, and the report names the deadline and
// no setting. Positive: the same two batches pass under a deadline that outlasts both. Boundary: a
// command started past the deadline fails at once, naming it, and every run lifts its deadline.
// The fixture deadlines compress the gate's 480 s one, which the reports still name.
function gateDeadlineSelfTest(temporary) {
  const fixture = path.join(temporary, "gate-deadline-fixture");
  writeFixtureFiles(fixture, { "docs/last.md": "# Last\n" });
  const full = Array.from({ length: FULL_BATCH_PATHS },
    (_, index) => `docs/${String(index).padStart(2, "0")}-${"n".repeat(FULL_BATCH_PATH_BYTES - 11)}.md`);
  assert.equal(Buffer.byteLength(full[0]), FULL_BATCH_PATH_BYTES);
  const files = [...full, "docs/last.md"];
  assert.deepEqual(batches(files), [full, ["docs/last.md"]]);
  const settings = { ...DEFAULT_SETTINGS, lintTimeoutSeconds: 10 };
  const lint = () => runMarkdownlint(fixture, temporary, files, false, settings, PLANTED_CHILD);
  assert.throws(() => withGateDeadline(PLANTED_DEADLINE_MS, lint), budgetFailure(
    "lint batch 2 of 2 (1 file, 7 bytes) ran past the gate's 480 s deadline before its 10 s budget ran out; " +
    "the deadline keeps the gate inside the hosted job's 10-minute limit, and no setting moves it\n" +
    "suspects, the batch's largest files (size is no proof):\n  docs/last.md (7 bytes)"));
  assert.equal(gateDeadline, Infinity);
  assert.equal(withGateDeadline(4 * PLANTED_DEADLINE_MS, lint), 0);
  const started = performance.now();
  assert.throws(() => withGateDeadline(0, () => command(PLANTED_CHILD[0], PLANTED_CHILD.slice(1))),
    budgetFailure(`${process.execPath} ran past the gate's 480 s deadline; the deadline keeps the gate ` +
      "inside the hosted job's 10-minute limit, and no setting moves it"));
  assert.ok(performance.now() - started < PLANTED_CHILD_MS, "a command past the deadline was not stopped at once");
  assert.equal(gateDeadline, Infinity);
  process.stdout.write("gate deadline fixtures: a batch within its budget stops at the gate deadline and names it, " +
    "a run inside the deadline passes, a command past it fails at once\n");
}

// Positive: --only lints a named style-selected file, named from the repository root or from a
// subdirectory, and a name given twice once. Negative: a file the style rules skip, a missing file,
// a path outside the repository and a declared exclusion are refused before any lint child, and a
// command line of another shape is refused. Boundary: one name past the file-count ceiling fails.
function onlyModeSelfTest(temporary) {
  const fixture = path.join(temporary, "only-mode-fixture");
  writeFixtureFiles(fixture,
    { "AGENTS.md": "#Generated surface\n", "docs/guide.md": "# Guide\n", "docs/bad.md": "#Bad\n" });
  command("git", ["init", "--quiet"], { cwd: fixture });
  const root = fs.realpathSync(fixture);
  assert.deepEqual(namedStyleFiles(root, DEFAULT_SETTINGS, ["docs/guide.md", "./docs/guide.md"], root),
    ["docs/guide.md"]);
  const named = namedStyleFiles(root, DEFAULT_SETTINGS, ["guide.md", "bad.md"], path.join(root, "docs"));
  assert.deepEqual(named, ["docs/guide.md", "docs/bad.md"]);
  assert.equal(runMarkdownlint(root, temporary, named.slice(0, 1), false), 0);
  assert.equal(runMarkdownlint(root, temporary, named, false), 1);
  for (const name of ["AGENTS.md", "docs/missing.md", "../outside.md", path.join(root, "AGENTS.md")]) {
    assert.throws(() => namedStyleFiles(root, DEFAULT_SETTINGS, [name], root),
      (error) => error.message === `${name} is not a style-selected Markdown file of the inventory`);
  }
  const excluded = { ...DEFAULT_SETTINGS, declared: true, styleExclude: ["docs/bad.md"] };
  assert.throws(() => namedStyleFiles(root, excluded, ["docs/bad.md"], root),
    /^Error: docs\/bad\.md is not a style-selected/u);
  const tooMany = new Array(MAX_FILES_CEILING + 1).fill("x.md");
  assert.throws(() => namedStyleFiles(root, DEFAULT_SETTINGS, tooMany, root),
    /--only names 16385 files; maximum is 16384/u);
  assert.deepEqual(gateMode([]), { name: "gate", files: [] });
  assert.deepEqual(gateMode(["--self-test"]), { name: "--self-test", files: [] });
  assert.deepEqual(gateMode([ONLY_MODE, "docs/guide.md"]), { name: ONLY_MODE, files: ["docs/guide.md"] });
  for (const args of [[ONLY_MODE], ["--self-test", "x"], ["docs/guide.md"]]) {
    assert.throws(() => gateMode(args),
      /usage: node tools\/markdownlint\/verify\.mjs \[--self-test \| --only <file>\.\.\.\]/u);
  }
  process.stdout.write("--only fixtures: named style-selected files lint alone, every other name refused\n");
}

function runScratchRule(root, temporary, files, selfTest, emitDiagnostics = true) {
  const rule = path.join(temporary, "no-private-scratch-links.mjs");
  const args = selfTest ? [rule, "--self-test"] : [rule, root, path.join(temporary, "inventory.json")];
  if (!selfTest) {
    fs.writeFileSync(args[2], `${JSON.stringify(files)}\n`, { mode: 0o600 });
  }
  const result = command(process.execPath, args, { cwd: root, allowFailure: true });
  const budget = outputBudget();
  let overflow = false;
  if (emitDiagnostics && (selfTest || result.status !== 0)) {
    overflow ||= emitBounded(result.stdout, process.stdout, budget, "private-link");
    overflow ||= emitBounded(result.stderr, process.stderr, budget, "private-link");
  }
  return overflow ? 2 : result.status ?? 2;
}

// The installed markdownlint must be the version the copied lock pins and expose the synchronous
// entry the lint child imports. The lock is the only place the version is written, so a pin update
// needs no change here; npm ci has already checked the package's integrity against the same lock.
function markdownlintLibrary(lock, metadata) {
  const locked = lock?.packages?.["node_modules/markdownlint"]?.version;
  if (typeof locked !== "string" || locked === "") {
    fail("package-lock.json pins no markdownlint version");
  }
  if (metadata?.version !== locked || metadata?.exports?.["./sync"] !== MARKDOWNLINT_ENTRY) {
    fail(`installed markdownlint package does not match locked ${locked} library contract`);
  }
  return MARKDOWNLINT_ENTRY;
}

function markdownlintEntry(temporary) {
  const lock = JSON.parse(fs.readFileSync(path.join(temporary, "package-lock.json"), "utf8"));
  const packageDir = path.join(temporary, "node_modules", "markdownlint");
  const metadata = JSON.parse(fs.readFileSync(path.join(packageDir, "package.json"), "utf8"));
  return path.join(packageDir, markdownlintLibrary(lock, metadata));
}

function markdownlintLibrarySelfTest() {
  const lock = { packages: { "node_modules/markdownlint": { version: "1.2.3" } } };
  const installed = { version: "1.2.3", exports: { "./sync": MARKDOWNLINT_ENTRY } };
  assert.equal(markdownlintLibrary(lock, installed), MARKDOWNLINT_ENTRY);
  assert.throws(() => markdownlintLibrary(lock, { ...installed, version: "1.2.4" }),
    (error) => error.message === "installed markdownlint package does not match locked 1.2.3 library contract");
  assert.throws(() => markdownlintLibrary(lock, { ...installed, exports: { "./sync": "./other.mjs" } }),
    /does not match locked 1\.2\.3 library contract/u);
  assert.throws(() => markdownlintLibrary(lock, { version: "1.2.3" }), /does not match locked 1\.2\.3/u);
  for (const missing of [{}, { packages: {} }, { packages: { "node_modules/markdownlint": { version: "" } } }]) {
    assert.throws(() => markdownlintLibrary(missing, installed),
      (error) => error.message === "package-lock.json pins no markdownlint version");
  }
  process.stdout.write("markdownlint library fixtures: the lock's version passes, another version, " +
    "another entry or an unpinned lock fails\n");
}

// Negative: an installation whose markdownlint is not the locked version fails the style run with
// status 2 before any lint child starts, rather than reading as findings. Boundary: it fails so
// with no file to lint too. Positive: the locked installation passes with no file to lint.
function lintEntrySelfTest(temporary) {
  const fixture = path.join(temporary, "lint-entry-fixture");
  const install = path.join(temporary, "lint-entry-install");
  const lock = JSON.parse(fs.readFileSync(path.join(temporary, "package-lock.json"), "utf8"));
  const locked = lock.packages["node_modules/markdownlint"].version;
  writeFixtureFiles(install, {
    "package-lock.json": JSON.stringify(lock),
    "node_modules/markdownlint/package.json": JSON.stringify({
      version: `${locked}-other`,
      exports: { "./sync": MARKDOWNLINT_ENTRY },
    }),
  });
  writeFixtureFiles(fixture, { "docs/guide.md": "# Guide\n" });
  const expected = `installed markdownlint package does not match locked ${locked} library contract`;
  for (const files of [[], ["docs/guide.md"]]) {
    assert.throws(() => runMarkdownlint(fixture, install, files, false),
      (error) => error instanceof GateFailure && error.status === 2 && error.message === expected);
  }
  assert.equal(runMarkdownlint(fixture, temporary, [], false), 0);
  assert.equal(runMarkdownlint(fixture, temporary, ["docs/guide.md"], false), 0);
  process.stdout.write("lint entry fixtures: a markdownlint other than the locked one fails with status 2 " +
    "before any lint child, with or without files\n");
}

// lintConfiguration reads the rules from markdownlint-cli2.yaml: one bounded YAML mapping whose
// config mapping is the rule configuration. noProgress, a markdownlint-cli2 display option, is
// accepted and changes nothing; any other key is refused rather than silently ignored.
function lintConfiguration(yaml, text) {
  let document;
  try {
    document = yaml.load(text, {
      filename: LINT_CONFIG_FILE,
      maxAliases: MAX_MANIFEST_ALIASES,
      maxDepth: MAX_MANIFEST_DEPTH,
    });
  } catch (error) {
    fail(`${LINT_CONFIG_FILE} is not valid YAML: ${error.message}`);
  }
  if (!isMapping(document) || !isMapping(document.config)) {
    fail(`${LINT_CONFIG_FILE} must be a mapping with a config mapping`);
  }
  const unknown = Object.keys(document).find((key) => !LINT_CONFIG_KEYS.has(key));
  if (unknown !== undefined) {
    fail(`${LINT_CONFIG_FILE} has unsupported key ${JSON.stringify(unknown.slice(0, 64))}; the gate reads config`);
  }
  return document.config;
}

// configurationParsers returns the parsers markdownlint-cli2 0.23.3 gave the library for inline
// markdownlint-configure-file comments, in its order: JSONC, TOML, then YAML. They parse data
// only; the library tries each until one succeeds and ignores a comment none can parse.
function configurationParsers(temporary, yaml) {
  const jsonc = loadDependency(temporary, "jsonc-parser");
  const toml = loadDependency(temporary, "smol-toml");
  const parseJSONC = (text) => {
    const errors = [];
    const result = jsonc.parse(text, errors, { allowTrailingComma: true });
    if (errors.length > 0) {
      throw new Error(`Unable to parse JSONC content: ${errors.length} errors`);
    }
    return result;
  };
  return [parseJSONC, (text) => toml.parse(text),
    (text) => yaml.load(text, { maxAliases: MAX_MANIFEST_ALIASES, maxDepth: MAX_MANIFEST_DEPTH })];
}

// lintFinding formats one result as markdownlint-cli2's default formatter printed it, so a failure
// reads as before: file:line[:column] severity RULE/alias description [detail] [Context: "..."].
function lintFinding(result) {
  const column = result.errorRange?.[0] ? `:${result.errorRange[0]}` : "";
  const severity = result.severity ? ` ${result.severity}` : "";
  const detail = result.errorDetail ? ` [${result.errorDetail}]` : "";
  const context = result.errorContext ? ` [Context: "${result.errorContext}"]` : "";
  return `${result.fileName}:${result.lineNumber}${column}${severity} ${result.ruleNames.join("/")} ` +
    `${result.ruleDescription}${detail}${context}`;
}

// compareFindings orders results as markdownlint-cli2 did: by file, line and first rule name, then
// in the order the library reported them.
function compareFindings(left, right) {
  return left.fileName.localeCompare(right.fileName) || left.lineNumber - right.lineNumber ||
    left.ruleNames[0].localeCompare(right.ruleNames[0]) || left.order - right.order;
}

function readLintSource(relative) {
  const stat = fs.lstatSync(relative, { throwIfNoEntry: false });
  if (stat === undefined || stat.isSymbolicLink() || !stat.isFile()) {
    fail(`refusing to lint non-file Markdown source ${relative}`);
  }
  if (stat.size > MAX_FILE_BYTES_CEILING) {
    fail(`${relative} is ${stat.size} bytes; per-file ceiling is ${MAX_FILE_BYTES_CEILING}`);
  }
  return fs.readFileSync(relative, "utf8");
}

// lintChild is the child-process side of runMarkdownlint, started from the repository root. It
// hands the library each file's text as a string, so markdownlint opens no file and finds no
// configuration of its own: the rules are the locked config mapping and nothing in the repository.
// markdownlint-cli2, which the gate ran before, read .markdownlint-cli2.* and .markdownlint.*
// files from every directory down to a linted file and executed their .cjs and .mjs forms (#533);
// nothing here looks for them. One file is linted at a time, so the heap holds one file's parse.
async function lintChild(args) {
  if (args.length < 1) {
    fail(`usage: node tools/markdownlint/verify.mjs ${LINT_MODE} <install directory> <file>...`);
  }
  const [temporary, ...files] = args;
  if (files.length > MAX_FILES_CEILING) {
    fail(`lint batch has ${files.length} files; maximum is ${MAX_FILES_CEILING}`);
  }
  const { lint } = await import(pathToFileURL(markdownlintEntry(temporary)).href);
  const yaml = loadDependency(temporary, "js-yaml");
  const options = {
    config: lintConfiguration(yaml, fs.readFileSync(path.join(temporary, LINT_CONFIG_FILE), "utf8")),
    configParsers: configurationParsers(temporary, yaml),
    handleRuleFailures: true,
    noInlineConfig: false,
  };
  const findings = [];
  for (let index = 0; index < files.length && index < MAX_FILES_CEILING; index += 1) {
    const results = lint({ ...options, strings: { [files[index]]: readLintSource(files[index]) } });
    for (const result of results[files[index]] ?? []) {
      findings.push({ ...result, fileName: files[index], order: findings.length });
    }
  }
  findings.sort(compareFindings);
  process.stderr.write(findings.map((finding) => `${lintFinding(finding)}\n`).join(""));
  return findings.length === 0 ? 0 : 1;
}

function batches(files) {
  const result = [];
  let current = [];
  let bytes = 0;
  for (let index = 0; index < files.length && index < MAX_FILES_CEILING; index += 1) {
    const size = Buffer.byteLength(files[index]) + 1;
    if (current.length > 0 && bytes + size > MAX_COMMAND_BYTES) {
      result.push(current);
      current = [];
      bytes = 0;
    }
    current.push(files[index]);
    bytes += size;
  }
  if (current.length > 0) {
    result.push(current);
  }
  return result;
}

// counted names a count of a noun: "1 file", "2 files".
function counted(count, noun) {
  return `${count} ${noun}${count === 1 ? "" : "s"}`;
}

// lintBudgetReport describes a lint batch whose child was stopped: its position, file count and
// bytes, the limit it ran into, its largest files as suspects, and the command that lints a
// suspect alone. For its own budget the report names the setting that raises it while it is below
// its ceiling; for the gate deadline (deadline true) it says that no setting moves it, since the
// earlier steps and batches took the rest of the run's time. Size is no proof: in one adopter
// repository the markdownlint library's GFM autolink-literal extension took minutes over one long
// paragraph holding an unbalanced `[` (#784), and a smaller file can hold such a paragraph too.
function lintBudgetReport(root, batch, position, seconds, deadline = false) {
  const sized = [];
  let total = 0;
  for (let index = 0; index < batch.length && index < MAX_FILES_CEILING; index += 1) {
    const size = fs.lstatSync(path.join(root, batch[index]), { throwIfNoEntry: false })?.size ?? 0;
    sized.push({ file: batch[index], size });
    total += size;
  }
  sized.sort((left, right) => right.size - left.size || left.file.localeCompare(right.file));
  const suspects = sized.slice(0, MAX_BUDGET_SUSPECTS);
  const alone = batch.length > 1 ?
    `\nlint a suspect alone to time it: node tools/markdownlint/verify.mjs ${ONLY_MODE} ${suspects[0].file}` : "";
  const limit = deadline ?
    `ran past the gate's ${GATE_DEADLINE_SECONDS} s deadline before its ${seconds} s budget ran out${DEADLINE_NOTE}` :
    `exceeded its ${seconds} s budget${boundHint("lint_timeout_seconds", seconds, LINT_TIMEOUT_SECONDS_CEILING)}`;
  return `lint batch ${position} (${counted(batch.length, "file")}, ${total} bytes) ${limit}\n` +
    "suspects, the batch's largest files (size is no proof):\n" +
    suspects.map(({ file, size }) => `  ${file} (${size} bytes)`).join("\n") + alone;
}

// runMarkdownlint lints the style-selected files in child processes of this script (lintChild),
// one batch of paths each, as the gate ran markdownlint-cli2 before: every batch gets a fresh
// heap, the declared lint budget cut to the time left before the gate deadline, and a bounded
// capture, and its diagnostics share one output budget. A child stopped by either limit fails the
// gate with status 2 and lintBudgetReport. It checks the
// installed library against the lock before the first child, even with no file to lint, so a
// mismatch fails the gate with status 2 instead of reading as findings. child replaces the
// command and leading arguments of every lint child; only lintBudgetSelfTest passes it.
function runMarkdownlint(root, temporary, files, emitDiagnostics = true, settings = DEFAULT_SETTINGS, child = null) {
  markdownlintEntry(temporary);
  const script = fileURLToPath(import.meta.url);
  const [file, ...leading] = child ?? [process.execPath, script, LINT_MODE, temporary];
  const all = batches(files);
  let failed = false;
  let overflow = false;
  const budget = outputBudget();
  for (let index = 0; index < all.length && index < MAX_FILES_CEILING; index += 1) {
    const result = command(file, [...leading, ...all[index]], {
      cwd: root,
      allowFailure: true,
      timeout: settings.lintTimeoutSeconds * 1_000,
      timeoutReport: (deadline) => lintBudgetReport(root, all[index], `${index + 1} of ${all.length}`,
        settings.lintTimeoutSeconds, deadline),
    });
    failed ||= result.status !== 0;
    if (emitDiagnostics && result.status !== 0) {
      overflow ||= emitBounded(result.stdout, process.stdout, budget, "markdownlint");
      overflow ||= emitBounded(result.stderr, process.stderr, budget, "markdownlint");
    }
  }
  return overflow ? 2 : failed ? 1 : 0;
}

// namedStyleFiles resolves the files `--only` names, relative to cwd, to repository paths. Each
// must be a style-selected file of the inventory, so --only lints nothing the gate would not; a
// name given twice is linted once.
function namedStyleFiles(root, settings, named, cwd) {
  if (named.length > MAX_FILES_CEILING) {
    fail(`${ONLY_MODE} names ${named.length} files; maximum is ${MAX_FILES_CEILING}`);
  }
  const styled = new Set(styleSelection(inventory(root, settings), settings, process.platform).styled);
  const files = new Set();
  for (let index = 0; index < named.length && index < MAX_FILES_CEILING; index += 1) {
    const relative = path.relative(root, path.resolve(cwd, named[index])).split(path.sep).join("/");
    if (!styled.has(relative)) {
      fail(`${named[index]} is not a style-selected Markdown file of the inventory`);
    }
    files.add(relative);
  }
  return [...files];
}

// gateMode reads the command line: no argument runs the gate, --self-test replays its fixtures,
// and --only <file>... lints the named files alone.
function gateMode(args) {
  if (args.length === 0 || (args.length === 1 && args[0] === "--self-test")) {
    return { name: args[0] ?? "gate", files: [] };
  }
  if (args.length > 1 && args[0] === ONLY_MODE) {
    return { name: ONLY_MODE, files: args.slice(1) };
  }
  return fail(`usage: node tools/markdownlint/verify.mjs [--self-test | ${ONLY_MODE} <file>...]`);
}

function runGate(root, temporary, settings) {
  const scratchFiles = inventory(root, settings);
  const selection = styleSelection(scratchFiles, settings, process.platform);
  const styleFiles = selection.styled;
  reportSettings(settings, selection);
  const scratchStatus = runScratchRule(root, temporary, scratchFiles, false);
  const lintStatus = runMarkdownlint(root, temporary, styleFiles, true, settings);
  process.stdout.write(summaryLine(settings, selection, scratchFiles.length));
  return scratchStatus === 0 && lintStatus === 0 ? 0 : scratchStatus > 1 || lintStatus > 1 ? 2 : 1;
}

function runOnly(root, temporary, settings, named) {
  const files = namedStyleFiles(root, settings, named, process.cwd());
  const started = Date.now();
  const status = runMarkdownlint(root, temporary, files, true, settings);
  process.stdout.write(`markdown-governance: styled ${counted(files.length, "named Markdown file")} in ` +
    `${Date.now() - started} ms (budget ${settings.lintTimeoutSeconds} s per lint child, ` +
    `${GATE_DEADLINE_SECONDS} s for the run)\n`);
  return status;
}

function runSelfTest(toolDir, temporary) {
  npmInvocationSelfTest(temporary);
  markdownlintLibrarySelfTest();
  styleExclusionGrammarSelfTest();
  install(toolDir, temporary);
  inventorySelfTest(temporary);
  settingsSelfTest(temporary);
  lintConfigurationSelfTest(temporary);
  lintEntrySelfTest(temporary);
  hermeticConfigSelfTest(temporary);
  lintOutputSelfTest(temporary);
  mathSelfTest(temporary);
  lintMemorySelfTest(temporary);
  styleExclusionSelfTest(temporary);
  raisedBoundSelfTest(temporary);
  lintBudgetSelfTest(temporary);
  gateDeadlineSelfTest(temporary);
  onlyModeSelfTest(temporary);
  return runScratchRule(process.cwd(), temporary, [], true);
}

function main() {
  const mode = gateMode(process.argv.slice(2));
  const toolDir = path.dirname(fileURLToPath(import.meta.url));
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "praetor-markdownlint-"));
  try {
    if (mode.name === "--self-test") {
      process.exitCode = runSelfTest(toolDir, temporary);
      return;
    }
    process.exitCode = withGateDeadline(GATE_DEADLINE_SECONDS * 1_000, () => {
      install(toolDir, temporary);
      const root = repositoryRoot();
      const settings = repositorySettings(root, loadDependency(temporary, "js-yaml"));
      return mode.name === ONLY_MODE ? runOnly(root, temporary, settings, mode.files) :
        runGate(root, temporary, settings);
    });
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

// entry runs the gate, or, given LINT_MODE, the lint child runMarkdownlint starts.
async function entry() {
  if (process.argv[2] === LINT_MODE) {
    process.exitCode = await lintChild(process.argv.slice(3));
    return;
  }
  main();
}

entry().catch((error) => {
  process.stderr.write(`markdown-governance: ${error.message}\n`);
  process.exitCode = error instanceof GateFailure ? error.status : 2;
});
