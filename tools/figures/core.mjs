// The render core of the figure engine: spec validation, the derived text description, the
// three committed outputs of one figure (docs/adr/0015-interactive-figures-from-vendored-interfig.md,
// sections 2, 3 and 6) and the figure markup every page, README and wiki shows
// (docs/adr/0016-figures-for-adopters.md, section 4). Pure functions over a spec object; it reads
// and writes no file.
//
// Its bytes are hashed with the vendored render files (ENGINE_FILES), so an edit here marks every
// committed figure stale. The command-line wrapper, build.mjs, and the checks, checks.mjs, are not
// hashed: editing them leaves the figures current.
import { createHash } from 'node:crypto';
import { toSvg } from './third_party/interfig/upstream/src/svg.ts';

/**
 * The files whose bytes decide what an SVG looks like, relative to the repository root.
 * `engineHash` in checks.mjs hashes this one list, for `build` and for `sources`.
 */
export const ENGINE_FILES = [
  'tools/figures/third_party/interfig/upstream/src/svg.ts',
  'tools/figures/third_party/interfig/upstream/src/geometry.ts',
  'tools/figures/third_party/interfig/upstream/src/model.ts',
  'tools/figures/core.mjs',
];
/** HISS-02 bounds on every spec, and on the number of specs one run reads. */
export const LIMITS = Object.freeze({
  alt: 125, boxes: 40, groups: 40, steps: 12, edges: 80, beats: 32, hops: 8, rows: 16,
  text: 2500, specs: 256, depth: 8,
});
/**
 * The markers `decorate` writes around the props every figure SVG embeds, as upstream's
 * figure-svg.mjs does. The `site` check (checks.mjs) reads them from here; the browser loader
 * (loader.ts) keeps its own copy, because importing this module would bundle node:crypto and the
 * render engine into the player.
 */
export const SPEC_OPEN = '<metadata id="figure-spec"><![CDATA[';
export const SPEC_CLOSE = ']]></metadata>';
const EVIDENCE = /^[^\s:]+:[A-Za-z_][\w.]*$/;
const TONES = new Set(['blue', 'purple', 'green', 'orange', 'gray']);
const SHAPES = new Set(['box', 'decision', 'store']);

export const sha256 = (data) => createHash('sha256').update(data).digest('hex');
const isStr = (v) => typeof v === 'string';
const optStr = (v) => v === undefined || isStr(v);
const isObj = (v) => v != null && typeof v === 'object' && !Array.isArray(v);

/**
 * Every box and group in a layout tree, walked with an explicit stack (HISS-01: no recursion).
 * Each box records the groups it sits in, so an edge to a group can expand to its boxes.
 */
export function walkLayout(layout) {
  const boxes = [];
  const groups = [];
  const errors = [];
  const stack = [{ item: layout, ancestors: [], depth: 0 }];
  const cap = LIMITS.boxes + LIMITS.groups + 1;
  for (let seen = 0; stack.length > 0; seen++) {
    if (seen >= cap) {
      errors.push(`layout has more than ${cap - 1} boxes and groups`);
      break;
    }
    const { item, ancestors, depth } = stack.pop();
    if (!isObj(item)) {
      errors.push('layout holds an entry that is not an object');
      continue;
    }
    if (!Array.isArray(item.children)) {
      boxes.push({ node: item, ancestors });
      continue;
    }
    if (depth >= LIMITS.depth) {
      errors.push(`layout nests deeper than ${LIMITS.depth} groups`);
      continue;
    }
    const group = { group: item, ancestors, boxIds: [] };
    groups.push(group);
    const inner = [...ancestors, group];
    for (let i = item.children.length - 1; i >= 0; i--) stack.push({ item: item.children[i], ancestors: inner, depth: depth + 1 });
  }
  for (const box of boxes) for (const group of box.ancestors) group.boxIds.push(box.node.id);
  return { boxes, groups, errors };
}

