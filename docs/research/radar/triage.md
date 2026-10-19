<!-- markdownlint-disable MD013 -->
# Research radar triage procedure

An agent lane runs this once a week, after the digest issue exists (see the
[README](README.md)). It is not part of CI: it needs judgement and writes to the
issue tracker.

## Steps

1. Read the newest issue labelled `research-radar` and the `manual` entries of
   [`sources.yaml`](sources.yaml) (standards documents, proceedings, datasets).
2. For each item, look for what VMAFx already has: search the open and closed issues
   of the repository for its name, its method and its authors' repository, and read
   the existing metric list in `docs/metrics/`.
3. Give the item one class: `have`, `oracle`, `metric`, `data`, `next-gen`, `watch`
   or `skip` (definitions in the README).
4. Give it a licence class (`permissive`, `copyleft`, `custom`, `none`) by reading the
   licence file and the file headers of the repository, not the GitHub badge alone.
   Write what you read ("MIT text plus a citation clause"), not a guess.
5. Add a patent note: "none known", or the patent or standard that may read on the
   method. The check is a search of the paper's text and references, the venue's
   disclosures and a patent database for the method's name. Record that the check was
   done, by whom and when; absence of a hit is not a clearance.
6. Act on the class:
   - `oracle` and `metric` and `data` and `next-gen`: comment on the existing issue
     that covers it, or open a new issue in the milestone (RC7 reference
     conformance #2286, 1.3 New metrics, 1.4 A/B and data, 1.5 next model
     generation). Search first; never open a duplicate.
   - `have`, `watch`, `skip`: no new issue. A `watch` item goes to the research watch
     (#2168) when it concerns neural or generative codecs, otherwise it stays in the
     triage table.
7. Keep one table per sweep (item, class, licence class, patent note, outcome) in
   the working notes, and link the issues and comments it produced from the epic.
8. When the licence class is `none` and the item is valuable, open at most one
   polite issue on the authors' repository asking whether they would add a licence,
   after searching its issues for an earlier request. State what the project would
   use it for (an open-source quality library and reference conformance testing),
   name no person, and accept any answer. Limit: five requests per sweep.

## The VMAFx shape of an adopted metric

Every issue this procedure opens states the same plan:

- One CPU reference extractor, written from the paper. No code is copied from the
  original. If a permissively licensed implementation exists (for example
  `live_python_qa`, BSD-3-Clause), the issue says that porting it directly with its
  notice is an option and compares it with the clean-room route.
- GPU twins (CUDA, SYCL, HIP) and SIMD paths under the exact-twin contract: bit
  identical to the CPU extractor, or a measured tolerance recorded in an ADR.
- Conformance measured against the original implementation, run locally as an oracle
  and never shipped (the RC7 reference-conformance method, #2286): per-frame and
  pooled differences on the published example pairs and on the project fixtures.
- A patent check before any work starts, recorded in the issue.
- A speed comparison against the original as an RC7 / 1.x benchmark row. The aim is a
  faster and more accurate implementation; nothing is claimed until the measurement
  exists.

## Neutrality

The registry, the digests, the issues and the docs name sources: repositories,
organisations, venues, queries, standards and datasets. They do not describe people,
their employers or their activity, and they do not name companies that are not
sources. A maintainer-only directory of contacts is kept outside the repository and is
never copied into a tracked file or an issue.
