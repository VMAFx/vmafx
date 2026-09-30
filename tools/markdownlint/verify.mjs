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
import { fileURLToPath } from "node:url";

// Inventory bounds. documentation.max_files and documentation.max_file_bytes in .standards.yaml
// raise the file-count and per-file bounds from their defaults up to their ceilings; nothing
// raises the aggregate bound. internal/config/documentation.go validates the same ranges for
// audit, and TestDocumentationSettingsMirrorConfig keeps the two in step. The per-file ceiling
// is a memory bound: markdownlint-cli2 lints a 4 MiB file of linked bullets in a 1 GiB heap,
// while at 8 MiB it needed more than 1.5 GiB, close to Node's default heap on a 7 GB runner.
const DEFAULT_MAX_FILES = 4_096;
const MAX_FILES_CEILING = 16_384;
const DEFAULT_MAX_FILE_BYTES = 1_048_576;
const MAX_FILE_BYTES_CEILING = 4_194_304;
const MAX_TOTAL_BYTES = 67_108_864;
const MANIFEST_FILE = ".standards.yaml";
const MAX_MANIFEST_BYTES = 1_048_576;
const MAX_MANIFEST_DEPTH = 64;
const MAX_MANIFEST_ALIASES = 1_024;
const MAX_STYLE_EXCLUSIONS = 64;
const MAX_STYLE_EXCLUSION_BYTES = 256;
const DOCUMENTATION_KEYS = new Set(["max_files", "max_file_bytes", "style_exclude"]);
// A declared exclusion is a micromatch glob over the repository-relative path; dot lets
// `docs/**` reach dot-directories below docs/ as well.
const GLOB_OPTIONS = Object.freeze({ dot: true });
const STYLE_TREE = "style-tree";
const DEFAULT_SETTINGS = Object.freeze({
  declared: false,
  maxFiles: DEFAULT_MAX_FILES,
  maxFileBytes: DEFAULT_MAX_FILE_BYTES,
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
const MARKDOWN_SUFFIXES = [".md", ".markdown", ".mdx", ".md.tmpl", ".markdown.tmpl", ".mdx.tmpl"];
const SCRATCH_ROOTS = new Set([".workingdir", ".workingdir2"]);
const FILESYSTEM_SYMLINK_UNAVAILABLE = new Set(["EPERM", "EACCES", "ENOSYS"]);
const TOOL_FILES = [
  "package.json",
  "package-lock.json",
  "markdownlint-cli2.yaml",
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

function command(commandName, args, options = {}) {
  const timeout = options.timeout ?? DEFAULT_COMMAND_TIMEOUT_MS;
  const result = spawnSync(commandName, args, {
    cwd: options.cwd,
    encoding: options.binary ? null : "utf8",
    env: { ...process.env, ...options.env },
    input: options.input,
    maxBuffer: options.maxBuffer ?? MAX_CAPTURE_BYTES,
    stdio: "pipe",
    timeout,
    windowsHide: true,
  });
  if (result.error) {
    if (result.error.code === "ETIMEDOUT") {
      fail(`${commandName} exceeded ${timeout} ms`);
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

function boundedSetting(value, key, defaultValue, ceiling) {
  if (value === undefined) {
    return defaultValue;
  }
  if (!Number.isSafeInteger(value) || value < defaultValue || value > ceiling) {
    const got = typeof value === "number" || value === null ? String(value) : `a ${typeof value}`;
    fail(`${MANIFEST_FILE} documentation.${key} must be an integer from ${defaultValue} to ${ceiling}; got ${got}`);
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
    const problem = styleExclusionProblem(value[index]);
    if (problem !== null) {
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
function styleSelection(files, settings, micromatch) {
  const matchers = settings.styleExclude.map((pattern) => micromatch.matcher(pattern, GLOB_OPTIONS));
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
    `${settings.maxFileBytes} bytes per file (defaults ${DEFAULT_MAX_FILES}, ${DEFAULT_MAX_FILE_BYTES}; ` +
    `ceilings ${MAX_FILES_CEILING}, ${MAX_FILE_BYTES_CEILING})\n`);
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
    if (Buffer.byteLength(relative) > MAX_PATH_BYTES || /[\0\r\n]/u.test(relative)) {
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
  const allFiles = inventory(fs.realpathSync(fixture));
  assert.deepEqual(allFiles, ["AGENTS.md", "README.md", "docs/guide.md", "docs/page.mdx",
    "notes.markdown", "templates/Card.mdx.tmpl", "templates/README.md.tmpl",
    "tools/figures/third_party/interfig/VENDOR.md", "tools/figures/third_party/interfig/upstream/README.md",
    "vendor/README.md"]);
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
    { declared: true, maxFiles: 8_192, maxFileBytes: DEFAULT_MAX_FILE_BYTES, styleExclude: ["changelog.d/**"] });
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

// Negative: markdownlint configuration files anywhere in the repository, loosening (rules off,
// everything ignored, rules replaced by code) or tightening (an 80-column MD013 against the
// locked MD013: false), change nothing, and a configuration module is never executed. The
// control run from the repository root shows the same files do take effect when markdownlint-cli2
// is left to discover them, which is what the staged copy prevents.
function hermeticConfigSelfTest(temporary) {
  const fixture = path.join(temporary, "hermetic-fixture");
  const marker = path.join(temporary, "adopter-configuration-ran");
  const cli = markdownlintEntry(temporary);
  const config = path.join(temporary, "markdownlint-cli2.yaml");
  writeFixtureFiles(fixture, {
    ".markdownlint.json": "{ \"default\": false }\n",
    ".markdownlint-cli2.jsonc": "{ \"ignores\": [\"**\"], \"config\": { \"default\": false } }\n",
    "docs/.markdownlint.yaml": "default: false\n",
    "docs/.markdownlint-cli2.mjs": `import fs from "node:fs";\nfs.writeFileSync(${JSON.stringify(marker)}, "ran");\n` +
      "export default { config: { default: false } };\n",
    "README.md": "#Malformed root\n",
    "docs/guide.md": "#Malformed guide\n",
  });
  const files = ["README.md", "docs/guide.md"];
  assert.equal(runMarkdownlint(fixture, temporary, files, false), 1);
  assert.equal(fs.existsSync(marker), false);
  const tree = stageStyleTree(fixture, temporary, files);
  const staged = command(process.execPath, [cli, "--config", config, "--no-globs", ":docs/guide.md"], {
    cwd: tree,
    allowFailure: true,
  });
  assert.match(`${staged.stdout}${staged.stderr}`, /^docs\/guide\.md:1(?::\d+)? error MD018/mu);
  const discovered = command(process.execPath, [cli, "--config", config, "--no-globs", ":README.md", ":docs/guide.md"], {
    cwd: fixture,
    allowFailure: true,
  });
  assert.equal(discovered.status, 0, "control: markdownlint-cli2 no longer discovers repository configuration");
  assert.equal(fs.existsSync(marker), true, "control: markdownlint-cli2 no longer imports a configuration module");
  const longLine = `# Guide\n\n${"word ".repeat(40).trim()}\n`;
  writeFixtureFiles(fixture, {
    ".markdownlint.json": "{ \"MD013\": { \"line_length\": 80 } }\n",
    ".markdownlint-cli2.jsonc": "{ \"config\": { \"MD013\": { \"line_length\": 80 } } }\n",
    "docs/.markdownlint.yaml": "MD013:\n  line_length: 80\n",
    "docs/.markdownlint-cli2.mjs": "export default { config: { MD013: { line_length: 80 } } };\n",
    "README.md": longLine,
    "docs/guide.md": longLine,
  });
  assert.equal(runMarkdownlint(fixture, temporary, files, false), 0);
  process.stdout.write("hermetic configuration fixtures: repository markdownlint files ignored, never executed\n");
}

// Positive: declared globs remove fragments, generated indexes and fixtures from the style run
// and report what each removed, while unmatched and dot-directory globs behave as globs. Negative:
// a declaration that removes every styled file fails the gate, and an excluded file still fails
// the private-link rule. Boundary: all but one file excluded passes, and a repository whose only
// Markdown is built-in excluded has nothing for a declaration to empty.
function styleExclusionSelfTest(temporary) {
  const micromatch = loadDependency(temporary, "micromatch");
  const files = ["AGENTS.md", "README.md", "changelog.d/fixed/fragment.md", "docs/adr/_index_fragments/0001.md",
    "docs/adr/_index_fragments/nested/0002.md", "docs/guide.md", "pkg/bench/testdata/golden.md"];
  const declared = (styleExclude) => ({ ...DEFAULT_SETTINGS, declared: true, styleExclude });
  const selection = styleSelection(files,
    declared(["changelog.d/**", "docs/adr/_index_fragments/*.md", "**/testdata/**", "unused/**"]), micromatch);
  assert.deepEqual(selection, {
    styled: ["README.md", "docs/adr/_index_fragments/nested/0002.md", "docs/guide.md"],
    excluded: 3,
    counts: [1, 1, 1, 0],
  });
  assert.deepEqual(styleSelection(files, DEFAULT_SETTINGS, micromatch).styled, files.filter(isStyleSelected));
  assert.deepEqual(styleSelection([".github/ISSUE_TEMPLATE/bug.md", "README.md"], declared([".github/**"]),
    micromatch).styled, ["README.md"]);
  assert.throws(() => styleSelection(files, declared(["README.md", "docs/**", "changelog.d/**", "pkg/**"]), micromatch),
    /documentation\.style_exclude excludes all 6 style-selected Markdown files/u);
  assert.deepEqual(styleSelection(files, declared(["docs/**", "changelog.d/**", "pkg/**"]), micromatch).styled,
    ["README.md"]);
  assert.deepEqual(styleSelection(["AGENTS.md"], declared(["docs/**"]), micromatch).styled, []);
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
  const fixtureSelection = styleSelection(all, settings, micromatch);
  assert.deepEqual(fixtureSelection.styled, ["README.md"]);
  assert.equal(summaryLine(settings, fixtureSelection, all.length), "markdown-governance: styled 1 public Markdown " +
    "files (1 excluded by documentation.style_exclude); checked 2 tracked/non-ignored Markdown files for private links\n");
  assert.equal(runMarkdownlint(fixture, temporary, fixtureSelection.styled, false), 0);
  assert.equal(runMarkdownlint(fixture, temporary, all.filter(isStyleSelected), false), 1);
  assert.equal(runScratchRule(fixture, temporary, all, false, false), 1);
  process.stdout.write("style exclusion fixtures: declared globs, counts, never everything, private links kept\n");
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

function markdownlintEntry(temporary) {
  const metadata = JSON.parse(fs.readFileSync(path.join(temporary, "node_modules", "markdownlint-cli2", "package.json"), "utf8"));
  if (metadata.version !== "0.23.2" || metadata.bin?.["markdownlint-cli2"] !== "./markdownlint-cli2-bin.mjs") {
    fail("installed markdownlint-cli2 package does not match locked 0.23.2 binary contract");
  }
  return path.join(temporary, "node_modules", "markdownlint-cli2", metadata.bin["markdownlint-cli2"]);
}

function batches(files) {
  const result = [];
  let current = [];
  let bytes = 0;
  for (let index = 0; index < files.length && index < MAX_FILES_CEILING; index += 1) {
    const literal = `:${files[index]}`;
    if (current.length > 0 && bytes + Buffer.byteLength(literal) + 1 > MAX_COMMAND_BYTES) {
      result.push(current);
      current = [];
      bytes = 0;
    }
    current.push(literal);
    bytes += Buffer.byteLength(literal) + 1;
  }
  if (current.length > 0) {
    result.push(current);
  }
  return result;
}

// markdownlint-cli2 0.23.2 has no option that turns configuration discovery off. Beside the
// --config file it reads .markdownlint-cli2.{jsonc,yaml,cjs,mjs} and
// .markdownlint.{jsonc,json,yaml,yml,cjs,mjs} from its working directory and from every directory
// between it and a linted file (getAndProcessDirInfo and enumerateParents in
// markdownlint-cli2.mjs). A .markdownlint.* file found there replaces the --config rules
// outright, and the .cjs and .mjs forms execute repository code. The gate therefore lints a copy:
// stageStyleTree copies only the selected Markdown files, at their repository-relative paths,
// into an empty directory the linter runs from. No configuration file name ends in a Markdown
// suffix, so the copy holds none, and diagnostics keep the repository-relative paths.
function stageStyleTree(root, temporary, files) {
  const tree = path.join(temporary, STYLE_TREE);
  fs.rmSync(tree, { recursive: true, force: true });
  fs.mkdirSync(tree, { recursive: true });
  for (let index = 0; index < files.length && index < MAX_FILES_CEILING; index += 1) {
    const target = path.join(tree, files[index]);
    if (target === tree || escapesDirectory(tree, target)) {
      fail(`refusing to stage Markdown path outside the style tree: ${files[index]}`);
    }
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.copyFileSync(path.join(root, files[index]), target);
  }
  return tree;
}

function runMarkdownlint(root, temporary, files, emitDiagnostics = true) {
  const cli = markdownlintEntry(temporary);
  const config = path.join(temporary, "markdownlint-cli2.yaml");
  const tree = stageStyleTree(root, temporary, files);
  let failed = false;
  let overflow = false;
  const budget = outputBudget();
  for (const batch of batches(files)) {
    const result = command(process.execPath, [cli, "--config", config, "--no-globs", ...batch], {
      cwd: tree,
      allowFailure: true,
    });
    failed ||= result.status !== 0;
    if (emitDiagnostics && result.status !== 0) {
      overflow ||= emitBounded(result.stdout, process.stdout, budget, "markdownlint");
      overflow ||= emitBounded(result.stderr, process.stderr, budget, "markdownlint");
    }
  }
  return overflow ? 2 : failed ? 1 : 0;
}

function main() {
  const selfTest = process.argv.length === 3 && process.argv[2] === "--self-test";
  if (!selfTest && process.argv.length !== 2) {
    fail("usage: node tools/markdownlint/verify.mjs [--self-test]");
  }
  const toolDir = path.dirname(fileURLToPath(import.meta.url));
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "praetor-markdownlint-"));
  try {
    if (selfTest) {
      npmInvocationSelfTest(temporary);
    }
    install(toolDir, temporary);
    if (selfTest) {
      inventorySelfTest(temporary);
      settingsSelfTest(temporary);
      hermeticConfigSelfTest(temporary);
      styleExclusionSelfTest(temporary);
      raisedBoundSelfTest(temporary);
      process.exitCode = runScratchRule(process.cwd(), temporary, [], true);
      return;
    }
    const root = repositoryRoot();
    const settings = repositorySettings(root, loadDependency(temporary, "js-yaml"));
    const scratchFiles = inventory(root, settings);
    const selection = styleSelection(scratchFiles, settings, loadDependency(temporary, "micromatch"));
    const styleFiles = selection.styled;
    reportSettings(settings, selection);
    const scratchStatus = runScratchRule(root, temporary, scratchFiles, false);
    const lintStatus = runMarkdownlint(root, temporary, styleFiles);
    process.stdout.write(summaryLine(settings, selection, scratchFiles.length));
    process.exitCode = scratchStatus === 0 && lintStatus === 0 ? 0 :
      scratchStatus > 1 || lintStatus > 1 ? 2 : 1;
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

try {
  main();
} catch (error) {
  process.stderr.write(`markdown-governance: ${error.message}\n`);
  process.exitCode = error instanceof GateFailure ? error.status : 2;
}
