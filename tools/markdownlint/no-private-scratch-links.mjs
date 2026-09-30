// SPDX-FileCopyrightText: 2026 lusoris <lusoris@pm.me>
//
// SPDX-License-Identifier: EUPL-1.2

import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { isMainThread, parentPort, Worker, workerData } from "node:worker_threads";

import { parse, postprocess, preprocess } from "micromark";
import { mdxjs } from "micromark-extension-mdxjs";
import { parseFragment } from "parse5";

// MAX_EVENTS is the per-file floor of the Markdown parse bound. A file may parse to one event
// per byte above it (parseEventLimit): verify.mjs bounds every file's size, 1 MiB by default and
// raisable in .standards.yaml, and a larger document would otherwise fail on its size alone.
// Ordinary documentation parses to 0.2-0.3 events per byte.
const MAX_EVENTS = 262_144;
const MAX_EVENTS_PER_BYTE = 1;
const MAX_HTML_NODES = 65_536;
const MAX_HTML_DEPTH = 8;
const MAX_SRCSET_CANDIDATES = 4_096;
const MAX_CSS_TOKENS = 65_536;
const MAX_CSS_DEPTH = 64;
const MAX_ESTREE_NODES = 65_536;
const MAX_ESTREE_DEPTH = 128;
const MAX_ESTREE_PROPERTIES = 32;
const SYNTHETIC_ORIGIN = "https://praetor-markdown.invalid";
const SYNTHETIC_REPOSITORY_PATH = "/__praetor_repository__/";
const DESTINATION_TYPES = new Set([
  "resourceDestinationString",
  "definitionDestinationString",
  "autolinkProtocol",
]);
const HTML_TYPES = new Set(["htmlFlow", "htmlText"]);
const MDX_JSX_TAG_TYPES = new Set(["mdxJsxFlowTag", "mdxJsxTextTag"]);
const MDX_JSX_ATTRIBUTE_TYPES = new Set([
  "mdxJsxFlowTagAttribute",
  "mdxJsxTextTagAttribute",
]);
const MDX_JSX_SPREAD_TYPES = new Set([
  "mdxJsxFlowTagExpressionAttribute",
  "mdxJsxTextTagExpressionAttribute",
]);
const MDX_STATIC_SOURCE_TYPES = new Set([
  "ImportDeclaration",
  "ExportNamedDeclaration",
  "ExportAllDeclaration",
]);
const MDX_EXPRESSION_URL_PROPERTIES = new Set(["path", "to", "url"]);
const HTML_URL_ATTRIBUTES = new Set([
  "action",
  "archive",
  "background",
  "cite",
  "codebase",
  "data",
  "formaction",
  "href",
  "icon",
  "itemid",
  "longdesc",
  "manifest",
  "ping",
  "poster",
  "profile",
  "src",
  "usemap",
  "xlink:href",
  "xlinkhref",
]);
const HTML_SRCSET_ATTRIBUTES = new Set(["imagesrcset", "srcset"]);
const HTML_SPACE_SEPARATED_URL_ATTRIBUTES = new Set(["archive", "ping"]);
const HTML_CSS_URL_ATTRIBUTES = new Set([
  "clip-path",
  "clippath",
  "cursor",
  "fill",
  "filter",
  "marker",
  "marker-end",
  "marker-mid",
  "marker-start",
  "markerend",
  "markermid",
  "markerstart",
  "mask",
  "stroke",
]);
const SCRATCH_ROOTS = new Set([".workingdir", ".workingdir2"]);
// The hard ceilings of documentation.max_files and documentation.max_file_bytes. verify.mjs
// enforces the repository's declared bounds, which never exceed these, before it writes the
// inventory this rule reads.
const MAX_FILES = 16_384;
const MAX_FILE_BYTES = 4_194_304;
// The event bound above is checked only after micromark has built a file's whole event array, at
// 350 to 1,100 heap bytes per event, and dense Markdown reaches four events per byte: a 1 MiB
// list of one-word items needs more than 3 GB. Every file is therefore scanned in one worker
// thread whose V8 old generation is capped at PARSE_HEAP_BUDGET_MB. A file that exhausts it ends
// the worker, not the process, and the rule fails naming the file and the budget. The budget is
// twice what a MAX_FILE_BYTES file of linked, code-spanned bullets needs (0.4 events per byte,
// twice the density of this repository's documentation), and the self-test scans that file.
const PARSE_HEAP_BUDGET_MB = 1_536;
// Node ends a worker at its heap limit by granting 16 MB more and stopping it. With V8's default
// young generation a single scavenge could promote more than that, and V8 then aborted the whole
// process instead (17 of 40 runs of the self-test's 64 MiB list fixture). A 16 MiB young
// generation keeps promotions below the grant: no abort in 130 runs of that fixture or in 32 runs
// of dense 4 MiB files at the production budget. Should V8 still abort, verify.mjs fails the gate
// on the signal.
const SCAN_YOUNG_GENERATION_MB = 16;
// Parse time is superlinear for some inline constructs (a run of `*a*` doubles in size and takes
// four times as long), so each file's scan also has a deadline.
const SCAN_DEADLINE_MS = 60_000;
const SCAN_BOUNDS = Object.freeze({ heapBudgetMb: PARSE_HEAP_BUDGET_MB, deadlineMs: SCAN_DEADLINE_MS });
const SCAN_WORKER_ROLE = "praetor-private-scratch-link-scan";
const MAX_FINDINGS = 64;
const MAX_DIAGNOSTIC_FIELD_CHARS = 256;
const MAX_SOURCE_PATH_BYTES = 4_096;