function validateTop(figure) {
  const errors = [];
  if (!isStr(figure.title) || !figure.title.trim()) errors.push('title must be a non-empty string');
  if (!isStr(figure.alt) || !figure.alt.trim()) errors.push('alt must be a non-empty string');
  else if (figure.alt.length > LIMITS.alt) errors.push(`alt is ${figure.alt.length} characters; at most ${LIMITS.alt}`);
  if (!Array.isArray(figure.evidence) || figure.evidence.length === 0) errors.push('evidence must list at least one path:Symbol');
  else if (!figure.evidence.every((e) => isStr(e) && EVIDENCE.test(e))) errors.push('every evidence entry must read path:Symbol');
  if (figure.describe !== undefined && !(Array.isArray(figure.describe) && figure.describe.every(isStr))) {
    errors.push('describe must be a list of strings');
  }
  if (!isObj(figure.props) || !isObj(figure.props.layout)) errors.push('props.layout must be a group');
  return errors;
}

function validateLayout(walked) {
  const errors = [...walked.errors];
  const ids = new Map();
  const claim = (id, what) => {
    if (!isStr(id) || !id) return errors.push(`${what} needs a string id`);
    if (ids.has(id)) return errors.push(`duplicate id "${id}"`);
    ids.set(id, what);
  };
  if (walked.boxes.length > LIMITS.boxes) errors.push(`${walked.boxes.length} boxes; at most ${LIMITS.boxes}`);
  if (walked.groups.length > LIMITS.groups) errors.push(`${walked.groups.length} groups; at most ${LIMITS.groups}`);
  for (const { node } of walked.boxes) {
    claim(node.id, 'box');
    if (!isStr(node.label)) errors.push(`box "${node.id}": label must be a string`);
    if (!optStr(node.sub)) errors.push(`box "${node.id}": sub must be a string`);
    if (node.shape !== undefined && !SHAPES.has(node.shape)) errors.push(`box "${node.id}": unknown shape "${node.shape}"`);
  }
  for (const { group } of walked.groups) {
    if (group.id !== undefined) claim(group.id, 'group');
    if (!optStr(group.label)) errors.push(`group "${group.id ?? '(unnamed)'}": label must be a string`);
  }
  return { errors, ids };
}

const edgeIdOf = (e) => e.id ?? `${e.from}->${e.to}`;

function validateEdges(edges, ids) {
  const errors = [];
  const edgeIds = new Set();
  if (!Array.isArray(edges)) return { errors: ['props.edges must be a list'], edgeIds };
  if (edges.length > LIMITS.edges) errors.push(`${edges.length} edges; at most ${LIMITS.edges}`);
  for (const edge of edges.slice(0, LIMITS.edges)) {
    if (!isObj(edge)) {
      errors.push('an edge is not an object');
      continue;
    }
    const id = edgeIdOf(edge);
    if (edgeIds.has(id)) errors.push(`duplicate edge id "${id}"`);
    edgeIds.add(id);
    if (!ids.has(edge.from)) errors.push(`edge "${id}": from "${edge.from}" is not a box or group id`);
    if (!ids.has(edge.to)) errors.push(`edge "${id}": to "${edge.to}" is not a box or group id`);
    if (!optStr(edge.label)) errors.push(`edge "${id}": label must be a string`);
  }
  return { errors, edgeIds };
}

function validateContent(content, where) {
  if (isStr(content)) return [];
  if (!Array.isArray(content)) return [`${where}: content must be a string or a list of rows`];
  if (content.length > LIMITS.rows) return [`${where}: ${content.length} rows; at most ${LIMITS.rows}`];
  const errors = [];
  for (const row of content) {
    const strings = isObj(row) && isStr(row.text) && optStr(row.tag) && optStr(row.meta) && optStr(row.mark);
    if (!strings) errors.push(`${where}: every row needs string text, and tag, meta and mark must be strings`);
    else if (row.tone !== undefined && !TONES.has(row.tone)) errors.push(`${where}: unknown tone "${row.tone}"`);
  }
  return errors;
}

