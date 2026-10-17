// Astro integration: interactive figures on an Astro Starlight site
// (docs/adr/0016-figures-for-adopters.md, section 6). It imports only node: builtins and the figure
// engine's own modules, so a site that adds it installs no npm package and its lockfile does not
// change.
//
//   // astro.config.mjs
//   import figures from './tools/figures/astro.mjs';
//   export default defineConfig({
//     integrations: [starlight({ customCss: ['./tools/figures/figures.css'] }), figures()],
//   });
//
// * A Markdown plugin replaces each ```figure code block, in .md and .mdx pages alike, with the
//   markup tools/figures/core.mjs wrote into docs/assets/figures/<slug>.json as `html`: `{{base}}`
//   becomes the root-absolute URL of the figures under the site's `base`, and the `{{link}}` line is
//   dropped (`fillSlots` in checks.mjs). It works on the Markdown syntax tree, so it needs no fence
//   scanner. A block naming a figure that has no JSON fails the build of an .mdx page; on an .md
//   page Astro's content loader logs the error and builds the page without its content, so
//   `make docs-figures`, whose `sources` check fails on the block, is the gate for both.
//   `addFigurePlugin` picks the plugin the site's Markdown processor runs. From Astro 6.4, Markdown
//   and MDX render through `markdown.processor`: Sätteri, Astro 7's default (`satteriFigures`), or
//   `unified()` from @astrojs/markdown-remark, Astro 6's default and an option on Astro 7
//   (`remarkFigures`). Astro 6.3 and earlier have no processor and take `remarkFigures` in
//   `markdown.remarkPlugins`.
// * Astro keeps the rendered .md pages of a content collection in node_modules/.astro/data-store.json
//   and renders one again only when its own bytes change, or when the Astro configuration does
//   (the digest check in astro/dist/content/content-layer.js). The plugin's options therefore carry
//   a digest of the figure JSON (`figuresDigest`): a rebuilt figure changes the configuration, so a
//   warm `astro build` renders every page again. The development server restarts when a figure JSON
//   changes, is added or is removed (`watchFigures`), which runs the setup, and so the digest, again.
// * A head script on every page imports the loader, tools/figures/player/loader.js, which mounts the
//   player from the props each SVG embeds; the loader imports player.js beside it.
// * The development server answers the figure files and the player files from the repository, and
//   `astro build` copies them into the built site, both through serve.mjs at FIGURES_URI and
//   PLAYER_URI, the site paths the MkDocs hook publishes them at. A file the built site already
//   holds at one of those paths (from public/, for example) is kept and reported.
//
// The stylesheet, tools/figures/figures.css, goes into Starlight's `customCss`; it reads Starlight's
// theme variables where Material's are absent.
//
// Option `root`: the repository root that holds docs/assets/figures/, as a path or a file: URL. By
// default it is the directory two levels above this file, where build.mjs writes the figures.
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { CheckError, OUT_DIR, ROOT, figureMeta, figureSlug, filesDigest, fillSlots } from './checks.mjs';
import { FIGURES_URI, PLAYER_URI, basePath, mountFiles, publish, staticHandler } from './serve.mjs';

export const NAME = 'praetor-figures';
/** The committed player files beside this module, as the MkDocs hook reads them beside itself. */
const PLAYER_DIR = fileURLToPath(new URL('./player', import.meta.url));
const LOADER_URI = `${PLAYER_URI}/loader.js`;
/** Markdown nodes one page may hold before the walk stops (HISS-02). */
export const MAX_NODES = 200_000;
/** How long the development server waits after the last figure JSON event before it restarts: one rebuild writes every JSON. */
export const RESTART_DELAY_MS = 250;

/** A repository root given as a path or a file: URL, as a path; the default root when none is given. */
export function rootPath(root) {
  if (root === undefined || root === null) return ROOT;
  if (root instanceof URL || String(root).startsWith('file:')) return fileURLToPath(root);
  return String(root);
}

/** The site paths of the figure files and of the player files, each with the directory it is served from. */
export const figureMounts = (root) => [
  { uri: FIGURES_URI, dir: join(rootPath(root), ...OUT_DIR.split('/')) },
  { uri: PLAYER_URI, dir: PLAYER_DIR },
];

/** The URL of docs/assets/figures on a site built for `base`: '/assets/figures', '/docs/assets/figures'. */
export const figureBase = (base) => `${basePath(base)}${FIGURES_URI}`;

