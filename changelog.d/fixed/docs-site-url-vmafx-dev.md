- **The documentation site names `https://vmafx.dev/` as its address.** The
  site has been served from `vmafx.dev` while `mkdocs.yml` still set
  `site_url` to `https://vmafx.github.io/vmafx/`, so every page's canonical
  link and all sitemap entries pointed at the GitHub Pages host, and the 404
  page loaded its styles from `/vmafx/assets/`, which does not exist on
  `vmafx.dev`. `site_url`, the Prometheus `runbook_url` annotations
  (`deploy/prometheus/vmafx-rules.yaml`, the Helm `PrometheusRule`), the
  Python and Rust package `Documentation` links and the README now use
  `https://vmafx.dev/`. Links to `https://vmafx.github.io/vmafx/...` keep
  working: GitHub Pages redirects each path to the same path on `vmafx.dev`.
