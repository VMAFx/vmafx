- **The README badges render on github.com again.** GitHub's image proxy
  gives up after about four seconds, and the shields.io badges computed on
  request often took longer, so most badges showed as broken images. The
  workflow badges now use GitHub's own `badge.svg`, the Best Practices badge
  comes from bestpractices.dev, the release badge is static and kept current
  by release-please (`README.md` is now a release-please extra file), and the
  Scorecard badge is a static link to the Scorecard viewer. The rules are in
  [`docs/README-STANDARD.md`](docs/README-STANDARD.md#badge-sources).