function markdownDestinationString(value, markdownEscapes) {
  const decoded = markdownEscapes ? value.replace(/\\([!-/:-@[-`{-~])/gu, "$1") : value;
  const safe = decoded.replaceAll("<", "&lt;").replaceAll(">", "&gt;");
  const fragment = parseFragment(`<span>${safe}</span>`, { scriptingEnabled: false });
  return fragment.childNodes?.[0]?.childNodes?.map((node) => node.value ?? "").join("") ?? decoded;
}

function preprocessURLInput(value) {
  let start = 0;
  let end = value.length;
  while (start < end && value.charCodeAt(start) <= 0x20) {
    start += 1;
  }
  while (end > start && value.charCodeAt(end - 1) <= 0x20) {
    end -= 1;
  }
  let result = "";
  for (let position = start; position < end; position += 1) {
    const code = value.charCodeAt(position);
    if (code !== 0x09 && code !== 0x0a && code !== 0x0d) {
      result += value[position];
    }
  }
  return result;
}

function decodeASCIITriplets(value) {
  let result = "";
  for (let position = 0; position < value.length; position += 1) {
    if (value[position] === "%" && position + 2 < value.length &&
      /^[0-9A-Fa-f]{2}$/u.test(value.slice(position + 1, position + 3))) {
      const code = Number.parseInt(value.slice(position + 1, position + 3), 16);
      if (code <= 0x7f) {
        result += String.fromCharCode(code);
        position += 2;
        continue;
      }
    }
    result += value[position];
  }
  return result;
}

function sourceURL(source) {
  const portable = source.replaceAll("\\", "/");
  const segments = portable.split("/").filter(Boolean).map((segment) => encodeURIComponent(segment));
  return new URL(SYNTHETIC_REPOSITORY_PATH + segments.join("/"), SYNTHETIC_ORIGIN);
}

function scratchResolution(pathname, anywhere) {
  const portable = path.posix.normalize(decodeASCIITriplets(pathname).replaceAll("\\", "/"));
  const parts = portable.split("/").filter(Boolean);
  const candidates = anywhere ? parts : parts.slice(0, 1);
  const root = candidates.map((part) => part.toLowerCase()).find((part) => SCRATCH_ROOTS.has(part));
  if (root === undefined) {
    return null;
  }
  return { root, resolved: portable.replace(/^\/+/, "") };
}

function resolveURLDestination(source, value) {
  let candidate = preprocessURLInput(value);
  if (candidate === "") {
    return null;
  }
  if (candidate.startsWith("\\\\")) {
    candidate = `file:${candidate.replaceAll("\\", "/")}`;
  } else if (/^[A-Za-z]:[\\/]/u.test(candidate)) {
    candidate = `file:///${candidate.replaceAll("\\", "/")}`;
  }
  let resolved;
  try {
    resolved = new URL(candidate, sourceURL(source));
  } catch {
    return null;
  }
  if (resolved.protocol === "file:") {
    return scratchResolution(resolved.pathname, true);
  }
  if (resolved.origin !== SYNTHETIC_ORIGIN) {
    return null;
  }
  const pathname = decodeASCIITriplets(resolved.pathname).replaceAll("\\", "/");
  if (pathname.startsWith(SYNTHETIC_REPOSITORY_PATH)) {
    return scratchResolution(pathname.slice(SYNTHETIC_REPOSITORY_PATH.length), false);
  }
  return candidate.startsWith("/") ? scratchResolution(pathname, true) : null;
}

function forbiddenResolution(source, destination, context) {
  const candidates = context === "markdown"
    ? [markdownDestinationString(destination, true), markdownDestinationString(destination, false)]
    : [destination];
  for (let index = 0; index < candidates.length; index += 1) {
    const resolution = resolveURLDestination(source, candidates[index]);
    if (resolution !== null) {
      return resolution;
    }
    if (context === "snippet" || context === "mdx-property") {
      const rootResolution = resolveURLDestination("README.md", candidates[index]);
      if (rootResolution !== null) {
        return rootResolution;
      }
    }
  }
  return null;
}

function isHTMLSpace(character) {
  return character === "\t" || character === "\n" || character === "\f" ||
    character === "\r" || character === " ";
}

function parserBounds(overrides = {}) {
  const bounds = {
    maxHTMLNodes: overrides.maxHTMLNodes ?? MAX_HTML_NODES,
    maxHTMLDepth: overrides.maxHTMLDepth ?? MAX_HTML_DEPTH,
    maxCSSTokens: overrides.maxCSSTokens ?? MAX_CSS_TOKENS,
    maxCSSDepth: overrides.maxCSSDepth ?? MAX_CSS_DEPTH,
  };
  const names = ["maxHTMLNodes", "maxHTMLDepth", "maxCSSTokens", "maxCSSDepth"];
  for (let index = 0; index < names.length && index < 4; index += 1) {
    const name = names[index];
    const value = bounds[name];
    const minimum = name.endsWith("Depth") ? 0 : 1;
    if (!Number.isInteger(value) || value < minimum) {
      throw new Error(`${name} must be an integer at least ${minimum}`);
    }
  }
  return bounds;
}

function createHTMLState(overrides = {}) {
  return { bounds: parserBounds(overrides), nodes: 0, cssTokens: 0 };
}

function spendCSSToken(state) {
  if (state.cssTokens >= state.bounds.maxCSSTokens) {
    throw new Error(`embedded CSS exceeds ${state.bounds.maxCSSTokens} tokens`);
  }
  state.cssTokens += 1;
}

function isCSSHex(character) {
  return character !== undefined && /[0-9A-Fa-f]/u.test(character);
}

function consumeCSSEscape(value, position) {
  let cursor = position + 1;
  if (cursor >= value.length) {
    return { decoded: "�", position: cursor };
  }
  if (value[cursor] === "\n" || value[cursor] === "\f") {
    return { decoded: "", position: cursor + 1 };
  }
  if (value[cursor] === "\r") {
    cursor += value[cursor + 1] === "\n" ? 2 : 1;
    return { decoded: "", position: cursor };
  }
  if (!isCSSHex(value[cursor])) {
    return { decoded: value[cursor], position: cursor + 1 };
  }
  const start = cursor;
  while (cursor < value.length && cursor - start < 6 && isCSSHex(value[cursor])) {
    cursor += 1;
  }
  const code = Number.parseInt(value.slice(start, cursor), 16);
  if (isHTMLSpace(value[cursor])) {
    cursor += 1;
  }
  const decoded = code === 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)
    ? "�" : String.fromCodePoint(code);
  return { decoded, position: cursor };
}

function isCSSName(character) {
  if (character === undefined) {
    return false;
  }
  return /[A-Za-z0-9_-]/u.test(character) || character.codePointAt(0) >= 0x80;
}

function consumeCSSName(value, position) {
  let decoded = "";
  let cursor = position;
  while (cursor < value.length) {
    if (value[cursor] === "\\") {
      const escaped = consumeCSSEscape(value, cursor);
      decoded += escaped.decoded;
      cursor = escaped.position;
    } else if (isCSSName(value[cursor])) {
      decoded += value[cursor];
      cursor += 1;
    } else {
      break;
    }
  }
  return { decoded, position: cursor };
}

function consumeCSSComment(value, position) {
  let cursor = position + 2;
  while (cursor < value.length && !(value[cursor] === "*" && value[cursor + 1] === "/")) {
    cursor += 1;
  }
  return cursor < value.length ? cursor + 2 : cursor;
}

function consumeCSSString(value, position) {
  const quote = value[position];
  let cursor = position + 1;
  let decoded = "";
  while (cursor < value.length && value[cursor] !== quote) {
    if (value[cursor] === "\n" || value[cursor] === "\r" || value[cursor] === "\f") {
      return { decoded, position: cursor, valid: false };
    }
    if (value[cursor] === "\\") {
      const escaped = consumeCSSEscape(value, cursor);
      decoded += escaped.decoded;
      cursor = escaped.position;
    } else {
      decoded += value[cursor];
      cursor += 1;
    }
  }
  return { decoded, position: cursor < value.length ? cursor + 1 : cursor, valid: cursor < value.length };
}

function skipCSSSpaceAndComments(value, position, state) {
  let cursor = position;
  while (cursor < value.length) {
    if (isHTMLSpace(value[cursor])) {
      cursor += 1;
    } else if (value[cursor] === "/" && value[cursor + 1] === "*") {
      spendCSSToken(state);
      cursor = consumeCSSComment(value, cursor);
    } else {
      break;
    }
  }
  return cursor;
}

function consumeCSSURL(value, position, state) {
  let cursor = skipCSSSpaceAndComments(value, position, state);
  if (value[cursor] === "\"" || value[cursor] === "'") {
    const quoted = consumeCSSString(value, cursor);
    cursor = skipCSSSpaceAndComments(value, quoted.position, state);
    return { destination: quoted.valid ? quoted.decoded : null,
      position: value[cursor] === ")" ? cursor + 1 : cursor };
  }
  let decoded = "";
  while (cursor < value.length && value[cursor] !== ")" && !isHTMLSpace(value[cursor])) {
    if (value[cursor] === "\"" || value[cursor] === "'" || value[cursor] === "(") {
      return { destination: null, position: cursor + 1 };
    }
    if (value[cursor] === "/" && value[cursor + 1] === "*") {
      spendCSSToken(state);
      cursor = consumeCSSComment(value, cursor);
    } else if (value[cursor] === "\\") {
      const escaped = consumeCSSEscape(value, cursor);
      decoded += escaped.decoded;
      cursor = escaped.position;
    } else {
      decoded += value[cursor];
      cursor += 1;
    }
  }
  cursor = skipCSSSpaceAndComments(value, cursor, state);
  return { destination: decoded, position: value[cursor] === ")" ? cursor + 1 : cursor };
}

function cssDestinations(value, line, state, allowImport = false) {
  const found = [];
  const imageSetDepths = new Set();
  let position = 0;
  let depth = 0;
  while (position < value.length) {
    const character = value[position];
    if (isHTMLSpace(character)) {
      position += 1;
      continue;
    }
    if (character === "/" && value[position + 1] === "*") {
      spendCSSToken(state);
      position = consumeCSSComment(value, position);
      continue;
    }
    if (character === "\"" || character === "'") {
      spendCSSToken(state);
      const quoted = consumeCSSString(value, position);
      if (quoted.valid && quoted.decoded !== "" && imageSetDepths.has(depth)) {
        found.push({ destination: quoted.decoded, line, context: "html" });
      }
      position = quoted.position;
      continue;
    }
    if (character === "@" && allowImport && depth === 0 &&
      (isCSSName(value[position + 1]) || value[position + 1] === "\\")) {
      const name = consumeCSSName(value, position + 1);
      spendCSSToken(state);
      position = skipCSSSpaceAndComments(value, name.position, state);
      if (name.decoded.toLowerCase() === "import" &&
        (value[position] === "\"" || value[position] === "'")) {
        spendCSSToken(state);
        const imported = consumeCSSString(value, position);
        if (imported.valid && imported.decoded !== "") {
          found.push({ destination: imported.decoded, line, context: "html" });
        }
        position = imported.position;
      }
      continue;
    }
    if (isCSSName(character) || character === "\\") {
      const name = consumeCSSName(value, position);
      let afterName = name.position;
      while (value[afterName] === "/" && value[afterName + 1] === "*") {
        spendCSSToken(state);
        afterName = consumeCSSComment(value, afterName);
      }
      spendCSSToken(state);
      if (value[afterName] === "(") {
        if (depth + 1 > state.bounds.maxCSSDepth) {
          throw new Error(`embedded CSS depth exceeds ${state.bounds.maxCSSDepth}`);
        }
        const functionName = name.decoded.toLowerCase();
        if (functionName === "url") {
          const parsed = consumeCSSURL(value, afterName + 1, state);
          if (parsed.destination !== null && parsed.destination !== "") {
            found.push({ destination: parsed.destination, line, context: "html" });
          }
          position = parsed.position;
          continue;
        }
        depth += 1;
        if (functionName === "image-set" || functionName === "-webkit-image-set") {
          imageSetDepths.add(depth);
        }
        position = afterName + 1;
        continue;
      }
      position = afterName;
      continue;
    }
    spendCSSToken(state);
    if (character === "(" || character === "[" || character === "{") {
      depth += 1;
      if (depth > state.bounds.maxCSSDepth) {
        throw new Error(`embedded CSS depth exceeds ${state.bounds.maxCSSDepth}`);
      }
    } else if ((character === ")" || character === "]" || character === "}") && depth > 0) {
      if (character === ")") {
        imageSetDepths.delete(depth);
      }
      depth -= 1;
    }
    position += 1;
  }
  return found;
}

function srcsetDestinations(value) {
  const found = [];
  let position = 0;
  while (position < value.length && found.length <= MAX_SRCSET_CANDIDATES) {
    while (position < value.length && (isHTMLSpace(value[position]) || value[position] === ",")) {
      position += 1;
    }
    const start = position;
    const dataURL = value.slice(position, position + "data:".length).toLowerCase() === "data:";
    while (position < value.length && !isHTMLSpace(value[position]) &&
      (dataURL || value[position] !== ",")) {
      position += 1;
    }
    const destination = value.slice(start, position);
    if (destination !== "") {
      found.push(destination);
    }
    if (position < value.length && value[position] === ",") {
      position += 1;
      continue;
    }
    while (position < value.length && value[position] !== ",") {
      position += 1;
    }
  }
  if (found.length > MAX_SRCSET_CANDIDATES) {
    throw new Error(`embedded HTML srcset exceeds ${MAX_SRCSET_CANDIDATES} candidates`);
  }
  return found;
}

function spaceSeparatedDestinations(value, limit) {
  const found = [];
  let position = 0;
  while (position < value.length && found.length <= limit) {
    while (position < value.length && isHTMLSpace(value[position])) {
      position += 1;
    }
    const start = position;
    while (position < value.length && !isHTMLSpace(value[position])) {
      position += 1;
    }
    if (position > start) {
      found.push(value.slice(start, position));
    }
  }
  if (found.length > limit) {
    throw new Error(`embedded HTML destinations exceed ${limit}`);
  }
  return found;
}

function htmlAttributeDestinations(name, value, limit) {
  if (HTML_SRCSET_ATTRIBUTES.has(name)) {
    return srcsetDestinations(value);
  }
  if (HTML_SPACE_SEPARATED_URL_ATTRIBUTES.has(name)) {
    return spaceSeparatedDestinations(value, limit);
  }
  return [value];
}

function appendHTMLDestination(found, destination, line, limit, context = "html") {
  if (found.length >= limit) {
    throw new Error(`embedded HTML destinations exceed ${limit}`);
  }
  found.push({ destination, line, context });
}

function rejectJSXURLExpression(name, value) {
  if (value.trimStart().startsWith("{")) {
    throw new Error(`unsupported JSX expression in URL attribute ${name}`);
  }
}

function nodeAttribute(node, name) {
  const attributes = node?.attrs ?? [];
  if (attributes.length > MAX_HTML_NODES) {
    throw new Error(`embedded HTML exceeds ${MAX_HTML_NODES} attributes on one node`);
  }
  for (let index = 0; index < attributes.length && index < MAX_HTML_NODES; index += 1) {
    if (attributes[index].name.toLowerCase() === name) {
      return attributes[index];
    }
  }
  return null;
}

function skipHTMLSpace(value, position) {
  let cursor = position;
  while (cursor < value.length && isHTMLSpace(value[cursor])) {
    cursor += 1;
  }
  return cursor;
}

function isASCIIDigit(character) {
  return character !== undefined && character >= "0" && character <= "9";
}

function metaRefreshDestination(value) {
  let position = skipHTMLSpace(value, 0);
  const timeStart = position;
  while (position < value.length && isASCIIDigit(value[position])) {
    position += 1;
  }
  if (position === timeStart && value[position] !== ".") {
    return null;
  }
  while (position < value.length && (isASCIIDigit(value[position]) || value[position] === ".")) {
    position += 1;
  }
  if (position >= value.length ||
    (value[position] !== ";" && value[position] !== "," && !isHTMLSpace(value[position]))) {
    return null;
  }
  position = skipHTMLSpace(value, position);
  if (value[position] === ";" || value[position] === ",") {
    position += 1;
  }
  position = skipHTMLSpace(value, position);
  if (position >= value.length) {
    return null;
  }
  const fallbackStart = position;
  if (value[position]?.toLowerCase() === "u") {
    position += 1;
    if (value[position]?.toLowerCase() !== "r") {
      return value.slice(fallbackStart);
    }
    position += 1;
    if (value[position]?.toLowerCase() !== "l") {
      return value.slice(fallbackStart);
    }
    position = skipHTMLSpace(value, position + 1);
    if (value[position] !== "=") {
      return value.slice(fallbackStart);
    }
    position = skipHTMLSpace(value, position + 1);
  }
  const quote = value[position] === "\"" || value[position] === "'" ? value[position] : "";
  if (quote !== "") {
    position += 1;
    const end = value.indexOf(quote, position);
    return end < 0 ? value.slice(position) : value.slice(position, end);
  }
  return value.slice(position);
}

function collectHTMLAttributes(node, startLine, state, found, limit, mdxJSX) {
  const locations = node?.sourceCodeLocation?.attrs ?? {};
  const attributes = node?.attrs ?? [];
  if (attributes.length > MAX_HTML_NODES) {
    throw new Error(`embedded HTML exceeds ${MAX_HTML_NODES} attributes on one node`);
  }
  for (let attributeIndex = 0; attributeIndex < attributes.length &&
    attributeIndex < MAX_HTML_NODES; attributeIndex += 1) {
    const attribute = attributes[attributeIndex];
    const name = attribute.name.toLowerCase();
    const location = locations[attribute.name] ?? locations[name];
    const line = startLine + (location?.startLine ?? 1) - 1;
    if (HTML_URL_ATTRIBUTES.has(name) || HTML_SRCSET_ATTRIBUTES.has(name)) {
      if (mdxJSX && attribute.value.trimStart().startsWith("{")) {
        continue;
      }
      rejectJSXURLExpression(name, attribute.value);
      const values = htmlAttributeDestinations(name, attribute.value, limit - found.length);
      for (let index = 0; index < values.length; index += 1) {
        appendHTMLDestination(found, values[index], line, limit);
      }
    } else if (name === "style" || HTML_CSS_URL_ATTRIBUTES.has(name)) {
      if (mdxJSX && attribute.value.trimStart().startsWith("{")) {
        continue;
      }
      rejectJSXURLExpression(name, attribute.value);
      const css = cssDestinations(attribute.value, line, state);
      for (let index = 0; index < css.length; index += 1) {
        appendHTMLDestination(found, css[index].destination, css[index].line, limit);
      }
    }
  }
}

function collectMetaRefresh(node, startLine, found, limit) {
  if (node?.tagName?.toLowerCase() !== "meta") {
    return;
  }
  const httpEquiv = nodeAttribute(node, "http-equiv") ?? nodeAttribute(node, "httpequiv");
  const content = nodeAttribute(node, "content");
  if (httpEquiv?.value.trim().toLowerCase() !== "refresh" || content === null) {
    return;
  }
  const destination = metaRefreshDestination(content.value);
  if (destination === null || destination === "") {
    return;
  }
  const locations = node?.sourceCodeLocation?.attrs ?? {};
  const location = locations[content.name] ?? locations.content;
  appendHTMLDestination(found, destination, startLine + (location?.startLine ?? 1) - 1, limit);
}

function collectStyleElement(node, startLine, state, found, limit) {
  if (node?.tagName?.toLowerCase() !== "style") {
    return;
  }
  const children = node.childNodes ?? [];
  if (children.length > MAX_HTML_NODES) {
    throw new Error(`embedded HTML exceeds ${MAX_HTML_NODES} style children`);
  }
  for (let childIndex = 0; childIndex < children.length && childIndex < MAX_HTML_NODES; childIndex += 1) {
    const child = children[childIndex];
    if (child.nodeName !== "#text" || child.value === "") {
      continue;
    }
    const line = startLine + (child.sourceCodeLocation?.startLine ?? 1) - 1;
    const css = cssDestinations(child.value, line, state, true);
    for (let index = 0; index < css.length; index += 1) {
      appendHTMLDestination(found, css[index].destination, css[index].line, limit);
    }
  }
}

function collectHTML(fragment, startLine, depth, state, found, limit, mdxJSX) {
  if (depth > state.bounds.maxHTMLDepth) {
    throw new Error(`embedded HTML depth exceeds ${state.bounds.maxHTMLDepth}`);
  }
  const parsed = parseFragment(fragment, { sourceCodeLocationInfo: true, scriptingEnabled: false });
  const pending = [...(parsed.childNodes ?? [])];
  while (pending.length > 0) {
    if (state.nodes >= state.bounds.maxHTMLNodes) {
      throw new Error(`embedded HTML exceeds ${state.bounds.maxHTMLNodes} nodes`);
    }
    const node = pending.pop();
    state.nodes += 1;
    collectHTMLAttributes(node, startLine, state, found, limit, mdxJSX);
    collectMetaRefresh(node, startLine, found, limit);
    collectStyleElement(node, startLine, state, found, limit);
    const srcdoc = nodeAttribute(node, "srcdoc");
    if (srcdoc !== null) {
      const locations = node?.sourceCodeLocation?.attrs ?? {};
      const location = locations[srcdoc.name] ?? locations.srcdoc;
      collectHTML(srcdoc.value, startLine + (location?.startLine ?? 1) - 1,
        depth + 1, state, found, limit, mdxJSX);
    }
    const children = node?.childNodes ?? [];
    for (let index = 0; index < children.length; index += 1) {
      pending.push(children[index]);
    }
    const contentChildren = node?.content?.childNodes ?? [];
    for (let index = 0; index < contentChildren.length; index += 1) {
      pending.push(contentChildren[index]);
    }
  }
}

function htmlDestinations(fragment, startLine, limit = MAX_EVENTS, options = {}, sharedState = null) {
  const state = sharedState ?? createHTMLState(options);
  const found = [];
  collectHTML(fragment, startLine, 0, state, found, limit, options.mdxJSX === true);
  return found;
}

function isMDXSource(source) {
  const lower = source.toLowerCase();
  return lower.endsWith(".mdx") || lower.endsWith(".mdx.tmpl");
}

function isURLAttributeName(name) {
  return HTML_URL_ATTRIBUTES.has(name) || HTML_SRCSET_ATTRIBUTES.has(name) ||
    HTML_CSS_URL_ATTRIBUTES.has(name) || name === "style";
}

function isMDXExpressionURLProperty(name) {
  return isURLAttributeName(name) || MDX_EXPRESSION_URL_PROPERTIES.has(name) ||
    name.endsWith("href") || name.endsWith("src") || name.endsWith("url") || name.endsWith("path");
}

function collectMDXJSXAttribute(markdown, token, found, limit, expressions) {
  const attribute = markdown.slice(token.start.offset, token.end.offset);
  const equals = attribute.indexOf("=");
  if (equals < 0) {
    return;
  }
  const name = attribute.slice(0, equals).trim().toLowerCase();
  let valueStart = equals + 1;
  while (valueStart < attribute.length && isHTMLSpace(attribute[valueStart])) {
    valueStart += 1;
  }
  const rawValue = attribute.slice(valueStart).trimEnd();
  if (rawValue.startsWith("{")) {
    if (expressions.size >= MAX_EVENTS) {
      throw new Error(`MDX property expressions exceed ${MAX_EVENTS}`);
    }
    expressions.set(token.start.offset + valueStart, { name, line: token.start.line });
    return;
  }
  if (isURLAttributeName(name) || rawValue.length < 2) {
    return;
  }
  const quote = rawValue[0];
  if ((quote !== "\"" && quote !== "'") || rawValue.at(-1) !== quote) {
    throw new Error(`unsupported JSX literal form in attribute ${name}`);
  }
  const value = markdownDestinationString(rawValue.slice(1, -1), false);
  if (value !== "") {
    appendHTMLDestination(found, value, token.start.line, limit, "mdx-property");
  }
}

function staticMDXString(estree) {
  const body = estree?.type === "Program" ? estree.body : null;
  if (!Array.isArray(body) || body.length !== 1 || body[0]?.type !== "ExpressionStatement") {
    return null;
  }
  const expression = body[0].expression;
  if (expression?.type === "Literal" && typeof expression.value === "string") {
    return expression.value;
  }
  if (expression?.type === "TemplateLiteral" && expression.expressions?.length === 0 &&
    expression.quasis?.length === 1 && typeof expression.quasis[0]?.value?.cooked === "string") {
    return expression.quasis[0].value.cooked;
  }
  return null;
}

function appendMDXPropertyValue(name, value, line, state, found, limit) {
  if (HTML_URL_ATTRIBUTES.has(name) || HTML_SRCSET_ATTRIBUTES.has(name)) {
    const destinations = htmlAttributeDestinations(name, value, limit - found.length);
    for (let index = 0; index < destinations.length; index += 1) {
      appendHTMLDestination(found, destinations[index], line, limit);
    }
  } else if (name === "style" || HTML_CSS_URL_ATTRIBUTES.has(name)) {
    const destinations = cssDestinations(value, line, state);
    for (let index = 0; index < destinations.length; index += 1) {
      appendHTMLDestination(found, destinations[index].destination, line, limit);
    }
  } else if (value !== "") {
    appendHTMLDestination(found, value, line, limit, "mdx-property");
  }
}

function collectMDXPropertyExpression(token, property, state, found, limit) {
  const value = staticMDXString(token.estree);
  if (value !== null) {
    appendMDXPropertyValue(property.name, value, property.line, state, found, limit);
    return;
  }
  if (isMDXExpressionURLProperty(property.name)) {
    throw new Error(`unsupported JSX expression in URL attribute ${property.name}`);
  }
}

function appendESTreeNode(pending, node, depth) {
  if (node === null || typeof node !== "object") {
    return;
  }
  if (pending.length >= MAX_ESTREE_NODES) {
    throw new Error(`MDX syntax tree exceeds ${MAX_ESTREE_NODES} pending nodes`);
  }
  pending.push({ node, depth });
}

function collectMDXESTree(token, found, limit) {
  if (token.estree === null || typeof token.estree !== "object") {
    throw new Error(`${token.type} lacks a parsed MDX syntax tree`);
  }
  const pending = [{ node: token.estree, depth: 0 }];
  const seen = new WeakSet();
  for (let visited = 0; pending.length > 0 && visited < MAX_ESTREE_NODES; visited += 1) {
    const { node, depth } = pending.pop();
    if (seen.has(node)) {
      continue;
    }
    seen.add(node);
    if (depth > MAX_ESTREE_DEPTH) {
      throw new Error(`MDX syntax tree exceeds depth ${MAX_ESTREE_DEPTH}`);
    }
    const source = node.source;
    if (MDX_STATIC_SOURCE_TYPES.has(node.type) && typeof source?.value === "string") {
      appendHTMLDestination(found, source.value, source.loc?.start?.line ?? token.start.line, limit);
    } else if (node.type === "ImportExpression") {
      if (source?.type !== "Literal" || typeof source.value !== "string") {
        throw new Error("unsupported dynamic import target may contain a private scratch path");
      }
      appendHTMLDestination(found, source.value, source.loc?.start?.line ?? token.start.line, limit);
    }
    const keys = Object.keys(node);
    if (keys.length > MAX_ESTREE_PROPERTIES) {
      throw new Error(`MDX syntax-tree node exceeds ${MAX_ESTREE_PROPERTIES} properties`);
    }
    for (let keyIndex = 0; keyIndex < keys.length && keyIndex < MAX_ESTREE_PROPERTIES; keyIndex += 1) {
      const key = keys[keyIndex];
      if (key === "loc" || key === "range" || key === "comments") {
        continue;
      }
      const child = node[key];
      if (Array.isArray(child)) {
        if (child.length > MAX_ESTREE_NODES) {
          throw new Error(`MDX syntax-tree array exceeds ${MAX_ESTREE_NODES} nodes`);
        }
        for (let childIndex = 0; childIndex < child.length && childIndex < MAX_ESTREE_NODES; childIndex += 1) {
          appendESTreeNode(pending, child[childIndex], depth + 1);
        }
      } else {
        appendESTreeNode(pending, child, depth + 1);
      }
    }
  }
  if (pending.length > 0) {
    throw new Error(`MDX syntax tree exceeds ${MAX_ESTREE_NODES} nodes`);
  }
}

function isEscaped(value, position) {
  let slashes = 0;
  let cursor = position - 1;
  while (cursor >= 0 && value[cursor] === "\\") {
    slashes += 1;
    cursor -= 1;
  }
  return slashes % 2 === 1;
}

function attributeListEnd(value, position, end) {
  let quote = "";
  for (let cursor = position; cursor < end; cursor += 1) {
    if (quote !== "") {
      if (value[cursor] === quote && !isEscaped(value, cursor)) {
        quote = "";
      }
    } else if (value[cursor] === "\"" || value[cursor] === "'") {
      quote = value[cursor];
    } else if (value[cursor] === "}") {
      return cursor;
    }
  }
  return -1;
}

function appendAttributeValue(name, value, line, state, found, limit) {
  const decoded = markdownDestinationString(value, true);
  if (HTML_URL_ATTRIBUTES.has(name) || HTML_SRCSET_ATTRIBUTES.has(name)) {
    const destinations = htmlAttributeDestinations(name, decoded, limit - found.length);
    for (let index = 0; index < destinations.length; index += 1) {
      appendHTMLDestination(found, destinations[index], line, limit);
    }
  } else if (name === "style" || HTML_CSS_URL_ATTRIBUTES.has(name)) {
    const destinations = cssDestinations(decoded, line, state);
    for (let index = 0; index < destinations.length; index += 1) {
      appendHTMLDestination(found, destinations[index].destination, line, limit);
    }
  }
}

function collectAttributeListBody(value, start, end, line, state, found, limit) {
  let position = start;
  if (value[position] === ":") {
    position += 1;
  }
  while (position < end) {
    while (position < end && isHTMLSpace(value[position])) {
      position += 1;
    }
    if (value[position] === "#" || value[position] === ".") {
      while (position < end && !isHTMLSpace(value[position])) {
        position += 1;
      }
      continue;
    }
    const nameStart = position;
    while (position < end && !isHTMLSpace(value[position]) && value[position] !== "=") {
      position += 1;
    }
    const name = value.slice(nameStart, position).toLowerCase();
    while (position < end && isHTMLSpace(value[position])) {
      position += 1;
    }
    if (value[position] !== "=") {
      while (position < end && !isHTMLSpace(value[position])) {
        position += 1;
      }
      continue;
    }
    position += 1;
    while (position < end && isHTMLSpace(value[position])) {
      position += 1;
    }
    const quote = value[position] === "\"" || value[position] === "'" ? value[position] : "";
    const valueStart = position + (quote === "" ? 0 : 1);
    position = valueStart;
    while (position < end && (quote === "" ? !isHTMLSpace(value[position]) : value[position] !== quote)) {
      position += 1;
    }
    appendAttributeValue(name, value.slice(valueStart, position), line, state, found, limit);
    if (quote !== "" && position < end) {
      position += 1;
    }
  }
}

function collectAttributeLists(markdown, start, end, startLine, state, found, limit) {
  let position = start;
  let line = startLine;
  while (position < end) {
    const open = markdown.indexOf("{", position);
    if (open < 0 || open >= end) {
      return;
    }
    for (let cursor = position; cursor < open; cursor += 1) {
      line += markdown[cursor] === "\n" ? 1 : 0;
    }
    const close = isEscaped(markdown, open) ? -1 : attributeListEnd(markdown, open + 1, end);
    if (close >= 0) {
      collectAttributeListBody(markdown, open + 1, close, line, state, found, limit);
      for (let cursor = open; cursor <= close; cursor += 1) {
        line += markdown[cursor] === "\n" ? 1 : 0;
      }
      position = close + 1;
    } else {
      position = open + 1;
    }
  }
}

function parseEventLimit(markdown) {
  return Math.max(MAX_EVENTS, Buffer.byteLength(markdown) * MAX_EVENTS_PER_BYTE);
}

function snippetDestinations(markdown, limit, maxLines) {
  const lines = markdown.split("\n");
  if (lines.length > maxLines) {
    throw new Error(`snippet input exceeds ${maxLines} lines`);
  }
  const found = [];
  let inBlock = false;
  for (let index = 0; index < lines.length && index < maxLines; index += 1) {
    const trimmed = lines[index].trim();
    if (trimmed.startsWith(";")) {
      continue;
    }
    const marker = /^-+8<-+\s*(.*)$/u.exec(trimmed);
    if (inBlock) {
      if (marker !== null && marker[1] === "") {
        inBlock = false;
      } else if (trimmed !== "" && !trimmed.startsWith(";")) {
        const pathValue = /^(["'])(.*)\1$/u.exec(trimmed)?.[2] ?? trimmed;
        appendHTMLDestination(found, pathValue, index + 1, limit, "snippet");
      }
    } else if (marker !== null && marker[1] === "") {
      inBlock = true;
    } else if (marker !== null) {
      const quoted = /^(["'])(.*)\1$/u.exec(marker[1]);
      if (quoted !== null) {
        appendHTMLDestination(found, quoted[2], index + 1, limit, "snippet");
      }
    }
  }
  return found;
}

export function findPrivateScratchLinks(source, markdown, limit = MAX_FINDINGS + 1) {
  if (!Number.isInteger(limit) || limit < 1 || limit > MAX_FINDINGS + 1) {
    throw new Error(`finding limit must be between 1 and ${MAX_FINDINGS + 1}`);
  }
  const options = isMDXSource(source) ? { extensions: [mdxjs()] } : {};
  const events = postprocess(parse(options).document().write(preprocess()(markdown, undefined, true)));
  const eventLimit = parseEventLimit(markdown);
  if (events.length > eventLimit) {
    throw new Error(`${source}: Markdown parse exceeds ${eventLimit} events`);
  }
  const destinations = snippetDestinations(markdown, MAX_EVENTS, eventLimit);
  const htmlState = createHTMLState();
  const mdxPropertyExpressions = new Map();
  for (let index = 0; index < events.length && index < eventLimit; index += 1) {
    const [phase, token] = events[index];
    if (phase !== "enter") {
      continue;
    }
    if (DESTINATION_TYPES.has(token.type)) {
      if (destinations.length >= MAX_EVENTS) {
        throw new Error(`${source}: link destinations exceed ${MAX_EVENTS}`);
      }
      destinations.push({
        destination: markdown.slice(token.start.offset, token.end.offset),
        line: token.start.line,
        context: "markdown",
      });
    } else if (HTML_TYPES.has(token.type)) {
      const fragment = markdown.slice(token.start.offset, token.end.offset);
      const embedded = htmlDestinations(fragment, token.start.line,
        MAX_EVENTS - destinations.length, {}, htmlState);
      for (let embeddedIndex = 0; embeddedIndex < embedded.length; embeddedIndex += 1) {
        destinations.push(embedded[embeddedIndex]);
      }
    } else if (MDX_JSX_TAG_TYPES.has(token.type)) {
      const fragment = markdown.slice(token.start.offset, token.end.offset);
      const embedded = htmlDestinations(fragment, token.start.line,
        MAX_EVENTS - destinations.length, { mdxJSX: true }, htmlState);
      for (let embeddedIndex = 0; embeddedIndex < embedded.length; embeddedIndex += 1) {
        destinations.push(embedded[embeddedIndex]);
      }
    } else if (MDX_JSX_ATTRIBUTE_TYPES.has(token.type)) {
      collectMDXJSXAttribute(markdown, token, destinations, MAX_EVENTS, mdxPropertyExpressions);
    } else if (MDX_JSX_SPREAD_TYPES.has(token.type)) {
      throw new Error("unsupported JSX spread attribute may contain a URL");
    } else if (token.estree !== undefined) {
      const property = mdxPropertyExpressions.get(token.start.offset);
      if (property !== undefined) {
        collectMDXPropertyExpression(token, property, htmlState, destinations, MAX_EVENTS);
        mdxPropertyExpressions.delete(token.start.offset);
      }
      collectMDXESTree(token, destinations, MAX_EVENTS);
    } else if (token.type === "data") {
      collectAttributeLists(markdown, token.start.offset, token.end.offset, token.start.line,
        htmlState, destinations, MAX_EVENTS);
    }
  }
  if (mdxPropertyExpressions.size > 0) {
    throw new Error("MDX property expression lacks a parsed syntax tree");
  }
  const findings = [];
  for (let index = 0; index < destinations.length && index < MAX_EVENTS; index += 1) {
    const destination = destinations[index];
    const resolution = forbiddenResolution(source, destination.destination, destination.context);
    if (resolution !== null) {
      findings.push({ source, line: destination.line, original: destination.destination, ...resolution });
      if (findings.length === limit) {
        break;
      }
    }
  }
  return findings;
}

function boundedField(value) {
  return value.length <= MAX_DIAGNOSTIC_FIELD_CHARS ? value :
    `${value.slice(0, MAX_DIAGNOSTIC_FIELD_CHARS)}…(${value.length} chars)`;
}

function diagnostic(finding) {
  return `${finding.source}:${finding.line}: error PRAETOR-MD001 private scratch link ` +
    `destination=${JSON.stringify(boundedField(finding.original))} ` +
    `resolved=${JSON.stringify(boundedField(finding.resolved))} ` +
    `forbidden_root=${JSON.stringify(finding.root)}`;
}

function capFindings(findings, remaining) {
  return {
    emitted: findings.slice(0, remaining),
    firstOmitted: findings.length > remaining ? findings[remaining] : null,
  };
}

function selfTest() {
  const clean = [
    "[guide](../guide.md?view=full#intro)",
    "Literal .workingdir/OPEN.md prose.",
    "`.workingdir/OPEN.md`",
    "```markdown\n[x](.workingdir/OPEN.md)\n```",
    "[remote](https://example.invalid/.workingdir/OPEN.md)",
    "[remote-file-name](https://example.invalid/docs/.workingdir2.md)",
    "<img srcset=\"https://example.invalid/.workingdir/image.png 1x, ../public.png 2x\" alt=\"public\">",
    "<img srcset=\"data:image/svg+xml,%3Csvg%3E.workingdir%3C/svg%3E 1x,../public.png 2x\" alt=\"data URL\">",
    "<a href=\"../&amp;period;workingdir/OPEN.md\">literal entity text</a>",
    "[encoded-control](<fi%09le:../.workingdir/OPEN.md>)",
    "<a href=\"fi%09le:../.workingdir/OPEN.md\">encoded control</a>",
    "[encoded-drive-separator](C:%5Crepo%5C.workingdir2%5CSTATE.md)",
    "<div style=\"content: 'url(../.workingdir/OPEN.md)'\">literal CSS string</div>",
    "<style>.x::before { content: \"url(../.workingdir/OPEN.md)\"; }</style>",
    "<style>.x::before { content: \"../.workingdir/OPEN.md\"; }</style>",
    "<style>/* url(../.workingdir/comment.png) */ .x { background: image-set('../public.png' 1x); }</style>",
    "<svg><path fill=\"none\" stroke=\"currentColor\" clip-path=\"url(../public.svg#clip)\"></path></svg>",
    "[escaped-attributes](../README.md)\\{: href=\"../.workingdir/OPEN.md\" }",
    "```markdown\n[x](../README.md){: href=\"../.workingdir/OPEN.md\" }\n```",
    ";--8<-- \"../.workingdir/OPEN.md\"",
    "--8<-- \"; ../.workingdir/OPEN.md\"",
    "[lookalike](.workingdirectory/OPEN.md)",
    "[drive-lookalike](C:/repo/.workingdirectory/OPEN.md)",
  ].join("\n\n");
  assert.deepEqual(findPrivateScratchLinks("docs/guide.md", clean), []);
  const malformedMetaRefresh = [
    "<meta http-equiv=refresh content=\"soon;../.workingdir/OPEN.md\">",
    "<meta http-equiv=refresh content=\"0x;../.workingdir/OPEN.md\">",
    "<meta http-equiv=refresh content=\"0;;../.workingdir/OPEN.md\">",
    "<meta http-equiv=not-refresh content=\"0;../.workingdir/OPEN.md\">",
  ];
  for (let index = 0; index < malformedMetaRefresh.length && index < 4; index += 1) {
    assert.deepEqual(findPrivateScratchLinks("docs/meta.md", malformedMetaRefresh[index]), [],
      malformedMetaRefresh[index]);
  }
  assert.deepEqual(findPrivateScratchLinks("docs/page.mdx",
    "<Card href=\"../README.md\">Public</Card>"), []);
  assert.deepEqual(findPrivateScratchLinks("docs/page.mdx",
    "import Public from '../components/Public.mdx'\n\n<Card title=\"public\" />"), []);
  assert.deepEqual(findPrivateScratchLinks("docs/page.mdx",
    "export const component = import('../components/Public.mdx')"), []);
  assert.deepEqual(findPrivateScratchLinks("docs/page.mdx",
    "{import('../components/Public.mdx')}"), []);
  assert.deepEqual(findPrivateScratchLinks("docs/page.mdx",
    "export const count = value + 1\n\n{count + 1}"), []);
  assert.deepEqual(findPrivateScratchLinks("docs/page.mdx",
    "<Card title={'public'} href={'../README.md'} other={target} />"), []);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "<Card href=\"../.workingdir/OPEN.md\">Private</Card>").length, 1);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "<Link to=\"../.workingdir/OPEN.md\" />").length, 1);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "<Card path=\"../.workingdir2/x\" />").length, 1);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "<Card path=\".workingdir2/root-relative-x\" />").length, 1);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "<svg><a xlinkHref=\"../.workingdir/OPEN.md\">Private</a></svg>").length, 1);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "<meta httpEquiv=\"refresh\" content=\".0,../.workingdir/OPEN.md\" />").length, 1);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "import Private from '../.workingdir/Private.mdx'").length, 1);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "export {Private} from '../.workingdir2/Private.mdx'").length, 1);
  const dynamicESM = findPrivateScratchLinks("docs/page.mdx",
    "# Page\n\nexport const component = import('../.workingdir/Private.mdx')");
  assert.equal(dynamicESM.length, 1);
  assert.equal(dynamicESM[0].line, 3);
  const dynamicExpression = findPrivateScratchLinks("docs/page.mdx",
    "# Page\n\n{\nimport('../.workingdir2/Private.mdx')\n}");
  assert.equal(dynamicExpression.length, 1);
  assert.equal(dynamicExpression[0].line, 4);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "Text {import('../.workingdir/Private.mdx')}").length, 1);
  assert.equal(findPrivateScratchLinks("docs/page.mdx",
    "<Card title={import('../.workingdir2/Private.mdx')} />").length, 1);
  assert.throws(() => findPrivateScratchLinks("docs/page.mdx", "{import(modulePath)}"),
    /unsupported dynamic import target/u);

  const blocked = [
    "[direct](../.workingdir/OPEN.md)",
    "![image](../.workingdir2/image.png)",
    "[encoded](../%2Eworkingdir%2FSTATE.md)",
    "[literal](<../.workingdir2/file name.md>)",
    "<file:../.workingdir/STATE.md>",
    "<a href=\"../.workingdir2/report.html\">report</a>",
    "<img src='../.workingdir/diagram.svg'>",
    "[private][state]\n\n[state]: ../.workingdir/STATE.md",
    "[drive](C:/repo/.workingdir2/STATE.md)",
    "[root](/.workingdir/OPEN.md?raw=1#task)",
    "[dots](.././.workingdir2/../.workingdir2/evidence.md?view=full#result)",
    "[escaped](../\\.workingdir/OPEN.md)",
    "<file:///repo/.workingdir2/cache.md>",
    "<a href=\"file:..\\.workingdir\\OPEN.md\">Windows file link</a>",
    "[file-windows](<file:..\\.workingdir\\OPEN.md>)",
    "<img srcset=\"../.workingdir2/private.png 1x, ../public.png 2x\" alt=\"private\">",
    "<img srcset=\"../public.png,../.workingdir2/no-space.png 2x\" alt=\"private\">",
    "<picture><source srcset=\"../%2eworkingdir/private.png 1x\"></picture>",
    "<video poster=\"../.workingdir/private.png\"></video>",
    "<object data=\"../.workingdir2/private.svg\"></object>",
    "<form action=\"../.WORKINGDIR/private\"></form>",
    "<button formaction=\"../.WorkingDir2/private\">submit</button>",
    "<a href=\"../&#x2e;workingdir/OPEN.md\">encoded HTML</a>",
  ].join("\n\n");
  const findings = findPrivateScratchLinks("docs/guide.md", blocked);
  assert.equal(findings.length, 23, JSON.stringify(findings));
  assert.deepEqual(new Set(findings.map((finding) => finding.root)), SCRATCH_ROOTS);

  const structuralBypasses = [
    "[tab](<fi&#x9;le:../.workingdir/OPEN.md>)",
    "[lf](<fi&#xA;le:../.workingdir/OPEN.md>)",
    "[cr](<fi&#xD;le:../.workingdir/OPEN.md>)",
    "[leading-c0](<&#x1;file:../.workingdir/OPEN.md>)",
    "<a href=\"fi&#x9;le:../.workingdir/OPEN.md\">tab</a>",
    "<a href=\"fi&#xA;le:../.workingdir/OPEN.md\">lf</a>",
    "<a href=\"fi&#xD;le:../.workingdir/OPEN.md\">cr</a>",
    "<a href=\"&#x1;file:../.workingdir/OPEN.md\">leading C0</a>",
    "[malformed-percent](../%2eworkingdir/%ZZ/../OPEN.md)",
    "<a href=\"file:///repo/%2eworkingdir/%ZZ/../OPEN.md\">malformed percent</a>",
    "<iframe srcdoc=\"&lt;a href='../.workingdir/OPEN.md'&gt;private&lt;/a&gt;\"></iframe>",
    "<meta HTTP-EQUIV=\"Refresh\" content=\"0; URL=../.workingdir/OPEN.md\">",
    "<meta http-equiv=refresh content=\".0,../.workingdir/dot-delay\">",
    "<meta http-equiv=refresh content=\"0.5 ../.workingdir/space-separator\">",
    "<meta http-equiv=refresh content=\"0;../.workingdir/no-url-prefix\">",
    "<div style=\"background: url('../.workingdir/OPEN.md')\">private</div>",
    "<style>.private { background-image: URL(../.workingdir2/private.png); }</style>",
    "<style>.private { background-image: u\\72l(../.workingdir2/escaped.png); }</style>",
    "<style>@import '../.workingdir/private.css';</style>",
    "<style>@import URL(../.workingdir2/private.css);</style>",
    "<style>.private { background-image: image-set('../.workingdir/private.png' 1x, url(../public.png) 2x); }</style>",
    "<style>.private { background-image: -webkit-image-set(url(../.workingdir2/private.png) 1x); }</style>",
    "<svg><g filter=\"url(../.workingdir/filter.svg#x)\"></g></svg>",
    "<svg><path fill=\"url(../.workingdir/fill.svg#x)\"></path></svg>",
    "<svg><path stroke=\"url(../.workingdir/stroke.svg#x)\"></path></svg>",
    "<svg><path clip-path=\"url(../.workingdir/clip.svg#x)\"></path></svg>",
    "<svg><path mask=\"url(../.workingdir/mask.svg#x)\"></path></svg>",
    "<svg><path marker=\"url(../.workingdir/marker.svg#x)\"></path></svg>",
    "<svg><path marker-start=\"url(../.workingdir/start.svg#x)\"></path></svg>",
    "[unc](<\\\\server\\repo\\.workingdir\\OPEN.md>)",
    "<a href=\"\\\\server\\repo\\.workingdir2\\OPEN.md\">UNC</a>",
    "<a href=\"file://server/repo/.workingdir/OPEN.md\">file UNC</a>",
    "<noscript><a href=\"../.workingdir/OPEN.md\">private</a></noscript>",
    "[override](../README.md){: href=\"../.workingdir/OPEN.md\" }",
    "![override](../public.png){src=../.workingdir2/private.png}",
    "[compact](../README.md){:href=../.workingdir/OPEN.md}",
    "![set](../public.png){: srcset=\"../public.png 1x, ../.workingdir/private.png 2x\"}",
    "Paragraph\n{: style=\"background:url(../.workingdir/a.png)\"}",
    "--8<-- \"../.workingdir/STATE.md\"",
    "-8<- \".workingdir2/OPEN.md:1:3\"",
    "```text\n--8<-- \"../.workingdir/STATE.md\"\n```",
  ];
  for (let index = 0; index < structuralBypasses.length; index += 1) {
    assert.equal(findPrivateScratchLinks("docs/structural.md", structuralBypasses[index]).length, 1,
      structuralBypasses[index]);
  }

  const jsxLiteralExpressions = [
    "<Card href={'../.workingdir/OPEN.md'} />",
    "<img src={\"../.workingdir2/x.png\"} />",
    "<Card href={`../.workingdir/OPEN.md`} />",
    "<svg><a xlinkHref={'../.workingdir/OPEN.md'} /></svg>",
    "<Card path={`../.workingdir/OPEN.md`} />",
    "<Card url={'../.workingdir2/OPEN.md'} />",
    "<Card title={'../.workingdir/OPEN.md'} />",
    "<Card title={'..\\u002f.workingdir2\\u002fOPEN.md'} />",
    "<svg><path clipPath={'url(../.workingdir/clip.svg#x)'} /></svg>",
  ];
  for (let index = 0; index < jsxLiteralExpressions.length; index += 1) {
    assert.equal(findPrivateScratchLinks("docs/page.mdx", jsxLiteralExpressions[index]).length, 1,
      jsxLiteralExpressions[index]);
  }
  const jsxURLExpressions = [
    "<Card href={target} />",
    "<Link to={target} />",
    "<Card path={`../${directory}/OPEN.md`} />",
  ];
  for (let index = 0; index < jsxURLExpressions.length; index += 1) {
    assert.throws(() => findPrivateScratchLinks("docs/page.mdx", jsxURLExpressions[index]),
      /unsupported JSX expression in URL attribute/u, jsxURLExpressions[index]);
  }
  assert.throws(() => findPrivateScratchLinks("docs/page.mdx", "<Card {...props} />"),
    /unsupported JSX spread attribute/u);
  const blockSnippet = "--8<--\n.workingdir/OPEN.md\n../.workingdir2/STATE.md:1:3\n; .workingdir/skip.md\n--8<--";
  assert.equal(findPrivateScratchLinks("docs/snippets.md", blockSnippet).length, 2);

  const boundary = findPrivateScratchLinks("docs/deep/guide.md", "[private](../../.workingdir/STATE.md)");
  assert.equal(boundary.length, 1);
  assert.equal(boundary[0].resolved, ".workingdir/STATE.md");
  assert.doesNotThrow(() => validateFileList(new Array(MAX_FILES).fill("fixture.md")));
  assert.throws(() => validateFileList(new Array(MAX_FILES + 1).fill("fixture.md")), /maximum is 16384/u);
  // The parse bound grows with the file: a document past the MAX_EVENTS floor that parses to
  // fewer events than its bytes passes, one that parses to more fails, and the limit is counted
  // in UTF-8 bytes, not characters.
  assert.equal(parseEventLimit("a".repeat(MAX_EVENTS)), MAX_EVENTS);
  assert.equal(parseEventLimit("a".repeat(MAX_EVENTS + 1)), MAX_EVENTS + 1);
  assert.equal(parseEventLimit("\u00e9".repeat(MAX_EVENTS)), 2 * MAX_EVENTS);
  const sparseLines = "abcdef\n".repeat(70_000);
  assert.deepEqual(findPrivateScratchLinks("docs/large.md", sparseLines), []);
  const denseLines = "x\n".repeat(70_000);
  assert.throws(() => findPrivateScratchLinks("docs/dense.md", denseLines),
    /docs\/dense\.md: Markdown parse exceeds 262144 events/u);
  const findingFixture = new Array(MAX_FINDINGS + 1).fill(findings[0]);
  assert.equal(capFindings(findingFixture.slice(0, MAX_FINDINGS), MAX_FINDINGS).firstOmitted, null);
  assert.equal(capFindings(findingFixture, MAX_FINDINGS).firstOmitted, findings[0]);
  const exactFindingMarkdown = new Array(MAX_FINDINGS).fill("[x](../.workingdir/OPEN.md)").join("\n");
  const excessFindingMarkdown = `${exactFindingMarkdown}\n[x](../.workingdir/OPEN.md)`;
  assert.equal(findPrivateScratchLinks("docs/limit.md", exactFindingMarkdown).length, MAX_FINDINGS);
  assert.equal(findPrivateScratchLinks("docs/limit.md", excessFindingMarkdown).length, MAX_FINDINGS + 1);
  const exactSrcset = new Array(MAX_SRCSET_CANDIDATES).fill("../public.png 1x").join(", ");
  const excessSrcset = `${exactSrcset}, ../public.png 1x`;
  assert.deepEqual(findPrivateScratchLinks("docs/srcset.md", `<img srcset=\"${exactSrcset}\">`), []);
  assert.throws(() => findPrivateScratchLinks("docs/srcset.md", `<source srcset=\"${excessSrcset}\">`),
    /srcset exceeds 4096 candidates/u);
  const estreeToken = (estree) => ({ estree, start: { line: 1 }, type: "fixture" });
  const exactESTreeNodes = new Array(MAX_ESTREE_NODES - 1);
  for (let index = 0; index < exactESTreeNodes.length && index < MAX_ESTREE_NODES; index += 1) {
    exactESTreeNodes[index] = { type: "Identifier" };
  }
  assert.doesNotThrow(() => collectMDXESTree(estreeToken({ body: exactESTreeNodes }), [], MAX_EVENTS));
  exactESTreeNodes.push({ type: "Identifier" });
  assert.throws(() => collectMDXESTree(estreeToken({ body: exactESTreeNodes }), [], MAX_EVENTS),
    /MDX syntax tree exceeds 65536 nodes/u);
  const exactESTreeDepth = { type: "Identifier" };
  let depthCursor = exactESTreeDepth;
  for (let depth = 0; depth < MAX_ESTREE_DEPTH && depth < 128; depth += 1) {
    depthCursor.child = { type: "Identifier" };
    depthCursor = depthCursor.child;
  }
  assert.doesNotThrow(() => collectMDXESTree(estreeToken(exactESTreeDepth), [], MAX_EVENTS));
  depthCursor.child = { type: "Identifier" };
  assert.throws(() => collectMDXESTree(estreeToken(exactESTreeDepth), [], MAX_EVENTS),
    /MDX syntax tree exceeds depth 128/u);
  const exactESTreeProperties = {};
  for (let index = 0; index < MAX_ESTREE_PROPERTIES && index < 32; index += 1) {
    exactESTreeProperties[`property${index}`] = null;
  }
  assert.doesNotThrow(() => collectMDXESTree(estreeToken(exactESTreeProperties), [], MAX_EVENTS));
  exactESTreeProperties.overflow = null;
  assert.throws(() => collectMDXESTree(estreeToken(exactESTreeProperties), [], MAX_EVENTS),
    /MDX syntax-tree node exceeds 32 properties/u);
  assert.equal(htmlDestinations("<img src='one'><img src='two'>", 1, 2).length, 2);
  assert.throws(() => htmlDestinations("<img src='one'><img src='two'>", 1, 1),
    /embedded HTML destinations exceed 1/u);
  assert.equal(htmlDestinations("<a ping='one two'>", 1, 2).length, 2);
  assert.throws(() => htmlDestinations("<a ping='one two'>", 1, 1),
    /embedded HTML destinations exceed 1/u);
  const quoteSrcdoc = (html) => `<iframe srcdoc=\"${html.replaceAll("&", "&amp;").replaceAll("\"", "&quot;")}\"></iframe>`;
  const oneSrcdoc = quoteSrcdoc("<a href='../public.md'>public</a>");
  assert.equal(htmlDestinations(oneSrcdoc, 1, MAX_EVENTS, { maxHTMLDepth: 1 }).length, 1);
  assert.throws(() => htmlDestinations(quoteSrcdoc(oneSrcdoc), 1, MAX_EVENTS, { maxHTMLDepth: 1 }),
    /embedded HTML depth exceeds 1/u);
  assert.equal(htmlDestinations("<img src='one'><img src='two'>", 1, MAX_EVENTS,
    { maxHTMLNodes: 2 }).length, 2);
  assert.throws(() => htmlDestinations("<img src='one'><img src='two'>", 1, MAX_EVENTS,
    { maxHTMLNodes: 1 }), /embedded HTML exceeds 1 nodes/u);
  const twoCSSURLs = "<style>url(one)url(two)</style>";
  assert.equal(htmlDestinations(twoCSSURLs, 1, MAX_EVENTS, { maxCSSTokens: 2 }).length, 2);
  assert.throws(() => htmlDestinations(twoCSSURLs, 1, MAX_EVENTS, { maxCSSTokens: 1 }),
    /embedded CSS exceeds 1 tokens/u);
  const nestedCSS = "<div style='background:wrap(url(one))'></div>";
  assert.equal(htmlDestinations(nestedCSS, 1, MAX_EVENTS, { maxCSSDepth: 2 }).length, 1);
  assert.throws(() => htmlDestinations(nestedCSS, 1, MAX_EVENTS, { maxCSSDepth: 1 }),
    /embedded CSS depth exceeds 1/u);
  const self = fileURLToPath(import.meta.url);
  assert.equal(invokedAsScript(self, self), true);
  assert.equal(invokedAsScript(path.dirname(self), self), false);
  assert.equal(invokedAsScript(path.join(path.dirname(self), "missing-entry.mjs"), self), false);
  assert.equal(invokedAsScript(undefined, self), false);
  assert.equal(invokedAsScript("", self), false);
  process.stdout.write("private-scratch-link fixtures: positive, negative, boundary pass\n");
}

