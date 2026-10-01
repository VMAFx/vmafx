// Serves and publishes the files a figure page loads besides its own HTML
// (docs/adr/0016-figures-for-adopters.md, section 6). It imports only node: builtins.
//
// A mount is one directory of files published under one site path (`uri`, relative to the site's
// base path). The Astro integration (astro.mjs) mounts the committed figure outputs at FIGURES_URI
// and the committed player at PLAYER_URI: its development server answers from them through
// `staticHandler`, and `astro build` copies them into the built site through `publish`. The smoke
// test (smoke.mjs) serves a whole built site as one mount at the site's base path through `serve`.
// The MkDocs hook publishes the same files at the same site paths from Python (DIST_URI and the
// docs_dir copy of docs/assets/figures in mkdocs_hook.py).
import { createServer } from 'node:http';
import { copyFileSync, mkdirSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { extname, join, resolve, sep } from 'node:path';

/** Where a site serves the committed figure outputs (docs/assets/figures/), relative to its base path. */
export const FIGURES_URI = 'assets/figures';
/** Where a site serves the committed player files (tools/figures/dist/), relative to its base path. */
export const PLAYER_URI = 'assets/javascripts/figures';
/** The files one mount publishes: dist/ holds three, a figure three per spec (LIMITS.specs in core.mjs is 256). */
export const MAX_MOUNT_FILES = 1024;
/** How long the smoke server waits for a request (HISS-02). */
const REQUEST_TIMEOUT_MS = 10_000;
const HEADERS_TIMEOUT_MS = 5_000;
export const TYPES = Object.freeze({
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.ico': 'image/x-icon',
  '.xml': 'application/xml', '.txt': 'text/plain; charset=utf-8', '.woff2': 'font/woff2',
});

/** A site's base path with one leading and one trailing slash: '' and '/' give '/', 'docs' gives '/docs/'. */
export function basePath(base = '/') {
  const inner = String(base).split('/').filter(Boolean).join('/');
  return inner ? `/${inner}/` : '/';
}

/**
 * The file status of `path`, or null when nothing readable is there (missing, under a file, a
 * symbolic-link loop, a name too long); any other failure is an error that names the path. The
 * figure checks (`statOf` in checks.mjs) read paths through this too.
 */
export function statOrNull(path) {
  try {
    return statSync(path);
  } catch (error) {
    if (['ENOENT', 'ENOTDIR', 'ELOOP', 'ENAMETOOLONG'].includes(error.code)) return null;
    throw new Error(`cannot read ${path}: ${error.message}`, { cause: error });
  }
}


/**
 * What a request for `pathname` gets from `mounts` under `base`: `{ status: 0 }` when no mount's
 * URL prefix claims it, a 400, 403 or 404 status when one does but names no file inside its
 * directory, and `{ status: 200, file, type }` otherwise. A directory answers with its index.html.
 */
export function locate(mounts, base, pathname) {
  let decoded;
  try {
    decoded = decodeURIComponent(pathname);
  } catch {
    return { status: 400 };
  }
  const root = basePath(base);
  const mount = mounts.find((candidate) => decoded.startsWith(candidate.uri ? `${root}${candidate.uri}/` : root));
  if (!mount) return { status: 0 };
  const dir = resolve(mount.dir);
  const rest = decoded.slice(mount.uri ? root.length + mount.uri.length + 1 : root.length);
  let file = resolve(dir, `.${sep}${rest}`);
  if (file !== dir && !file.startsWith(dir + sep)) return { status: 403 };
  if (statOrNull(file)?.isDirectory()) file = join(file, 'index.html');
  if (!statOrNull(file)?.isFile()) return { status: 404 };
  return { status: 200, file, type: TYPES[extname(file).toLowerCase()] ?? 'application/octet-stream' };
}

/**
 * A request handler that answers from `mounts` under `base`. A request no mount claims goes to
 * `next` when the caller passes one (a development server's middleware chain), else gets a 404.
 */
export function staticHandler(mounts, base = '/') {
  return (request, response, next) => {
    let found;
    let body;
    try {
      found = locate(mounts, base, new URL(request.url ?? '/', 'http://127.0.0.1').pathname);
      if (found.status === 200) body = readFileSync(found.file);
    } catch {
      found = { status: 500 };
    }
    if (found.status === 0 && typeof next === 'function') return void next();
    if (found.status !== 200) return void response.writeHead(found.status || 404).end();
    response.writeHead(200, { 'content-type': found.type });
    response.end(body);
  };
}

/** A static file server for the built site in `site`, served under `base` on 127.0.0.1, with bounded request timeouts. */
export function serve(site, base = '/') {
  const server = createServer(staticHandler([{ uri: '', dir: site }], base));
  server.requestTimeout = REQUEST_TIMEOUT_MS;
  server.headersTimeout = HEADERS_TIMEOUT_MS;
  return new Promise((resolveServer, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => resolveServer(server));
  });
}

/** The regular files directly in `dir`, sorted; none when it does not exist. */
export function mountFiles(dir) {
  if (!statOrNull(dir)?.isDirectory()) return [];
  const names = readdirSync(dir, { withFileTypes: true }).filter((entry) => entry.isFile()).map((entry) => entry.name).sort();
  if (names.length > MAX_MOUNT_FILES) throw new Error(`${dir} holds more than ${MAX_MOUNT_FILES} files`);
  return names;
}

/**
 * Copies the files of every mount into the built site at `outDir`, each under its mount's `uri`.
 * A file the site already holds at that path is kept, as the MkDocs hook keeps a docs_dir file,
 * and returned in `kept`; `copied` lists the site paths written.
 */
export function publish(mounts, outDir) {
  const copied = [];
  const kept = [];
  for (const mount of mounts) {
    const names = mountFiles(mount.dir);
    if (names.length) mkdirSync(join(outDir, mount.uri), { recursive: true });
    for (const name of names) {
      const uri = mount.uri ? `${mount.uri}/${name}` : name;
      const target = join(outDir, mount.uri, name);
      if (statOrNull(target)) {
        kept.push(uri);
        continue;
      }
      copyFileSync(join(mount.dir, name), target);
      copied.push(uri);
    }
  }
  return { copied, kept };
}
