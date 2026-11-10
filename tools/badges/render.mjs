// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// Render README status badges as SVG with badge-maker, the shields.io
// renderer (MIT OR Apache-2.0, https://github.com/badges/shields).
//
// Usage: node tools/badges/render.mjs <spec.json> <out-dir>
//
// <spec.json> is written by `scripts/ci/readme_badges.py spec`: a list of
// {"slug", "label", "message", "color"} objects. Each entry becomes
// <out-dir>/<slug>.svg in the for-the-badge style the README header uses
// (docs/README-STANDARD.md). The script reads no network and no
// environment; it fails on any entry outside the allow-listed shape.

import { readFileSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { makeBadge } from 'badge-maker'

const LABEL_COLOR = '#111a35'
const SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/
const COLOR = /^#[0-9a-f]{6}$/
const TEXT = /^[\x20-\x7e]{1,40}$/
const MAX_BADGES = 32

function checked(entry, index) {
  const keys = Object.keys(entry ?? {}).sort().join(',')
  if (keys !== 'color,label,message,slug') {
    throw new Error(`entry ${index}: keys ${keys}, want color,label,message,slug`)
  }
  const { slug, label, message, color } = entry
  if (!SLUG.test(slug)) throw new Error(`entry ${index}: bad slug ${JSON.stringify(slug)}`)
  if (!TEXT.test(label)) throw new Error(`${slug}: bad label ${JSON.stringify(label)}`)
  if (!TEXT.test(message)) throw new Error(`${slug}: bad message ${JSON.stringify(message)}`)
  if (!COLOR.test(color)) throw new Error(`${slug}: bad color ${JSON.stringify(color)}`)
  return entry
}

function main(argv) {
  if (argv.length !== 2) {
    throw new Error('usage: node tools/badges/render.mjs <spec.json> <out-dir>')
  }
  const [specPath, outDir] = argv
  const spec = JSON.parse(readFileSync(specPath, 'utf8'))
  if (!Array.isArray(spec) || spec.length === 0 || spec.length > MAX_BADGES) {
    throw new Error(`${specPath}: want a list of 1 to ${MAX_BADGES} badges`)
  }
  const seen = new Set()
  spec.map(checked).forEach(({ slug, label, message, color }) => {
    if (seen.has(slug)) throw new Error(`${slug}: listed twice`)
    seen.add(slug)
    const svg = makeBadge({ label, message, color, labelColor: LABEL_COLOR, style: 'for-the-badge' })
    writeFileSync(join(outDir, `${slug}.svg`), `${svg}\n`, 'utf8')
  })
  process.stdout.write(`rendered ${seen.size} badges into ${outDir}\n`)
}

try {
  main(process.argv.slice(2))
} catch (error) {
  process.stderr.write(`render.mjs: ${error.message}\n`)
  process.exitCode = 1
}