function validateFileList(files) {
  if (!Array.isArray(files)) {
    throw new Error("inventory must be a JSON array");
  }
  if (files.length > MAX_FILES) {
    throw new Error(`inventory has ${files.length} files; maximum is ${MAX_FILES}`);
  }
  for (let index = 0; index < files.length && index < MAX_FILES; index += 1) {
    if (typeof files[index] !== "string" || files[index] === "") {
      throw new Error(`inventory entry ${index} must be a non-empty string`);
    }
    if (Buffer.byteLength(files[index]) > MAX_SOURCE_PATH_BYTES || /[\0\r\n]/u.test(files[index])) {
      throw new Error(`inventory entry ${index} has an unsafe or oversized path`);
    }
  }
}

// Worker side: read one inventory file at a time, refusing one past the per-file ceiling before
// reading it, and reply with its findings or the error that stopped the scan.
function readBoundedSource(root, source) {
  const full = path.join(root, source);
  const size = fs.statSync(full).size;
  if (size > MAX_FILE_BYTES) {
    throw new Error(`${source} is ${size} bytes; per-file maximum is ${MAX_FILE_BYTES}`);
  }
  return fs.readFileSync(full, "utf8");
}

function serveScans(root) {
  parentPort.on("message", (request) => {
    try {
      const markdown = readBoundedSource(root, request.source);
      parentPort.postMessage({ findings: findPrivateScratchLinks(request.source, markdown, request.limit) });
    } catch (error) {
      parentPort.postMessage({ error: error.message });
    }
  });
}