function hopsOf(beat) {
  const isBeat = isObj(beat) && !('edge' in beat);
  const edges = isBeat ? beat.edges : beat;
  const list = edges == null ? [] : Array.isArray(edges) ? edges : [edges];
  return list.map((h) => (isStr(h) ? { edge: h } : h));
}

function validateBeat(beat, where, boxIds, edgeIds) {
  const errors = [];
  const hops = hopsOf(beat);
  if (hops.length > LIMITS.hops) errors.push(`${where}: ${hops.length} hops; at most ${LIMITS.hops}`);
  for (const hop of hops.slice(0, LIMITS.hops)) {
    if (!isObj(hop) || !edgeIds.has(hop.edge)) errors.push(`${where}: hop names no edge ("${hop?.edge}")`);
    else if (!optStr(hop.data)) errors.push(`${where}: hop data must be a string`);
  }
  if (!isObj(beat) || 'edge' in beat) return errors;
  if (!optStr(beat.say)) errors.push(`${where}: say must be a string`);
  for (const [key, content] of Object.entries(beat.show ?? {})) {
    if (!boxIds.has(key)) errors.push(`${where}: show key "${key}" is not a box id`);
    errors.push(...validateContent(content, `${where} show "${key}"`));
  }
  for (const id of beat.light ?? []) if (!boxIds.has(id)) errors.push(`${where}: light "${id}" is not a box id`);
  return errors;
}

function validateSteps(steps, boxIds, edgeIds) {
  if (steps === undefined) return [];
  if (!Array.isArray(steps)) return ['props.steps must be a list'];
  const errors = steps.length > LIMITS.steps ? [`${steps.length} steps; at most ${LIMITS.steps}`] : [];
  steps.slice(0, LIMITS.steps).forEach((step, si) => {
    const where = `step ${si + 1}`;
    if (!isObj(step) || !isStr(step.label)) return errors.push(`${where}: label must be a string`);
    if (!optStr(step.caption)) errors.push(`${where}: caption must be a string`);
    if (!Array.isArray(step.flow) || step.flow.length > LIMITS.beats) return errors.push(`${where}: flow must list 0-${LIMITS.beats} beats`);
    step.flow.forEach((beat, bi) => errors.push(...validateBeat(beat, `${where} beat ${bi + 1}`, boxIds, edgeIds)));
    for (const id of step.nodes ?? []) if (!boxIds.has(id)) errors.push(`${where}: node "${id}" is not a box id`);
  });
  return errors;
}

/** Every rule a spec breaks, or [] (ADR-0015, section 3). */
export function validate(figure) {
  if (!isObj(figure)) return ['the spec must default-export a figure object'];
  const top = validateTop(figure);
  if (top.length) return top;
  const walked = walkLayout(figure.props.layout);
  const layout = validateLayout(walked);
  const boxIds = new Set(walked.boxes.map((b) => b.node.id));
  const edges = validateEdges(figure.props.edges, layout.ids);
  return [...layout.errors, ...edges.errors, ...validateSteps(figure.props.steps, boxIds, edges.edgeIds)];
}

/** A label for a box or group id: a box's label, a group's label, or the id itself. */
function labeller(walked) {
  const names = new Map();
  for (const { group } of walked.groups) if (group.id) names.set(group.id, group.label ?? group.id);
  for (const { node } of walked.boxes) names.set(node.id, node.label);
  return (id) => names.get(id) ?? id;
}

function describeLayout(walked) {
  const lines = [];
  const loose = [];
  const byGroup = new Map();
  for (const { node, ancestors } of walked.boxes) {
    const text = node.sub ? `${node.label} (${node.sub})` : node.label;
    const home = ancestors.findLast((a) => a.group.label != null);
    if (!home) loose.push(text);
    else byGroup.set(home, [...(byGroup.get(home) ?? []), text]);
  }
  for (const entry of walked.groups) if (byGroup.has(entry)) lines.push(`${entry.group.label}: ${byGroup.get(entry).join(', ')}.`);
  if (loose.length) lines.push(`Boxes: ${loose.join(', ')}.`);
  return lines;
}

