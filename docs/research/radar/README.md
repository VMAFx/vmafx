<!-- markdownlint-disable MD013 -->
# Research radar

A weekly look at public video-quality research, so that a metric, dataset or
standard that matters to VMAFx is noticed within a week and ends up as a tracked
issue instead of a stray link. The decision and its alternatives are in
[ADR-2171](../../adr/2171-research-radar.md). The recurring epic that links each
weekly sweep is [#2395](https://github.com/VMAFx/vmafx/issues/2395).

## What is watched

[`sources.yaml`](sources.yaml) is the registry. It lists public sources only: GitHub
organisations and repositories, arXiv and OpenAlex queries, standards-body
document lists, conference proceedings and datasets. An entry names a source and
says why it matters and what is known about its licence; it never describes a
person.

| Kind | Read by | What the weekly run reports |
| --- | --- | --- |
| `github_owner` | collector | new repositories, releases, pushes |
| `github_repo` | collector | releases, pushes (`releases_only` drops pushes) |
| `arxiv_query` | collector | papers submitted in the window |
| `openalex_query` | collector | works published in the window (title and abstract match) |
| `manual` | triage lane | standards documents, proceedings and datasets; no machine-readable feed |

To add a source, append an entry to `sources.yaml` (the fields are described in its
header) and run the tests:

```bash
python3 -m pytest scripts/research/tests -q
```

## Cadence

The workflow [`research-radar.yml`](../../../.github/workflows/research-radar.yml) runs
every Monday at 06:00 UTC on a hosted runner and files one issue labelled
`research-radar`, titled `Research radar digest <year>-W<week>`. It runs
[`scripts/research/radar_collect.py`](../../../scripts/research/radar_collect.py), which
uses no model and keeps no state: the digest covers the seven days before the run, and
consecutive weeks partition time exactly. Nothing changed means no issue. A source that
could not be read is listed in the digest. To run it by hand:

```bash
GITHUB_TOKEN="$(gh auth token)" python3 scripts/research/radar_collect.py \
  --days 7 --output /tmp/digest.md
```

`--days` takes 1 to 366 and `--now` fixes the window end (useful to rebuild an old week).

## Triage

An agent lane works through each digest issue and the `manual` sources, as written in
[triage.md](triage.md): every item gets one class, a licence class and a patent
note, and becomes a comment on an existing issue or a new issue in the right
milestone.

| Class | Meaning | Where it lands |
| --- | --- | --- |
| `have` | VMAFx already covers it | nothing, or a note on the existing issue |
| `oracle` | a reference implementation to compare against | RC7 reference conformance (#2286) |
| `metric` | a candidate new metric | milestone 1.3, New metrics |
| `data` | a dataset or a method for A/B and training | milestone 1.4 |
| `next-gen` | input for the next model generation | milestone 1.5 |
| `watch` | relevant but not actionable yet | the research watch (#2168) or the epic |
| `skip` | not relevant | nothing |

## Licence gate

Nothing from a source enters the tree because it was found by the radar. The licence
class decides what may happen to its code:

| Licence class | Meaning | Allowed |
| --- | --- | --- |
| `permissive` | MIT, BSD, Apache and similar | port directly with the notice the licence requires, or use as an oracle |
| `copyleft` | GPL and similar | oracle only; never linked or copied |
| `custom` | a bespoke or partial notice | read the text first; oracle only until it is read |
| `none` | no licence file | oracle run locally; clean-room re-implementation; ask the authors for a licence |

Oracle use means running the original locally to compare numbers; its code and its
model files are never vendored or shipped. A clean-room implementation is written from
the paper without reading the source, after a patent check. Details and the
VMAFx implementation shape are in [triage.md](triage.md).