function workerFailure(source, error, bounds) {
  if (error?.code === "ERR_WORKER_OUT_OF_MEMORY") {
    return new Error(`${source}: Markdown parse exceeds the ${bounds.heapBudgetMb} MiB parse memory budget`);
  }
  return new Error(`${source}: scan worker failed: ${error?.message ?? error}`);
}

// Main side: one request in flight at a time, so a worker that runs out of heap, dies or misses
// the deadline is always attributed to the file it was scanning.
class ScanWorker {
  #bounds;
  #retired = false;
  #pending = null;
  #worker;

  constructor(root, bounds) {
    this.#bounds = bounds;
    this.#worker = new Worker(new URL(import.meta.url), {
      workerData: { role: SCAN_WORKER_ROLE, root },
      resourceLimits: {
        maxOldGenerationSizeMb: bounds.heapBudgetMb,
        maxYoungGenerationSizeMb: SCAN_YOUNG_GENERATION_MB,
      },
    });
    this.#worker.on("message", (reply) => {
      this.#settle(reply.error === undefined ? null : new Error(reply.error), reply.findings);
    });
    this.#worker.on("error", (error) => {
      this.#settle(workerFailure(this.#pending?.source, error, this.#bounds));
    });
    this.#worker.on("exit", (code) => {
      this.#retired = true;
      this.#settle(new Error(`${this.#pending?.source}: scan worker exited ${code}`));
    });
  }

  scan(source, limit) {
    if (this.#retired) {
      return Promise.reject(new Error(`${source}: scan worker has stopped`));
    }
    if (this.#pending !== null) {
      return Promise.reject(new Error(`${source}: scan requested while ${this.#pending.source} is in flight`));
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.#expire(source), this.#bounds.deadlineMs);
      this.#pending = { source, resolve, reject, timer };
      this.#worker.postMessage({ source, limit });
    });
  }

  // A worker past its deadline is still parsing, and its late reply would answer the next
  // request, so it is stopped and takes no further file.
  #expire(source) {
    this.#retired = true;
    this.#settle(new Error(`${source}: Markdown scan exceeds ${this.#bounds.deadlineMs} ms`));
    this.#worker.terminate().catch(() => {});
  }

  // A late event for a request already settled (the exit after a memory failure or after the
  // deadline) finds no pending request and is dropped.
  #settle(error, findings) {
    const pending = this.#pending;
    if (pending === null) {
      return;
    }
    this.#pending = null;
    clearTimeout(pending.timer);
    if (error === null) {
      pending.resolve(findings);
    } else {
      pending.reject(error);
    }
  }

  close() {
    return this.#worker.terminate();
  }
}

