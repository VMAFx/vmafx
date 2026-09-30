#!/usr/bin/env node
// Renders this repository's documentation figures from docs/figures/<slug>.ts with the vendored
// interfig engine under tools/figures/third_party/interfig/, and checks them. It needs Node and no
// npm package.
//
//   node tools/figures/build.mjs build      validate every spec, write docs/assets/figures/<slug>.{svg,static.svg,json}
//   node tools/figures/build.mjs check      validate, render in memory, and compare the bytes with the committed files
//   node tools/figures/build.mjs sources    check hashes, sizes, markup, spec/JSON pairs, fences, evidence and the README block
//                                           (the pages of every mkdocs.yml or astro.config.* at the root, unless --config names one)
//   node tools/figures/build.mjs site       after mkdocs build or astro build: every diagram fence became a diagram that resolves
//   node tools/figures/build.mjs portable   render figures for READMEs and wiki pages, which run no JavaScript
//
// In a repository with no spec and no committed output, `check` and `sources` print that they
// skipped and why, and exit 0.
//
// The render core is core.mjs; its bytes and the vendored render files make up the engine hash
// recorded in every figure's JSON. The checks are checks.mjs. Neither this wrapper nor the checks
// are hashed, so editing them leaves the figures current.
//
// `toSvg` is imported from the vendored source directly. Upstream's scripts/figure-svg.mjs is not
// called: it registers a module hook that Node 26 reports as deprecated (DEP0205), and it passes
// no title or description. The spec is embedded with the same <metadata id="figure-spec"> markers,
// so `node tools/figures/third_party/interfig/upstream/scripts/figure-svg.mjs --spec <file>` still
// reads it back.
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import { CheckError, OUT_DIR, REBUILD, ROOT, SLUG, SPEC_DIR, checkSite, engineHash, portable, readText, siteUrl, sourceSites, sources } from './checks.mjs';
import { LIMITS, render, sha256, validate } from './core.mjs';

export { OUT_DIR, REBUILD, ROOT, SPEC_DIR, engineHash };
export const VENDOR_JSON = 'tools/figures/third_party/interfig/vendor.json';
const TIMEOUT_MS = 120_000;