const isFigureJson = (name) => name.endsWith('.json');

/**
 * One value for the figure JSON in `figuresDir` (`filesDigest` of its .json files) that changes when
 * a rebuild changes, adds or removes a figure. A missing directory gives the digest of no files.
 */
export const figuresDigest = (figuresDir) => filesDigest(figuresDir, mountFiles(figuresDir).filter(isFigureJson));

/**
 * Restarts the development server `server` (Vite's, which Astro's restart replaces) once the figure
 * JSON in `figuresDir` has been quiet for `delay` ms after a change, an addition or a removal. The
 * directory is added to the server's watcher, which watches only the Astro project otherwise.
 */
export function watchFigures(server, figuresDir, logger, delay = RESTART_DELAY_MS) {
  const dir = resolve(figuresDir);
  let timer = null;
  const restart = () => {
    logger?.info(`${dir}: a figure changed, so the development server restarts to render it again`);
    Promise.resolve(server.restart()).catch((error) => logger?.error(`figures: the development server did not restart: ${error.message}`));
  };
  const onEvent = (path) => {
    if (dirname(resolve(path)) !== dir || !isFigureJson(path)) return;
    clearTimeout(timer);
    timer = setTimeout(restart, delay);
    timer.unref?.();
  };
  server.watcher.add(dir);
  for (const event of ['add', 'change', 'unlink']) server.watcher.on(event, onEvent);
}

/** The head script that imports the loader on a site built for `base`; a failed import is logged, not thrown. */
export function loaderScript(base) {
  const url = JSON.stringify(`${basePath(base)}${LOADER_URI}`);
  return `import(${url}).catch((error) => console.error('figures: cannot load the figure loader', error));`;
}

const isFigureBlock = (node) => node?.type === 'code' && typeof node.lang === 'string' && node.lang.toLowerCase() === 'figure';

/** The HTML node that replaces a ```figure block, or null with the finding recorded in `errors`. */
function figureNode(node, options, errors) {
  try {
    const slug = figureSlug(String(node.value ?? '').split('\n'));
    return { type: 'html', value: fillSlots(figureMeta(options.figuresDir, slug), options.base), position: node.position };
  } catch (error) {
    if (!(error instanceof CheckError)) throw error;
    const line = node.position?.start?.line;
    errors.push(line ? `line ${line}: ${error.message}` : error.message);
    return null;
  }
}

/**
 * Replaces every ```figure code block in a Markdown syntax tree with its figure markup, walking the
 * tree with an explicit stack (HISS-01), and returns the findings for the blocks it could not
 * render, which stay in place. `options.figuresDir` holds the JSON; `options.base` fills `{{base}}`.
 */
export function replaceFigures(tree, options) {
  const errors = [];
  const stack = [tree];
  for (let seen = 0; stack.length > 0; seen++) {
    if (seen >= MAX_NODES) throw new Error(`figures: the page holds more than ${MAX_NODES} Markdown nodes`);
    const node = stack.pop();
    if (!Array.isArray(node?.children)) continue;
    node.children.forEach((child, index) => {
      const replaced = isFigureBlock(child) ? figureNode(child, options, errors) : null;
      if (replaced) node.children[index] = replaced;
      else stack.push(child);
    });
  }
  return errors;
}

/**
 * The remark plugin: figure blocks become figure markup, and a block that cannot fails the page.
 * `options.digest` is not read here: it is in the options so that the Astro configuration, which
 * holds them, changes whenever a figure does.
 */
export function remarkFigures(options) {
  return (tree, file) => {
    const errors = replaceFigures(tree, options);
    if (errors.length) throw pageError(file?.path, errors);
  };
}

/** The error that fails the page at `path` (a path, a file: URL or nothing) for the findings `errors`. */
function pageError(path, errors) {
  const page = path ? rootPath(path) : 'a Markdown page';
  return new Error(`figures: ${page}: ${errors.join('; ')}`);
}

/**
 * The node that puts the figure markup `html` on a page Sätteri compiles as `format`. A Markdown
 * page takes an `html` node, which Sätteri writes out unchanged. Sätteri's MDX compiler refuses an
 * `html` node unless the MDX integration optimizes static content, which Starlight's own mdx() does
 * and an mdx() the site registers itself does not by default. An MDX page therefore takes
 * `<Fragment set:html>` with the markup as a string: the element Astro's static optimization writes
 * itself, from the Fragment every MDX page receives (the `components` of Content in
 * @astrojs/mdx/dist/vite-plugin-mdx-postprocess.js). The markup stays one string, byte for byte:
 * re-parsed as JSX, its unclosed <source> and <img> would swallow the elements after them, and its
 * braces would become expressions.
 */