function describeSteps(steps) {
  return steps.map((step, i) => {
    const said = step.flow.filter((b) => isObj(b) && !('edge' in b) && isStr(b.say)).map((b) => b.say);
    const head = `Scenario ${i + 1}, ${step.label}${step.caption ? `: ${step.caption}` : '.'}`;
    return [head, ...said].join(' ');
  });
}

/** Keeps whole lines within LIMITS.text characters, noting when anything was dropped. */
export function capLines(lines, slug) {
  const note = `(Shortened to ${LIMITS.text} characters; docs/figures/${slug}.ts holds the full content.)`;
  if (lines.join('\n').length <= LIMITS.text) return lines;
  const kept = [];
  let used = note.length;
  for (const line of lines) {
    if (used + line.length + 1 > LIMITS.text) break;
    kept.push(line);
    used += line.length + 1;
  }
  return [...kept, note];
}

/** Edges as sentences, "A → B (label)"; an edge that starts where the previous one ended continues it. */
export function describeEdges(edges, name) {
  const lines = [];
  let chain = '';
  let end = null;
  for (const edge of edges) {
    const arrow = ` → ${name(edge.to)}${edge.label ? ` (${edge.label})` : ''}`;
    if (chain && edge.from === end) chain += arrow;
    else {
      if (chain) lines.push(`${chain}.`);
      chain = name(edge.from) + arrow;
    }
    end = edge.to;
  }
  if (chain) lines.push(`${chain}.`);
  return lines;
}

/** The long description, derived from the spec so it cannot drift from the figure (ADR-0015, section 6). */
export function describe(figure, slug) {
  const walked = walkLayout(figure.props.layout);
  const edges = describeEdges(figure.props.edges, labeller(walked));
  const lines = [...describeLayout(walked), ...edges, ...describeSteps(figure.props.steps ?? []), ...(figure.describe ?? [])];
  return capLines(lines, slug);
}

/** Edges with box labels at both ends; an edge to a group becomes one edge per box inside it. */
export function normalizedEdges(figure) {
  const walked = walkLayout(figure.props.layout);
  const labels = new Map(walked.boxes.map((b) => [b.node.id, b.node.label]));
  const inside = new Map(walked.groups.filter((g) => g.group.id).map((g) => [g.group.id, g.boxIds]));
  const expand = (id) => (inside.get(id) ?? [id]).map((box) => labels.get(box) ?? box);
  const out = [];
  for (const edge of figure.props.edges) {
    for (const from of expand(edge.from)) {
      for (const to of expand(edge.to)) out.push(edge.label ? { from, to, label: edge.label } : { from, to });
    }
  }
  return out;
}

const ENTITIES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#x27;' };

/**
 * Text escaped for HTML and XML content and attribute values, as Python's `html.escape` escapes it
 * (quote=True). A brace that another brace follows is also written as `&#123;`, so figure text can
 * never form a `{{slot}}` of the markup; a browser reads both the same.
 */
