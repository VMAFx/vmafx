# Accessibility

This file states what VMAFx aims for on accessibility, what is known today,
and how to report a barrier. It is a statement of intent and of the known
state, not a claim of conformance to any standard. No part of the project has
been tested for accessibility yet.

## Commitment

VMAFx is used by people with a range of abilities and tools, including screen
readers, keyboard-only navigation, high-contrast and reduced-motion settings,
and terminals that do not render colour. Barriers that you report will be
treated as defects and fixed on a best-effort basis (see
[What to expect](CONTRIBUTING.md#what-to-expect)).

## Goal

The goal for the documentation site and for the generated dashboards is
[WCAG 2.2](https://www.w3.org/TR/WCAG22/) level AA. This is a goal. The
project does not claim conformance with it, and will not do so until an
automated check and a manual pass say so and the result is recorded here.

## Scope

This statement covers:

- the documentation site (<https://vmafx.dev/>) and the Markdown
  files in this repository, including `README.md`;
- the command-line tools (`vmaf` / `vmafx`) and their output;
- the Grafana dashboards generated from `pkg/observability/obsgen`
  (`deploy/grafana/dashboards/`);
- the output of the ffmpeg and GStreamer filters, which is the text those
  hosts print;
- the MCP tools (`docs/mcp/`), which return machine-readable results;
- the tester kits (`tools/rc1-tester`), which write plain files.

The project does not serve a web application. A planned report viewer
([#2316](https://github.com/VMAFx/vmafx/issues/2316)) will need its own
accessibility criteria before it ships. GitHub's own interface (issues,
pull requests, Discussions) is not part of this project; report barriers in
it to GitHub.

## Supported environments

- **Documentation site:** current releases of the major evergreen browsers.
  No browser and assistive-technology combination has been tested, so none is
  listed as supported.
- **Command line:** the terminals of the platforms in the
  [getting started guide](docs/getting-started/index.md). Progress output is
  shown only when standard error is a terminal; machine-readable output is
  available with `--json`, `--xml` and `--csv`.

## What exists today

Verified by reading the sources, not by testing with assistive technology:

- Images in the Markdown documentation carry alternative text.
- The documentation figures carry a title and a description, and charts take
  their text description from the image alternative text.
- The documentation site stylesheet defines a visible keyboard focus outline
  and honours the `prefers-reduced-motion` and `forced-colors` settings
  (`docs/stylesheets/vmafx.css`).
- The site follows the operating system's light or dark setting.

## Known limitations

- No accessibility audit, keyboard pass or screen-reader pass has been done on
  any surface.
- The contrast ratios of the custom documentation palette have not been
  measured.
- There is no automated accessibility check in continuous integration.
- The command-line tools do not read `NO_COLOR` and have no `--no-color`
  option.
- Whether the generated Grafana dashboards ever convey state by colour alone
  has not been checked.
- Whether the built documentation pages declare their language has not been
  checked.
- The progress display overwrites a line in place, which some screen readers
  read poorly; it is shown only on a terminal.

Work on colour control in the command line, an automated check of the
documentation site, and the dashboards is tracked in issues labelled
[`accessibility`](https://github.com/VMAFx/vmafx/labels/accessibility).

## Report a barrier

Open an
[accessibility issue](https://github.com/VMAFx/vmafx/issues/new?template=accessibility.yml).
Please include:

- the page, command or dashboard, and the project version;
- your browser or terminal, operating system, and assistive technology with
  its version;
- what you expected, what happened, and how much it blocks you.

If you cannot use the form, ask in
[Discussions](https://github.com/VMAFx/vmafx/discussions) and say that it is
an accessibility matter. Responses are best effort and carry no guaranteed
time; see [What to expect](CONTRIBUTING.md#what-to-expect).

## For contributors

A change to the documentation, a dashboard, or any output a person reads:

- gives every image alternative text, and keeps heading levels in order;
- uses link text that makes sense out of context;
- never carries meaning by colour alone;
- keeps command output usable when it is piped, or shown without colour.

## Ownership

The maintainer listed in [MAINTAINERS.md](MAINTAINERS.md) owns this file and
reviews it whenever a surface above changes or a limitation is fixed.