async function withScanWorker(root, bounds, action) {
  const scanner = new ScanWorker(root, bounds);
  try {
    return await action(scanner);
  } finally {
    await scanner.close();
  }
}

async function reportFindings(files, scanner) {
  let emitted = 0;
  for (let index = 0; index < files.length && index < MAX_FILES; index += 1) {
    const remaining = MAX_FINDINGS - emitted;
    const found = await scanner.scan(files[index], remaining + 1);
    found.sort((left, right) => left.line - right.line || left.original.localeCompare(right.original));
    const capped = capFindings(found, remaining);
    for (let findingIndex = 0; findingIndex < capped.emitted.length &&
      findingIndex < MAX_FINDINGS; findingIndex += 1) {
      process.stderr.write(`${diagnostic(capped.emitted[findingIndex])}\n`);
      emitted += 1;
    }
    if (capped.firstOmitted !== null) {
      process.stderr.write(`${capped.firstOmitted.source}:${capped.firstOmitted.line}: error PRAETOR-MD002 ` +
        `private scratch-link diagnostic limit exceeded maximum=${MAX_FINDINGS}\n`);
      return 2;
    }
  }
  return emitted === 0 ? 0 : 1;
}

function runFileList(root, inventoryPath, bounds = SCAN_BOUNDS) {
  const files = JSON.parse(fs.readFileSync(inventoryPath, "utf8"));
  validateFileList(files);
  return withScanWorker(root, bounds, (scanner) => reportFindings(files, scanner));
}