export const escapeHtml = (s) => s.replace(/[&<>"']/g, (c) => ENTITIES[c]).replace(/\{(?=\{)/g, '&#123;');

/** Adds the accessibility markup, the credit and the embedded spec after the opening <svg> tag. */
export function decorate(svg, figure, vendor) {
  const open = /^<svg [^>]*>\n?/.exec(svg);
  if (!open) throw new Error('toSvg returned no opening <svg> tag');
  const tag = open[0].replace('<svg ', '<svg role="img" aria-labelledby="figure-title figure-desc" ');
  const credit = `interfig (c) 2025 Vectorize AI, Inc. MIT ${vendor.repo}/tree/${vendor.commit}/${vendor.path}`;
  const spec = JSON.stringify({ props: figure.props }).replaceAll(']]>', ']]\\u003e');
  const head = `<title id="figure-title">${escapeHtml(figure.title)}</title>\n` +
    `<desc id="figure-desc">${escapeHtml(figure.alt)}</desc>\n<!-- ${credit} -->\n${SPEC_OPEN}${spec}${SPEC_CLOSE}\n`;
  return tag + head + svg.slice(open[0].length);
}

/** The slots of the figure markup; see `markup`. */
export const SLOTS = Object.freeze({ base: '{{base}}', link: '{{link}}' });

/**
 * The HTML of one figure outside the player: a <figure> with a <picture> of both SVGs and the
 * caption, a link to the interactive figure, and a <details> text description
 * (docs/adr/0016-figures-for-adopters.md, section 4). This is the one markup source: every caller
 * reads it from the figure's JSON `html` and only fills the slots.
 *
 * - `{{base}}` is the URL prefix of docs/assets/figures for the reader, inserted as given:
 *   relative on the site and in the README, absolute in the wiki.
 * - `{{link}}` is the URL of the interactive figure, HTML-escaped. It sits on a line of its own;
 *   a caller without such a URL drops that line.
 *
 * The static SVG has no scenario area, so it is shorter than the animated one: each image carries
 * its own size, and the browser reserves the size of the one it picks, with no letterboxing or
 * layout shift under reduced motion (BUG-1002).
 */
export function markup(meta) {
  const { slug } = meta;
  const items = meta.text.map((line) => `<li>${escapeHtml(line)}</li>`).join('\n');
  return [
    `<figure class="praetor-figure" id="fig-${slug}" data-figure="${slug}" aria-describedby="fig-${slug}-text">`,
    '<picture>',
    `<source media="(prefers-reduced-motion: reduce)" srcset="${SLOTS.base}/${slug}.static.svg" ` +
      `width="${meta.static_width}" height="${meta.static_height}">`,
    `<img src="${SLOTS.base}/${slug}.svg" alt="${escapeHtml(meta.alt)}" width="${meta.width}" height="${meta.height}" loading="lazy">`,
    '</picture>',
    `<figcaption>${escapeHtml(meta.title)}</figcaption>`,
    '</figure>',
    `<p><a href="${SLOTS.link}">Open the interactive figure</a></p>`,
    `<details class="praetor-figure__text" id="fig-${slug}-text"><summary>Text description</summary>`,
    `<ul>\n${items}\n</ul>`,
    '</details>',
  ].join('\n');
}

/** The intrinsic size of an SVG's opening tag, rounded up to whole pixels. */
export function svgSize(svg) {
  const size = /^<svg [^>]*?width="([\d.]+)" height="([\d.]+)"/.exec(svg);
  if (!size) throw new Error('toSvg returned an <svg> tag without width and height');
  return { width: Math.ceil(Number(size[1])), height: Math.ceil(Number(size[2])) };
}

/**
 * The three committed outputs for one validated figure. The static SVG drops the steps, and with
 * them the narration and card area, so its intrinsic size differs from the animated one: the JSON
 * records both, and its `html` (see `markup`) gives each <picture> source its own size.
 */
export function render(figure, slug, specBytes, context) {
  const svg = decorate(toSvg(figure.props), figure, context.vendor);
  const still = decorate(toSvg({ ...figure.props, steps: [] }), figure, context.vendor);
  const stillSize = svgSize(still);
  const meta = {
    slug,
    title: figure.title,
    alt: figure.alt,
    text: describe(figure, slug),
    evidence: figure.evidence,
    spec_sha256: sha256(specBytes),
    engine: { commit: context.vendor.commit, sha256: context.engine },
    svg_sha256: sha256(svg),
    static_sha256: sha256(still),
    ...svgSize(svg),
    static_width: stillSize.width,
    static_height: stillSize.height,
    edges: normalizedEdges(figure),
  };
  meta.html = markup(meta);
  return { [`${slug}.svg`]: svg, [`${slug}.static.svg`]: still, [`${slug}.json`]: `${JSON.stringify(meta, null, 2)}\n` };
}