/** Rejects when `promise` has not settled within `ms` (HISS-02: every I/O is bounded). */
export async function withTimeout(promise, ms, what) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${what} did not finish within ${ms} ms`)), ms);
  });
  try {
    return await Promise.race([promise, timeout]);
  } finally {
    clearTimeout(timer);
  }
}

/** Spec slugs under docs/figures, sorted, each a lowercase kebab-case name. */
export function listSpecs(root = ROOT) {
  const dir = join(root, SPEC_DIR);
  if (!existsSync(dir)) return [];
  const names = readdirSync(dir).filter((n) => n.endsWith('.ts')).sort();
  if (names.length > LIMITS.specs) throw new Error(`${SPEC_DIR} holds more than ${LIMITS.specs} specs`);
  const bad = names.filter((n) => !SLUG.test(n.slice(0, -3)));
  if (bad.length) throw new Error(`${SPEC_DIR}: spec names must be lowercase kebab-case: ${bad.join(', ')}`);
  return names.map((n) => n.slice(0, -3));
}

async function loadSpec(root, slug) {
  const file = join(root, SPEC_DIR, `${slug}.ts`);
  const bytes = readFileSync(file);
  const url = `${pathToFileURL(file).href}?sha256=${sha256(bytes)}`;
  const mod = await withTimeout(import(url), TIMEOUT_MS, `loading ${SPEC_DIR}/${slug}.ts`);
  return { bytes, figure: mod.default };
}

/** Every figure's outputs in memory, or the validation errors that stopped them. */
export async function renderAll(root = ROOT) {
  const context = { vendor: JSON.parse(readFileSync(join(root, VENDOR_JSON), 'utf8')), engine: engineHash(root) };
  const outputs = {};
  const errors = [];
  const slugs = listSpecs(root);
  for (const slug of slugs) {
    const { bytes, figure } = await loadSpec(root, slug);
    const problems = validate(figure);
    if (problems.length) errors.push(...problems.map((p) => `${SPEC_DIR}/${slug}.ts: ${p}`));
    else Object.assign(outputs, render(figure, slug, bytes, context));
  }
  return { slugs, outputs, errors };
}

/** Committed output names under docs/assets/figures: <slug>.svg, <slug>.static.svg, <slug>.json. */
function committedOutputs(dir) {
  if (!existsSync(dir)) return [];
  return readdirSync(dir).filter((n) => /^[a-z0-9-]+(\.static)?\.svg$|^[a-z0-9-]+\.json$/.test(n)).sort();
}

/** The line `check` and `sources` print when the repository has nothing for them to check. */
export const NO_FIGURES = `skipped: this repository has no figure spec (${SPEC_DIR}/*.ts) and no figure output (${OUT_DIR})`;

/**
 * Whether the repository at `root` has a figure to check: a spec under docs/figures or a committed
 * output under docs/assets/figures. Without either, `check` and `sources` pass with NO_FIGURES, so
 * the managed Makefile target, the documentation workflow step and a direct run skip alike on
 * every platform (docs/adr/0016-figures-for-adopters.md, section 5). An output left without its
 * spec is still checked and fails.
 */
export function hasFigures(root = ROOT) {
  const dir = join(root, SPEC_DIR);
  const specs = existsSync(dir) ? readdirSync(dir).filter((n) => n.endsWith('.ts')) : [];
  return specs.length > 0 || committedOutputs(join(root, OUT_DIR)).length > 0;
}

/** Differences between rendered outputs and the files in `dir`. */
export function compareOutputs(outputs, dir) {
  const problems = [];
  for (const [name, text] of Object.entries(outputs)) {
    const file = join(dir, name);
    if (!existsSync(file)) problems.push(`${OUT_DIR}/${name} is missing`);
    else if (readFileSync(file, 'utf8') !== text) problems.push(`${OUT_DIR}/${name} is stale`);
  }
  for (const name of committedOutputs(dir)) if (!(name in outputs)) problems.push(`${OUT_DIR}/${name} has no spec`);
  return problems;
}

function writeOutputs(outputs, dir) {
  mkdirSync(dir, { recursive: true });
  for (const name of committedOutputs(dir)) if (!(name in outputs)) rmSync(join(dir, name));
  for (const [name, text] of Object.entries(outputs)) writeFileSync(join(dir, name), text);
}

async function runBuild(root) {
  const { slugs, outputs, errors } = await renderAll(root);
  if (errors.length) return errors;
  writeOutputs(outputs, join(root, OUT_DIR));
  console.log(`figures: wrote ${slugs.length} figure(s) to ${OUT_DIR}`);
  return [];
}

async function runCheck(root) {
  if (!hasFigures(root)) return report([], NO_FIGURES);
  const { slugs, outputs, errors } = await renderAll(root);
  if (errors.length) return errors;
  const problems = compareOutputs(outputs, join(root, OUT_DIR));
  if (problems.length) return [...problems, `rebuild with: ${REBUILD}`];
  console.log(`figures: ${slugs.length} spec(s) valid; ${OUT_DIR} matches them byte for byte`);
  return [];
}

/** Prints the success line when there are no findings; returns the findings. */
function report(errors, success) {
  if (errors.length === 0) console.log(`figures: ${success}`);
  return errors;
}

/**
 * `sources` over the pages of the sites `sourceSites` names: `--config` and `--docs` when given,
 * else every site configuration at the repository root, so the managed target and the workflow step
 * read a Starlight site's pages as they read an MkDocs site's. The success line names the pages read.
 */
function runSources(root, { values }) {
  const repository = resolve(values.root ?? root);
  if (!hasFigures(repository)) return report([], NO_FIGURES);
  const read = sourceSites(repository, values.docs, values.config).map((site) => `${site.docs} (${site.config})`);
  const errors = sources(repository, values.docs, values.config, values.readme);
  return report(errors, `figure sources, hashes, markup and evidence are consistent; pages read: ${read.join(', ')}.`);
}

function runSite(_root, { values }) {
  if (!values.config || !values.docs || !values.site) throw new UsageError('site needs --config, --docs and --site');
  const { errors, diagrams } = checkSite(values.config, values.docs, values.site, values.base);
  return report(errors, `${diagrams} diagram(s) under ${values.docs} render.`);
}

function runPortable(root, { values, positionals }) {
  const modes = [values.base !== undefined, values.wiki, values.write].filter(Boolean);
  if (modes.length !== 1 || positionals.length === 0) throw new UsageError('portable needs one of --base, --wiki or --write, and a file');
  const repository = resolve(values.root ?? root);
  const base = values.wiki ? siteUrl(readText(resolve(repository, values.config))) : values.base ?? null;
  return report(portable(positionals, base, repository), `rendered figures in ${positionals.length} file(s).`);
}

const STRING = { type: 'string' };
const FLAG = { type: 'boolean', default: false };
/** Each command: its options (node:util parseArgs), whether it takes files, and what it runs. */
const COMMANDS = {
  build: { options: {}, run: (root) => runBuild(root) },
  check: { options: {}, run: (root) => runCheck(root) },
  sources: {
    options: { root: STRING, docs: STRING, config: STRING, readme: { ...STRING, default: 'README.md' } },
    run: runSources,
  },
  site: { options: { config: STRING, docs: STRING, site: STRING, base: STRING }, run: runSite },
  portable: {
    options: { base: STRING, wiki: FLAG, write: FLAG, root: STRING, config: { ...STRING, default: 'mkdocs.yml' } },
    files: true,
    run: runPortable,
  },
};
const USAGE = [
  'usage: node build.mjs build|check',
  '       node build.mjs sources [--root DIR] [--docs DIR] [--config FILE] [--readme FILE]',
  '       node build.mjs site --config FILE --docs DIR --site DIR [--base PATH]',
  '       node build.mjs portable (--base URL | --wiki | --write) [--root DIR] [--config FILE] FILE...',
].join('\n');

/** A command line this wrapper cannot run. */
class UsageError extends Error {}

/** The parsed arguments of `command`, or UsageError. */
function parse(command, args) {
  try {
    return parseArgs({ args, options: command.options, allowPositionals: command.files === true, strict: true });
  } catch (error) {
    throw new UsageError(error.message);
  }
}

/**
 * Runs one command against the repository at `root`: exit status 0, 1 on findings, 2 on misuse or
 * on an input a check cannot read.
 */
export async function main(argv, root = ROOT) {
  const command = Object.hasOwn(COMMANDS, argv[0] ?? '') ? COMMANDS[argv[0]] : null;
  try {
    if (!command) throw new UsageError(argv.length ? `unknown command ${argv[0]}` : 'no command');
    const problems = await command.run(root, parse(command, argv.slice(1)));
    for (const problem of problems) console.error(`figures: ${problem}`);
    return problems.length ? 1 : 0;
  } catch (error) {
    if (error instanceof UsageError) console.error(`figures: ${error.message}\n${USAGE}`);
    else if (error instanceof CheckError) console.error(`figures: ${error.message}`);
    else throw error;
    return 2;
  }
}

if (import.meta.main ?? (process.argv[1] !== undefined && fileURLToPath(import.meta.url) === resolve(process.argv[1]))) {
  main(process.argv.slice(2)).then(
    (code) => { process.exitCode = code; },
    (error) => { console.error(`figures: ${error.message}`); process.exitCode = 2; },
  );
}