function ceilingFixture(tail) {
  const bullet = "- Lorem ipsum dolor sit amet, [consectetur](https://example.invalid/x) `adipiscing` elit.\n";
  const count = Math.floor((MAX_FILE_BYTES - tail.length - 16) / bullet.length);
  const headingBytes = MAX_FILE_BYTES - tail.length - count * bullet.length;
  return { markdown: `# ${"a".repeat(headingBytes - 4)}\n\n${bullet.repeat(count)}${tail}`, line: count + 3 };
}

function writeScanFixture(root, source, markdown) {
  fs.mkdirSync(path.dirname(path.join(root, source)), { recursive: true });
  fs.writeFileSync(path.join(root, source), markdown);
}

// Positive: a file exactly at the per-file ceiling, of linked and code-spanned bullets, scans
// inside the production budget and deadline, and the private link on its last line comes back
// as a finding. Negative: a list of one-word items exhausts a small budget and a run of `*a*`
// misses a short deadline, and each failure names the file and the bound while this process
// keeps running; a worker that ran out of heap or time takes no further file, and a second
// request while one is in flight is refused. Boundary: a file one byte past the ceiling is refused
// before it is read, and the worker then scans the next file.
async function scanWorkerSelfTest(root) {
  const ceiling = ceilingFixture("- [private](../.workingdir/OPEN.md)\n");
  assert.equal(Buffer.byteLength(ceiling.markdown), MAX_FILE_BYTES);
  writeScanFixture(root, "docs/ceiling.md", ceiling.markdown);
  writeScanFixture(root, "docs/over.md", `${ceiling.markdown}a`);
  writeScanFixture(root, "docs/small.md", "# Small\n\n[public](../README.md)\n");
  writeScanFixture(root, "docs/list.md", "- a\n".repeat(65_536));
  writeScanFixture(root, "docs/slow.md", "*a* ".repeat(16_384));
  await withScanWorker(root, SCAN_BOUNDS, async (scanner) => {
    const found = await scanner.scan("docs/ceiling.md", MAX_FINDINGS + 1);
    assert.deepEqual(found.map((finding) => [finding.line, finding.resolved]), [[ceiling.line, ".workingdir/OPEN.md"]]);
    await assert.rejects(scanner.scan("docs/over.md", 1), (error) =>
      error.message === `docs/over.md is ${MAX_FILE_BYTES + 1} bytes; per-file maximum is ${MAX_FILE_BYTES}`);
    const inFlight = scanner.scan("docs/small.md", 1);
    await assert.rejects(scanner.scan("docs/ceiling.md", 1),
      /docs\/ceiling\.md: scan requested while docs\/small\.md is in flight/u);
    assert.deepEqual(await inFlight, []);
  });
  await withScanWorker(root, { heapBudgetMb: 64, deadlineMs: SCAN_DEADLINE_MS }, async (scanner) => {
    assert.deepEqual(await scanner.scan("docs/small.md", 1), []);
    await assert.rejects(scanner.scan("docs/list.md", 1), (error) =>
      error.message === "docs/list.md: Markdown parse exceeds the 64 MiB parse memory budget");
    await assert.rejects(scanner.scan("docs/small.md", 1), /docs\/small\.md: scan worker has stopped/u);
  });
  const inventory = path.join(root, "inventory.json");
  fs.writeFileSync(inventory, JSON.stringify(["docs/small.md", "docs/list.md"]));
  await assert.rejects(runFileList(root, inventory, { heapBudgetMb: 64, deadlineMs: SCAN_DEADLINE_MS }),
    /^Error: docs\/list\.md: Markdown parse exceeds the 64 MiB parse memory budget$/u);
  fs.writeFileSync(inventory, JSON.stringify(["docs/small.md"]));
  assert.equal(await runFileList(root, inventory), 0);
  await withScanWorker(root, { heapBudgetMb: PARSE_HEAP_BUDGET_MB, deadlineMs: 50 }, async (scanner) => {
    await assert.rejects(scanner.scan("docs/slow.md", 1), (error) =>
      error.message === "docs/slow.md: Markdown scan exceeds 50 ms");
    await assert.rejects(scanner.scan("docs/small.md", 1), /docs\/small\.md: scan worker has stopped/u);
  });
  process.stdout.write("private-scratch-link scan worker fixtures: ceiling file inside the parse memory budget, " +
    "budget and deadline failures name the file, one byte past the ceiling refused\n");
}

