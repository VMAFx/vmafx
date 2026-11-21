- **Documentation site:** CI now scans the built site with `pa11y-ci` and
  axe-core in the light and dark palettes, and a planted-defect fixture proves
  the check can fail; retained deviations need a dated reason in
  `scripts/docs/accessibility-allowlist.json` (issue 2552, WCAG 2.2 AA goal).