function figureMarkupNode(html, format) {
  if (format !== 'mdx') return { type: 'html', value: html };
  const markup = { type: 'mdxJsxAttribute', name: 'set:html', value: html };
  return { type: 'mdxJsxFlowElement', name: 'Fragment', attributes: [markup], children: [] };
}

/**
 * The Sätteri plugin, for Astro 7's default Markdown processor: each figure block becomes its figure
 * markup (`figureMarkupNode`, by the page's `ctx.sourceFormat`), and a block that cannot fails the
 * page, as `remarkFigures` does. Sätteri reports a node's line only to a plugin that asks for
 * positions. The options ride on the plugin as `figures`, which Sätteri does not read, so that the
 * Astro configuration changes whenever a figure does.
 */
export function satteriFigures(options) {
  return {
    name: NAME,
    options: { position: true },
    figures: options,
    code(node, ctx) {
      if (!isFigureBlock(node)) return undefined;
      const errors = [];
      const replaced = figureNode(node, options, errors);
      if (!replaced) throw pageError(ctx?.fileURL, errors);
      return figureMarkupNode(replaced.value, ctx?.sourceFormat);
    },
  };
}

/**
 * Adds the figure plugin with `options` to the Markdown processor of the site Astro set up with
 * `config`, and returns the plugin kind: 'satteri' or 'unified' into `markdown.processor`, where
 * Astro 6.4 and later keep the plugins their Markdown and MDX pages run, and 'remark' through
 * `updateConfig` into `markdown.remarkPlugins` for Astro 6.3 and earlier, which have no processor.
 * A processor that takes neither plugin fails the setup, since every figure block would stay a
 * code block.
 */
export function addFigurePlugin(config, updateConfig, options) {
  const processor = config.markdown?.processor;
  if (!processor) {
    updateConfig({ markdown: { remarkPlugins: [[remarkFigures, options]] } });
    return 'remark';
  }
  const lists = processor.options ?? {};
  if (Array.isArray(lists.mdastPlugins)) {
    lists.mdastPlugins.push(satteriFigures(options));
    return 'satteri';
  }
  if (Array.isArray(lists.remarkPlugins)) {
    lists.remarkPlugins.push([remarkFigures, options]);
    return 'unified';
  }
  throw new Error(`figures: the Markdown processor ${JSON.stringify(processor.name ?? '')} runs neither Sätteri nor remark plugins, so figure blocks would stay code blocks; use Astro's default processor or unified() from @astrojs/markdown-remark`);
}

/** Copies the figure and player files into the built site at `dir` (a path or a file: URL), logging what it could not. */
export function publishFigures(mounts, dir, logger) {
  const { copied, kept } = publish(mounts, rootPath(dir));
  for (const uri of kept) logger?.warn(`${uri}: the built site already holds this path, so the figures integration does not publish over it`);
  if (!copied.includes(LOADER_URI) && !kept.includes(LOADER_URI)) logger?.warn(`${PLAYER_DIR} holds no loader.js, so every figure stays its SVG`);
  return { copied, kept };
}

/** The integration: `figures()` in the `integrations` list of astro.config.mjs. */
export default function figures(options = {}) {
  const mounts = figureMounts(options.root);
  return {
    name: NAME,
    hooks: {
      'astro:config:setup': ({ config, updateConfig, injectScript }) => {
        const base = config.base ?? '/';
        const figuresDir = mounts[0].dir;
        const plugin = { figuresDir, base: figureBase(base), digest: figuresDigest(figuresDir) };
        addFigurePlugin(config, updateConfig, plugin);
        injectScript('head-inline', loaderScript(base));
      },
      // Astro's development server cuts the site's base path from a request before an
      // integration's middleware sees it (baseMiddleware in astro/dist/vite-plugin-astro-server/base.js),
      // so the mounts are matched from the root.
      'astro:server:setup': ({ server, logger }) => {
        server.middlewares.use(staticHandler(mounts, '/'));
        watchFigures(server, mounts[0].dir, logger);
      },
      'astro:build:done': ({ dir, logger }) => {
        publishFigures(mounts, dir, logger);
      },
    },
  };
}