async function runSelfTests() {
  selfTest();
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "praetor-scratch-scan-"));
  try {
    await scanWorkerSelfTest(root);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

// Node loads a main module from its real path (symlinks resolved), while process.argv[1] keeps
// the spelling the caller used. Every macOS temp directory sits under /var -> /private/var, so
// comparing the two strings skipped main() for verify.mjs's temporary copy of this rule, and
// the gate exited 0 without reading one file. Both operands are compared in real form instead.
export function invokedAsScript(scriptPath, modulePath) {
  if (typeof scriptPath !== "string" || scriptPath === "") {
    return false;
  }
  try {
    return fs.realpathSync(scriptPath) === fs.realpathSync(modulePath);
  } catch {
    return false;
  }
}

async function main() {
  if (!isMainThread) {
    if (workerData?.role === SCAN_WORKER_ROLE) {
      serveScans(workerData.root);
    }
    return;
  }
  if (!invokedAsScript(process.argv[1], fileURLToPath(import.meta.url))) {
    return;
  }
  if (process.argv[2] === "--self-test" && process.argv.length === 3) {
    await runSelfTests();
    return;
  }
  if (process.argv.length !== 4) {
    process.stderr.write("usage: no-private-scratch-links.mjs <repository-root> <inventory.json>\n");
    process.exitCode = 2;
    return;
  }
  process.exitCode = await runFileList(path.resolve(process.argv[2]), path.resolve(process.argv[3]));
}

main().catch((error) => {
  process.stderr.write(`markdown-scratch-links: ${error.message}\n`);
  process.exitCode = 2;
});
