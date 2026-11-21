# Documentation accessibility check

The docs build runs `scripts/docs/check_accessibility.py` after
`mkdocs build --strict`. It scans a fixed page set with
[pa11y-ci](https://github.com/pa11y/pa11y-ci) and axe-core against WCAG 2.2 A
and AA, in the light and dark palettes. This is an automated check, not a
conformance claim or a substitute for a manual or screen-reader pass.

## What is scanned

- the home page;
- `backends/sycl/overview/` (a backend page);
- `metrics/psnr/` (a metric page);
- `development/netflix-benchmark-baselines/` (a page with generated charts);
- an ADR page;
- the search page.

The same set runs twice: once with the default palette and once with
`--force-dark-mode`, because `mkdocs.yml` selects the palette with
`prefers-color-scheme`.

## Run it locally

```bash
mkdocs build --strict
npm install --global --no-fund --no-audit pa11y-ci@5.0.0
python3 scripts/docs/check_accessibility.py --site build-docs/site
```

`pa11y-ci` downloads a Chrome for Testing build. To use an installed Chrome or
Edge instead, pass its path:

```bash
python3 scripts/docs/check_accessibility.py --site build-docs/site \
  --chrome "/usr/bin/google-chrome"
```

The check first runs `scripts/docs/accessibility-fixture.html`, which contains
an image without alt text and a dark-palette low-contrast pair. It must fail on
`image-alt` and `color-contrast`; a green site result is not trusted until that
negative control passes.

## Exceptions

Fix a finding rather than listing it. When a deviation cannot be fixed in this
change, add one entry to `scripts/docs/accessibility-allowlist.json`:

```json
{
  "page": "metrics/psnr/index.html",
  "rule": "color-contrast",
  "selector": "html > body > p:nth-child(3)",
  "reason": "why the deviation is retained and who owns it",
  "expires": "2027-01-31"
}
```

Every entry needs a reason and an expiry date. The checker fails on an expired
entry and on an entry that matches no issue, so the list cannot silently rot.
The known-limitations section of `ACCESSIBILITY.md` records the same state.
